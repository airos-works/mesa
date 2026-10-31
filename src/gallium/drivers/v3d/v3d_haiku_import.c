/*
 * Copyright 2026, air/OS. SPDX-License-Identifier: MIT
 *
 * Memory of this team as V3D buffers (air/OS haiku#49): the frame buffer
 * cloned from the display driver, app_server's copy of the screen. The
 * render device wires them in this team until the buffer and its jobs are
 * gone (V3D_HAIKU_IMPORT_BO), so they must stay mapped; the buffers are
 * for shader storage only, never mapped through the driver (the caller has
 * the memory mapped already).
 */

#ifdef __HAIKU__

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <xf86drm.h>

#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "v3d_context.h"
#include "v3d_haiku_cache.h"
#include "v3d_resource.h"
#include "v3d_screen.h"
#include "../../winsys/v3d/drm/v3d_drm_public.h"

static bool
is_v3d(struct pipe_screen *pscreen)
{
        return pscreen && pscreen->get_name == v3d_screen_get_name;
}

bool
v3d_haiku_can_import(struct pipe_screen *pscreen)
{
        return is_v3d(pscreen) && v3d_screen(pscreen)->haiku_can_import;
}

struct pipe_resource *
v3d_haiku_import_buffer(struct pipe_screen *pscreen, void *address,
                        size_t size, bool *cached)
{
        if (!v3d_haiku_can_import(pscreen) || !address || !size ||
            size > UINT32_MAX - 4096)
                return NULL;
        struct v3d_screen *screen = v3d_screen(pscreen);

        struct haiku_v3d_import_bo import = {
                .address = (uintptr_t)address,
                .size = size,
        };
        int ret;
        do {
                ret = ioctl(screen->fd, HAIKU_V3D_IMPORT_BO, &import,
                            sizeof(import));
        } while (ret != 0 && errno == B_INTERRUPTED);
        if (ret != 0)
                return NULL;

        struct v3d_bo *bo = CALLOC_STRUCT(v3d_bo);
        struct v3d_resource *rsc = CALLOC_STRUCT(v3d_resource);
        if (!bo || !rsc) {
                struct haiku_v3d_handle close = { import.handle, 0 };
                ioctl(screen->fd, HAIKU_V3D_CLOSE_BO, &close, sizeof(close));
                FREE(bo);
                FREE(rsc);
                return NULL;
        }

        /* Not private: never kept in the buffer cache for reuse. No map:
         * freeing it must not unmap the caller's memory. */
        pipe_reference_init(&bo->reference, 1);
        bo->screen = screen;
        bo->name = "haiku import";
        bo->handle = import.handle;
        bo->size = align(size, 4096);
        bo->offset = import.offset;
        bo->private = false;
        screen->bo_count++;
        screen->bo_size += bo->size;

        struct pipe_resource *prsc = &rsc->base;
        pipe_reference_init(&prsc->reference, 1);
        prsc->screen = pscreen;
        prsc->target = PIPE_BUFFER;
        prsc->format = PIPE_FORMAT_R8_UNORM;
        prsc->width0 = size;
        prsc->height0 = prsc->depth0 = prsc->array_size = 1;
        prsc->bind = PIPE_BIND_SHADER_BUFFER;
        prsc->usage = PIPE_USAGE_DEFAULT;
        rsc->cpp = 1;
        rsc->internal_format = prsc->format;
        rsc->size = size;
        rsc->slices[0].offset = 0;
        rsc->slices[0].stride = size;
        rsc->slices[0].padded_height = 1;
        rsc->slices[0].size = size;
        rsc->slices[0].tiling = V3D_TILING_RASTER;
        rsc->bo = bo;
        rsc->serial_id++;

        *cached = (import.flags & HAIKU_V3D_IMPORT_CACHED) != 0;
        return prsc;
}

int
v3d_haiku_prepare_ranges(struct pipe_resource *buffer,
                         const struct v3d_haiku_byte_range *ranges,
                         unsigned count, bool no_wait)
{
        if (!buffer || !is_v3d(buffer->screen) || !ranges || !count)
                return -EINVAL;
        struct v3d_resource *rsc = v3d_resource(buffer);
        int fd = v3d_screen(buffer->screen)->fd;
        /* the kernel takes at most 1024 at a time */
        for (unsigned first = 0; first < count; first += 1024) {
                struct haiku_v3d_cpu_ranges request = {
                        .handle = rsc->bo->handle,
                        .flags = no_wait ? HAIKU_V3D_RANGES_NO_WAIT : 0,
                        .ranges = (uintptr_t)(ranges + first),
                        .count = MIN2(count - first, 1024),
                };
                int ret;
                do {
                        ret = ioctl(fd, HAIKU_V3D_CPU_PREPARE_RANGES, &request,
                                    sizeof(request));
                } while (ret != 0 && errno == B_INTERRUPTED);
                if (ret != 0)
                        return -abs(errno);
        }
        return 0;
}

int
v3d_haiku_last_job_token(struct pipe_context *pctx)
{
        struct v3d_context *v3d = v3d_context(pctx);
        int token = -1;
        if (drmSyncobjExportSyncFile(v3d->fd, v3d->out_sync, &token) != 0)
                return -1;
        return token;
}

int
v3d_haiku_wait_job_token(struct pipe_screen *pscreen, int token,
                         int64_t timeout_ns)
{
        if (!is_v3d(pscreen) || token < 0)
                return -EINVAL;
        int ret;
        do {
                ret = haiku_v3d_wait_token(v3d_screen(pscreen)->fd, token,
                                           timeout_ns);
                /* -errno, and Haiku's error codes are negative */
        } while (ret == -B_INTERRUPTED);
        return ret;
}

#endif /* __HAIKU__ */

#ifdef __HAIKU__
struct pipe_resource *
v3d_haiku_linear_view(struct pipe_resource *buffer, uint64_t offset,
                      unsigned width, unsigned height, unsigned stride)
{
        if (!buffer || !is_v3d(buffer->screen) || !width || !height ||
            stride < width * 4 ||
            offset + (uint64_t)stride * height > buffer->width0)
                return NULL;
        struct v3d_resource *source = v3d_resource(buffer);
        struct v3d_resource *rsc = CALLOC_STRUCT(v3d_resource);
        if (!rsc)
                return NULL;

        struct pipe_resource *prsc = &rsc->base;
        pipe_reference_init(&prsc->reference, 1);
        prsc->screen = buffer->screen;
        prsc->target = PIPE_TEXTURE_2D;
        prsc->format = PIPE_FORMAT_B8G8R8A8_UNORM;
        prsc->width0 = width;
        prsc->height0 = height;
        prsc->depth0 = prsc->array_size = 1;
        prsc->bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR;
        prsc->usage = PIPE_USAGE_DEFAULT;
        rsc->cpp = 4;
        rsc->tiled = false;
        rsc->internal_format = prsc->format;
        rsc->slices[0].offset = offset;
        rsc->slices[0].stride = stride;
        rsc->slices[0].padded_height = height;
        rsc->slices[0].size = stride * height;
        rsc->slices[0].tiling = V3D_TILING_RASTER;
        rsc->size = offset + (uint64_t)stride * height;
        /* contents undefined: nothing is loaded before it is drawn */
        rsc->initialized_buffers = 0;
        rsc->bo = v3d_bo_reference(source->bo);
        rsc->serial_id++;
        return prsc;
}
#endif
