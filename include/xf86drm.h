/*
 * Copyright 2026, air/OS. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

/* air/OS (Haiku): what Mesa's v3d driver uses of libdrm, on top of the V3D
 * render device (/dev/graphics/v3d/0, see headers/private/graphics/v3d in
 * the air/OS tree). The device takes the drm_v3d_* structures as they are;
 * this only translates the request codes and stands in for sync objects.
 *
 * There are no sync files: a "sync file descriptor" from
 * drmSyncobjExportSyncFile() is a token for a job's sequence number, not a
 * file descriptor. It must not be closed or duplicated; v3d_fence.c knows.
 */
#ifndef HAIKU_V3D_XF86DRM_H
#define HAIKU_V3D_XF86DRM_H

#ifndef __HAIKU__
#error This xf86drm.h is the air/OS stand-in for libdrm.
#endif

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <Drivers.h>

#include "drm-uapi/drm.h"
#include "drm-uapi/v3d_drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAIKU_V3D_DEVICE_PATH "/dev/graphics/v3d/0"

enum {
   HAIKU_V3D_FIRST_OP = B_DEVICE_OP_CODES_END + 0x7600,
   HAIKU_V3D_GET_PARAM = HAIKU_V3D_FIRST_OP,
   HAIKU_V3D_CREATE_BO,
   HAIKU_V3D_MMAP_BO,
   HAIKU_V3D_GET_BO_OFFSET,
   HAIKU_V3D_WAIT_BO,
   HAIKU_V3D_SUBMIT_CL,
   HAIKU_V3D_SUBMIT_TFU,
   HAIKU_V3D_SUBMIT_CSD,
   HAIKU_V3D_CLOSE_BO,
   HAIKU_V3D_SYNC_CREATE,
   HAIKU_V3D_SYNC_DESTROY,
   HAIKU_V3D_SYNC_WAIT,
   HAIKU_V3D_SYNC_GET,
   HAIKU_V3D_SEQNO_WAIT,
   HAIKU_V3D_SYNC_RESET,
   HAIKU_V3D_SYNC_SIGNAL,
   HAIKU_V3D_SYNC_TRANSFER,
};

struct haiku_v3d_handle {
   uint32_t handle;
   uint32_t pad;
};

struct haiku_v3d_sync {
   uint32_t handle;
   uint32_t pad;
   uint64_t seqno;
   int64_t timeout_ns;
};

/* haiku_v3d_handle.pad at HAIKU_V3D_SYNC_CREATE: starts without a job and
 * not signalled (DRM's default) */
#define HAIKU_V3D_SYNC_UNSIGNALED 0x1
/* haiku_v3d_sync.pad at HAIKU_V3D_SYNC_WAIT: a sync object without a job is
 * waited for too */
#define HAIKU_V3D_SYNC_WAIT_FOR_SUBMIT 0x1

/* handle value that marks "seqno" as a 31-bit fence token */
#define HAIKU_V3D_TOKEN 0xffffffffu

static inline int
drmIoctl(int fd, unsigned long request, void *arg)
{
   switch (request) {
   case DRM_IOCTL_V3D_GET_PARAM:
      return ioctl(fd, HAIKU_V3D_GET_PARAM, arg, sizeof(struct drm_v3d_get_param));
   case DRM_IOCTL_V3D_CREATE_BO:
      return ioctl(fd, HAIKU_V3D_CREATE_BO, arg, sizeof(struct drm_v3d_create_bo));
   case DRM_IOCTL_V3D_MMAP_BO:
      /* returns the address of the mapping in "offset" */
      return ioctl(fd, HAIKU_V3D_MMAP_BO, arg, sizeof(struct drm_v3d_mmap_bo));
   case DRM_IOCTL_V3D_GET_BO_OFFSET:
      return ioctl(fd, HAIKU_V3D_GET_BO_OFFSET, arg, sizeof(struct drm_v3d_get_bo_offset));
   case DRM_IOCTL_V3D_WAIT_BO:
      return ioctl(fd, HAIKU_V3D_WAIT_BO, arg, sizeof(struct drm_v3d_wait_bo));
   case DRM_IOCTL_V3D_SUBMIT_CL:
      return ioctl(fd, HAIKU_V3D_SUBMIT_CL, arg, sizeof(struct drm_v3d_submit_cl));
   case DRM_IOCTL_V3D_SUBMIT_TFU:
      return ioctl(fd, HAIKU_V3D_SUBMIT_TFU, arg, sizeof(struct drm_v3d_submit_tfu));
   case DRM_IOCTL_V3D_SUBMIT_CSD:
      return ioctl(fd, HAIKU_V3D_SUBMIT_CSD, arg, sizeof(struct drm_v3d_submit_csd));
   case DRM_IOCTL_GEM_CLOSE: {
      struct haiku_v3d_handle handle = { ((struct drm_gem_close *)arg)->handle, 0 };
      return ioctl(fd, HAIKU_V3D_CLOSE_BO, &handle, sizeof(handle));
   }
   default:
      /* flink names, performance monitors, CPU jobs */
      errno = ENOSYS;
      return -1;
   }
}

static inline int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   struct haiku_v3d_handle request = { 0, 0 };
   (void)flags;
   if (ioctl(fd, HAIKU_V3D_SYNC_CREATE, &request, sizeof(request)) != 0)
      return -errno;
   *handle = request.handle;
   return 0;
}

static inline int
drmSyncobjDestroy(int fd, uint32_t handle)
{
   struct haiku_v3d_handle request = { handle, 0 };
   return ioctl(fd, HAIKU_V3D_SYNC_DESTROY, &request, sizeof(request)) != 0
      ? -errno : 0;
}

static inline int64_t
haiku_v3d_relative_timeout(int64_t absolute_ns)
{
   struct timespec now;
   int64_t now_ns;
   if (absolute_ns == INT64_MAX)
      return -1;
   clock_gettime(CLOCK_MONOTONIC, &now);
   now_ns = (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec;
   return absolute_ns > now_ns ? absolute_ns - now_ns : 0;
}

/* timeout_nsec is absolute (CLOCK_MONOTONIC), as in libdrm */
static inline int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
               int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   (void)flags;
   for (unsigned i = 0; i < num_handles; i++) {
      struct haiku_v3d_sync request = { handles[i], 0, 0,
         haiku_v3d_relative_timeout(timeout_nsec) };
      if (ioctl(fd, HAIKU_V3D_SYNC_WAIT, &request, sizeof(request)) != 0)
         return -ETIME;
   }
   if (first_signaled != NULL)
      *first_signaled = 0;
   return 0;
}

/* The "sync file" is a token: the low 31 bits of the job's sequence number. */
static inline int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   struct haiku_v3d_sync request = { handle, 0, 0, 0 };
   if (ioctl(fd, HAIKU_V3D_SYNC_GET, &request, sizeof(request)) != 0)
      return -errno;
   *sync_file_fd = (int)(request.seqno & 0x7fffffff);
   return 0;
}

static inline int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
   /* Jobs run in submission order: there is nothing to import. */
   (void)fd; (void)handle; (void)sync_file_fd;
   return 0;
}

/* Waits for the job behind a token; timeout_ns is relative, negative waits
 * for good. Returns 0 once the job is done. */
static inline int
haiku_v3d_wait_token(int fd, int token, int64_t timeout_ns)
{
   struct haiku_v3d_sync request = { HAIKU_V3D_TOKEN, 0, (uint32_t)token,
      timeout_ns };
   return ioctl(fd, HAIKU_V3D_SEQNO_WAIT, &request, sizeof(request)) != 0
      ? -errno : 0;
}

static inline int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd; (void)handle; (void)flags; (void)prime_fd;
   errno = ENOSYS;
   return -1;
}

static inline int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd; (void)prime_fd; (void)handle;
   errno = ENOSYS;
   return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* HAIKU_V3D_XF86DRM_H */
