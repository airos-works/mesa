/*
 * Copyright © 2023 Collabora, Ltd. (Panfrost synchronization bookkeeping)
 * SPDX-License-Identifier: MIT
 *
 * Native Haiku CSF backend. Panthor structures used by the command builder
 * are translated in-process; no DRM ioctl or Linux descriptor ABI is exposed.
 */
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "util/os_time.h"
#include "util/u_math.h"
#include "pan_kmod_backend.h"
#include "haiku_csf_native.h"
#include "haiku_csf_sync.h"
#include "haiku_kmod.h"

#define HAIKU_MAX_MAPPINGS 256
#define HAIKU_MAX_GROUPS 8
#define HAIKU_MAX_HEAPS 128

struct haiku_range { uint64_t start, size; };
struct haiku_vm {
   struct pan_kmod_vm base;
   simple_mtx_t map_lock, sync_lock;
   uint64_t start, limit;
   struct haiku_range mappings[HAIKU_MAX_MAPPINGS];
   unsigned count;
   uint32_t sync_handle;
   uint64_t sync_point;
};
struct haiku_bo {
   struct pan_kmod_bo base;
   simple_mtx_t lock;
   struct pan_haiku_map map;
   uint32_t sync_handle;
   uint64_t read_point, write_point;
};
struct haiku_object { uint32_t handle, vm; };
struct haiku_dev {
   struct pan_kmod_dev base;
   struct pan_haiku_properties props;
   struct drm_panthor_csif_info csif;
   uint32_t bootstrap_vm, bootstrap_queue;
   simple_mtx_t lock;
   struct haiku_object groups[HAIKU_MAX_GROUPS], heaps[HAIKU_MAX_HEAPS];
};

const struct pan_kmod_ops haiku_kmod_ops;

static int fail(int error) { errno = error; return -1; }
static void log_failure(const char *operation, int result)
{
   if (result) mesa_loge("Haiku CSF %s failed (errno=%d)", operation, errno);
}

struct pan_kmod_dev *
haiku_kmod_dev_create(int fd, uint32_t flags,
                     const struct pan_kmod_allocator *allocator)
{
   if (flags & ~PAN_KMOD_DEV_FLAG_OWNS_FD) {
      errno = EINVAL;
      return NULL;
   }
   if (pan_haiku_check_client(fd)) return NULL;
   struct haiku_dev *dev = pan_kmod_alloc(allocator, sizeof(*dev));
   if (!dev) { errno = ENOMEM; return NULL; }
   if (pan_haiku_vm_create(fd, 0, &dev->bootstrap_vm)) goto fail_free;
   const char *firmware = getenv("HAIKU_CSF_FIRMWARE");
   if (!firmware || !*firmware) {
      /* Packaged, non-packaged, then per-user firmware locations. */
      static const char *const candidates[] = {
         "/boot/system/data/firmware/mali/arch10.8/mali_csffw.bin",
         "/boot/system/non-packaged/data/firmware/mali/arch10.8/mali_csffw.bin",
         "/boot/home/config/non-packaged/data/firmware/mali/arch10.8/mali_csffw.bin",
      };
      firmware = candidates[0];
      for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
         if (access(candidates[i], R_OK) == 0) {
            firmware = candidates[i];
            break;
         }
      }
   }
   if (pan_haiku_queue_create_with_file(fd, dev->bootstrap_vm, firmware,
                                      &dev->bootstrap_queue)) goto fail_vm;
   if (pan_haiku_queue_properties(fd, dev->bootstrap_queue, &dev->props)) goto fail_queue;
   /* This backend currently targets the native arch10 runtime. Refuse other
    * architectures instead of entering one of Mesa's DRM-only JM paths.
    */
   if ((dev->props.gpu_id >> 28) != 10 || !dev->props.shader_present
       || !dev->props.tiler_present || dev->props.work_registers < 96
       || dev->props.reserved_registers > 4 || dev->props.scoreboards < 8
       || !dev->props.max_threads || !dev->props.max_workgroup_size
       || !(dev->props.thread_features >> 24)
       || !(dev->props.thread_features & 0x3fffff)) {
      errno = ENOTSUP;
      goto fail_queue;
   }
   dev->csif = (struct drm_panthor_csif_info) {
      .csg_slot_count = dev->props.group_count,
      .cs_slot_count = dev->props.stream_count,
      .cs_reg_count = dev->props.work_registers,
      .scoreboard_slot_count = dev->props.scoreboards,
      .unpreserved_cs_reg_count = dev->props.reserved_registers,
   };
   simple_mtx_init(&dev->lock, mtx_plain);
   /* No fabricated DRM version. Capabilities come from the native query. */
   pan_kmod_dev_init(&dev->base, fd, flags, NULL, &haiku_kmod_ops, allocator);
   return &dev->base;
fail_queue: {
   int error = errno;
   log_failure("bootstrap queue destroy", pan_haiku_queue_destroy(fd, dev->bootstrap_queue));
   errno = error;
}
fail_vm: {
   int error = errno;
   log_failure("bootstrap VM destroy", pan_haiku_vm_destroy(fd, dev->bootstrap_vm));
   errno = error;
}
fail_free:
   pan_kmod_free(allocator, dev);
   return NULL;
}

static void haiku_dev_destroy(struct pan_kmod_dev *base)
{
   struct haiku_dev *dev = container_of(base, struct haiku_dev, base);
   /* Normally context cleanup already retired these objects. Keep cleanup
    * complete after a partially failed context initialization, too.
    */
   for (unsigned i = 0; i < HAIKU_MAX_GROUPS; i++)
      if (dev->groups[i].handle)
         log_failure("queue destroy", pan_haiku_queue_destroy(base->fd, dev->groups[i].handle));
   for (unsigned i = 0; i < HAIKU_MAX_HEAPS; i++)
      if (dev->heaps[i].handle)
         log_failure("heap destroy", pan_haiku_heap_destroy(base->fd, dev->heaps[i].vm,
                                                          dev->heaps[i].handle));
   log_failure("bootstrap queue destroy", pan_haiku_queue_destroy(base->fd, dev->bootstrap_queue));
   log_failure("bootstrap VM destroy", pan_haiku_vm_destroy(base->fd, dev->bootstrap_vm));
   simple_mtx_destroy(&dev->lock);
   pan_kmod_dev_cleanup(base);
   pan_kmod_free(base->allocator, dev);
}

static void haiku_dev_props(const struct pan_kmod_dev *base,
                            struct pan_kmod_dev_props *out)
{
   const struct haiku_dev *dev = container_of(base, struct haiku_dev, base);
   const struct pan_haiku_properties *p = &dev->props;
   *out = (struct pan_kmod_dev_props) {
      .gpu_id = p->gpu_id, .gpu_variant = p->core_features & 0xff,
      .shader_present = p->shader_present, .tiler_features = p->tiler_features,
      .mem_features = p->mem_features, .mmu_features = p->mmu_features,
      .max_threads_per_core = p->max_threads,
      .max_threads_per_wg = p->max_workgroup_size,
      .max_tasks_per_core = p->thread_features >> 24,
      .num_registers_per_core = p->thread_features & 0x3fffff,
      .max_tls_instance_per_core = p->max_threads,
      .allowed_group_priorities_mask = PAN_KMOD_GROUP_ALLOW_PRIORITY_MEDIUM,
      /* GPU timestamps and coherent propagation are not native capabilities. */
   };
   memcpy(out->texture_features, p->texture_features, sizeof(out->texture_features));
}

static struct pan_kmod_va_range haiku_dev_va(const struct pan_kmod_dev *base)
{
   const struct haiku_dev *dev = container_of(base, struct haiku_dev, base);
   return (struct pan_kmod_va_range) { .start = 0, .size = dev->props.user_va_limit };
}

static struct pan_kmod_bo *haiku_bo_alloc(struct pan_kmod_dev *dev,
   struct pan_kmod_vm *exclusive_vm, uint64_t size, uint32_t flags)
{
   const uint32_t supported = PAN_KMOD_BO_FLAG_EXECUTABLE | PAN_KMOD_BO_FLAG_NO_MMAP
      | PAN_KMOD_BO_FLAG_GPU_UNCACHED;
   if (flags & ~supported || !size || (size & 4095)
       || (exclusive_vm && exclusive_vm->dev != dev)) {
      errno = EINVAL;
      return NULL;
   }
   struct haiku_bo *bo = pan_kmod_dev_alloc(dev, sizeof(*bo));
   if (!bo) { errno = ENOMEM; return NULL; }
   bo->map.area = -1;
   uint32_t handle;
   if (pan_haiku_buffer_create(dev->fd, size, &handle)) goto fail;
   if (exclusive_vm)
      bo->sync_handle = container_of(exclusive_vm, struct haiku_vm, base)->sync_handle;
   else if (pan_haiku_sync_create(dev->fd, 1, &bo->sync_handle)) {
      int error = errno;
      log_failure("buffer destroy", pan_haiku_buffer_destroy(dev->fd, handle));
      errno = error;
      goto fail;
   }
   simple_mtx_init(&bo->lock, mtx_plain);
   pan_kmod_bo_init(&bo->base, dev, exclusive_vm, size, flags, handle);
   return &bo->base;
fail:
   pan_kmod_dev_free(dev, bo);
   return NULL;
}

static void haiku_bo_free(struct pan_kmod_bo *base)
{
   struct haiku_bo *bo = container_of(base, struct haiku_bo, base);
   if (bo->map.area >= 0)
      log_failure("area delete", pan_haiku_buffer_unmap(bo->map.area));
   if (!base->exclusive_vm)
      log_failure("BO sync destroy", pan_haiku_sync_destroy(base->dev->fd, bo->sync_handle));
   log_failure("buffer destroy", pan_haiku_buffer_destroy(base->dev->fd, base->handle));
   simple_mtx_destroy(&bo->lock);
   pan_kmod_dev_free(base->dev, bo);
}

void *haiku_kmod_bo_mmap(struct pan_kmod_bo *base, off_t offset, size_t size,
                        int prot, int flags, void *address)
{
   if (offset != 0 || size != base->size || address || prot != (PROT_READ | PROT_WRITE)
       || flags != MAP_SHARED || (base->flags & PAN_KMOD_BO_FLAG_NO_MMAP)) {
      errno = ENOTSUP;
      return MAP_FAILED;
   }
   struct haiku_bo *bo = container_of(base, struct haiku_bo, base);
   simple_mtx_lock(&bo->lock);
   int ret = bo->map.area >= 0 ? fail(EBUSY)
      : pan_haiku_buffer_map(base->dev->fd, base->handle, &bo->map);
   void *result = ret ? MAP_FAILED : bo->map.address;
   simple_mtx_unlock(&bo->lock);
   return result;
}

int haiku_kmod_bo_munmap(struct pan_kmod_bo *base, void *address, size_t size)
{
   struct haiku_bo *bo = container_of(base, struct haiku_bo, base);
   simple_mtx_lock(&bo->lock);
   int ret = address != bo->map.address || size != bo->map.bytes || bo->map.area < 0
      ? fail(EINVAL) : pan_haiku_buffer_unmap(bo->map.area);
   if (!ret) bo->map = (struct pan_haiku_map) { .area = -1 };
   simple_mtx_unlock(&bo->lock);
   return ret;
}

static bool haiku_bo_wait(struct pan_kmod_bo *base, int64_t timeout, bool read_only)
{
   struct haiku_bo *bo = container_of(base, struct haiku_bo, base);
   simple_mtx_lock(&bo->lock);
   uint64_t point = read_only ? bo->write_point : MAX2(bo->read_point, bo->write_point);
   simple_mtx_unlock(&bo->lock);
   if (!point) return true;
   int64_t now = os_time_get_nano();
   int64_t deadline = timeout < 0 || timeout >= INT64_MAX - now
      ? INT64_MAX : now + timeout;
   return pan_haiku_sync_wait(base->dev->fd, &bo->sync_handle, &point, 1,
                             deadline, 1, NULL) == 0;
}

static struct pan_kmod_vm *haiku_vm_create(struct pan_kmod_dev *dev, uint32_t flags,
                                          uint64_t start, uint64_t range)
{
   /* This first backend implements Gallium's auto-VA, activity-tracked VM.
    * Explicit asynchronous binds (used by Vulkan) are rejected below.
    */
   if (flags != (PAN_KMOD_VM_FLAG_AUTO_VA | PAN_KMOD_VM_FLAG_TRACK_ACTIVITY)
       || !start || !range || ((start | range) & 4095)
       || start > UINT64_MAX - range || start + range > haiku_dev_va(dev).size) {
      errno = ENOTSUP;
      return NULL;
   }
   struct haiku_vm *vm = pan_kmod_dev_alloc(dev, sizeof(*vm));
   if (!vm) { errno = ENOMEM; return NULL; }
   uint32_t handle;
   if (pan_haiku_vm_create(dev->fd, start + range, &handle)) goto fail;
   if (pan_haiku_sync_create(dev->fd, 1, &vm->sync_handle)) {
      int error = errno;
      log_failure("VM destroy", pan_haiku_vm_destroy(dev->fd, handle));
      errno = error;
      goto fail;
   }
   simple_mtx_init(&vm->map_lock, mtx_plain);
   simple_mtx_init(&vm->sync_lock, mtx_plain);
   vm->start = start;
   vm->limit = start + range;
   pan_kmod_vm_init(&vm->base, dev, handle, flags, PAN_PGSIZE_4K);
   return &vm->base;
fail:
   pan_kmod_dev_free(dev, vm);
   return NULL;
}

static void haiku_vm_destroy(struct pan_kmod_vm *base)
{
   struct haiku_vm *vm = container_of(base, struct haiku_vm, base);
   log_failure("VM destroy", pan_haiku_vm_destroy(base->dev->fd, base->handle));
   log_failure("VM sync destroy", pan_haiku_sync_destroy(base->dev->fd, vm->sync_handle));
   simple_mtx_destroy(&vm->map_lock);
   simple_mtx_destroy(&vm->sync_lock);
   pan_kmod_dev_free(base->dev, vm);
}

static int haiku_vm_bind(struct pan_kmod_vm *base, enum pan_kmod_vm_op_mode mode,
                         struct pan_kmod_vm_op *ops, uint32_t count)
{
   if (mode != PAN_KMOD_VM_OP_MODE_IMMEDIATE
       && mode != PAN_KMOD_VM_OP_MODE_DEFER_TO_NEXT_IDLE_POINT) return fail(ENOTSUP);
   if (!count || count > 64 || !ops) return fail(EINVAL);
   struct haiku_vm *vm = container_of(base, struct haiku_vm, base);
   struct haiku_range next[HAIKU_MAX_MAPPINGS];
   struct pan_haiku_vm_op native[64] = {0};
   uint64_t output_va[64];
   simple_mtx_lock(&vm->map_lock);
   unsigned next_count = vm->count;
   memcpy(next, vm->mappings, next_count * sizeof(*next));
   int ret = -1;
   for (unsigned i = 0; i < count; i++) {
      const struct pan_kmod_vm_op *op = &ops[i];
      if (op->syncs.count || !op->va.size || (op->va.size & 4095)) {
         errno = EINVAL; goto out;
      }
      if (op->type == PAN_KMOD_VM_OP_TYPE_MAP) {
         if (op->va.start != PAN_KMOD_VM_MAP_AUTO_VA || !op->map.bo
             || op->map.bo->dev != base->dev || (op->map.bo->exclusive_vm
                && op->map.bo->exclusive_vm != base)
             || op->map.bo_offset < 0 || (op->map.bo_offset & 4095)
             || (uint64_t)op->map.bo_offset > op->map.bo->size
             || op->va.size > op->map.bo->size - (uint64_t)op->map.bo_offset) {
            errno = EINVAL; goto out;
         }
         if (next_count == HAIKU_MAX_MAPPINGS) { errno = ENOSPC; goto out; }
         uint64_t address = vm->start;
         /* Search the sorted live intervals; no allocations after commit. */
         unsigned slot = 0;
         for (; slot < next_count; slot++) {
            if (op->va.size <= next[slot].start - address) break;
            address = next[slot].start + next[slot].size;
         }
         if (address > vm->limit || op->va.size > vm->limit - address) {
            errno = ENOSPC; goto out;
         }
         memmove(next + slot + 1, next + slot, (next_count - slot) * sizeof(*next));
         next[slot] = (struct haiku_range) { address, op->va.size };
         next_count++;
         native[i] = (struct pan_haiku_vm_op) {
            .type = 0, .flags = op->map.bo->flags & PAN_KMOD_BO_FLAG_EXECUTABLE ? 1 : 2,
            .address = address, .bytes = op->va.size, .buffer = op->map.bo->handle,
            .offset = op->map.bo_offset,
         };
         if (op->map.bo->flags & PAN_KMOD_BO_FLAG_GPU_UNCACHED) native[i].flags |= 4;
         output_va[i] = address;
      } else if (op->type == PAN_KMOD_VM_OP_TYPE_UNMAP) {
         unsigned slot = 0;
         for (; slot < next_count; slot++)
            if (next[slot].start == op->va.start && next[slot].size == op->va.size) break;
         /* Auto-VA clients free exactly the range they allocated. Reject
          * partial/double free rather than corrupting the local VA allocator.
          */
         if (slot == next_count) { errno = EINVAL; goto out; }
         memmove(next + slot, next + slot + 1, (next_count - slot - 1) * sizeof(*next));
         next_count--;
         native[i] = (struct pan_haiku_vm_op) { .type = 1,
            .address = op->va.start, .bytes = op->va.size };
         output_va[i] = op->va.start;
      } else { errno = ENOTSUP; goto out; }
   }
   /* The kernel captures an immutable VM generation for every submission.
    * Retired mappings survive until all jobs that captured them complete.
    * Thus publishing the new generation is a safe deferred unmap even when
    * old jobs are pending, including immediate reuse of the same GPU VA.
    * Failed native transactions leave both kernel and local ranges intact.
    */
   ret = pan_haiku_vm_bind(base->dev->fd, base->handle, native, count, 0, NULL);
   if (!ret) {
      memcpy(vm->mappings, next, next_count * sizeof(*next));
      vm->count = next_count;
      for (unsigned i = 0; i < count; i++) ops[i].va.start = output_va[i];
   }
out:
   simple_mtx_unlock(&vm->map_lock);
   return ret;
}

static enum pan_kmod_vm_state haiku_vm_state(struct pan_kmod_vm *base)
{
   struct haiku_dev *dev = container_of(base->dev, struct haiku_dev, base);
   struct pan_haiku_queue_state state;
   /* The qualified kernel retains a failed runtime until recovery. A dead
    * runtime cannot be repaired by recreating a Gallium context or VM.
    */
   if (pan_haiku_queue_info(base->dev->fd, dev->bootstrap_queue, &state)
       || (state.state != 1 && state.state != 2)) return PAN_KMOD_VM_FAULTY;
   uint64_t generation;
   return pan_haiku_vm_generation(base->dev->fd, base->handle, &generation)
      ? PAN_KMOD_VM_FAULTY : PAN_KMOD_VM_USABLE;
}

int haiku_kmod_bo_attach_sync_point(struct pan_kmod_bo *base, uint32_t handle,
                                   uint64_t point, bool written)
{
   struct haiku_bo *bo = container_of(base, struct haiku_bo, base);
   simple_mtx_lock(&bo->lock);
   int ret = 0;
   if (base->exclusive_vm) {
      if (handle != bo->sync_handle) ret = fail(EINVAL);
      else {
         bo->read_point = MAX2(bo->read_point, point);
         if (written) bo->write_point = MAX2(bo->write_point, point);
      }
   } else {
      uint64_t prior = MAX2(bo->read_point, bo->write_point);
      if (prior == UINT64_MAX) ret = fail(EOVERFLOW);
      else {
         uint64_t next = prior + 1;
         ret = pan_haiku_sync_transfer(base->dev->fd, bo->sync_handle, next,
                                      handle, point, 0);
         if (!ret) {
            bo->read_point = next;
            if (written) bo->write_point = next;
         }
      }
   }
   simple_mtx_unlock(&bo->lock);
   return ret;
}

int haiku_kmod_bo_get_sync_point(struct pan_kmod_bo *base, uint32_t *handle,
                                uint64_t *point, bool read_only)
{
   if (!handle || !point) return fail(EINVAL);
   struct haiku_bo *bo = container_of(base, struct haiku_bo, base);
   simple_mtx_lock(&bo->lock);
   *handle = bo->sync_handle;
   *point = read_only ? bo->write_point : MAX2(bo->read_point, bo->write_point);
   simple_mtx_unlock(&bo->lock);
   return 0;
}

uint32_t haiku_kmod_vm_sync_handle(struct pan_kmod_vm *base)
{
   return container_of(base, struct haiku_vm, base)->sync_handle;
}
uint64_t haiku_kmod_vm_sync_lock(struct pan_kmod_vm *base)
{
   struct haiku_vm *vm = container_of(base, struct haiku_vm, base);
   simple_mtx_lock(&vm->sync_lock);
   return vm->sync_point;
}
void haiku_kmod_vm_sync_unlock(struct pan_kmod_vm *base, uint64_t point)
{
   struct haiku_vm *vm = container_of(base, struct haiku_vm, base);
   assert(point >= vm->sync_point);
   vm->sync_point = point;
   simple_mtx_unlock(&vm->sync_lock);
}
uint32_t haiku_kmod_get_flush_id(const struct pan_kmod_dev *dev)
{
   (void)dev;
   /* Panthor UAPI documents zero as an unconditional flush. Haiku's queue
    * wrapper already executes that flush before calling the user stream.
    */
   return 0;
}
const struct drm_panthor_csif_info *haiku_kmod_get_csif_props(const struct pan_kmod_dev *base)
{
   return &container_of(base, struct haiku_dev, base)->csif;
}

static struct haiku_object *find_object(struct haiku_object *objects, unsigned count,
                                       uint32_t handle)
{
   for (unsigned i = 0; i < count; i++) if (objects[i].handle == handle) return &objects[i];
   return NULL;
}

int haiku_kmod_group_create(struct pan_kmod_dev *base, struct drm_panthor_group_create *req)
{
   struct haiku_dev *dev = container_of(base, struct haiku_dev, base);
   if (req->queues.count != 1 || req->queues.stride != sizeof(struct drm_panthor_queue_create)
       || !req->queues.array || req->pad || req->group_handle
       || req->priority != PANTHOR_GROUP_PRIORITY_MEDIUM
       || req->compute_core_mask != dev->props.shader_present
       || req->fragment_core_mask != dev->props.shader_present
       || req->tiler_core_mask != dev->props.tiler_present
       || req->max_compute_cores != util_bitcount64(dev->props.shader_present)
       || req->max_fragment_cores != util_bitcount64(dev->props.shader_present)
       || req->max_tiler_cores != util_bitcount64(dev->props.tiler_present)) return fail(ENOTSUP);
   const struct drm_panthor_queue_create *queue = (const void *)(uintptr_t)req->queues.array;
   if (queue->priority != 1 || queue->ringbuf_size != 65536
       || queue->pad[0] || queue->pad[1] || queue->pad[2]) return fail(ENOTSUP);
   simple_mtx_lock(&dev->lock);
   struct haiku_object *slot = find_object(dev->groups, HAIKU_MAX_GROUPS, 0);
   uint32_t handle;
   int ret = slot ? pan_haiku_queue_create(base->fd, req->vm_id, NULL, 0, &handle) : fail(ENOSPC);
   if (!ret) {
      *slot = (struct haiku_object) { handle, req->vm_id };
      req->group_handle = handle;
   }
   simple_mtx_unlock(&dev->lock);
   return ret;
}
int haiku_kmod_group_destroy(struct pan_kmod_dev *base, struct drm_panthor_group_destroy *req)
{
   struct haiku_dev *dev = container_of(base, struct haiku_dev, base);
   if (!req->group_handle || req->pad) return fail(EINVAL);
   simple_mtx_lock(&dev->lock);
   struct haiku_object *slot = find_object(dev->groups, HAIKU_MAX_GROUPS, req->group_handle);
   int ret = slot ? pan_haiku_queue_destroy(base->fd, slot->handle) : fail(EINVAL);
   if (!ret) memset(slot, 0, sizeof(*slot));
   simple_mtx_unlock(&dev->lock);
   return ret;
}

int haiku_kmod_group_submit(struct pan_kmod_dev *base, struct drm_panthor_group_submit *req)
{
   if (!req->group_handle || req->pad || req->queue_submits.count != 1
       || req->queue_submits.stride != sizeof(struct drm_panthor_queue_submit)
       || !req->queue_submits.array) return fail(ENOTSUP);
   const struct drm_panthor_queue_submit *q = (const void *)(uintptr_t)req->queue_submits.array;
   if (q->queue_index || q->pad || q->latest_flush
       || q->syncs.count > 64 || (q->syncs.count
          && (q->syncs.stride != sizeof(struct drm_panthor_sync_op) || !q->syncs.array)))
      return fail(ENOTSUP);
   const struct drm_panthor_sync_op *syncs = (const void *)(uintptr_t)q->syncs.array;
   struct pan_haiku_sync_point waits[64], signals[64];
   unsigned wait_count = 0, signal_count = 0;
   for (unsigned i = 0; i < q->syncs.count; i++) {
      uint32_t type = syncs[i].flags & DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK;
      if ((syncs[i].flags & ~(DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK | DRM_PANTHOR_SYNC_OP_SIGNAL))
          || (type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ
              && type != DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ)
          || (type == DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ && syncs[i].timeline_value))
         return fail(EINVAL);
      struct pan_haiku_sync_point point = { syncs[i].handle, syncs[i].timeline_value };
      if (syncs[i].flags & DRM_PANTHOR_SYNC_OP_SIGNAL) signals[signal_count++] = point;
      else waits[wait_count++] = point;
   }
   struct haiku_dev *dev = container_of(base, struct haiku_dev, base);
   simple_mtx_lock(&dev->lock);
   struct haiku_object *slot = find_object(dev->groups, HAIKU_MAX_GROUPS, req->group_handle);
   int ret = slot ? pan_haiku_queue_submit(base->fd, slot->handle, 0,
      q->stream_addr, q->stream_size, waits, wait_count, signals, signal_count, NULL) : fail(EINVAL);
   simple_mtx_unlock(&dev->lock);
   return ret;
}

int haiku_kmod_group_get_state(struct pan_kmod_dev *base, struct drm_panthor_group_get_state *req)
{
   if (!req->group_handle || req->pad) return fail(EINVAL);
   struct pan_haiku_queue_state state;
   if (pan_haiku_queue_info(base->fd, req->group_handle, &state)) return -1;
   req->state = (state.state == 1 || state.state == 2) ? 0 : DRM_PANTHOR_GROUP_STATE_FATAL_FAULT;
   req->fatal_queues = req->state ? 1 : 0;
   return 0;
}

int haiku_kmod_group_get_reset_info(struct pan_kmod_dev *base, uint32_t group,
                                  struct pan_haiku_reset_info *info)
{
   return pan_haiku_queue_reset_info(base->fd, group, info);
}

int haiku_kmod_heap_create(struct pan_kmod_dev *base, struct drm_panthor_tiler_heap_create *req)
{
   if (req->handle || req->tiler_heap_ctx_gpu_va || req->first_heap_chunk_gpu_va) return fail(EINVAL);
   struct haiku_dev *dev = container_of(base, struct haiku_dev, base);
   simple_mtx_lock(&dev->lock);
   struct haiku_object *slot = find_object(dev->heaps, HAIKU_MAX_HEAPS, 0);
   struct pan_haiku_heap heap;
   int ret = slot ? pan_haiku_heap_create(base->fd, req->vm_id, req->chunk_size,
      req->initial_chunk_count, req->max_chunks, req->target_in_flight, &heap) : fail(ENOSPC);
   if (!ret) {
      *slot = (struct haiku_object) { heap.handle, req->vm_id };
      req->handle = heap.handle;
      req->tiler_heap_ctx_gpu_va = heap.context_address;
      req->first_heap_chunk_gpu_va = heap.first_chunk_address;
   }
   simple_mtx_unlock(&dev->lock);
   return ret;
}
int haiku_kmod_heap_destroy(struct pan_kmod_dev *base, struct drm_panthor_tiler_heap_destroy *req)
{
   if (!req->handle || req->pad) return fail(EINVAL);
   struct haiku_dev *dev = container_of(base, struct haiku_dev, base);
   simple_mtx_lock(&dev->lock);
   struct haiku_object *slot = find_object(dev->heaps, HAIKU_MAX_HEAPS, req->handle);
   int ret = slot ? pan_haiku_heap_destroy(base->fd, slot->vm, slot->handle) : fail(EINVAL);
   if (!ret) memset(slot, 0, sizeof(*slot));
   simple_mtx_unlock(&dev->lock);
   return ret;
}

const struct pan_kmod_ops haiku_kmod_ops = {
   /* Native dev_create has a separate path, without DRM version discovery. */
   .dev_destroy = haiku_dev_destroy, .dev_query_props = haiku_dev_props,
   .dev_query_user_va_range = haiku_dev_va,
   .bo_alloc = haiku_bo_alloc, .bo_free = haiku_bo_free, .bo_wait = haiku_bo_wait,
   .vm_create = haiku_vm_create, .vm_destroy = haiku_vm_destroy,
   .vm_bind = haiku_vm_bind, .vm_query_state = haiku_vm_state,
};
