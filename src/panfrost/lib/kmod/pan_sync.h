/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define PAN_SYNC_CREATE_SIGNALED 1
#define PAN_SYNC_WAIT_ALL 1
#define PAN_SYNC_WAIT_FOR_SUBMIT 2
#define PAN_SYNC_WAIT_AVAILABLE 4
#ifdef __HAIKU__
#include "haiku_csf_sync.h"
#define pan_sync_create pan_haiku_sync_create
#define pan_sync_destroy pan_haiku_sync_destroy
#define pan_sync_transfer pan_haiku_sync_transfer
#define pan_sync_import_shared pan_haiku_sync_import
static inline int pan_sync_wait(int fd, uint32_t *handles, unsigned count, int64_t deadline, unsigned flags, uint32_t *first)
{ return pan_haiku_sync_wait(fd, handles, NULL, count, deadline, flags, first); }
static inline int pan_sync_timeline_wait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t deadline, unsigned flags, uint32_t *first)
{ return pan_haiku_sync_wait(fd, handles, points, count, deadline, flags, first); }
static inline int pan_sync_export_snapshot(int fd, uint32_t handle, int *out)
{ return pan_haiku_sync_export(fd, handle, 0, 1, out); }
static inline int pan_sync_import_snapshot(int fd, uint32_t handle, int snapshot)
{ return pan_haiku_sync_import_snapshot(fd, handle, 0, snapshot); }
static inline int pan_sync_export_shared(int fd, uint32_t handle, int *out)
{ return pan_haiku_sync_export(fd, handle, 0, 0, out); }
static inline int pan_sync_reset(int fd, uint32_t *handles, unsigned count)
{ return pan_haiku_sync_update(fd, handles, NULL, count, 1); }
static inline int pan_sync_signal(int fd, uint32_t *handles, unsigned count)
{ return pan_haiku_sync_update(fd, handles, NULL, count, 0); }
static inline int pan_sync_timeline_signal(int fd, uint32_t *handles, uint64_t *points, unsigned count)
{ return pan_haiku_sync_update(fd, handles, points, count, 0); }
#else
#include <xf86drm.h>
#define pan_sync_create drmSyncobjCreate
#define pan_sync_destroy drmSyncobjDestroy
#define pan_sync_wait drmSyncobjWait
#define pan_sync_timeline_wait drmSyncobjTimelineWait
#define pan_sync_transfer drmSyncobjTransfer
#define pan_sync_export_snapshot drmSyncobjExportSyncFile
#define pan_sync_import_snapshot drmSyncobjImportSyncFile
#define pan_sync_import_shared drmSyncobjFDToHandle
#define pan_sync_export_shared drmSyncobjHandleToFD
#define pan_sync_reset drmSyncobjReset
#define pan_sync_signal drmSyncobjSignal
#define pan_sync_timeline_signal drmSyncobjTimelineSignal
#endif
