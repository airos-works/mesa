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

#ifndef __V3D_DRM_PUBLIC_H__
#define __V3D_DRM_PUBLIC_H__

struct pipe_screen;
struct pipe_screen_config;
struct renderonly;

struct pipe_screen *v3d_drm_screen_create(int drmFD,
                                          const struct pipe_screen_config *config);
struct pipe_screen *v3d_drm_screen_create_renderonly(int fd, struct renderonly *ro,
                                                     const struct pipe_screen_config *config);

#ifdef __HAIKU__
/* Opens the V3D render device of air/OS and creates the screen on it. */
struct pipe_screen *v3d_haiku_screen_create(const char *device);
/* False for any other Gallium driver or when the ownership API is absent. */
int v3d_haiku_gpu_readback_enabled(struct pipe_screen *screen);

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pipe_context;
struct pipe_resource;

/* Bytes of a buffer: rows rows of bytes bytes, stride apart (0 rows: 1). */
struct v3d_haiku_byte_range {
   uint64_t offset;
   uint32_t bytes;
   uint32_t rows;
   uint32_t stride;
   uint32_t pad;
};

/* Whether the device imports memory (V3D_HAIKU_PARAM_IMPORT_BO). */
bool v3d_haiku_can_import(struct pipe_screen *screen);
/* A buffer (PIPE_BUFFER, for shader storage) of the page-aligned memory at
 * address in this team, which the driver keeps wired until the resource and
 * the jobs using it are gone: the memory must stay mapped until then
 * (deleting its area waits for that). *cached says whether the CPU caches it: then
 * v3d_haiku_prepare_ranges() before a job writes it and before the CPU reads
 * what one wrote. NULL where the device cannot. */
struct pipe_resource *v3d_haiku_import_buffer(struct pipe_screen *screen,
                                              void *address, size_t size,
                                              bool *cached);
/* Cache maintenance of an imported cached buffer's ranges; waits for its
 * jobs first unless no_wait. 0 or a negative error. */
int v3d_haiku_prepare_ranges(struct pipe_resource *buffer,
                             const struct v3d_haiku_byte_range *ranges,
                             unsigned count, bool no_wait);
/* A token for the last job the context submitted, and waiting for one
 * (relative timeout, negative: for good; 0 once it is done). */
int v3d_haiku_last_job_token(struct pipe_context *context);
int v3d_haiku_wait_job_token(struct pipe_screen *screen, int token,
                             int64_t timeout_ns);
#endif

#endif /* __V3D_DRM_PUBLIC_H__ */
