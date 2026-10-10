/* Copyright 2026 air/OS. SPDX-License-Identifier: MIT
 * Native Haiku implementation of Mesa's internal AMD OS abstraction.
 * DRM-shaped argument structures stay inside Mesa; all kernel calls use the
 * separately versioned native ABI. Raw GPU work is currently root-only.
 */
#include "ac_linux_drm.h"
#include "ac_gpu_info.h"
#include "haiku/amdgpu_haiku.h"
#include "util/u_sync_provider.h"
#include "util/vma.h"
#include "util/os_time.h"
#include <OS.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <new>
#include <vector>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Haiku's ENOMEM is INT_MIN. Unsigned subtraction preserves Mesa's -errno
// convention without signed overflow when converting that particular value.
static constexpr int negative_enomem = (int)(0u - (unsigned)ENOMEM);

struct amdgpu_bo {
   uint32_t id;
   amdgpu_buffer native;
   uint32_t heap;
   uint64_t flags;
   void *cpu = nullptr;
   area_id area = -1;
   unsigned maps = 0;
};
struct NativeMapping { amdgpu_bo *bo; uint64_t offset, size; uint32_t permissions; };
struct NativeSync : util_sync_provider { ac_drm_device *dev; };
struct amdgpu_va { ac_drm_device *dev; util_vma_heap *heap; uint64_t address, size; };
struct ac_drm_device {
   int fd = -1;
   amdgpu_info identity = {};
   amdgpu_render_info info = {};
   std::mutex mutex;
   std::condition_variable changed;
   NativeSync sync = {};
   std::map<uint32_t, amdgpu_bo*> bos;
   std::map<uint64_t, NativeMapping> mappings;
   std::map<uint32_t, bool> syncs;
   std::map<uint32_t, uint64_t> contexts;
   uint32_t next_bo = 1, next_sync = 1, next_context = 1;
   util_vma_heap low, high;
   amdgpu_buffer commands = {};
   void *command_cpu = nullptr;
   area_id command_area = -1;
   bool lost = false;
};

template<class T> static T request()
{
   T value = {};
   value.version = AMDGPU_HAIKU_ABI_VERSION;
   value.size = sizeof(value);
   return value;
}
template<class T> static int native_call(ac_drm_device *d, uint32_t op, T& value)
{
   if (ioctl(d->fd, op, &value, sizeof(value)) == 0) return 0;
   switch (errno) {
   case B_BAD_VALUE: case B_BAD_ADDRESS: return -EINVAL;
   case B_NOT_ALLOWED: return -EACCES;
   case B_NO_MEMORY: return negative_enomem;
   case B_NOT_SUPPORTED: return -ENOTSUP;
   default: return -EIO;
   }
}
static bool range(uint64_t offset, uint64_t bytes, uint64_t size)
{ return offset <= size && bytes <= size - offset; }
static bool pow2(uint64_t value) { return value && !(value & (value - 1)); }
static amdgpu_bo *lookup(ac_drm_device *d, uint32_t id)
{
   auto it = d->bos.find(id);
   return it == d->bos.end() ? nullptr : it->second;
}
static int map_cpu(ac_drm_device *d, amdgpu_bo *bo)
{
   if (bo->cpu) return 0;
   auto r = request<amdgpu_buffer>(); r.handle = bo->native.handle;
   int err = native_call(d, AMDGPU_MAP_BUFFER, r);
   if (err) return err;
   bo->cpu = (void*)(uintptr_t)r.address; bo->area = r.area;
   return 0;
}
static ac_drm_device *sync_device(util_sync_provider *p)
{ return static_cast<NativeSync*>(p)->dev; }
static int sync_create(util_sync_provider *p, uint32_t flags, uint32_t *out)
{
   auto d = sync_device(p); std::lock_guard<std::mutex> lock(d->mutex);
   if (!out || flags & ~DRM_SYNCOBJ_CREATE_SIGNALED || !d->next_sync) return -EINVAL;
   *out = d->next_sync++;
   d->syncs.emplace(*out, flags & DRM_SYNCOBJ_CREATE_SIGNALED);
   return 0;
}
static int sync_destroy(util_sync_provider *p, uint32_t id)
{
   auto d = sync_device(p); std::lock_guard<std::mutex> lock(d->mutex);
   bool erased = d->syncs.erase(id); d->changed.notify_all();
   return erased ? 0 : -EINVAL;
}
static int sync_set(util_sync_provider *p, const uint32_t *ids, uint32_t count, bool value)
{
   auto d = sync_device(p); std::lock_guard<std::mutex> lock(d->mutex);
   for (uint32_t i = 0; i < count; ++i) if (!d->syncs.count(ids[i])) return -EINVAL;
   for (uint32_t i = 0; i < count; ++i) d->syncs[ids[i]] = value;
   d->changed.notify_all(); return 0;
}
static int sync_reset(util_sync_provider *p, const uint32_t *ids, uint32_t count)
{ return sync_set(p, ids, count, false); }
static int sync_signal(util_sync_provider *p, const uint32_t *ids, uint32_t count)
{ return sync_set(p, ids, count, true); }
static int sync_wait(util_sync_provider *p, uint32_t *ids, unsigned count,
                     int64_t deadline, unsigned flags, uint32_t *first)
{
   auto d = sync_device(p); std::unique_lock<std::mutex> lock(d->mutex);
   if (!ids || !count || flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT))
      return -EINVAL;
   for (;;) {
      unsigned signaled = 0, index = 0;
      for (unsigned i = 0; i < count; ++i) {
         auto it = d->syncs.find(ids[i]); if (it == d->syncs.end()) return -EINVAL;
         if (it->second) { if (!signaled) index = i; signaled++; }
      }
      if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) ? signaled == count : signaled != 0) {
         if (first) *first = index;
         return 0;
      }
      if (d->lost) return -EIO;
      int64_t now = os_time_get_nano();
      if (deadline <= now) return -ETIME;
      // Both os_time_get_nano and Mesa's absolute fence deadlines are monotonic.
      d->changed.wait_for(lock, std::chrono::nanoseconds(std::min<int64_t>(deadline - now, 1000000000)));
   }
}
static int sync_to_fd(util_sync_provider*, uint32_t, int*) { return -ENOTSUP; }
static int sync_from_fd(util_sync_provider*, int, uint32_t*) { return -ENOTSUP; }
static int sync_import(util_sync_provider*, uint32_t, int) { return -ENOTSUP; }
static void sync_finalize(util_sync_provider*) {}

int ac_drm_device_initialize(int fd, bool virtio, uint32_t *major, uint32_t *minor, ac_drm_device **out)
{
   if (virtio || !out || geteuid() != 0) return -EACCES;
   *out = nullptr;
   auto d = new(std::nothrow) ac_drm_device;
   if (!d) return negative_enomem;
   d->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
   if (d->fd < 0) { delete d; return -errno; }
   d->identity = request<amdgpu_info>();
   d->info = request<amdgpu_render_info>();
   auto vm = request<amdgpu_vm_info>();
   int err = native_call(d, AMDGPU_GET_INFO, d->identity);
   if (!err) err = native_call(d, AMDGPU_RENDER_INFO, d->info);
   if (!err) err = native_call(d, AMDGPU_VM_INFO, vm);
   if (!err && (d->identity.device != 0x67c7 || d->info.gfx_major != 8
               || d->info.address_end <= (8ull << 30) || d->info.max_ib_bytes < 65536)) err = -ENOTSUP;
   // Mesa's color-2D table slot must describe the eight-pipe Polaris layout.
   // Firmware's inherited table is not necessarily initialized for graphics.
   // Reject it before ac_query_gpu_info/addrlib can assert; never substitute
   // invented query values while the hardware still has different tables.
   if (!err && ((d->info.tile_mode[14] >> 6) & 31) != 12) {
      fprintf(stderr, "amdgpu: native kernel must initialize Polaris tile tables (color-2D %#x)\n",
              d->info.tile_mode[14]);
      err = -ENOTSUP;
   }
   // Reserve a private submission buffer outside the application VA heaps.
   if (!err) {
      d->commands = request<amdgpu_buffer>(); d->commands.bytes = d->info.max_ib_bytes;
      err = native_call(d, AMDGPU_CREATE_SYSTEM_BUFFER, d->commands);
   }
   if (!err) {
      auto cpu = request<amdgpu_buffer>(); cpu.handle = d->commands.handle;
      err = native_call(d, AMDGPU_MAP_BUFFER, cpu);
      if (!err) { d->command_cpu = (void*)(uintptr_t)cpu.address; d->command_area = cpu.area; }
   }
   if (!err) {
      auto m = request<amdgpu_vm_mapping>(); m.handle = d->commands.handle;
      m.address = d->info.address_start; m.bytes = d->commands.bytes;
      m.permissions = AMDGPU_VM_READ | AMDGPU_VM_EXECUTE;
      err = native_call(d, AMDGPU_VM_MAP, m);
   }
   if (err) { close(d->fd); delete d; return err; }
   util_vma_heap_init(&d->low, (1ull << 32) + 65536, (1ull << 32) - 65536);
   util_vma_heap_init(&d->high, 2ull << 32, d->info.address_end - (2ull << 32));
   d->sync.dev = d; d->sync.create = sync_create; d->sync.destroy = sync_destroy;
   d->sync.wait = sync_wait; d->sync.reset = sync_reset; d->sync.signal = sync_signal;
   d->sync.handle_to_fd = sync_to_fd; d->sync.fd_to_handle = sync_from_fd;
   d->sync.export_sync_file = sync_to_fd; d->sync.import_sync_file = sync_import;
   d->sync.finalize = sync_finalize;
   *major = 0; *minor = AMDGPU_HAIKU_ABI_VERSION; // Native ABI, not a Linux DRM version.
   *out = d;
   fprintf(stderr, "amdgpu: native Haiku ABI %u, %u CUs, root synchronous prototype\n", *minor, d->info.active_cus);
   return 0;
}
util_sync_provider *ac_drm_device_get_sync_provider(ac_drm_device *d) { return &d->sync; }
uintptr_t ac_drm_device_get_cookie(ac_drm_device *d) { return (uintptr_t)d; }
int ac_drm_device_get_fd(ac_drm_device *d) { return d->fd; }
void ac_drm_device_deinitialize(ac_drm_device *d)
{
   // The kernel revokes CPU clones and retires retained VM mappings on close.
   close(d->fd);
   for (auto& entry : d->bos) delete entry.second;
   util_vma_heap_finish(&d->low); util_vma_heap_finish(&d->high);
   delete d;
}
int ac_drm_query_pci_bus_info(ac_drm_device *d, radeon_info *info)
{
   info->pci.domain = 0; info->pci.bus = d->identity.bus;
   info->pci.dev = d->identity.slot; info->pci.func = d->identity.function;
   return 0;
}
const char *ac_drm_get_marketing_name(ac_drm_device*) { return "AMD Radeon Pro WX 5100 (native Haiku prototype)"; }
int ac_drm_query_hw_ip_count(ac_drm_device*, unsigned type, uint32_t *count)
{ *count = type == AMDGPU_HW_IP_GFX ? 1 : 0; return 0; }
int ac_drm_query_hw_ip_info(ac_drm_device *d, unsigned type, unsigned instance, drm_amdgpu_info_hw_ip *info)
{
   memset(info, 0, sizeof(*info));
   if (type != AMDGPU_HW_IP_GFX || instance) return -ENOTSUP;
   info->hw_ip_version_major = d->info.gfx_major; info->hw_ip_version_minor = d->info.gfx_minor;
   info->available_rings = 1; info->ib_start_alignment = d->info.ib_address_alignment;
   info->ib_size_alignment = d->info.ib_size_alignment;
   return 0;
}
int ac_drm_query_firmware_version(ac_drm_device *d, unsigned type, unsigned instance,
                                  unsigned index, uint32_t *version, uint32_t *feature)
{
   if (instance || index) return -EINVAL;
   unsigned slot;
   switch (type) {
   case AMDGPU_INFO_FW_GFX_CE: slot = 0; break;
   case AMDGPU_INFO_FW_GFX_PFP: slot = 1; break;
   case AMDGPU_INFO_FW_GFX_ME: slot = 2; break;
   case AMDGPU_INFO_FW_GFX_RLC: slot = 3; break;
   case AMDGPU_INFO_FW_GFX_MEC: slot = 4; break;
   default: return -ENOTSUP;
   }
   *version = d->info.firmware_version[slot]; *feature = d->info.firmware_feature[slot]; return 0;
}
int ac_drm_query_sw_info(ac_drm_device*, amdgpu_sw_info type, void *value)
{
   if (type != amdgpu_sw_info_address32_hi) return -ENOTSUP;
   *(uint32_t*)value = 1; return 0;
}
int ac_drm_query_gpu_info(ac_drm_device *d, amdgpu_gpu_info *g)
{
   memset(g, 0, sizeof(*g)); auto& i = d->info;
   g->asic_id = d->identity.device; g->chip_rev = i.chip_revision; g->chip_external_rev = i.chip_revision + 0x50;
   g->family_id = AMDGPU_FAMILY_VI; g->pci_rev_id = d->identity.revision;
   g->num_shader_engines = i.shader_engines; g->num_shader_arrays_per_engine = i.shader_arrays_per_engine;
   g->rb_pipes = i.shader_engines * i.backends_per_engine; g->enabled_rb_pipes_mask = i.enabled_backends;
   g->gpu_counter_freq = i.timestamp_khz; g->mc_arb_ramcfg = i.mc_arb_ramcfg; g->gb_addr_cfg = i.gb_addr_config;
   memcpy(g->gb_tile_mode, i.tile_mode, sizeof(i.tile_mode));
   memcpy(g->gb_macro_tile_mode, i.macrotile_mode, sizeof(i.macrotile_mode));
   for (unsigned se = 0; se < 4; ++se) for (unsigned sa = 0; sa < 2; ++sa) g->cu_bitmap[se][sa] = i.cu_mask[se][sa];
   g->vram_type = AMDGPU_VRAM_TYPE_GDDR5; g->vram_bit_width = i.vram_bus_width;
   g->ce_ram_size = 32768; g->num_hw_gfx_contexts = 8; g->cu_active_number = i.active_cus;
   for (unsigned se = 0; se < 4; ++se) {
      g->backend_disable[se] = i.rb_disable[se][0];
      g->pa_sc_raster_cfg[se] = i.raster_config[se][0];
      g->pa_sc_raster_cfg1[se] = i.raster_config_1[se][0];
   }
   return 0;
}
int ac_drm_query_info(ac_drm_device *d, unsigned type, unsigned size, void *value)
{
   if (!value) return -EINVAL;
   auto& i = d->info;
   if (type == AMDGPU_INFO_DEV_INFO && size == sizeof(drm_amdgpu_info_device)) {
      auto g = (drm_amdgpu_info_device*)value; memset(g, 0, size);
      g->device_id = d->identity.device; g->chip_rev = i.chip_revision;
      g->external_rev = i.chip_revision + 0x50; g->pci_rev = d->identity.revision; g->family = AMDGPU_FAMILY_VI;
      g->num_shader_engines = i.shader_engines; g->num_shader_arrays_per_engine = i.shader_arrays_per_engine;
      g->gpu_counter_freq = i.timestamp_khz;
      // No DPM maximum has been measured; do not label ROM defaults as maxima.
      g->cu_active_number = i.active_cus; g->enabled_rb_pipes_mask = i.enabled_backends;
      g->num_rb_pipes = i.shader_engines * i.backends_per_engine; g->num_hw_gfx_contexts = 8;
      g->virtual_address_offset = 1ull << 32; g->virtual_address_max = i.address_end;
      g->virtual_address_alignment = g->gart_page_size = g->pte_fragment_size = i.page_size;
      g->ce_ram_size = 32768; g->vram_type = AMDGPU_VRAM_TYPE_GDDR5; g->vram_bit_width = i.vram_bus_width;
      g->wave_front_size = 64; g->num_shader_visible_vgprs = 256;
      g->num_cu_per_sh = i.cu_per_array; g->num_tcc_blocks = i.tcc_blocks;
      for (unsigned se = 0; se < 4; ++se) for (unsigned sa = 0; sa < 2; ++sa)
         g->cu_bitmap[se][sa] = g->cu_ao_bitmap[se][sa] = i.cu_mask[se][sa];
      return 0;
   }
   if (type == AMDGPU_INFO_MEMORY && size == sizeof(drm_amdgpu_memory_info)) {
      auto m = (drm_amdgpu_memory_info*)value; memset(m, 0, size);
      m->vram.total_heap_size = m->vram.usable_heap_size = i.total_vram;
      m->cpu_accessible_vram.total_heap_size = m->cpu_accessible_vram.usable_heap_size = i.visible_vram;
      m->gtt.total_heap_size = m->gtt.usable_heap_size = i.total_gart;
      return 0;
   }
   if (type == AMDGPU_INFO_MAX_IBS && size % 4 == 0 && size >= 4) {
      memset(value, 0, size); ((uint32_t*)value)[AMDGPU_HW_IP_GFX] = 2; return 0;
   }
   if (type == AMDGPU_INFO_TIMESTAMP && size == 8) {
      auto r = request<amdgpu_render_info>(); int err = native_call(d, AMDGPU_RENDER_INFO, r);
      if (!err) *(uint64_t*)value = r.gpu_timestamp;
      return err;
   }
   return -ENOTSUP;
}
void ac_haiku_limit_gpu_info(ac_drm_device *d, radeon_info *i)
{
   i->has_userptr = false; i->has_timeline_syncobj = false; i->has_fence_to_handle = false;
   i->has_vm_always_valid = false; i->has_bo_metadata = false; i->has_sparse_vm_mappings = false;
   i->has_gang_submit = false; i->has_gpuvm_fault_query = false; i->has_pcie_bandwidth_info = false;
   i->has_stable_pstate = false; i->has_kernelq_reg_shadowing = false; i->has_fw_based_shadowing = false;
   i->has_tmz_support = false; i->has_trap_handler_support = false; i->kernel_has_modifiers = false;
   i->has_default_zerovram_support = true; i->userq_ip_mask = 0;
   i->max_heap_size_kb = d->info.max_buffer_bytes / 1024;
}
int ac_drm_read_mm_registers(ac_drm_device *d, unsigned offset, unsigned count,
                            uint32_t instance, uint32_t flags, uint32_t *values)
{
   if (!values || flags || instance != UINT32_MAX) return -ENOTSUP;
   auto& i = d->info;
   if (offset >= 0x2644 && offset - 0x2644 <= 32 && count <= 32 - (offset - 0x2644))
      memcpy(values, i.tile_mode + offset - 0x2644, count * 4);
   else if (offset >= 0x2664 && offset - 0x2664 <= 16 && count <= 16 - (offset - 0x2664))
      memcpy(values, i.macrotile_mode + offset - 0x2664, count * 4);
   else if (count == 1 && offset == 0x263e) *values = i.gb_addr_config;
   else if (count == 1 && offset == 0xa0d4) *values = i.raster_config[0][0];
   else if (count == 1 && offset == 0xa0d5) *values = i.raster_config_1[0][0];
   else return -ENOTSUP;
   return 0;
}

int ac_drm_bo_alloc(ac_drm_device *d, amdgpu_bo_alloc_request *a, ac_drm_bo *out)
{
   std::lock_guard<std::mutex> lock(d->mutex);
   const uint64_t supported = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED | AMDGPU_GEM_CREATE_NO_CPU_ACCESS
      | AMDGPU_GEM_CREATE_CPU_GTT_USWC | AMDGPU_GEM_CREATE_VRAM_CLEARED;
   if (d->lost) return -EIO;
   if (!a || !out || !a->alloc_size || a->alloc_size > d->info.max_buffer_bytes
       || a->flags & ~supported || a->preferred_heap & ~(AMDGPU_GEM_DOMAIN_VRAM | AMDGPU_GEM_DOMAIN_GTT)
       || !a->preferred_heap || !d->next_bo) return -EINVAL;
   auto b = new(std::nothrow) amdgpu_bo;
   if (!b) return negative_enomem;
   b->native = request<amdgpu_buffer>(); b->native.bytes = a->alloc_size;
   b->heap = (a->preferred_heap & AMDGPU_GEM_DOMAIN_VRAM) ? AMDGPU_GEM_DOMAIN_VRAM : AMDGPU_GEM_DOMAIN_GTT;
   b->flags = a->flags;
   uint32_t op = b->heap == AMDGPU_GEM_DOMAIN_GTT ? AMDGPU_CREATE_SYSTEM_BUFFER
      : (a->flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS) ? AMDGPU_CREATE_DEVICE_BUFFER : AMDGPU_CREATE_BUFFER;
   int err = native_call(d, op, b->native);
   if (err) { delete b; return err; }
   b->id = d->next_bo++; d->bos.emplace(b->id, b); out->abo = b;
   return 0;
}
int ac_drm_bo_export(ac_drm_device *d, ac_drm_bo bo, amdgpu_bo_handle_type type, uint32_t *id)
{
   std::lock_guard<std::mutex> lock(d->mutex);
   if (type != amdgpu_bo_handle_type_kms || !bo.abo || lookup(d, bo.abo->id) != bo.abo) return -ENOTSUP;
   *id = bo.abo->id; return 0;
}
int ac_drm_bo_free(ac_drm_device *d, ac_drm_bo bo)
{
   std::lock_guard<std::mutex> lock(d->mutex);
   auto b = bo.abo;
   if (!b || lookup(d, b->id) != b) return -EINVAL;
   // Winsys removes every VA mapping before freeing its BO.
   for (auto& m : d->mappings) if (m.second.bo == b) return -EBUSY;
   auto r = request<amdgpu_buffer>(); r.handle = b->native.handle;
   int err = native_call(d, AMDGPU_FREE_BUFFER, r);
   if (err) return err;
   d->bos.erase(b->id); delete b; return 0;
}
int ac_drm_bo_cpu_map(ac_drm_device *d, ac_drm_bo bo, void **cpu)
{
   std::lock_guard<std::mutex> lock(d->mutex);
   if (!bo.abo || lookup(d, bo.abo->id) != bo.abo) return -EINVAL;
   int err = map_cpu(d, bo.abo);
   if (!err) { bo.abo->maps++; *cpu = bo.abo->cpu; }
   return err;
}
int ac_drm_bo_cpu_unmap(ac_drm_device *d, ac_drm_bo bo)
{
   std::lock_guard<std::mutex> lock(d->mutex);
   if (!bo.abo || lookup(d, bo.abo->id) != bo.abo || !bo.abo->maps) return -EINVAL;
   bo.abo->maps--; return 0; // Keep one CPU clone until native FREE revokes it.
}
int ac_drm_bo_query_info(ac_drm_device *d, uint32_t id, amdgpu_bo_info *out)
{
   std::lock_guard<std::mutex> lock(d->mutex); auto b = lookup(d, id);
   if (!b) return -EINVAL;
   memset(out, 0, sizeof(*out)); out->alloc_size = b->native.bytes; out->phys_alignment = 4096;
   out->preferred_heap = b->heap; out->alloc_flags = b->flags; return 0;
}
int ac_drm_bo_wait_for_idle(ac_drm_device *d, ac_drm_bo bo, uint64_t, bool *busy)
{
   // The queue worker has a winsys fence for unsubmitted jobs. Native calls
   // are synchronous and hold this mutex until the private hardware fence.
   std::lock_guard<std::mutex> lock(d->mutex);
   if (!bo.abo || lookup(d, bo.abo->id) != bo.abo) return -EINVAL;
   *busy = false; return d->lost ? -EIO : 0;
}
int ac_drm_bo_va_op_raw(ac_drm_device *d, uint32_t id, uint64_t offset, uint64_t size,
                        uint64_t addr, uint64_t flags, uint32_t op)
{
   std::lock_guard<std::mutex> lock(d->mutex);
   if (!size || (addr | size | offset) & 4095) return -EINVAL;
   auto r = request<amdgpu_vm_mapping>(); r.address = addr; r.bytes = size;
   if (op == AMDGPU_VA_OP_UNMAP || op == AMDGPU_VA_OP_CLEAR) {
      auto it = d->mappings.find(addr);
      if (it == d->mappings.end() || it->second.size != size) return -EINVAL;
      int err = native_call(d, AMDGPU_VM_UNMAP, r);
      if (!err) d->mappings.erase(it);
      return err;
   }
   const uint64_t supported = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE;
   if (op != AMDGPU_VA_OP_MAP || flags & ~supported) return -ENOTSUP;
   auto b = lookup(d, id);
   if (!b || !range(offset, size, b->native.bytes)) return -EINVAL;
   r.handle = b->native.handle; r.buffer_offset = offset;
   if (flags & AMDGPU_VM_PAGE_READABLE) r.permissions |= AMDGPU_VM_READ;
   if (flags & AMDGPU_VM_PAGE_WRITEABLE) r.permissions |= AMDGPU_VM_WRITE;
   if (flags & AMDGPU_VM_PAGE_EXECUTABLE) r.permissions |= AMDGPU_VM_EXECUTE;
   int err = native_call(d, AMDGPU_VM_MAP, r);
   if (!err) d->mappings.emplace(addr, NativeMapping{b, offset, size, r.permissions});
   return err;
}
int ac_drm_bo_va_op(ac_drm_device *d, uint32_t id, uint64_t o, uint64_t s, uint64_t a, uint64_t f, uint32_t op)
{ return ac_drm_bo_va_op_raw(d, id, o, s, a, f, op); }
int ac_drm_va_range_alloc(ac_drm_device *d, amdgpu_gpu_va_range type, uint64_t size,
                          uint64_t align, uint64_t required, uint64_t *allocated,
                          amdgpu_va_handle *out, uint64_t flags)
{
   if (type != amdgpu_gpu_va_range_general || !size || size > d->info.address_end
       || flags & ~(AMDGPU_VA_RANGE_32_BIT | AMDGPU_VA_RANGE_HIGH) || (align && !pow2(align))) return -EINVAL;
   size = (size + 4095) & ~4095ull; align = std::max<uint64_t>(align, 4096);
   auto v = new(std::nothrow) amdgpu_va;
   if (!v) return negative_enomem;
   std::lock_guard<std::mutex> lock(d->mutex);
   v->dev = d; v->heap = flags & AMDGPU_VA_RANGE_32_BIT ? &d->low : &d->high; v->size = size;
   v->address = required ? ((required % align == 0 && util_vma_heap_alloc_addr(v->heap, required, size)) ? required : 0)
      : util_vma_heap_alloc(v->heap, size, align);
   if (!v->address) { delete v; return -ENOSPC; }
   *allocated = v->address; *out = v; return 0;
}
int ac_drm_va_range_free(amdgpu_va_handle v)
{
   if (!v) return -EINVAL;
   { std::lock_guard<std::mutex> lock(v->dev->mutex); util_vma_heap_free(v->heap, v->address, v->size); }
   delete v; return 0;
}
// The current winsys still uses these two libdrm-named VA accessors directly.
uint64_t amdgpu_va_get_start_addr(amdgpu_va_handle v) { return v->address; }
int amdgpu_va_range_free(amdgpu_va_handle v) { return ac_drm_va_range_free(v); }
int ac_drm_va_range_query(ac_drm_device *d, amdgpu_gpu_va_range type, uint64_t *start, uint64_t *end)
{
   if (type != amdgpu_gpu_va_range_general) return -ENOTSUP;
   *start = 1ull << 32; *end = d->info.address_end - 1; return 0;
}
int ac_drm_cs_ctx_create2(ac_drm_device *d, uint32_t priority, uint32_t *id)
{
   std::lock_guard<std::mutex> lock(d->mutex);
   if (priority != AMDGPU_CTX_PRIORITY_NORMAL || !d->next_context) return -ENOTSUP;
   *id = d->next_context++; d->contexts.emplace(*id, 0); return 0;
}
int ac_drm_cs_ctx_free(ac_drm_device *d, uint32_t id)
{
   std::lock_guard<std::mutex> lock(d->mutex); return d->contexts.erase(id) ? 0 : -EINVAL;
}
int ac_drm_cs_query_reset_state2(ac_drm_device *d, uint32_t id, uint64_t *flags)
{
   std::lock_guard<std::mutex> lock(d->mutex);
   if (!d->contexts.count(id)) return -EINVAL;
   auto r = request<amdgpu_memory_info>(); int err = native_call(d, AMDGPU_MEMORY_INFO, r);
   if (err) return err;
   if (r.faulted) d->lost = true;
   *flags = d->lost ? AMDGPU_CTX_QUERY2_FLAGS_RESET | AMDGPU_CTX_QUERY2_FLAGS_RESET_IN_PROGRESS : 0;
   return 0;
}
int ac_drm_cs_query_fence_status(ac_drm_device *d, uint32_t id, uint32_t type, uint32_t instance,
                                 uint32_t ring, uint64_t seq, uint64_t, uint64_t flags, uint32_t *expired)
{
   std::lock_guard<std::mutex> lock(d->mutex);
   auto it = d->contexts.find(id);
   if (it == d->contexts.end() || type != AMDGPU_HW_IP_GFX || instance || ring
       || flags & ~AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE) return -EINVAL;
   *expired = seq <= it->second; return d->lost ? -EIO : 0;
}
int ac_drm_cs_create_syncobj2(ac_drm_device *d, uint32_t flags, uint32_t *id) { return sync_create(&d->sync, flags, id); }
int ac_drm_cs_destroy_syncobj(ac_drm_device *d, uint32_t id) { return sync_destroy(&d->sync, id); }
int ac_drm_cs_syncobj_wait(ac_drm_device *d, uint32_t *ids, unsigned count, int64_t deadline, unsigned flags, uint32_t *first)
{ return sync_wait(&d->sync, ids, count, deadline, flags, first); }
void ac_drm_cs_chunk_fence_info_to_data(uint32_t id, uint64_t offset, drm_amdgpu_cs_chunk_data *data)
{ data->fence_data.handle = id; data->fence_data.offset = offset * sizeof(uint64_t); }

int ac_drm_cs_submit_raw2(ac_drm_device *d, uint32_t ctx, uint32_t list,
                          int count, drm_amdgpu_cs_chunk *chunks, uint64_t *seq)
{
   std::vector<uint32_t> dependencies, signals;
   std::vector<drm_amdgpu_cs_chunk_ib> ibs;
   drm_amdgpu_cs_chunk_fence fence = {};
   bool has_fence = false;
   if (list || count <= 0 || count > 8 || !chunks || !seq) return -EINVAL;
   // Parse the complete submit before issuing any work or modifying a fence.
   std::unique_lock<std::mutex> lock(d->mutex);
   if (d->lost) return -EIO;
   if (!d->contexts.count(ctx)) return -EINVAL;
   uint64_t bytes = 0;
   for (int n = 0; n < count; ++n) {
      auto& c = chunks[n]; void *ptr = (void*)(uintptr_t)c.chunk_data;
      if (!ptr || c.length_dw > 65536) return -EINVAL;
      if (c.chunk_id == AMDGPU_CHUNK_ID_IB) {
         if (c.length_dw != sizeof(drm_amdgpu_cs_chunk_ib) / 4 || ibs.size() == 2) return -EINVAL;
         auto ib = *(drm_amdgpu_cs_chunk_ib*)ptr;
         if (ib.ip_type != AMDGPU_HW_IP_GFX || ib.ip_instance || ib.ring
             || ib.flags & ~AMDGPU_IB_FLAG_PREAMBLE || !ib.ib_bytes || ib.ib_bytes & 3) return -ENOTSUP;
         bytes += ib.ib_bytes; ibs.push_back(ib);
      } else if (c.chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_IN || c.chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_OUT) {
         auto sem = (drm_amdgpu_cs_chunk_sem*)ptr;
         for (unsigned j = 0; j < c.length_dw; ++j) {
            if (!d->syncs.count(sem[j].handle)) return -EINVAL;
            (c.chunk_id == AMDGPU_CHUNK_ID_SYNCOBJ_IN ? dependencies : signals).push_back(sem[j].handle);
         }
      } else if (c.chunk_id == AMDGPU_CHUNK_ID_FENCE) {
         if (has_fence || c.length_dw != sizeof(fence) / 4) return -EINVAL;
         fence = *(drm_amdgpu_cs_chunk_fence*)ptr; has_fence = true;
         auto b = lookup(d, fence.handle);
         if (!b || (fence.offset & 7) || !range(fence.offset, 8, b->native.bytes)) return -EINVAL;
         int err = map_cpu(d, b); if (err) return err;
      } else if (c.chunk_id == AMDGPU_CHUNK_ID_BO_HANDLES) {
         if (c.length_dw != sizeof(drm_amdgpu_bo_list_in) / 4) return -EINVAL;
         auto b = (drm_amdgpu_bo_list_in*)ptr;
         if (b->bo_number > d->info.max_buffers || b->bo_info_size != sizeof(drm_amdgpu_bo_list_entry)) return -EINVAL;
         auto entries = (drm_amdgpu_bo_list_entry*)(uintptr_t)b->bo_info_ptr;
         if (b->bo_number && !entries) return -EINVAL;
         for (unsigned j = 0; j < b->bo_number; ++j) if (!lookup(d, entries[j].bo_handle)) return -EINVAL;
      } else return -ENOTSUP;
   }
   uint64_t padded = (bytes + d->info.ib_size_alignment - 1) & ~(uint64_t)(d->info.ib_size_alignment - 1);
   if (ibs.empty() || padded > d->commands.bytes) return -ENOSPC;
   for (auto id : dependencies) {
      while (d->syncs.count(id) && !d->syncs[id] && !d->lost)
         d->changed.wait(lock);
      if (d->lost || !d->syncs.count(id)) return -EIO;
   }
   if (!d->contexts.count(ctx)) return -EINVAL;
   for (auto id : signals) if (!d->syncs.count(id)) return -EINVAL;
   if (has_fence && !lookup(d, fence.handle)) return -EINVAL;
   // No mapping can change while the submit lock is held. The kernel freezes
   // this VM during its synchronous ioctl as well. Preamble and main commands
   // are adjacent inside a single native IB, so another file cannot interleave.
   uint64_t cursor = 0;
   for (auto& ib : ibs) {
      auto it = d->mappings.upper_bound(ib.va_start);
      if (it == d->mappings.begin()) return -EINVAL;
      --it; auto& m = it->second;
      uint64_t offset = ib.va_start - it->first;
      if (!range(offset, ib.ib_bytes, m.size) || (m.permissions & (AMDGPU_VM_READ | AMDGPU_VM_EXECUTE))
          != (AMDGPU_VM_READ | AMDGPU_VM_EXECUTE)) return -EINVAL;
      int err = map_cpu(d, m.bo); if (err) return err;
      memcpy((char*)d->command_cpu + cursor, (char*)m.bo->cpu + m.offset + offset, ib.ib_bytes);
      cursor += ib.ib_bytes;
   }
   // GFX8 accepts type-2 NOP padding. No trailing packet is partly copied.
   for (; cursor < padded; cursor += 4) *(uint32_t*)((char*)d->command_cpu + cursor) = 0x80000000;
   for (auto id : signals) d->syncs[id] = false;
   __sync_synchronize();
   auto submit = request<amdgpu_gfx_submit>(); submit.address = d->info.address_start;
   submit.dwords = padded / 4;
   int err = native_call(d, AMDGPU_GFX_SUBMIT, submit);
   if (err || submit.status != B_OK) {
      d->lost = true; d->changed.notify_all();
      fprintf(stderr, "amdgpu: native submit failed %d status %#x fault %#x/%#x ring %u/%u\n",
              err, submit.status, submit.vm_fault_status[0], submit.vm_fault_status[1], submit.rptr, submit.wptr);
      return err ? err : -EIO;
   }
   *seq = ++d->contexts[ctx];
   if (has_fence) {
      auto b = lookup(d, fence.handle);
      __atomic_store_n((uint64_t*)((char*)b->cpu + fence.offset), *seq, __ATOMIC_RELEASE);
   }
   for (auto id : signals) d->syncs[id] = true;
   d->changed.notify_all(); return 0;
}
int ac_drm_query_heap_info(ac_drm_device *d, uint32_t heap, uint32_t flags, amdgpu_heap_info *out)
{
   memset(out, 0, sizeof(*out));
   if (heap == AMDGPU_GEM_DOMAIN_GTT) {
      auto r = request<amdgpu_gart_info>(); int err = native_call(d, AMDGPU_GART_INFO, r); if (err) return err;
      out->heap_size = r.total_bytes; out->heap_usage = r.allocated_bytes;
   } else if (heap == AMDGPU_GEM_DOMAIN_VRAM) {
      auto r = request<amdgpu_memory_info>(); int err = native_call(d, AMDGPU_MEMORY_INFO, r); if (err) return err;
      out->heap_size = flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED ? r.visible_vram : r.total_vram;
      out->heap_usage = r.allocated_bytes;
   } else return -ENOTSUP;
   out->max_allocation = d->info.max_buffer_bytes; return 0;
}

// Unsupported native capabilities fail explicitly; none issue a DRM ioctl.
int ac_drm_bo_set_metadata(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_metadata *info) { return -ENOTSUP; }
int ac_drm_bo_va_op_raw2(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops, uint32_t vm_timeline_syncobj_out, uint64_t vm_timeline_point, uint64_t input_fence_syncobj_handles, uint32_t num_syncobj_handles) { return -ENOTSUP; }
int ac_drm_cs_ctx_stable_pstate(ac_drm_device *dev, uint32_t ctx_id, uint32_t op, uint32_t flags, uint32_t *out_flags) { return -ENOTSUP; }
int ac_drm_cs_syncobj_query2(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned num_handles, uint32_t flags) { return -ENOTSUP; }
int ac_drm_cs_import_syncobj(ac_drm_device *dev, int shared_fd, uint32_t *handle) { return -ENOTSUP; }
int ac_drm_cs_syncobj_export_sync_file(ac_drm_device *dev, uint32_t syncobj, int *sync_file_fd) { return -ENOTSUP; }
int ac_drm_cs_syncobj_import_sync_file(ac_drm_device *dev, uint32_t syncobj, int sync_file_fd) { return -ENOTSUP; }
int ac_drm_cs_syncobj_export_sync_file2(ac_drm_device *dev, uint32_t syncobj, uint64_t point, uint32_t flags, int *sync_file_fd) { return -ENOTSUP; }
int ac_drm_cs_syncobj_transfer(ac_drm_device *dev, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags) { return -ENOTSUP; }
int ac_drm_cs_syncobj_timeline_wait(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled) { return -ENOTSUP; }
int ac_drm_query_uq_fw_area_info(ac_drm_device *dev, unsigned type, unsigned ip_instance, struct drm_amdgpu_info_uq_fw_areas *info) { return -ENOTSUP; }
int ac_drm_query_sensor_info(ac_drm_device *dev, unsigned sensor_type, unsigned size, void *value) { return -ENOTSUP; }
int ac_drm_query_video_caps_info(ac_drm_device *dev, unsigned cap_type, unsigned size, void *value) { return -ENOTSUP; }
int ac_drm_query_gpuvm_fault_info(ac_drm_device *dev, unsigned size, void *value) { return -ENOTSUP; }
int ac_drm_vm_reserve_vmid(ac_drm_device *dev, uint32_t flags) { return -ENOTSUP; }
int ac_drm_vm_unreserve_vmid(ac_drm_device *dev, uint32_t flags) { return -ENOTSUP; }
int ac_drm_bo_import(ac_drm_device *dev, enum amdgpu_bo_handle_type type, uint32_t shared_handle, struct ac_drm_bo_import_result *output) { return -ENOTSUP; }
int ac_drm_create_bo_from_user_mem(ac_drm_device *dev, void *cpu, uint64_t size, ac_drm_bo *bo) { return -ENOTSUP; }
int ac_drm_create_userqueue(ac_drm_device *dev, uint32_t ip_type, uint32_t doorbell_handle, uint32_t doorbell_offset, uint64_t queue_va, uint64_t queue_size, uint64_t wptr_va, uint64_t rptr_va, void *mqd_in, uint32_t flags, uint32_t *queue_id) { return -ENOTSUP; }
int ac_drm_free_userqueue(ac_drm_device *dev, uint32_t queue_id) { return -ENOTSUP; }
int ac_drm_userq_signal(ac_drm_device *dev, struct drm_amdgpu_userq_signal *signal_data) { return -ENOTSUP; }
int ac_drm_userq_wait(ac_drm_device *dev, struct drm_amdgpu_userq_wait *wait_data) { return -ENOTSUP; }
