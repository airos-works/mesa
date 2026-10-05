/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef PAN_HAIKU_CSF_SYNC_H
#define PAN_HAIKU_CSF_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native Haiku synchronization for the Mesa adapter. Every function returns
 * zero on success, -1 with errno on failure, including canceled GPU work.
 * Haiku errno values can be negative: returning -errno would turn failures
 * into apparent success for Mesa's ret >= 0 checks. No Linux FD ABI is claimed.
 * Signal publishes prior CPU writes; successful wait orders subsequent reads.
 * GPU cache maintenance remains the queue runtime's responsibility.
 */
int pan_haiku_sync_create(int fd, uint32_t flags, uint32_t *handle);
int pan_haiku_sync_destroy(int fd, uint32_t handle);
int pan_haiku_sync_update(int fd, const uint32_t *handles,
   const uint64_t *points, uint32_t count, int reset);
int pan_haiku_sync_transfer(int fd, uint32_t destination, uint64_t destination_point,
   uint32_t source, uint64_t source_point, uint32_t flags);

/* points=NULL selects binary fences. deadline_ns is absolute CLOCK_MONOTONIC
 * time, INT64_MAX means indefinite, and expired/negative deadlines poll.
 * Flags: 1 wait-all, 2 wait-for-submit, 4 wait-available. Interruptions are
 * reported, preserving the caller's deadline and ability to handle signals.
 */
int pan_haiku_sync_wait(int fd, const uint32_t *handles, const uint64_t *points,
   uint32_t count, int64_t deadline_ns, uint32_t flags, uint32_t *first);

/* snapshot=0 exports a shared native object; snapshot=1 exports a fence.
 * These close-on-exec FDs support the driver's ioctls, not poll or Linux
 * sync_file operations. Shared import creates a new handle. Snapshot import
 * replaces the selected fence of an existing handle.
 */
int pan_haiku_sync_export(int fd, uint32_t handle, uint64_t point,
   int snapshot, int *exported_fd);
int pan_haiku_sync_import(int fd, int shared_fd, uint32_t *handle);
int pan_haiku_sync_import_snapshot(int fd, uint32_t handle, uint64_t point,
   int snapshot_fd);

#ifdef __cplusplus
}
#endif
#endif
