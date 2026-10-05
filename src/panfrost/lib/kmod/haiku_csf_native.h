/* SPDX-License-Identifier: MIT */
#ifndef PAN_HAIKU_CSF_NATIVE_H
#define PAN_HAIKU_CSF_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mesa's process-local bridge to the native Haiku ABI. These C structures
 * are not ioctl payloads: the C++ implementation explicitly translates them.
 * All functions return 0, or -1 with errno (which may be negative on Haiku).
 * Output arguments are changed only on success. No ownership of fd is taken.
 */
struct pan_haiku_properties {
   uint32_t gpu_id, gpu_revision, core_features, tiler_features, mem_features, mmu_features;
   uint32_t texture_features[4];
   uint32_t max_threads, max_workgroup_size, thread_features;
   uint64_t shader_present, tiler_present, user_va_limit;
   uint32_t group_count, stream_count, work_registers, scoreboards;
   uint32_t reserved_registers, max_queues, max_queue_jobs;
};

struct pan_haiku_map {
   void *address;
   uint64_t bytes;
   int32_t area;
};

/* map=0, unmap=1; flags read-only=1, no-execute=2, uncached=4.
 * Executable GPU mappings must be read-only. CPU mappings remain RW/NC.
 */
struct pan_haiku_vm_op {
   uint32_t type, flags;
   uint64_t address, bytes;
   uint32_t buffer;
   uint64_t offset;
};

struct pan_haiku_sync_point { uint32_t handle; uint64_t point; };
struct pan_haiku_queue_state {
   uint32_t vm, state, pending;
   int32_t error;
   uint64_t submitted, completed, failed_sequence;
};
/* Process-local reset states translated from QueueResetInfo. */
enum pan_haiku_reset_state {
   PAN_HAIKU_RESET_NONE = 0,
   PAN_HAIKU_RESET_LOCAL_ERROR = 1,
   PAN_HAIKU_RESET_PENDING = 2,
   PAN_HAIKU_RESET_QUIESCENT = 3,
   PAN_HAIKU_RESET_UNRECOVERABLE = 4,
};
struct pan_haiku_reset_info { uint32_t state; int32_t error; };
struct pan_haiku_heap {
   uint32_t handle;
   uint64_t context_address, first_chunk_address;
};

int pan_haiku_check_client(int fd);
int pan_haiku_buffer_create(int fd, uint64_t bytes, uint32_t *handle);
int pan_haiku_buffer_destroy(int fd, uint32_t handle);
int pan_haiku_buffer_map(int fd, uint32_t handle, struct pan_haiku_map *map);
int pan_haiku_buffer_unmap(int32_t area);
int pan_haiku_vm_create(int fd, uint64_t limit, uint32_t *handle);
int pan_haiku_vm_destroy(int fd, uint32_t handle);
int pan_haiku_vm_generation(int fd, uint32_t vm, uint64_t *generation);
int pan_haiku_vm_bind(int fd, uint32_t vm,
   const struct pan_haiku_vm_op *operations, uint32_t count,
   uint64_t expected_generation, uint64_t *generation);

/* The first queue supplies the firmware. A running runtime accepts NULL/0.
 * create_with_file first tries to join, loads a bounded regular file only if
 * no runtime exists, and retries a join once if another client won that race.
 */
int pan_haiku_queue_create(int fd, uint32_t vm, const void *firmware,
   size_t firmware_bytes, uint32_t *handle);
int pan_haiku_queue_create_with_file(int fd, uint32_t vm,
   const char *firmware_path, uint32_t *handle);
int pan_haiku_queue_destroy(int fd, uint32_t handle);
int pan_haiku_queue_properties(int fd, uint32_t queue,
   struct pan_haiku_properties *properties);
int pan_haiku_queue_info(int fd, uint32_t queue,
   struct pan_haiku_queue_state *state);
int pan_haiku_queue_reset_info(int fd, uint32_t queue,
   struct pan_haiku_reset_info *info);
int pan_haiku_queue_submit(int fd, uint32_t queue, uint64_t generation,
   uint64_t address, uint32_t bytes,
   const struct pan_haiku_sync_point *waits, uint32_t wait_count,
   const struct pan_haiku_sync_point *signals, uint32_t signal_count,
   uint64_t *sequence);
int pan_haiku_heap_create(int fd, uint32_t vm, uint32_t chunk_size,
   uint32_t initial_chunks, uint32_t max_chunks, uint32_t target_in_flight,
   struct pan_haiku_heap *heap);
int pan_haiku_heap_destroy(int fd, uint32_t vm, uint32_t heap);

#ifdef __cplusplus
}
#endif
#endif
