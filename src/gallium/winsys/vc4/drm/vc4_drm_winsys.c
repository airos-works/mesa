/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "util/os_file.h"
#include "util/u_screen.h"

#include "renderonly/renderonly.h"
#include "kmsro/drm/kmsro_drm_public.h"
#include "vc4_drm_public.h"
#include "vc4/vc4_screen.h"
#include "drm-uapi/vc4_drm.h"

struct pipe_screen *
vc4_drm_screen_create(int fd, const struct pipe_screen_config *config)
{
   bool v3d_present = true;

#ifndef USE_VC4_SIMULATOR
   struct drm_vc4_get_param ident0 = {
      .param = DRM_VC4_PARAM_V3D_IDENT0,
   };

   int ret = ioctl(fd, DRM_IOCTL_VC4_GET_PARAM, &ident0);
   v3d_present = ret == 0;
#endif

   if (v3d_present)
      return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config,
                                            NULL, vc4_screen_create);

#ifdef GALLIUM_KMSRO
   return kmsro_drm_screen_create(fd, config);
#endif

   return NULL;
}

struct pipe_screen *
vc4_drm_screen_create_renderonly(int fd, struct renderonly *ro,
                                 const struct pipe_screen_config *config)
{
   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config,
                                         ro, vc4_screen_create);
}

#ifdef __HAIKU__
struct pipe_screen *
vc4_haiku_screen_create(const char *device)
{
   int fd = open(device, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return NULL;

   /* Not through the screen cache: its key is the device's inode, and the
    * screen owns this descriptor (vc4_screen_create() closes it when it
    * fails). The screen takes no driconf options. */
   return vc4_screen_create(fd, NULL, NULL);
}

/* There is no KMS device to scan out through; vc4 only reaches these with a
 * renderonly object, which the Haiku screen never has. The auxiliary library
 * that implements them is not built without DRM, and v3d's winsys has the
 * same stand-ins: weak, so that either one does. */
__attribute__((weak)) void
renderonly_scanout_destroy(struct renderonly_scanout *scanout,
                           struct renderonly *ro)
{
   (void)scanout;
   (void)ro;
}

__attribute__((weak)) struct renderonly_scanout *
renderonly_create_gpu_import_for_resource(struct pipe_resource *rsc,
                                          struct renderonly *ro,
                                          struct winsys_handle *out_handle)
{
   (void)rsc;
   (void)ro;
   (void)out_handle;
   return NULL;
}
#endif
