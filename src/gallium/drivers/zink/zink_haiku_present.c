/*
 * Presenting a finished frame straight into the screen on Haiku; see
 * zink_haiku_present.h.
 *
 * At 200 percent a browser frame is 7 megapixels. Read back to the processor
 * it costs a wait for the GPU and 28 MB over the bus, and app_server then
 * copies it twice more, into its own copy of the screen and from there into
 * the frame buffer. Here the GPU copies the frame into the frame buffer from
 * video memory (about 8 GB/s, nothing crosses the bus) and into the window
 * system's copy with a DMA the processor never waits for.
 *
 * SPDX-License-Identifier: MIT
 */

#include "zink_haiku_present.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "zink_batch.h"
#include "zink_context.h"
#include "zink_fence.h"
#include "zink_resource.h"
#include "zink_screen.h"
#include "zink_types.h"

#include "util/format/u_format.h"
#include "util/os_time.h"
#include "util/u_threaded_context.h"

#include "vk_haiku_scanout.h"

#define MAX_RECTS 256
#define MAX_QUEUED 16

/* A present the helper thread waits for: by the number of its batch on the
 * screen's timeline semaphore. The helper never touches Zink's fences or
 * batch states: those are recycled by the context's own thread, and a wait
 * through them from another thread marks a reused batch idle, after which
 * Zink hands out memory the GPU is still using. */
struct completion {
   uint64_t batch_id;
   void (*completed)(void *cookie);
   void *cookie;
};

static struct {
   pthread_mutex_t lock;
   pthread_cond_t changed;
   struct zink_screen *screen;

   /* The frame buffer the card scans out, in video memory. */
   VkDeviceMemory scanout_memory;
   VkBuffer scanout_buffer;
   uint32_t scanout_width, scanout_height, scanout_row_pitch;
   uint64_t scanout_size;
   int64_t scanout_refused_at;   /* os_time_get_nano() of the last refusal */

   /* The window system's copy of the screen, imported host memory. */
   void *back_bits;
   uint64_t back_size;
   VkDeviceMemory back_memory;
   VkBuffer back_buffer;

   /* Presents whose copies the helper thread waits for. */
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
      struct zink_screen *screen = state.screen;
      pthread_mutex_unlock(&state.lock);

      if (entry.batch_id != 0 && screen->sem != VK_NULL_HANDLE
          && !screen->device_lost) {
         VkSemaphoreWaitInfo wait = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &screen->sem,
            .pValues = &entry.batch_id,
         };
         VKSCR(WaitSemaphores)(screen->dev, &wait, UINT64_MAX);
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


/* With the lock held: waits until the GPU has finished every present, so
 * that memory one of them still writes is never let go of. Freeing an import
 * of host memory unlocks its pages, and a write after that lands in memory
 * that may already belong to someone else. */
static void
wait_idle_locked(void)
{
   while (state.submitted != state.finished)
      pthread_cond_wait(&state.changed, &state.lock);
}


static void
drop_scanout(struct zink_screen *screen)
{
   wait_idle_locked();
   if (state.scanout_buffer != VK_NULL_HANDLE)
      VKSCR(DestroyBuffer)(screen->dev, state.scanout_buffer, NULL);
   if (state.scanout_memory != VK_NULL_HANDLE)
      VKSCR(FreeMemory)(screen->dev, state.scanout_memory, NULL);
   state.scanout_buffer = VK_NULL_HANDLE;
   state.scanout_memory = VK_NULL_HANDLE;
   state.scanout_row_pitch = 0;
}


/* Takes hold of the frame buffer the accelerant publishes. A mode change
 * gives the screen a new one; its row length tells them apart. */
static bool
acquire_scanout(struct zink_screen *screen, uint32_t bytes_per_row)
{
   if (state.scanout_buffer != VK_NULL_HANDLE) {
      if (state.scanout_row_pitch == bytes_per_row)
         return true;
      drop_scanout(screen);
   }
   /* Asking costs a driver round trip; after a refusal, ask again later. */
   int64_t now = os_time_get_nano();
   if (state.scanout_refused_at != 0
       && now - state.scanout_refused_at < 1000000000LL)
      return false;

   uint32_t type = UINT32_MAX;
   for (uint32_t i = 0; i < screen->info.mem_props.memoryTypeCount; i++) {
      if (screen->info.mem_props.memoryTypes[i].propertyFlags
          & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
         type = i;
         break;
      }
   }
   VkImportScanoutMemoryHAIKU scanout = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_SCANOUT_MEMORY_HAIKU,
   };
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &scanout,
      .allocationSize = 0,
      .memoryTypeIndex = type,
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   if (type == UINT32_MAX
       || VKSCR(AllocateMemory)(screen->dev, &mai, NULL, &memory) != VK_SUCCESS) {
      state.scanout_refused_at = now;
      return false;
   }
   if (scanout.rowPitch != bytes_per_row) {
      /* Not the frame buffer the window system is showing. */
      VKSCR(FreeMemory)(screen->dev, memory, NULL);
      state.scanout_refused_at = now;
      return false;
   }
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = scanout.size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer buffer = VK_NULL_HANDLE;
   if (VKSCR(CreateBuffer)(screen->dev, &bci, NULL, &buffer) != VK_SUCCESS
       || VKSCR(BindBufferMemory)(screen->dev, buffer, memory, 0) != VK_SUCCESS) {
      if (buffer != VK_NULL_HANDLE)
         VKSCR(DestroyBuffer)(screen->dev, buffer, NULL);
      VKSCR(FreeMemory)(screen->dev, memory, NULL);
      state.scanout_refused_at = now;
      return false;
   }
   state.scanout_memory = memory;
   state.scanout_buffer = buffer;
   state.scanout_width = scanout.width;
   state.scanout_height = scanout.height;
   state.scanout_row_pitch = scanout.rowPitch;
   state.scanout_size = scanout.size;
   state.scanout_refused_at = 0;
   return true;
}


static void
drop_back(struct zink_screen *screen)
{
   wait_idle_locked();
   if (state.back_buffer != VK_NULL_HANDLE)
      VKSCR(DestroyBuffer)(screen->dev, state.back_buffer, NULL);
   if (state.back_memory != VK_NULL_HANDLE)
      VKSCR(FreeMemory)(screen->dev, state.back_memory, NULL);
   state.back_buffer = VK_NULL_HANDLE;
   state.back_memory = VK_NULL_HANDLE;
   state.back_bits = NULL;
   state.back_size = 0;
}


/* The window system's copy of the screen, as memory the GPU may write: the
 * driver locks its pages for as long as the import lives, so the caller must
 * keep that memory mapped for as long as it presents with it (Summit's web
 * process keeps one mapping for its whole life). */
static bool
acquire_back(struct zink_screen *screen, void *bits, uint64_t size)
{
   if (state.back_bits == bits && state.back_size >= size)
      return true;
   drop_back(screen);

   if (!screen->info.have_EXT_external_memory_host)
      return false;
   const uint64_t alignment =
      screen->info.ext_host_mem_props.minImportedHostPointerAlignment;
   if (alignment == 0 || ((uintptr_t)bits & (alignment - 1)) != 0)
      return false;
   size = align64(size, alignment);

   VkMemoryHostPointerPropertiesEXT props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
   };
   if (VKSCR(GetMemoryHostPointerPropertiesEXT)(screen->dev,
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, bits,
          &props) != VK_SUCCESS || props.memoryTypeBits == 0)
      return false;
   VkImportMemoryHostPointerInfoEXT import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
      .pHostPointer = bits,
   };
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import,
      .allocationSize = size,
      .memoryTypeIndex = ffs(props.memoryTypeBits) - 1,
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   if (VKSCR(AllocateMemory)(screen->dev, &mai, NULL, &memory) != VK_SUCCESS)
      return false;
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer buffer = VK_NULL_HANDLE;
   if (VKSCR(CreateBuffer)(screen->dev, &bci, NULL, &buffer) != VK_SUCCESS
       || VKSCR(BindBufferMemory)(screen->dev, buffer, memory, 0) != VK_SUCCESS) {
      if (buffer != VK_NULL_HANDLE)
         VKSCR(DestroyBuffer)(screen->dev, buffer, NULL);
      VKSCR(FreeMemory)(screen->dev, memory, NULL);
      return false;
   }
   state.back_bits = bits;
   state.back_size = size;
   state.back_memory = memory;
   state.back_buffer = buffer;
   return true;
}


static void release_at_exit(void);


/* Turns the rectangles into copy regions for a linear surface of
 * width x height pixels with the given row length. */
static unsigned
make_regions(VkBufferImageCopy *regions, const struct summit_haiku_present *p,
             struct zink_resource *res, unsigned image_width,
             unsigned image_height, uint32_t width, uint32_t height,
             uint32_t row_pitch)
{
   unsigned count = 0;
   for (uint32_t i = 0; i < p->rect_count && count < MAX_RECTS; i++) {
      const int32_t *r = p->rects + i * 4;
      int32_t left = MAX2(r[0], p->origin_x);
      int32_t top = MAX2(r[1], p->origin_y);
      int32_t right = MIN2(r[2] + 1, p->origin_x + (int32_t)image_width);
      int32_t bottom = MIN2(r[3] + 1, p->origin_y + (int32_t)image_height);
      left = MAX2(left, 0);
      top = MAX2(top, 0);
      right = MIN2(right, (int32_t)width);
      bottom = MIN2(bottom, (int32_t)height);
      if (right <= left || bottom <= top)
         continue;
      regions[count++] = (VkBufferImageCopy) {
         .bufferOffset = (uint64_t)top * row_pitch + (uint64_t)left * 4,
         .bufferRowLength = row_pitch / 4,
         .bufferImageHeight = bottom - top,
         .imageSubresource = {
            .aspectMask = res->aspect,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
         .imageOffset = { left - p->origin_x, top - p->origin_y, 0 },
         .imageExtent = { right - left, bottom - top, 1 },
      };
   }
   return count;
}


int
zink_haiku_present(struct pipe_context *pipe, struct pipe_resource *image,
                   const struct summit_haiku_present *p)
{
   if (pipe == NULL || image == NULL || p == NULL
       || p->version != SUMMIT_HAIKU_PRESENT_VERSION
       || (p->rect_count > 0 && p->rects == NULL))
      return -1;
   /* The screen holds 32 bit pixels in the order B, G, R, A; copying is
    * byte for byte, so the image must be in that order already. */
   if (image->format != PIPE_FORMAT_B8G8R8A8_UNORM
       && image->format != PIPE_FORMAT_B8G8R8X8_UNORM)
      return -2;

   struct zink_context *ctx = zink_context(threaded_context_unwrap_sync(pipe));
   struct zink_screen *screen = zink_screen(image->screen);
   struct zink_resource *res = zink_resource(image);
   if (res->obj->is_buffer)
      return -1;

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

   const unsigned image_width = image->width0;
   const unsigned image_height = image->height0;
   static VkBufferImageCopy front_regions[MAX_RECTS], back_regions[MAX_RECTS];
   unsigned front_count = 0, back_count = 0;
   int result = 0;

   if (p->front_bytes_per_row != 0) {
      if (acquire_scanout(screen, p->front_bytes_per_row)) {
         front_count = make_regions(front_regions, p, res, image_width,
                                    image_height, state.scanout_width,
                                    state.scanout_height,
                                    state.scanout_row_pitch);
      } else
         result = -3;
   }
   if (result == 0 && p->back_bits != NULL) {
      uint64_t size = (uint64_t)p->back_bytes_per_row * p->back_height;
      if (p->back_bytes_per_row < p->back_width * 4
          || !acquire_back(screen, p->back_bits, size))
         result = -4;
      else {
         back_count = make_regions(back_regions, p, res, image_width,
                                   image_height, p->back_width,
                                   p->back_height, p->back_bytes_per_row);
      }
   }
   if (result == 0 && front_count == 0 && back_count == 0)
      result = p->rect_count == 0 ? 0 : -5;
   if (result != 0 || (front_count == 0 && back_count == 0)) {
      pthread_mutex_unlock(&state.lock);
      if (result == 0 && p->completed != NULL)
         p->completed(p->cookie);
      return result;
   }

   zink_batch_no_rp(ctx);
   screen->image_barrier(ctx, res, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
   zink_batch_reference_resource_rw(ctx, res, false);
   VkCommandBuffer cmdbuf = ctx->bs->cmdbuf;

   /* The previous present wrote the same memory: order the writes. */
   VkMemoryBarrier before = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
   };
   VKCTX(CmdPipelineBarrier)(cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before,
                             0, NULL, 0, NULL);
   if (front_count != 0)
      VKCTX(CmdCopyImageToBuffer)(cmdbuf, res->obj->image, res->layout,
                                  state.scanout_buffer, front_count,
                                  front_regions);
   if (back_count != 0)
      VKCTX(CmdCopyImageToBuffer)(cmdbuf, res->obj->image, res->layout,
                                  state.back_buffer, back_count, back_regions);
   /* app_server reads its copy with the processor. */
   VkMemoryBarrier after = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT,
   };
   VKCTX(CmdPipelineBarrier)(cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &after,
                             0, NULL, 0, NULL);
   ctx->bs->has_work = true;

   struct pipe_fence_handle *fence = NULL;
   ctx->base.flush(&ctx->base, &fence, 0);
   /* The batch's number on the timeline, once it has been submitted (on the
    * flush thread when submission is threaded); then the fence goes, here,
    * on the context's own thread. */
   uint64_t batch_id = 0;
   struct zink_tc_fence *mfence = zink_tc_fence(fence);
   if (mfence != NULL && mfence->fence != NULL) {
      struct zink_batch_state *bs = zink_batch_state(mfence->fence);
      if (screen->threaded_submit)
         util_queue_fence_wait(&bs->flush_completed);
      batch_id = mfence->fence->batch_id;
   }
   screen->base.fence_reference(&screen->base, &fence, NULL);

   unsigned tail = (state.queue_head + state.queue_count) % MAX_QUEUED;
   state.queue[tail] = (struct completion) {
      .batch_id = batch_id,
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
zink_haiku_present_release(struct pipe_screen *pscreen)
{
   pthread_mutex_lock(&state.lock);
   struct zink_screen *screen = state.screen;
   if (screen != NULL && (pscreen == NULL || &screen->base == pscreen)) {
      /* Waits for the GPU first: a copy still writing the window system's
       * memory when the driver lets go of its pages has hung the machine. */
      drop_back(screen);
      drop_scanout(screen);
      if (pscreen != NULL)
         state.screen = NULL;
   }
   pthread_mutex_unlock(&state.lock);
}


static void
release_at_exit(void)
{
   zink_haiku_present_release(NULL);
}


void
zink_haiku_present_wait_idle(void)
{
   pthread_mutex_lock(&state.lock);
   while (state.submitted != state.finished)
      pthread_cond_wait(&state.changed, &state.lock);
   pthread_mutex_unlock(&state.lock);
}
