/*
 * Presenting a finished frame straight into the screen on Haiku with a
 * Gallium driver on amdgpu_drm; see haiku_drm_present.h.
 *
 * Without it a frame of Summit's on radeonsi (the Radeon Pro WX 5100, which
 * has no Vulkan driver for zink) is read back to the processor and handed
 * to app_server, which copies it into the screen twice more. Here the GPU
 * blits it into the frame buffer and the processor waits for nothing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "haiku_drm_present.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <drm-uapi/drm_fourcc.h>
#include <xf86drm.h>

#include "frontend/winsys_handle.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/format/u_format.h"
#include "util/macros.h"
#include "util/os_time.h"
#include "util/box.h"
#include "util/u_inlines.h"

#define MAX_QUEUED 16

/* amdgpu_drm's DRM_HAIKU_IOCTL_SCANOUT (libdrm's drm_haiku.h): the
 * accelerant publishes the frame buffer the screen scans out from, and op 2
 * gives a handle to it on this file. */
struct haiku_scanout {
   uint32_t op;
   uint32_t handle;
   uint32_t width;
   uint32_t height;
   uint32_t pitch;
   uint32_t format;
   uint32_t generation;
   uint32_t reserved;
   uint64_t size;
};

#define HAIKU_SCANOUT_GET 2
#define HAIKU_IOCTL_SCANOUT DRM_IOWR(0xf8, struct haiku_scanout)

/* A present the helper thread waits for. */
struct completion {
   struct pipe_fence_handle *fence;
   void (*completed)(void *cookie);
   void *cookie;
};

static struct {
   pthread_mutex_t lock;
   pthread_cond_t changed;
   struct pipe_screen *screen;

   /* The frame buffer the card scans out, as a linear texture. */
   struct pipe_resource *scanout;
   uint32_t scanout_pitch;
   int64_t scanout_refused_at;   /* os_time_get_nano() of the last refusal */

   struct completion queue[MAX_QUEUED];
   unsigned queue_head, queue_count;
   uint64_t submitted, finished;
   bool thread_started;
   pthread_t thread;
} state = {
   .lock = PTHREAD_MUTEX_INITIALIZER,
   .changed = PTHREAD_COND_INITIALIZER,
};


static void *
completion_thread(void *data)
{
   (void)data;
   pthread_mutex_lock(&state.lock);
   for (;;) {
      while (state.queue_count == 0)
         pthread_cond_wait(&state.changed, &state.lock);
      struct completion entry = state.queue[state.queue_head];
      struct pipe_screen *screen = state.screen;
      pthread_mutex_unlock(&state.lock);

      if (entry.fence != NULL) {
         screen->fence_finish(screen, NULL, entry.fence, OS_TIMEOUT_INFINITE);
         screen->fence_reference(screen, &entry.fence, NULL);
      }
      if (entry.completed != NULL)
         entry.completed(entry.cookie);

      pthread_mutex_lock(&state.lock);
      state.queue_head = (state.queue_head + 1) % MAX_QUEUED;
      state.queue_count--;
      state.finished++;
      pthread_cond_broadcast(&state.changed);
   }
   return NULL;
}


/* With the lock held: waits until the GPU has finished every present. */
static void
wait_idle_locked(void)
{
   while (state.submitted != state.finished)
      pthread_cond_wait(&state.changed, &state.lock);
}


static void
drop_scanout(void)
{
   wait_idle_locked();
   pipe_resource_reference(&state.scanout, NULL);
   state.scanout_pitch = 0;
}


/* Takes hold of the frame buffer the accelerant publishes. A mode change
 * gives the screen a new one; its row length tells them apart. */
static bool
acquire_scanout(struct pipe_screen *screen, uint32_t bytes_per_row)
{
   if (state.scanout != NULL) {
      if (state.scanout_pitch == bytes_per_row)
         return true;
      drop_scanout();
   }
   /* Asking costs a driver round trip; after a refusal, ask again later. */
   int64_t now = os_time_get_nano();
   if (state.scanout_refused_at != 0
       && now - state.scanout_refused_at < 1000000000LL)
      return false;
   state.scanout_refused_at = now;

   int fd = screen->get_screen_fd != NULL ? screen->get_screen_fd(screen) : -1;
   if (fd < 0)
      return false;
   struct haiku_scanout scanout;
   memset(&scanout, 0, sizeof(scanout));
   scanout.op = HAIKU_SCANOUT_GET;
   if (drmIoctl(fd, HAIKU_IOCTL_SCANOUT, &scanout) != 0)
      return false;

   /* libdrm takes the handle over, also when the import fails */
   struct winsys_handle handle;
   memset(&handle, 0, sizeof(handle));
   handle.type = WINSYS_HANDLE_TYPE_KMS;
   handle.handle = scanout.handle;
   handle.stride = scanout.pitch;
   handle.format = PIPE_FORMAT_B8G8R8X8_UNORM;
   handle.modifier = DRM_FORMAT_MOD_INVALID;
   struct pipe_resource templ;
   memset(&templ, 0, sizeof(templ));
   templ.target = PIPE_TEXTURE_2D;
   templ.format = PIPE_FORMAT_B8G8R8X8_UNORM;
   templ.width0 = scanout.width;
   templ.height0 = scanout.height;
   templ.depth0 = 1;
   templ.array_size = 1;
   templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT
      | PIPE_BIND_SHARED | PIPE_BIND_LINEAR;
   struct pipe_resource *texture = screen->resource_from_handle(screen,
      &templ, &handle, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
   if (texture == NULL)
      return false;
   if (scanout.pitch != bytes_per_row) {
      /* Not the frame buffer the window system is showing. */
      pipe_resource_reference(&texture, NULL);
      return false;
   }
   state.scanout = texture;
   state.scanout_pitch = scanout.pitch;
   state.scanout_refused_at = 0;
   return true;
}


static void release_at_exit(void);


int
haiku_drm_present(struct pipe_context *pipe, struct pipe_resource *image,
                  const struct summit_haiku_present *p)
{
   if (pipe == NULL || image == NULL || p == NULL
       || p->version != SUMMIT_HAIKU_PRESENT_VERSION
       || (p->rect_count > 0 && p->rects == NULL)
       || image->target != PIPE_TEXTURE_2D)
      return -1;
   if (p->back_bits != NULL)
      return -4;

   struct pipe_screen *screen = image->screen;
   pthread_mutex_lock(&state.lock);
   if (state.screen != NULL && state.screen != screen) {
      pthread_mutex_unlock(&state.lock);
      return -1;
   }
   state.screen = screen;
   if (!state.thread_started) {
      /* Normally the display is destroyed, or the program lets go, first. */
      atexit(release_at_exit);
      if (pthread_create(&state.thread, NULL, completion_thread, NULL) != 0) {
         pthread_mutex_unlock(&state.lock);
         return -1;
      }
      state.thread_started = true;
   }
   /* Keep the GPU from falling more than a few frames behind. */
   unsigned in_flight = p->frames_in_flight ? p->frames_in_flight : 2;
   while (state.submitted - state.finished >= in_flight
          || state.queue_count == MAX_QUEUED)
      pthread_cond_wait(&state.changed, &state.lock);

   if (p->front_bytes_per_row == 0
       || !acquire_scanout(screen, p->front_bytes_per_row)) {
      pthread_mutex_unlock(&state.lock);
      return p->front_bytes_per_row == 0 ? -1 : -3;
   }

   /* The pixels as they are: no sRGB conversion on the way. */
   struct pipe_blit_info blit;
   memset(&blit, 0, sizeof(blit));
   blit.src.resource = image;
   blit.src.format = util_format_linear(image->format);
   blit.dst.resource = state.scanout;
   blit.dst.format = state.scanout->format;
   blit.mask = PIPE_MASK_RGBA;
   blit.filter = PIPE_TEX_FILTER_NEAREST;

   const int32_t width = state.scanout->width0;
   const int32_t height = state.scanout->height0;
   unsigned copies = 0;
   for (uint32_t i = 0; i < p->rect_count; i++) {
      const int32_t *r = p->rects + i * 4;
      int32_t left = MAX2(r[0], p->origin_x);
      int32_t top = MAX2(r[1], p->origin_y);
      int32_t right = MIN2(r[2] + 1, p->origin_x + (int32_t)image->width0);
      int32_t bottom = MIN2(r[3] + 1, p->origin_y + (int32_t)image->height0);
      left = MAX2(left, 0);
      top = MAX2(top, 0);
      right = MIN2(right, width);
      bottom = MIN2(bottom, height);
      if (right <= left || bottom <= top)
         continue;
      u_box_2d(left - p->origin_x, top - p->origin_y, right - left,
               bottom - top, &blit.src.box);
      u_box_2d(left, top, right - left, bottom - top, &blit.dst.box);
      pipe->blit(pipe, &blit);
      copies++;
   }
   if (copies == 0) {
      pthread_mutex_unlock(&state.lock);
      if (p->rect_count == 0 && p->completed != NULL)
         p->completed(p->cookie);
      return p->rect_count == 0 ? 0 : -5;
   }

   /* Not deferred: the fence is the driver's own, which the helper thread
    * may wait for. */
   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);

   unsigned tail = (state.queue_head + state.queue_count) % MAX_QUEUED;
   state.queue[tail] = (struct completion) {
      .fence = fence,
      .completed = p->completed,
      .cookie = p->cookie,
   };
   state.queue_count++;
   state.submitted++;
   pthread_cond_broadcast(&state.changed);
   pthread_mutex_unlock(&state.lock);
   return 0;
}


void
haiku_drm_present_release(struct pipe_screen *screen)
{
   pthread_mutex_lock(&state.lock);
   if (state.screen != NULL && (screen == NULL || state.screen == screen)) {
      drop_scanout();
      if (screen != NULL)
         state.screen = NULL;
   }
   pthread_mutex_unlock(&state.lock);
}


static void
release_at_exit(void)
{
   haiku_drm_present_release(NULL);
}


void
haiku_drm_present_wait_idle(void)
{
   pthread_mutex_lock(&state.lock);
   wait_idle_locked();
   pthread_mutex_unlock(&state.lock);
}
