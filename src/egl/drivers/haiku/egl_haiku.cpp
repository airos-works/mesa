/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2014 Adrián Arroyo Calle <adrian.arroyocalle@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <new>
#include <cstdlib>
#include <cstring>

#include "eglconfig.h"
#include "eglcontext.h"
#include "eglcurrent.h"
#include "egldevice.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "eglimage.h"
#include "egllog.h"
#include "eglsurface.h"
#include "egltypedefs.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "state_tracker/st_context.h"
#include "util/box.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"
#include "mesa/glapi/glapi/glapi.h"

#include "hgl/hgl_sw_winsys.h"
#include "hgl_context.h"
#ifdef GALLIUM_PANFROST
#include "panfrost/pan_public.h"
#endif
#ifdef GALLIUM_V3D
extern "C" {
#include "v3d/drm/v3d_drm_public.h"
}
#endif

#include <Bitmap.h>

extern "C" {
#include "target-helpers/inline_sw_helper.h"
}

#if MESA_DEBUG
#define TRACE(x...) printf("egl_haiku: " x)
#define CALLED()    TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#define TRACE(x...)
#define CALLED()
#endif
#define ERROR(x...) printf("egl_haiku: " x)

_EGL_DRIVER_STANDARD_TYPECASTS(haiku_egl)

struct haiku_egl_display {
   int ref_count;
   bool native_csf;
   struct hgl_display *disp;
};

struct haiku_egl_config {
   _EGLConfig base;
};

struct haiku_egl_context {
   _EGLContext base;
   struct hgl_context *ctx;
};

struct haiku_present_buffer {
   BBitmap *bitmap;
   struct pipe_resource *linear;
   area_id area;
};

struct haiku_egl_surface {
   _EGLSurface base;
   struct hgl_buffer *fb;
   struct pipe_fence_handle *throttle_fence;
   struct haiku_present_buffer *front_buffer;
   struct haiku_present_buffer *back_buffer;
};

/* Private air/OS performance hint for the current EGL context. A negative
 * result means no preference, zero means whole rectangles, positive values
 * give a preferred CPU readback band size in bytes. No GL state is changed.
 */
extern "C" PUBLIC int
haiku_mesa_readback_band_bytes(void)
{
#ifdef GALLIUM_V3D
   struct st_context *st = st_api_get_current();
   if (st && v3d_haiku_gpu_readback_enabled(st->pipe->screen))
      return 0;
#endif
   return -1;
}

static void update_size(struct hgl_buffer *buffer);

/* A view owns its displayed bitmap until SetBitmap returns it. Keep each
 * shared GPU resource and area alive for exactly the same interval. */
static void
haiku_destroy_present_buffer(struct haiku_present_buffer *buffer)
{
   if (!buffer)
      return;
   delete buffer->bitmap;
   if (buffer->area >= B_OK)
      delete_area(buffer->area);
   pipe_resource_reference(&buffer->linear, NULL);
   delete buffer;
}

static bool
haiku_shared_present_enabled(struct pipe_screen *screen)
{
#ifdef GALLIUM_V3D
   const char *value = std::getenv("HAIKU_V3D_SHARED_PRESENT");
   return (!value || !std::strcmp(value, "1")) &&
          v3d_haiku_gpu_readback_enabled(screen);
#else
   return false;
#endif
}

/* V3D's linear staging resources have a stable cached area. Clone only that
 * owned area for app_server; normal transfer memory from other drivers must
 * never be treated as a shareable resource. Failure falls back to CPU copies. */
static bool
haiku_create_shared_bitmap(struct pipe_context *pipe,
                           struct haiku_present_buffer *buffer,
                           unsigned width, unsigned height)
{
   if (!haiku_shared_present_enabled(pipe->screen) ||
       width < 128 || height < 64 || uint64_t(width) * height < 128 * 1024)
      return false;

   struct pipe_resource tmpl = {};
   tmpl.target = PIPE_TEXTURE_2D;
   tmpl.format = PIPE_FORMAT_BGRA8888_UNORM;
   tmpl.width0 = align(width, 16);
   tmpl.height0 = align(height, 4);
   tmpl.depth0 = tmpl.array_size = 1;
   tmpl.nr_samples = tmpl.nr_storage_samples = 1;
   tmpl.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_LINEAR;
   tmpl.usage = PIPE_USAGE_STAGING;
   struct pipe_resource *linear = pipe->screen->resource_create(pipe->screen, &tmpl);
   if (!linear)
      return false;

   struct pipe_box box;
   u_box_2d(0, 0, tmpl.width0, tmpl.height0, &box);
   struct pipe_transfer *transfer = NULL;
   void *pixels = pipe->texture_map(pipe, linear, 0,
      PIPE_MAP_READ | PIPE_MAP_WRITE | PIPE_MAP_DIRECTLY, &box, &transfer);
   area_id shared = B_BAD_VALUE;
   BBitmap *bitmap = NULL;
   if (pixels && transfer->stride >= size_t(width) * 4 &&
       transfer->stride <= INT32_MAX) {
      area_info info;
      area_id original = area_for(pixels);
      if (original >= B_OK && get_area_info(original, &info) == B_OK) {
         size_t offset = static_cast<uint8_t *>(pixels) -
                         static_cast<uint8_t *>(info.address);
         size_t bytes = size_t(transfer->stride) * tmpl.height0;
         if (offset <= info.size && bytes <= info.size - offset) {
            void *base = NULL;
            shared = clone_area("Haiku EGL bitmap", &base, B_ANY_ADDRESS,
               B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA, original);
            if (shared >= B_OK) {
               // Initialize padding once. GPU blits touch only visible pixels.
               memset(pixels, 0, bytes);
               bitmap = new(std::nothrow) BBitmap(shared, offset,
                  BRect(0, 0, width - 1, height - 1), 0, B_RGBA32,
                  transfer->stride);
            }
         }
      }
   }
   if (pixels)
      pipe->texture_unmap(pipe, transfer);
   if (!bitmap || bitmap->InitCheck() != B_OK) {
      delete bitmap;
      if (shared >= B_OK)
         delete_area(shared);
      pipe_resource_reference(&linear, NULL);
      return false;
   }
   buffer->bitmap = bitmap;
   buffer->linear = linear;
   buffer->area = shared;
   return true;
}

static bool
haiku_present_texture(struct pipe_context *pipe, struct hgl_buffer *buffer,
                      struct pipe_resource *texture)
{
   if (!pipe || !buffer->winsysContext || !texture || texture->width0 == 0 ||
       texture->height0 == 0 || texture->width0 > INT32_MAX ||
       texture->format != PIPE_FORMAT_BGRA8888_UNORM)
      return false;

   struct haiku_egl_surface *surface =
      static_cast<struct haiku_egl_surface *>(buffer->present_private);
   if (!surface)
      return false;
   const unsigned width = texture->width0, height = texture->height0;
   struct haiku_present_buffer *back = surface->back_buffer;
   surface->back_buffer = NULL;
   if (back && (back->bitmap->Bounds().IntegerWidth() + 1 != int64_t(width) ||
                back->bitmap->Bounds().IntegerHeight() + 1 != int64_t(height))) {
      haiku_destroy_present_buffer(back);
      back = NULL;
   }
   if (!back) {
      back = new(std::nothrow) haiku_present_buffer{NULL, NULL, B_BAD_VALUE};
      if (!back)
         return false;
      if (!haiku_create_shared_bitmap(pipe, back, width, height))
         back->bitmap = new(std::nothrow) BBitmap(
            BRect(0, 0, width - 1, height - 1), 0, B_RGBA32);
   }
   if (!back->bitmap || back->bitmap->InitCheck() != B_OK) {
      haiku_destroy_present_buffer(back);
      return false;
   }

   struct pipe_resource *source = texture;
   if (back->linear) {
      struct pipe_blit_info blit = {};
      blit.src.resource = texture;
      blit.src.format = texture->format;
      u_box_2d(0, 0, width, height, &blit.src.box);
      blit.dst.resource = back->linear;
      blit.dst.format = back->linear->format;
      u_box_2d(0, 0, width, height, &blit.dst.box);
      blit.mask = PIPE_MASK_RGBA;
      blit.filter = PIPE_TEX_FILTER_NEAREST;
      pipe->blit(pipe, &blit);
      source = back->linear;
   }

   const size_t row_bytes = size_t(width) * 4;
   struct pipe_box box;
   u_box_2d(0, 0, width, height, &box);
   struct pipe_transfer *transfer = NULL;
   const uint8_t *pixels = static_cast<const uint8_t *>(
      pipe->texture_map(pipe, source, 0, PIPE_MAP_READ, &box, &transfer));
   if (!pixels || transfer->stride < row_bytes ||
       back->bitmap->BytesPerRow() < int64_t(row_bytes)) {
      if (pixels)
         pipe->texture_unmap(pipe, transfer);
      haiku_destroy_present_buffer(back);
      return false;
   }

   // Mapping waits for the GPU and prepares cached CPU reads. A shared bitmap
   // already aliases this storage, so app_server can read it without a copy.
   if (!back->linear) {
      const size_t destination_stride = back->bitmap->BytesPerRow();
      for (unsigned y = 0; y < height; ++y) {
         uint8_t *destination = static_cast<uint8_t *>(back->bitmap->Bits())
            + size_t(y) * destination_stride;
         memcpy(destination, pixels + size_t(y) * transfer->stride, row_bytes);
         if (destination_stride > row_bytes)
            memset(destination + row_bytes, 0, destination_stride - row_bytes);
      }
   }
   pipe->texture_unmap(pipe, transfer);
   BBitmap *retired =
      static_cast<BitmapHook *>(buffer->winsysContext)->SetBitmap(back->bitmap);
   // BitmapHook returns our prior front buffer under its draw lock, after
   // synchronous DrawBitmap reads have completed. Only that buffer is reused.
   assert(retired == (surface->front_buffer
      ? surface->front_buffer->bitmap : NULL));
   surface->back_buffer = surface->front_buffer;
   surface->front_buffer = back;
   return true;
}

// #pragma mark EGLSurface

// Called via eglCreateWindowSurface(), drv->CreateWindowSurface().
static _EGLSurface *
haiku_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                            void *native_window, const EGLint *attrib_list)
{
   if (!native_window) {
      _eglError(EGL_BAD_NATIVE_WINDOW, "Haiku EGL needs a BitmapHook");
      return NULL;
   }
   printf("haiku_create_window_surface\n");
   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);

   struct haiku_egl_surface *wgl_surf =
      (struct haiku_egl_surface *)calloc(1, sizeof(*wgl_surf));
   if (!wgl_surf)
      return NULL;

   if (!_eglInitSurface(&wgl_surf->base, disp, EGL_WINDOW_BIT, conf,
                        attrib_list, NULL)) {
      free(wgl_surf);
      return NULL;
   }

   struct st_visual visual;
   hgl_get_st_visual(&visual, HGL_DOUBLE | HGL_ALPHA | HGL_DEPTH);

   wgl_surf->fb =
      hgl_create_st_framebuffer(hgl_dpy->disp, &visual, native_window);
   if (!wgl_surf->fb) {
      free(wgl_surf);
      return NULL;
   }

   wgl_surf->fb->update_size = update_size;
   if (hgl_dpy->native_csf) {
      wgl_surf->fb->present = haiku_present_texture;
      wgl_surf->fb->present_private = wgl_surf;
   }
   update_size(wgl_surf->fb);
   wgl_surf->base.Width = wgl_surf->fb->newWidth;
   wgl_surf->base.Height = wgl_surf->fb->newHeight;

   // Unset and delete previously set bitmap if any.
   delete ((BitmapHook *)native_window)->SetBitmap(NULL);

   return &wgl_surf->base;
}

static _EGLSurface *
haiku_create_pixmap_surface(_EGLDisplay *disp, _EGLConfig *conf,
                            void *native_pixmap, const EGLint *attrib_list)
{
   printf("haiku_create_pixmap_surface\n");
   return NULL;
}

static _EGLSurface *
haiku_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf,
                             const EGLint *attrib_list)
{
   printf("haiku_create_pbuffer_surface\n");
   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);

   struct haiku_egl_surface *wgl_surf =
      (struct haiku_egl_surface *)calloc(1, sizeof(*wgl_surf));
   if (!wgl_surf)
      return NULL;

   if (!_eglInitSurface(&wgl_surf->base, disp, EGL_PBUFFER_BIT, conf,
                        attrib_list, NULL)) {
      free(wgl_surf);
      return NULL;
   }

   struct st_visual visual;
   hgl_get_st_visual(&visual, HGL_ALPHA | HGL_DEPTH);

   wgl_surf->fb = hgl_create_st_framebuffer(hgl_dpy->disp, &visual, NULL);
   if (!wgl_surf->fb) {
      free(wgl_surf);
      return NULL;
   }

   wgl_surf->fb->newWidth = wgl_surf->base.Width;
   wgl_surf->fb->newHeight = wgl_surf->base.Height;
   p_atomic_inc(&wgl_surf->fb->base.stamp);

   return &wgl_surf->base;
}

#ifdef GALLIUM_PANFROST
/* The mali_csf driver publishes its first device here. */
static const char kHaikuDefaultCsfDevice[] = "/dev/graphics/mali_csf/0";
#endif

static EGLBoolean
haiku_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   if (_eglPutSurface(surf)) {
      struct haiku_egl_display *hgl_dpy =
         haiku_egl_display(surf->Resource.Display);
      struct haiku_egl_surface *hgl_surf = haiku_egl_surface(surf);
      struct pipe_screen *screen = hgl_dpy->disp->fscreen->screen;
      screen->fence_reference(screen, &hgl_surf->throttle_fence, NULL);

      // Unset bitmap to release ownership. Bitmap will be deleted later
      // when destroying framebuffer.
      BitmapHook *bitmapHook = (BitmapHook*)hgl_surf->fb->winsysContext;
      if (bitmapHook != NULL) {
         BBitmap *bitmap = bitmapHook->SetBitmap(NULL);
         if (hgl_surf->fb->present)
            assert(bitmap == (hgl_surf->front_buffer
               ? hgl_surf->front_buffer->bitmap : NULL));
      }

      haiku_destroy_present_buffer(hgl_surf->front_buffer);
      haiku_destroy_present_buffer(hgl_surf->back_buffer);
      hgl_destroy_st_framebuffer(hgl_surf->fb);
      free(surf);
   }
   return EGL_TRUE;
}

static void
update_size(struct hgl_buffer *buffer)
{
   /* An offscreen surface keeps its EGL dimensions and has no BitmapHook. */
   if (!buffer->winsysContext)
      return;

   uint32_t newWidth, newHeight;
   ((BitmapHook *)buffer->winsysContext)->GetSize(newWidth, newHeight);
   if (buffer->newWidth != newWidth || buffer->newHeight != newHeight) {
      /* Software displaytargets own their bitmaps. Detach the borrowed bitmap
       * before drawable validation drops the old texture references on resize.
       * Native presentation bitmaps have an independent lifetime.
       */
      if (!buffer->present)
         ((BitmapHook *)buffer->winsysContext)->SetBitmap(NULL);
      buffer->newWidth = newWidth;
      buffer->newHeight = newHeight;
      p_atomic_inc(&buffer->base.stamp);
   }
}

static EGLBoolean
haiku_query_surface(_EGLDisplay *disp, _EGLSurface *surf, EGLint attribute,
                     EGLint *value)
{
   if (surf->Type == EGL_WINDOW_BIT) {
      struct hgl_buffer *buffer = haiku_egl_surface(surf)->fb;
      update_size(buffer);
      surf->Width = buffer->newWidth;
      surf->Height = buffer->newHeight;
   }
   return _eglQuerySurface(disp, surf, attribute, value);
}

static EGLBoolean
haiku_swap_buffers(_EGLDisplay *disp, _EGLSurface *surf)
{
   /* EGL swaps only window surfaces. Pbuffers retain their contents. */
   if (surf->Type == EGL_PBUFFER_BIT)
      return EGL_TRUE;

   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);
   struct haiku_egl_surface *hgl_surf = haiku_egl_surface(surf);
   struct haiku_egl_context *hgl_ctx = haiku_egl_context(surf->CurrentContext);
   if (hgl_ctx == NULL)
      return EGL_FALSE;

   struct st_context *st = hgl_ctx->ctx->st;
   struct pipe_screen *screen = hgl_dpy->disp->fscreen->screen;

   _mesa_glthread_finish(st->ctx);

   struct hgl_buffer *buffer = hgl_surf->fb;
   auto &frontBuffer = buffer->textures[ST_ATTACHMENT_FRONT_LEFT];
   auto &backBuffer = buffer->textures[ST_ATTACHMENT_BACK_LEFT];

   // Inform ST of a flush if double buffering is used
   if (backBuffer != NULL)
      st->pipe->flush_resource(st->pipe, backBuffer);

   struct pipe_fence_handle *new_fence = NULL;
   st_context_flush(st, ST_FLUSH_FRONT, &new_fence, NULL, NULL);
   if (hgl_surf->throttle_fence) {
      screen->fence_finish(screen, NULL, hgl_surf->throttle_fence,
                           OS_TIMEOUT_INFINITE);
      screen->fence_reference(screen, &hgl_surf->throttle_fence, NULL);
   }
   hgl_surf->throttle_fence = new_fence;

   // flush back buffer and swap buffers if double buffering is used
   if (backBuffer != NULL) {
      if (buffer->present) {
         if (!buffer->present(st->pipe, buffer, backBuffer))
            return _eglError(EGL_BAD_ALLOC, "Haiku EGL bitmap presentation failed");
      } else {
         screen->flush_frontbuffer(screen, st->pipe, backBuffer, 0, 0,
                                   buffer->winsysContext, 1, NULL);
      }
      std::swap(frontBuffer, backBuffer);
      p_atomic_inc(&buffer->base.stamp);
   }

   // XXX: right front / back if HGL_STEREO?

   update_size(buffer);

   st_context_invalidate_state(st, ST_INVALIDATE_FB_STATE);

   return EGL_TRUE;
}

// #pragma mark EGLDisplay

static EGLBoolean
haiku_add_configs_for_visuals(_EGLDisplay *disp)
{
   CALLED();

   struct haiku_egl_config *conf;
   conf = (struct haiku_egl_config *)calloc(1, sizeof(*conf));
   if (!conf)
      return _eglError(EGL_BAD_ALLOC, "haiku_add_configs_for_visuals");

   _eglInitConfig(&conf->base, disp, 1);
   TRACE("Config inited\n");

   conf->base.RedSize = 8;
   conf->base.BlueSize = 8;
   conf->base.GreenSize = 8;
   conf->base.LuminanceSize = 0;
   conf->base.AlphaSize = 8;
   conf->base.ColorBufferType = EGL_RGB_BUFFER;
   conf->base.BufferSize = conf->base.RedSize + conf->base.GreenSize +
                           conf->base.BlueSize + conf->base.AlphaSize;
   conf->base.ConfigCaveat = EGL_NONE;
   conf->base.ConfigID = 1;
   conf->base.BindToTextureRGB = EGL_FALSE;
   conf->base.BindToTextureRGBA = EGL_FALSE;
   conf->base.StencilSize = 8;
   conf->base.TransparentType = EGL_NONE;
   conf->base.NativeRenderable = EGL_FALSE;
   conf->base.NativeVisualID = 0;          // No visual
   conf->base.NativeVisualType = EGL_NONE; // No visual
   conf->base.RenderableType = disp->ClientAPIs;
   conf->base.SampleBuffers = 0; // TODO: How to get the right value ?
   conf->base.Samples = conf->base.SampleBuffers == 0 ? 0 : 0;
   conf->base.DepthSize = 24;
   conf->base.Level = 0;
   struct pipe_screen *screen = haiku_egl_display(disp)->disp->fscreen->screen;
   unsigned max_size = screen->caps.max_texture_2d_size;
   conf->base.MaxPbufferWidth = std::min<unsigned>(max_size, _EGL_MAX_PBUFFER_WIDTH);
   conf->base.MaxPbufferHeight = std::min<unsigned>(max_size, _EGL_MAX_PBUFFER_HEIGHT);
   conf->base.MaxPbufferPixels = conf->base.MaxPbufferWidth * conf->base.MaxPbufferHeight;
   conf->base.SurfaceType = EGL_PBUFFER_BIT | EGL_WINDOW_BIT;

   TRACE("Config configuated\n");
   if (!_eglValidateConfig(&conf->base, EGL_FALSE)) {
      _eglLog(_EGL_DEBUG, "Haiku: failed to validate config");
      goto cleanup;
   }
   TRACE("Validated config\n");

   _eglLinkConfig(&conf->base);
   if (!_eglGetArraySize(disp->Configs)) {
      _eglLog(_EGL_WARNING, "Haiku: failed to create any config");
      goto cleanup;
   }
   TRACE("Config successful\n");

   return EGL_TRUE;

cleanup:
   free(conf);
   return EGL_FALSE;
}

static void
haiku_display_destroy(_EGLDisplay *disp)
{
   if (!disp)
      return;

   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);

   assert(hgl_dpy->ref_count > 0);
   if (!p_atomic_dec_zero(&hgl_dpy->ref_count))
      return;

   _eglCleanupDisplay(disp);
   struct pipe_screen *screen = hgl_dpy->disp->fscreen->screen;
   hgl_destroy_display(hgl_dpy->disp);
   hgl_dpy->disp = NULL;
   screen->destroy(screen); // destroy will deallocate object

   free(hgl_dpy);
   disp->DriverData = NULL;
}

static EGLBoolean
haiku_initialize_impl(_EGLDisplay *disp, void *platformDisplay)
{
   struct haiku_egl_display *hgl_dpy;

   hgl_dpy =
      (struct haiku_egl_display *)calloc(1, sizeof(struct haiku_egl_display));
   if (!hgl_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   hgl_dpy->ref_count = 1;
   disp->DriverData = (void *)hgl_dpy;

   struct pipe_screen *screen = NULL;
#ifdef GALLIUM_V3D
   /* Raspberry Pi: the V3D render device whenever its driver published it.
    * HAIKU_V3D_DEVICE names another one; LIBGL_ALWAYS_SOFTWARE skips it. */
   if (!disp->Options.ForceSoftware) {
      const char *v3d_device = getenv("HAIKU_V3D_DEVICE");
      if (v3d_device == NULL || *v3d_device == '\0')
         v3d_device = "/dev/graphics/v3d/0";
      if (access(v3d_device, R_OK | W_OK) == 0)
         screen = v3d_haiku_screen_create(v3d_device);
      if (screen != NULL)
         hgl_dpy->native_csf = true;
   }
#endif
#ifdef GALLIUM_PANFROST
   const char *device = getenv("HAIKU_CSF_DEVICE");
   bool explicit_device = device != NULL && *device != '\0';
   if (!explicit_device && !disp->Options.ForceSoftware
       && access(kHaikuDefaultCsfDevice, R_OK | W_OK) == 0) {
      /* System default: the Mali CSF device whenever the driver published it. */
      device = kHaikuDefaultCsfDevice;
   }
   if (screen == NULL && device != NULL && *device != '\0') {
      hgl_dpy->native_csf = true;
      /* An explicit native device must never become a software fallback. */
      if (!disp->Options.ForceSoftware)
         screen = panfrost_haiku_screen_create(device);
      if (screen == NULL && !explicit_device) {
         /* The default device did not come up: software, as without a device. */
         hgl_dpy->native_csf = false;
         device = NULL;
      }
   }
   if (device == NULL || *device == '\0')
#endif
   if (screen == NULL) {
      struct sw_winsys *winsys = hgl_create_sw_winsys();
      if (winsys) {
         screen = sw_screen_create(winsys);
         if (!screen)
            winsys->destroy(winsys);
      }
   }
   if (!screen) {
      free(hgl_dpy);
      disp->DriverData = NULL;
      return _eglError(EGL_NOT_INITIALIZED, "Haiku screen creation failed");
   }
   hgl_dpy->disp = hgl_create_display(screen);
   if (!hgl_dpy->disp) {
      screen->destroy(screen);
      free(hgl_dpy);
      disp->DriverData = NULL;
      return _eglError(EGL_BAD_ALLOC, "Haiku display creation failed");
   }

   struct st_config_options options = {};
   int gl_core, gl_compat, gles1, gles2;
   st_api_query_versions(hgl_dpy->disp->fscreen, &options,
                        &gl_core, &gl_compat, &gles1, &gles2);
   disp->ClientAPIs = 0;
   if (_eglIsApiValid(EGL_OPENGL_API) && (gl_core || gl_compat))
      disp->ClientAPIs |= EGL_OPENGL_BIT;
   if (_eglIsApiValid(EGL_OPENGL_ES_API)) {
      if (gles1 >= 10)
         disp->ClientAPIs |= EGL_OPENGL_ES_BIT;
      if (gles2 >= 20)
         disp->ClientAPIs |= EGL_OPENGL_ES2_BIT;
      if (gles2 >= 30)
         disp->ClientAPIs |= EGL_OPENGL_ES3_BIT_KHR;
   }

   disp->Extensions.KHR_no_config_context = EGL_TRUE;
   disp->Extensions.KHR_surfaceless_context = EGL_TRUE;

   /* Report back to EGL the bitmask of priorities supported */
   disp->Extensions.IMG_context_priority =
      hgl_dpy->disp->fscreen->screen->caps.context_priority_mask;
   disp->Extensions.NV_context_priority_realtime =
      disp->Extensions.IMG_context_priority &
      (1 << __EGL_CONTEXT_PRIORITY_REALTIME_BIT);

   disp->Extensions.KHR_create_context = EGL_TRUE;
   disp->Extensions.EXT_create_context_robustness =
      hgl_dpy->disp->fscreen->screen->caps.device_reset_status_query &&
      hgl_dpy->disp->fscreen->screen->caps.robust_buffer_access_behavior;
   disp->RobustBufferAccess =
      hgl_dpy->disp->fscreen->screen->caps.robust_buffer_access_behavior;

   if (!disp->ClientAPIs || !haiku_add_configs_for_visuals(disp)) {
      haiku_display_destroy(disp);
      return _eglError(EGL_NOT_INITIALIZED, "Haiku EGL configuration failed");
   }

   return EGL_TRUE;
}

static EGLBoolean
haiku_initialize(_EGLDisplay *disp)
{
   EGLBoolean ret = EGL_FALSE;
   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);

   if (hgl_dpy) {
      p_atomic_inc(&hgl_dpy->ref_count);
      return EGL_TRUE;
   }

   switch (disp->Platform) {
   case _EGL_PLATFORM_SURFACELESS:
   case _EGL_PLATFORM_HAIKU:
      ret = haiku_initialize_impl(disp, NULL);
      break;
   case _EGL_PLATFORM_DEVICE:
      ret = haiku_initialize_impl(disp, disp->PlatformDisplay);
      break;
   default:
      UNREACHABLE("Callers ensure we cannot get here.");
      return EGL_FALSE;
   }

   if (!ret)
      return EGL_FALSE;

   hgl_dpy = haiku_egl_display(disp);

   return EGL_TRUE;
}

static EGLBoolean
haiku_terminate(_EGLDisplay *disp)
{
   /* Bound resources keep their display reference until they are unbound. */
   _eglReleaseDisplayResources(disp);
   haiku_display_destroy(disp);
   return EGL_TRUE;
}

// #pragma mark EGLContext

static _EGLContext *
haiku_create_context(_EGLDisplay *disp, _EGLConfig *conf,
                     _EGLContext *share_list, const EGLint *attrib_list)
{
   CALLED();

   struct haiku_egl_display *hgl_dpy = haiku_egl_display(disp);

   struct st_context_attribs attribs = {};
   enum st_context_error error = ST_CONTEXT_SUCCESS;
   hgl_get_st_visual(&attribs.visual, HGL_DOUBLE | HGL_ALPHA | HGL_DEPTH);

   struct haiku_egl_context *context =
      (struct haiku_egl_context *)calloc(1, sizeof(*context));
   if (!context) {
      _eglError(EGL_BAD_ALLOC, "haiku_create_context");
      return NULL;
   }

   if (!_eglInitContext(&context->base, disp, conf, share_list, attrib_list))
      goto cleanup;

   attribs.major = context->base.ClientMajorVersion;
   attribs.minor = context->base.ClientMinorVersion;
   switch (context->base.ClientAPI) {
   case EGL_OPENGL_ES_API:
      attribs.profile = attribs.major == 1 ? API_OPENGLES : API_OPENGLES2;
      break;
   case EGL_OPENGL_API:
      if (((attribs.major > 3 || (attribs.major == 3 && attribs.minor >= 2)) &&
           context->base.Profile == EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR) ||
          (attribs.major == 3 && attribs.minor == 1))
         attribs.profile = API_OPENGL_CORE;
      else
         attribs.profile = API_OPENGL_COMPAT;
      break;
   default:
      _eglError(EGL_BAD_PARAMETER, "Haiku EGL client API");
      goto cleanup;
   }
   if (context->base.Flags & EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR)
      attribs.flags |= ST_CONTEXT_FLAG_FORWARD_COMPATIBLE;
   if (context->base.Flags & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR)
      attribs.flags |= ST_CONTEXT_FLAG_DEBUG;
   if (context->base.Flags & EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR)
      attribs.context_flags |= PIPE_CONTEXT_ROBUST_BUFFER_ACCESS;
   if (context->base.ResetNotificationStrategy != EGL_NO_RESET_NOTIFICATION)
      attribs.context_flags |= PIPE_CONTEXT_LOSE_CONTEXT_ON_RESET;
   if (context->base.NoError)
      attribs.flags |= ST_CONTEXT_FLAG_NO_ERROR;
   if (context->base.ReleaseBehavior == EGL_CONTEXT_RELEASE_BEHAVIOR_NONE_KHR)
      attribs.flags |= ST_CONTEXT_FLAG_RELEASE_NONE;
   switch (context->base.ContextPriority) {
   case EGL_CONTEXT_PRIORITY_LOW_IMG:
      attribs.context_flags |= PIPE_CONTEXT_LOW_PRIORITY;
      break;
   case EGL_CONTEXT_PRIORITY_HIGH_IMG:
      attribs.context_flags |= PIPE_CONTEXT_HIGH_PRIORITY;
      break;
   case EGL_CONTEXT_PRIORITY_REALTIME_NV:
      attribs.context_flags |= PIPE_CONTEXT_REALTIME_PRIORITY;
      break;
   }

   context->ctx = hgl_create_context_attribs(
      hgl_dpy->disp, &attribs,
      share_list == NULL ? NULL : haiku_egl_context(share_list)->ctx->st, &error);
   if (context->ctx == NULL) {
      _eglError(error == ST_CONTEXT_ERROR_NO_MEMORY ? EGL_BAD_ALLOC : EGL_BAD_MATCH,
                "Haiku EGL context creation failed");
      goto cleanup;
   }

   return &context->base;

cleanup:
   free(context);
   return NULL;
}

static EGLBoolean
haiku_destroy_context(_EGLDisplay *disp, _EGLContext *ctx)
{
   if (_eglPutContext(ctx)) {
      struct haiku_egl_context *hgl_ctx = haiku_egl_context(ctx);
      hgl_destroy_context(hgl_ctx->ctx);
      free(ctx);
      ctx = NULL;
   }
   return EGL_TRUE;
}

static EGLBoolean
haiku_make_current(_EGLDisplay *disp, _EGLSurface *dsurf, _EGLSurface *rsurf,
                   _EGLContext *ctx)
{
   CALLED();

   struct haiku_egl_context *hgl_ctx = haiku_egl_context(ctx);
   struct haiku_egl_surface *hgl_dsurf = haiku_egl_surface(dsurf);
   struct haiku_egl_surface *hgl_rsurf = haiku_egl_surface(rsurf);
   _EGLContext *old_ctx;
   _EGLSurface *old_dsurf, *old_rsurf;

   if (!_eglBindContext(ctx, dsurf, rsurf, &old_ctx, &old_dsurf, &old_rsurf))
      return EGL_FALSE;

   if (old_ctx == ctx && old_dsurf == dsurf && old_rsurf == rsurf) {
      _eglPutSurface(old_dsurf);
      _eglPutSurface(old_rsurf);
      _eglPutContext(old_ctx);
      return EGL_TRUE;
   }

   _EGLDisplay *old_disp = old_ctx ? old_ctx->Resource.Display : NULL;
   if (old_ctx && old_ctx->ReleaseBehavior == EGL_CONTEXT_RELEASE_BEHAVIOR_FLUSH_KHR) {
      struct st_context *st = haiku_egl_context(old_ctx)->ctx->st;
      _mesa_glthread_finish(st->ctx);
      st_context_flush(st, 0, NULL, NULL, NULL);
   }

   if (hgl_dsurf)
      update_size(hgl_dsurf->fb);
   if (hgl_rsurf && hgl_rsurf != hgl_dsurf)
      update_size(hgl_rsurf->fb);

   if (!st_api_make_current(hgl_ctx ? hgl_ctx->ctx->st : NULL,
                            hgl_dsurf ? &hgl_dsurf->fb->base : NULL,
                            hgl_rsurf ? &hgl_rsurf->fb->base : NULL)) {
      _EGLContext *tmp_ctx;
      _EGLSurface *tmp_draw, *tmp_read;
      _eglBindContext(old_ctx, old_dsurf, old_rsurf,
                      &tmp_ctx, &tmp_draw, &tmp_read);
      assert(tmp_ctx == ctx && tmp_draw == dsurf && tmp_read == rsurf);
      _eglPutContext(tmp_ctx);
      _eglPutSurface(tmp_draw);
      _eglPutSurface(tmp_read);

      bool restored = st_api_make_current(
         old_ctx ? haiku_egl_context(old_ctx)->ctx->st : NULL,
         old_dsurf ? &haiku_egl_surface(old_dsurf)->fb->base : NULL,
         old_rsurf ? &haiku_egl_surface(old_rsurf)->fb->base : NULL);
      _eglPutContext(old_ctx);
      _eglPutSurface(old_dsurf);
      _eglPutSurface(old_rsurf);
      if (restored)
         return _eglError(EGL_BAD_MATCH, "Haiku EGL framebuffer binding failed");

      /* If restoration also fails, leave both EGL and GL unbound. */
      _eglBindContext(NULL, NULL, NULL, &old_ctx, &old_dsurf, &old_rsurf);
      st_api_make_current(NULL, NULL, NULL);
      haiku_destroy_surface(old_disp, old_dsurf);
      haiku_destroy_surface(old_disp, old_rsurf);
      haiku_destroy_context(old_disp, old_ctx);
      if (old_disp)
         haiku_display_destroy(old_disp);
      return _eglError(EGL_BAD_MATCH, "Haiku EGL context restoration failed");
   }

   if (ctx)
      p_atomic_inc(&haiku_egl_display(disp)->ref_count);
   if (old_dsurf != NULL)
      haiku_destroy_surface(old_disp, old_dsurf);
   if (old_rsurf != NULL)
      haiku_destroy_surface(old_disp, old_rsurf);
   if (old_ctx != NULL) {
      haiku_destroy_context(old_disp, old_ctx);
      haiku_display_destroy(old_disp);
   }

   return EGL_TRUE;
}

extern "C" const _EGLDriver _eglDriver = {
   .Initialize = haiku_initialize,
   .Terminate = haiku_terminate,
   .CreateContext = haiku_create_context,
   .DestroyContext = haiku_destroy_context,
   .MakeCurrent = haiku_make_current,
   .CreateWindowSurface = haiku_create_window_surface,
   .CreatePixmapSurface = haiku_create_pixmap_surface,
   .CreatePbufferSurface = haiku_create_pbuffer_surface,
   .DestroySurface = haiku_destroy_surface,
   .QuerySurface = haiku_query_surface,
   .SwapBuffers = haiku_swap_buffers,
};
