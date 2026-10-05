/*
 * Copyright 2026 air/OS
 * SPDX-License-Identifier: MIT
 */

/* DRM sync objects for Haiku's V3D render device (/dev/graphics/v3d/0).
 *
 * The device runs its jobs in the order they are submitted. A sync object
 * there holds the number of the job it was given, or that it was signalled or
 * reset by hand; that is enough for Vulkan's fences and binary semaphores.
 * There are no sync files and no timelines.
 */

#include "u_sync_provider.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <xf86drm.h>
#include "drm-uapi/drm.h"

struct haiku_sync_provider {
   struct util_sync_provider base;
   int fd;
};

static int
haiku_fd(struct util_sync_provider *p)
{
   return ((struct haiku_sync_provider *)p)->fd;
}

static int64_t
now_ns(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec;
}

static int
haiku_create(struct util_sync_provider *p, uint32_t flags, uint32_t *handle)
{
   struct haiku_v3d_handle request = { 0,
      (flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0
         ? 0 : HAIKU_V3D_SYNC_UNSIGNALED };
   if (ioctl(haiku_fd(p), HAIKU_V3D_SYNC_CREATE, &request, sizeof(request)) != 0)
      return -errno;
   *handle = request.handle;
   return 0;
}

static int
haiku_destroy(struct util_sync_provider *p, uint32_t handle)
{
   struct haiku_v3d_handle request = { handle, 0 };
   return ioctl(haiku_fd(p), HAIKU_V3D_SYNC_DESTROY, &request,
                sizeof(request)) != 0 ? -errno : 0;
}

static int
haiku_no_fd(struct util_sync_provider *p, uint32_t handle, int *fd)
{
   errno = ENOSYS;
   return -ENOSYS;
}

static int
haiku_no_handle(struct util_sync_provider *p, int fd, uint32_t *handle)
{
   errno = ENOSYS;
   return -ENOSYS;
}

static int
haiku_no_import(struct util_sync_provider *p, uint32_t handle, int fd)
{
   errno = ENOSYS;
   return -ENOSYS;
}

/* One sync object, for at most timeout_ns (relative; negative: for good). */
static int
wait_one(int fd, uint32_t handle, int64_t timeout_ns, unsigned flags)
{
   if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0) {
      /* only until it has a job (or was signalled) */
      int64_t end = timeout_ns < 0 ? INT64_MAX : now_ns() + timeout_ns;
      while (true) {
         struct haiku_v3d_sync request = { handle, 0, 0, 0 };
         if (ioctl(fd, HAIKU_V3D_SYNC_GET, &request, sizeof(request)) == 0)
            return 0;
         if (now_ns() >= end) {
            errno = ETIME;
            return -ETIME;
         }
         usleep(200);
      }
   }

   struct haiku_v3d_sync request = { handle,
      (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0
         ? HAIKU_V3D_SYNC_WAIT_FOR_SUBMIT : 0,
      0, timeout_ns };
   if (ioctl(fd, HAIKU_V3D_SYNC_WAIT, &request, sizeof(request)) == 0)
      return 0;
   if (errno == ETIMEDOUT || errno == EWOULDBLOCK)
      errno = ETIME;
   return -errno;
}

/* timeout_nsec is absolute (CLOCK_MONOTONIC), as with libdrm */
static int
haiku_wait(struct util_sync_provider *p, uint32_t *handles,
           unsigned num_handles, int64_t timeout_nsec, unsigned flags,
           uint32_t *first_signaled)
{
   int fd = haiku_fd(p);
   bool forever = timeout_nsec == INT64_MAX;

   if (num_handles == 0)
      return 0;

   if (num_handles == 1 || (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0) {
      for (unsigned i = 0; i < num_handles; i++) {
         int64_t left = -1;
         if (!forever) {
            left = timeout_nsec - now_ns();
            if (left < 0)
               left = 0;
         }
         int err = wait_one(fd, handles[i], left, flags);
         if (err != 0)
            return err;
      }
      if (first_signaled != NULL)
         *first_signaled = 0;
      return 0;
   }

   /* any of them: look at each in turn */
   while (true) {
      for (unsigned i = 0; i < num_handles; i++) {
         int err = wait_one(fd, handles[i], 0, flags);
         if (err == 0) {
            if (first_signaled != NULL)
               *first_signaled = i;
            return 0;
         }
         if (errno != ETIME)
            return err;
      }
      if (!forever && now_ns() >= timeout_nsec) {
         errno = ETIME;
         return -ETIME;
      }
      usleep(200);
   }
}

static int
set_all(struct util_sync_provider *p, unsigned long request_code,
        const uint32_t *handles, uint32_t count)
{
   for (uint32_t i = 0; i < count; i++) {
      struct haiku_v3d_sync request = { handles[i], 0, 0, 0 };
      if (ioctl(haiku_fd(p), request_code, &request, sizeof(request)) != 0)
         return -errno;
   }
   return 0;
}

static int
haiku_reset(struct util_sync_provider *p, const uint32_t *handles,
            uint32_t count)
{
   return set_all(p, HAIKU_V3D_SYNC_RESET, handles, count);
}

static int
haiku_signal(struct util_sync_provider *p, const uint32_t *handles,
             uint32_t count)
{
   return set_all(p, HAIKU_V3D_SYNC_SIGNAL, handles, count);
}

static int
haiku_transfer(struct util_sync_provider *p, uint32_t dst_handle,
               uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
               uint32_t flags)
{
   struct haiku_v3d_sync request = { dst_handle, 0, src_handle, 0 };
   return ioctl(haiku_fd(p), HAIKU_V3D_SYNC_TRANSFER, &request,
                sizeof(request)) != 0 ? -errno : 0;
}

static void
haiku_finalize(struct util_sync_provider *p)
{
   free(p);
}

static struct util_sync_provider *
haiku_clone(struct util_sync_provider *p)
{
   return util_sync_provider_drm(haiku_fd(p));
}

struct util_sync_provider *
util_sync_provider_drm(int drm_fd)
{
   struct haiku_sync_provider *provider = calloc(1, sizeof(*provider));
   if (provider == NULL)
      return NULL;

   provider->fd = drm_fd;
   provider->base.create = haiku_create;
   provider->base.destroy = haiku_destroy;
   provider->base.handle_to_fd = haiku_no_fd;
   provider->base.fd_to_handle = haiku_no_handle;
   provider->base.import_sync_file = haiku_no_import;
   provider->base.export_sync_file = haiku_no_fd;
   provider->base.wait = haiku_wait;
   provider->base.reset = haiku_reset;
   provider->base.signal = haiku_signal;
   provider->base.transfer = haiku_transfer;
   provider->base.finalize = haiku_finalize;
   provider->base.clone = haiku_clone;
   /* no timelines: timeline_signal, timeline_wait and query stay NULL */
   return &provider->base;
}
