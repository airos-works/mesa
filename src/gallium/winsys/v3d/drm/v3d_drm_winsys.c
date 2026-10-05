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

#include "util/os_file.h"
#include "util/u_screen.h"

#include "v3d_drm_public.h"

#include "v3d/v3d_screen.h"

struct pipe_screen *
v3d_drm_screen_create(int fd, const struct pipe_screen_config *config)
{
   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config,
                                         NULL, v3d_screen_create);
}

struct pipe_screen *
v3d_drm_screen_create_renderonly(int fd, struct renderonly *ro,
                                 const struct pipe_screen_config *config)
{
   return u_pipe_screen_lookup_or_create(fd, config,
                                         ro, v3d_screen_create);
}

#ifdef __HAIKU__
#include "renderonly/renderonly.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "util/macros.h"

static const driOptionDescription haiku_v3d_options[] = {
#include "v3d/driinfo_v3d.h"
};

struct pipe_screen *
v3d_haiku_screen_create(const char *device)
{
   int fd = open(device, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return NULL;

   driOptionCache info = {0}, options = {0};
   driParseOptionInfo(&info, haiku_v3d_options, ARRAY_SIZE(haiku_v3d_options));
   const struct pipe_screen_config config = {
      .options = &options,
      .options_info = &info,
   };
   /* Not through the screen cache: its key is the device's inode, and the
    * screen owns this descriptor. */
   struct pipe_screen *screen = v3d_screen_create(fd, &config, NULL);
   if (screen == NULL)
      close(fd);
   driDestroyOptionCache(&options);
   driDestroyOptionInfo(&info);
   return screen;
}

/* There is no KMS device to scan out through; v3d only reaches these with a
 * renderonly object, which the Haiku screen never has. The auxiliary
 * library that implements them is not built without DRM. */
void
renderonly_scanout_destroy(struct renderonly_scanout *scanout,
                           struct renderonly *ro)
{
   (void)scanout;
   (void)ro;
}

struct renderonly_scanout *
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
