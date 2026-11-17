/*
 * Copyright 2026, air/OS. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

/* air/OS (Haiku): the VideoCore IV 3D device (/dev/graphics/vc4/0, see
 * headers/private/graphics/vc4/vc4_haiku.h in the air/OS tree). It takes the
 * drm_vc4_* structures as they are; request VC4_HAIKU_FIRST_OP + n is
 * DRM_VC4_* n. What differs from Linux:
 *
 * - DRM_IOCTL_VC4_MMAP_BO maps the buffer and returns its address in
 *   "offset"; the mapping belongs to the driver and goes away with the
 *   handle (GEM_CLOSE), so it is neither unmapped nor mmap()ed here.
 * - No sync objects (DRM_CAP_SYNCOBJ is 0): fences are sequence numbers.
 * - Errors are Haiku's; a wait that timed out reports ETIME, as vc4_bufmgr.c
 *   expects, and interrupted requests are repeated.
 */
#ifndef VC4_HAIKU_H
#define VC4_HAIKU_H

#ifdef __HAIKU__

#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>

#include <Drivers.h>

#include "drm-uapi/drm.h"
#include "drm-uapi/vc4_drm.h"

#define HAIKU_VC4_DEVICE_PATH "/dev/graphics/vc4/0"

enum {
   HAIKU_VC4_FIRST_OP = B_DEVICE_OP_CODES_END + 0x7700,
   HAIKU_VC4_CLOSE_BO = HAIKU_VC4_FIRST_OP + 0x20,
};

struct haiku_vc4_handle {
   uint32_t handle;
   uint32_t pad;
};

static inline int
vc4_haiku_ioctl(int fd, unsigned long request, void *arg)
{
   struct haiku_vc4_handle close_request;
   unsigned long op;
   size_t size;

#define HAIKU_VC4_REQUEST(name, type)                    \
   case DRM_IOCTL_VC4_##name:                            \
      op = HAIKU_VC4_FIRST_OP + DRM_VC4_##name;          \
      size = sizeof(type);                               \
      break

   switch (request) {
   HAIKU_VC4_REQUEST(SUBMIT_CL, struct drm_vc4_submit_cl);
   HAIKU_VC4_REQUEST(WAIT_SEQNO, struct drm_vc4_wait_seqno);
   HAIKU_VC4_REQUEST(WAIT_BO, struct drm_vc4_wait_bo);
   HAIKU_VC4_REQUEST(CREATE_BO, struct drm_vc4_create_bo);
   HAIKU_VC4_REQUEST(MMAP_BO, struct drm_vc4_mmap_bo);
   HAIKU_VC4_REQUEST(CREATE_SHADER_BO, struct drm_vc4_create_shader_bo);
   HAIKU_VC4_REQUEST(GET_HANG_STATE, struct drm_vc4_get_hang_state);
   HAIKU_VC4_REQUEST(GET_PARAM, struct drm_vc4_get_param);
   HAIKU_VC4_REQUEST(SET_TILING, struct drm_vc4_set_tiling);
   HAIKU_VC4_REQUEST(GET_TILING, struct drm_vc4_get_tiling);
   HAIKU_VC4_REQUEST(LABEL_BO, struct drm_vc4_label_bo);
   HAIKU_VC4_REQUEST(GEM_MADVISE, struct drm_vc4_gem_madvise);
   case DRM_IOCTL_GEM_CLOSE:
      close_request.handle = ((struct drm_gem_close *)arg)->handle;
      close_request.pad = 0;
      arg = &close_request;
      op = HAIKU_VC4_CLOSE_BO;
      size = sizeof(close_request);
      break;
   default:
      /* flink names, PRIME, performance monitors */
      errno = ENOSYS;
      return -1;
   }
#undef HAIKU_VC4_REQUEST

   int ret;
   do {
      ret = ioctl(fd, op, arg, size);
   } while (ret != 0 && errno == B_INTERRUPTED);

   if (ret != 0 && errno == B_TIMED_OUT)
      errno = ETIME;
   return ret;
}

#endif /* __HAIKU__ */

#endif /* VC4_HAIKU_H */
