/* Copyright 2026 air/OS. SPDX-License-Identifier: MIT */
// Exercises the public ac_drm API against real queued hardware. No GL/winsys
// thread can mask an early completion by doing an implicit finish for us.
#include "ac_linux_drm.h"
#include "amdgpu_haiku.h"
#include "util/u_sync_provider.h"
#include "util/os_time.h"
#include <OS.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

static volatile uint32_t *gate;
static void release_gate() { if (gate) { *gate = 1; __sync_synchronize(); gate = nullptr; } }
static void require(bool ok, const char *why)
{
   if (!ok) { release_gate(); fprintf(stderr, "FAIL: %s (errno %d)\n", why, errno); exit(1); }
}
template<class T> static T request() { T r = {}; r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r); return r; }
struct Buffer { ac_drm_bo bo; uint32_t id; uint64_t va, bytes; uint32_t *cpu; };
static Buffer buffer(ac_drm_device *d, uint64_t va, uint64_t bytes)
{
   Buffer b = {}; b.va = va; b.bytes = bytes;
   amdgpu_bo_alloc_request a = {}; a.alloc_size = bytes; a.phys_alignment = 4096; a.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
   require(!ac_drm_bo_alloc(d, &a, &b.bo), "allocate BO");
   require(!ac_drm_bo_export(d, b.bo, amdgpu_bo_handle_type_kms, &b.id), "export local handle");
   require(!ac_drm_bo_cpu_map(d, b.bo, (void**)&b.cpu), "map CPU");
   if (va) require(!ac_drm_bo_va_op_raw(d, b.id, 0, bytes, va,
      AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE, AMDGPU_VA_OP_MAP), "map GPU");
   return b;
}
static uint64_t submit(ac_drm_device *d, uint32_t ctx, Buffer& ib, Buffer& data,
                       Buffer& fence, unsigned offset, uint32_t sync, unsigned slot, bool gated)
{
   std::vector<uint32_t> words;
   if (gated) words = {0xc0053c00, 0x13, uint32_t(data.va), uint32_t(data.va >> 32), 1, 0xffffffff, 4};
   uint64_t dst = data.va + 4096 + slot * 16;
   words.insert(words.end(), {0xc0063700, 0x100500, uint32_t(dst), uint32_t(dst >> 32),
      0x51000000 | slot, ~(0x51000000u | slot), slot, 0x51005100});
   words.resize(256, 0x80000000); memcpy(ib.cpu, words.data(), words.size() * 4);
   drm_amdgpu_cs_chunk_ib command = {}; command.va_start = ib.va; command.ib_bytes = words.size() * 4;
   command.ip_type = AMDGPU_HW_IP_GFX;
   drm_amdgpu_cs_chunk_fence f = {}; f.handle = fence.id; f.offset = offset;
   drm_amdgpu_cs_chunk_sem sem = {}; sem.handle = sync;
   drm_amdgpu_cs_chunk chunks[] = {
      {AMDGPU_CHUNK_ID_IB, sizeof(command) / 4, (uintptr_t)&command},
      {AMDGPU_CHUNK_ID_FENCE, sizeof(f) / 4, (uintptr_t)&f},
      {AMDGPU_CHUNK_ID_SYNCOBJ_OUT, sizeof(sem) / 4, (uintptr_t)&sem},
   };
   uint64_t seq = 0;
   require(!ac_drm_cs_submit_raw2(d, ctx, 0, sync ? 3 : 2, chunks, &seq), "submit returns before GPU gate");
   memset(ib.cpu, 0xd3, words.size() * 4); // Kernel must have captured the commands.
   return seq;
}
static void fence_status(ac_drm_device *d, uint32_t ctx, uint64_t seq, uint64_t timeout, uint64_t flags, bool done)
{
   uint32_t expired = 123;
   require(!ac_drm_cs_query_fence_status(d, ctx, AMDGPU_HW_IP_GFX, 0, 0, seq, timeout, flags, &expired)
      && expired == unsigned(done), "context fence status/deadline");
}
struct Drain {
   ac_drm_device *d; Buffer b; uint32_t ctx; unsigned operation;
   std::atomic<bool> entered{false}, done{false}; int result = -1;
};
static int32 drain(void *p)
{
   auto a = (Drain*)p; a->entered = true;
   if (a->operation == 0) a->result = ac_drm_bo_va_op_raw(a->d, a->b.id, 0, a->b.bytes, a->b.va, 0, AMDGPU_VA_OP_UNMAP);
   else if (a->operation == 1) a->result = ac_drm_bo_free(a->d, a->b.bo);
   else if (a->operation == 2) a->result = ac_drm_cs_ctx_free(a->d, a->ctx);
   else { ac_drm_device_deinitialize(a->d); a->result = 0; }
   a->done = true; return B_OK;
}
static void drain_test(Drain& a)
{
   auto t = spawn_thread(drain, "Mesa lifetime probe", B_NORMAL_PRIORITY, &a);
   require(t >= 0 && resume_thread(t) == B_OK, "start lifetime operation");
   while (!a.entered) snooze(100);
   snooze(5000);
   require(!a.done, "lifetime operation waits for actual pending GPU work");
   release_gate(); status_t result;
   require(wait_for_thread(t, &result) == B_OK && a.done && !a.result, "lifetime operation drains and joins");
}
int main(int argc, char **argv)
{
   disable_debugger(1);
   bool missing = argc == 2 && !strcmp(argv[1], "--expect-no-device");
   bool unprivileged = argc == 2 && !strcmp(argv[1], "--unprivileged");
   require(argc == 1 || missing || unprivileged, "arguments");
   if (unprivileged) require(setgid(65534) == 0 && setuid(65534) == 0, "drop privilege");
   setenv("HAIKU_AMDGPU_ASYNC", "1", 1); setenv("HAIKU_AMDGPU_VALIDATE_COMMANDS", "1", 1);
   int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
   if (missing) { require(fd < 0, "device absent"); puts("PASS: no native GPU device; no submission"); return 0; }
   require(fd >= 0, "open device");
   ac_drm_device *d = nullptr; uint32_t major, minor;
   int error = ac_drm_device_initialize(fd, false, &major, &minor, &d);
   if (unprivileged) { require(error == -EACCES && !d, "root gate"); close(fd); puts("PASS: unprivileged Mesa queue rejected"); return 0; }
   require(!error && d, "initialize queued backend");
   auto before = request<amdgpu_irq_info>(); require(!ioctl(fd, AMDGPU_IRQ_INFO, &before, sizeof(before)), "IRQ before");
   auto ib = buffer(d, 0x300000000, 65536), data = buffer(d, 0x300010000, 65536);
   auto f = buffer(d, 0, 4096), spare = buffer(d, 0x300020000, 4096);
   auto disposable = buffer(d, 0, 4096);
   for (unsigned i = 0; i < data.bytes / 4; ++i) data.cpu[i] = i ? 0x6b6b6b6b : 0;
   uint32_t a, b, signal, reset, destroyed;
   require(!ac_drm_cs_ctx_create2(d, AMDGPU_CTX_PRIORITY_NORMAL, &a)
      && !ac_drm_cs_ctx_create2(d, AMDGPU_CTX_PRIORITY_NORMAL, &b), "create contexts");
   auto p = ac_drm_device_get_sync_provider(d);
   require(!p->create(p, 0, &signal) && !p->create(p, 0, &reset) && !p->create(p, 0, &destroyed), "create syncs");
   gate = data.cpu;
   require(submit(d, a, ib, data, f, 0, signal, 0, true) == 1, "first context sequence");
   require(submit(d, b, ib, data, f, 8, reset, 1, false) == 1, "independent context sequence");
   require(submit(d, a, ib, data, f, 0, destroyed, 2, false) == 2, "next context sequence");
   fence_status(d, a, 2, 0, 0, false);
   auto start = os_time_get_nano(); fence_status(d, a, 2, 5000000, 0, false);
   require(os_time_get_nano() - start >= 4000000, "relative wait does not expire early");
   fence_status(d, b, 1, os_time_get_nano() + 5000000, AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE, false);
   require(((uint64_t*)f.cpu)[0] == 0 && ((uint64_t*)f.cpu)[1] == 0, "CPU fences remain unpublished while GPU waits");
   bool busy = false; require(!ac_drm_bo_wait_for_idle(d, data.bo, 0, &busy) && busy, "pending BO poll");
   require(p->wait(p, &signal, 1, os_time_get_nano(), 0, nullptr) == -ETIME, "pending sync poll");
   require(!p->reset(p, &reset, 1) && !p->destroy(p, destroyed), "replace/destroy pending output syncs");
   release_gate(); fence_status(d, a, 2, 5000000000, 0, true);
   require(((uint64_t*)f.cpu)[0] == 2 && ((uint64_t*)f.cpu)[1] == 1, "CPU fences publish actual completed sequences");
   require(!p->wait(p, &signal, 1, os_time_get_nano(), 0, nullptr), "completed sync");
   require(p->wait(p, &reset, 1, os_time_get_nano(), 0, nullptr) == -ETIME, "old completion cannot signal reset generation");
   require(!ac_drm_bo_wait_for_idle(d, data.bo, 5000000000, &busy) && !busy, "completed BO idle");
   for (unsigned operation = 0; operation < 3; ++operation) {
      data.cpu[0] = 0; gate = data.cpu; __sync_synchronize();
      require(submit(d, a, ib, data, f, 0, reset, 3 + operation, true) == 3 + operation, "lifetime sequence");
      Drain args; args.d = d; args.b = operation == 0 ? spare : disposable; args.ctx = a; args.operation = operation;
      drain_test(args);
      require(((uint64_t*)f.cpu)[0] == 3 + operation, "lifetime drain publishes CPU fence");
      require(!p->wait(p, &reset, 1, os_time_get_nano(), 0, nullptr), "new output sync generation signals");
   }
   for (unsigned i = 0; i < data.bytes / 4; ++i) {
      uint32_t expected = i ? 0x6b6b6b6b : 1;
      if (i >= 1024 && i < 1048) { unsigned slot = (i - 1024) / 4; uint32_t value = 0x51000000 | slot;
         const uint32_t values[] = {value, ~value, slot, 0x51005100}; expected = values[(i - 1024) % 4]; }
      require(data.cpu[i] == expected, "all queued outputs and guards");
   }
   require(!ac_drm_bo_free(d, spare.bo), "free unmapped spare");
   data.cpu[0] = 0; gate = data.cpu; __sync_synchronize();
   require(submit(d, b, ib, data, f, 8, 0, 6, true) == 2, "shutdown sequence");
   Drain args; args.d = d; args.operation = 3; drain_test(args);
   auto after = request<amdgpu_irq_info>(); require(!ioctl(fd, AMDGPU_IRQ_INFO, &after, sizeof(after))
      && after.status == B_OK && !after.vm_faults && !after.privileged_faults && !after.unknown && !after.overflows
      && after.completed_fences == before.completed_fences + 7 && after.waits == before.waits + 7,
      "seven real queued GPU jobs, healthy independent IRQ state");
   close(fd);
   puts("PASS: queued Mesa pending/completed CPU, context and sync fences; deadlines, generations, immutable IBs, guards, BO/VM/context/device lifetime");
   return 0;
}
