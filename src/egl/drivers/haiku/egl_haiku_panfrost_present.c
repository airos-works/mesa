/*
 * Copyright 2026, air/OS. SPDX-License-Identifier: MIT
 *
 * Presenting a finished frame straight into the screen with Panfrost on the
 * ROCK 5's Mali-G610 (air/OS haiku#71), for Summit's web process: the same
 * summit_haiku_present_framebuffer() as Zink's and the Raspberry Pi's V3D
 * (egl_haiku_v3d_present.c on mesa-rpi4), where it otherwise reads every
 * composited frame back (an AFBC render target: a staging blit, a flush, a
 * wait and a CPU read of each frame).
 *
 * The frame buffer the VOP2 scans out is rk3588_display's, cloned into this
 * team (kCloneFrameBuffer) and imported into the GPU VM through mali_csf's
 * kImportBuffer; app_server's copy of the screen is the window's
 * drawing_bits_area as Summit mapped it, imported the same way. One compute
 * dispatch copies each visible rectangle of the framebuffer object's colour
 * attachment into them, linear, pixel by pixel as B, G, R, A: outside the
 * rectangles the frame buffer holds other windows, and a render target's
 * tile write-back may not spill there. app_server's copy is CPU-cached
 * memory the GPU does not snoop: its lines are cleaned and invalidated (dc
 * civac, which user code may run on Haiku arm64) before the dispatch and
 * again once a helper thread has seen its fence signalled; the helper then
 * reports the present complete. The frame buffer is write-combined.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Drivers.h>
#include <OS.h>

#include "cso_cache/cso_context.h"
#include "compiler/nir/nir_builder.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/format/u_format.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_sampler.h"
#include "main/mtypes.h"
#include "state_tracker/st_atom.h"
#include "state_tracker/st_context.h"

#include "panfrost/pan_public.h"

#include "egl_haiku_panfrost_present.h"

#define MAX_RECTS 256
#define MAX_COPIES (2 * MAX_RECTS)
#define MAX_QUEUED 16
#define BLOCK_WIDTH 16
#define BLOCK_HEIGHT 4
#define PIXELS_PER_INVOCATION 4
#define GROUP_PIXELS (BLOCK_WIDTH * PIXELS_PER_INVOCATION)
#define MAX_GRID_X 32768

/* rk3588_display's accelerant interface (DisplayAccelerant.h in the air/OS
 * tree), as far as this needs it. Any handle may ask both. */
#define RK3588_DISPLAY_PATH "/dev/graphics/rk3588_display/0"
#define RK3588_GET_ACCELERANT_INFO 0x52444905
#define RK3588_CLONE_FRAME_BUFFER 0x52444906
#define RK3588_ACCELERANT_VERSION 1
#define RK3588_ACCELERANT_ACQUIRED 1

struct rk3588_accelerant_info {
   uint32_t version, flags;
   int32_t shared_area;
   uint32_t frame_buffer_physical, firmware_address, port, window, polls;
   uint32_t width, height, bytes_per_row;
   int32_t retrace_semaphore;
   uint32_t retraces, interrupt_calls, interrupt_spurious;
   int64_t first_retrace_micros, last_retrace_micros;
};

_Static_assert(sizeof(struct rk3588_accelerant_info) == 80,
               "AccelerantInfo as rk3588_display has it");

/* The dispatch's parameters (constant buffer 0): a header, then the copies
 * in the order of their first workgroups. Each workgroup copies
 * GROUP_PIXELS x BLOCK_HEIGHT pixels of one copy. */
struct copy_header {
   uint32_t count, grid_x, total_groups, pad;
};

struct copy_entry {
   uint32_t src_x, src_y, width, height;
   uint32_t offset, stride, target, groups_x;  /* target 0: front, 1: back */
   uint32_t first_group, pad[3];
};

#define HEADER_BYTES ((int)sizeof(struct copy_header))
#define ENTRY_BYTES ((int)sizeof(struct copy_entry))

_Static_assert(sizeof(struct copy_header) == 16, "vec4 header");
_Static_assert(sizeof(struct copy_entry) == 48, "three vec4 a copy");

struct params {
   struct copy_header header;
   struct copy_entry copies[MAX_COPIES];
};

/* Bytes of app_server's copy: rows rows of bytes bytes, stride apart. */
struct byte_range {
   uint32_t offset, bytes, rows, stride;
};

struct completion {
   struct pipe_fence_handle *fence;
   bigtime_t submitted;
   void (*completed)(void *cookie);
   void *cookie;
   uint8_t *back_bits;  /* NULL: nothing to maintain */
   unsigned back_count;
   struct byte_range back_ranges[MAX_RECTS];
};

static struct {
   pthread_mutex_t lock;
   pthread_cond_t changed;
   struct pipe_screen *screen;

   /* The compute shader, for the context it was made with. */
   struct pipe_context *shader_pipe;
   void *shader;

   /* rk3588_display's frame buffer and this team's clone of it. mali_csf
    * holds a contiguous area's pages by a reference to its memory, not by
    * wiring, so the display driver may replace the buffer meanwhile: the GPU
    * then writes into the old one until the next present notices. */
   int display;
   area_id front_clone;
   struct pipe_resource *front;
   uint32_t front_width, front_height, front_bytes_per_row, front_physical;
   int64_t front_refused_at;

   /* app_server's copy of the screen, as Summit mapped it: wired in this
    * team while imported, so Summit keeps its mapping. */
   void *back_bits;
   uint64_t back_size;
   struct pipe_resource *back;
   bool back_cached;

   struct params params;
   struct completion queue[MAX_QUEUED];
   unsigned queue_head, queue_count;
   uint64_t submitted, finished;
   bool thread_started;
   pthread_t thread;
} state = {
   .lock = PTHREAD_MUTEX_INITIALIZER,
   .changed = PTHREAD_COND_INITIALIZER,
   .display = -1,
   .front_clone = -1,
};


/* HAIKU_PANFROST_PRESENT_TRACE=1: where each present's time goes, every 10. */
static bool
trace_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *value = getenv("HAIKU_PANFROST_PRESENT_TRACE");
      enabled = value && strcmp(value, "0") != 0;
   }
   return enabled;
}

static struct {
   unsigned count;
   bigtime_t before, dispatch, gpu, after;
} trace;

static void
trace_add(bigtime_t *field, bigtime_t time)
{
   if (trace_enabled())
      p_atomic_add(field, time);
}


/* Cleans and invalidates the CPU's lines of the ranges: before the GPU
 * writes them (no dirty line may be written back over its results later),
 * and after, before the CPU reads them (a line the CPU fetched meanwhile,
 * speculatively even, is stale). */
static void
sync_cpu_cache(uint8_t *base, const struct byte_range *ranges, unsigned count)
{
#if defined(__aarch64__)
   uint64_t ctr;
   __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
   const uintptr_t line = 4u << ((ctr >> 16) & 0xf);
   __asm__ volatile("dsb sy" ::: "memory");
   for (unsigned i = 0; i < count; i++) {
      const struct byte_range *range = &ranges[i];
      for (uint32_t row = 0; row < range->rows; row++) {
         uintptr_t start = (uintptr_t)base + range->offset
            + (uintptr_t)row * range->stride;
         uintptr_t end = start + range->bytes;
         for (uintptr_t p = start & ~(line - 1); p < end; p += line)
            __asm__ volatile("dc civac, %0" ::"r"(p) : "memory");
      }
   }
   __asm__ volatile("dsb sy" ::: "memory");
#else
   (void)base; (void)ranges; (void)count;
#endif
}


static void *
completion_thread(void *data)
{
   (void)data;
   pthread_mutex_lock(&state.lock);
   for (;;) {
      while (state.queue_count == 0)
         pthread_cond_wait(&state.changed, &state.lock);
      struct completion *entry = &state.queue[state.queue_head];
      struct pipe_screen *screen = state.screen;
      pthread_mutex_unlock(&state.lock);

      if (entry->fence != NULL) {
         screen->fence_finish(screen, NULL, entry->fence, OS_TIMEOUT_INFINITE);
         screen->fence_reference(screen, &entry->fence, NULL);
      }
      bigtime_t done = system_time();
      trace_add(&trace.gpu, done - entry->submitted);
      /* What the CPU may hold of the rectangles the GPU just wrote goes;
       * later presents may already be writing elsewhere. */
      if (entry->back_bits != NULL)
         sync_cpu_cache(entry->back_bits, entry->back_ranges, entry->back_count);
      trace_add(&trace.after, system_time() - done);
      if (trace_enabled() && p_atomic_inc_return(&trace.count) % 10 == 0) {
         fprintf(stderr, "haiku panfrost present: per present %.2f ms cache before, "
                 "%.2f ms dispatch, %.2f ms submit to done, %.2f ms cache after\n",
                 trace.before / 10000.0, trace.dispatch / 10000.0,
                 trace.gpu / 10000.0, trace.after / 10000.0);
         trace.before = trace.dispatch = trace.gpu = trace.after = 0;
      }
      if (entry->completed != NULL)
         entry->completed(entry->cookie);

      pthread_mutex_lock(&state.lock);
      state.queue_head = (state.queue_head + 1) % MAX_QUEUED;
      state.queue_count--;
      state.finished++;
      pthread_cond_broadcast(&state.changed);
   }
   return NULL;
}


/* With the lock held: until the GPU has finished every present, so that
 * nothing still writes memory that is let go of. */
static void
wait_idle_locked(void)
{
   while (state.submitted != state.finished)
      pthread_cond_wait(&state.changed, &state.lock);
}


static void
drop_front(void)
{
   wait_idle_locked();
   /* The driver lets go of the pages once no GPU work maps them; the clone
    * is not wired, so it may go at once. */
   pipe_resource_reference(&state.front, NULL);
   if (state.front_clone >= 0)
      delete_area(state.front_clone);
   state.front_clone = -1;
   state.front_bytes_per_row = 0;
}


static void
drop_back(void)
{
   wait_idle_locked();
   pipe_resource_reference(&state.back, NULL);
   state.back_bits = NULL;
   state.back_size = 0;
}


static bool
get_display_info(struct rk3588_accelerant_info *info)
{
   memset(info, 0, sizeof(*info));
   info->version = RK3588_ACCELERANT_VERSION;
   return ioctl(state.display, RK3588_GET_ACCELERANT_INFO, info, sizeof(*info)) == 0
      && (info->flags & RK3588_ACCELERANT_ACQUIRED) != 0;
}


/* The frame buffer rk3588_display scans out, as a GPU buffer; a mode
 * change or a new desktop layout replaces it (its address tells). */
static bool
acquire_front(uint32_t bytes_per_row)
{
   if (state.display < 0) {
      state.display = open(RK3588_DISPLAY_PATH, O_RDONLY | O_CLOEXEC);
      if (state.display < 0)
         return false;
   }
   struct rk3588_accelerant_info info;
   if (!get_display_info(&info)) {
      if (state.front != NULL)
         drop_front();
      return false;
   }
   if (state.front != NULL && state.front_physical == info.frame_buffer_physical
       && state.front_width == info.width && state.front_height == info.height
       && state.front_bytes_per_row == bytes_per_row
       && info.bytes_per_row == bytes_per_row)
      return true;
   if (state.front != NULL)
      drop_front();
   if (info.bytes_per_row != bytes_per_row || info.width == 0 || info.height == 0
       || info.bytes_per_row < info.width * 4)
      return false;

   /* Asking costs a round trip and an import; after a refusal, later. */
   const bigtime_t now = system_time();
   if (state.front_refused_at != 0 && now - state.front_refused_at < 1000000)
      return false;

   area_info clone;
   if (ioctl(state.display, RK3588_CLONE_FRAME_BUFFER, &clone, sizeof(clone)) != 0) {
      state.front_refused_at = now;
      return false;
   }
   /* Replaced in between: the clone is of the old one. */
   struct rk3588_accelerant_info after;
   uint64_t size = align64((uint64_t)info.bytes_per_row * info.height, B_PAGE_SIZE);
   bool cached = false;
   if (get_display_info(&after)
       && after.frame_buffer_physical == info.frame_buffer_physical
       && after.width == info.width && after.height == info.height
       && after.bytes_per_row == info.bytes_per_row && size <= clone.size) {
      state.front = panfrost_haiku_import_buffer(state.screen, clone.address,
                                                 size, &cached);
   }
   if (state.front == NULL) {
      delete_area(clone.area);
      state.front_refused_at = now;
      return false;
   }
   /* A frame buffer the CPU caches is not what the VOP2 reads: no copies
    * into it without maintenance this does not do. */
   if (cached) {
      pipe_resource_reference(&state.front, NULL);
      delete_area(clone.area);
      state.front_refused_at = now;
      return false;
   }
   state.front_clone = clone.area;
   state.front_width = info.width;
   state.front_height = info.height;
   state.front_bytes_per_row = info.bytes_per_row;
   state.front_physical = info.frame_buffer_physical;
   state.front_refused_at = 0;
   return true;
}


static bool
acquire_back(void *bits, uint64_t size)
{
   size = align64(size, B_PAGE_SIZE);
   if (state.back != NULL && state.back_bits == bits && state.back_size >= size)
      return true;
   drop_back();
   if (((uintptr_t)bits & (B_PAGE_SIZE - 1)) != 0)
      return false;
   state.back = panfrost_haiku_import_buffer(state.screen, bits, size,
                                             &state.back_cached);
   if (state.back == NULL)
      return false;
   state.back_bits = bits;
   state.back_size = size;
   return true;
}


static nir_def *
load_params(nir_builder *b, unsigned components, nir_def *offset)
{
   return nir_load_ubo(b, components, 32, nir_imm_int(b, 0), offset,
                       .align_mul = 16, .align_offset = 0, .range = ~0);
}


/* Stores the invocation's pixels at start in SSBO index: all four with one
 * store when they are inside the copy, else the ones that are, one by one. */
static void
store_pixels(nir_builder *b, unsigned index, nir_def *start, nir_def *all,
             nir_def **pixels, nir_def *whole, nir_def *first, nir_def *width)
{
   nir_def *buffer = nir_imm_int(b, index);
   nir_push_if(b, whole);
   nir_store_ssbo(b, all, buffer, start,
                  .write_mask = (1 << PIXELS_PER_INVOCATION) - 1, .align_mul = 4);
   nir_push_else(b, NULL);
   for (int k = 0; k < PIXELS_PER_INVOCATION - 1; k++) {
      nir_push_if(b, nir_ult(b, nir_iadd_imm(b, first, k), width));
      nir_store_ssbo(b, pixels[k], buffer, nir_iadd_imm(b, start, 4 * k),
                     .write_mask = 0x1, .align_mul = 4);
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);
}


/* One workgroup a GROUP_PIXELS x BLOCK_HEIGHT block of one copy: the copy is
 * found by a binary search of the copies' first workgroups. Its texels
 * (src_x + x, src_y + y) go as B, G, R, A pixels to offset + y * stride +
 * x * 4 of SSBO 0 (the frame buffer) or 1 (app_server's copy). */
static void *
create_shader(struct pipe_context *pipe)
{
   struct pipe_screen *screen = pipe->screen;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      screen->nir_options[MESA_SHADER_COMPUTE], "haiku panfrost present");
   b.shader->info.workgroup_size[0] = BLOCK_WIDTH;
   b.shader->info.workgroup_size[1] = BLOCK_HEIGHT;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ubos = 1;
   /* Panfrost: UBO 0 is pushable and ours, as for its own compute shaders. */
   b.shader->info.first_ubo_is_default_ubo = true;
   b.shader->info.num_ssbos = 2;
   b.shader->info.num_textures = 1;
   BITSET_SET(b.shader->info.textures_used, 0);
   BITSET_SET(b.shader->info.textures_used_by_txf, 0);
   BITSET_SET(b.shader->info.samplers_used, 0);
   nir_variable *params = nir_variable_create(b.shader, nir_var_mem_ubo,
      glsl_array_type(glsl_uint_type(), sizeof(struct params) / 4, 0), "params");
   params->data.driver_location = 0;
   nir_variable *front = nir_variable_create(b.shader, nir_var_mem_ssbo,
      glsl_array_type(glsl_uint_type(), 0, 4), "front");
   front->data.binding = 0;
   nir_variable *back = nir_variable_create(b.shader, nir_var_mem_ssbo,
      glsl_array_type(glsl_uint_type(), 0, 4), "back");
   back->data.binding = 1;

   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *header = load_params(&b, 4, zero);
   nir_def *count = nir_channel(&b, header, 0);
   nir_def *workgroup = nir_load_workgroup_id(&b);
   nir_def *group = nir_iadd(&b,
      nir_imul(&b, nir_channel(&b, workgroup, 1), nir_channel(&b, header, 1)),
      nir_channel(&b, workgroup, 0));

   nir_push_if(&b, nir_ult(&b, group, nir_channel(&b, header, 2)));
   {
      /* The last copy whose first workgroup is not past this one. */
      nir_def *lo = zero, *hi = count;
      for (int i = 0; i < 10; i++) {  /* 2^10 > MAX_COPIES */
         nir_def *mid = nir_ushr_imm(&b, nir_iadd(&b, lo, hi), 1);
         nir_def *first = load_params(&b, 1, nir_iadd_imm(&b,
            nir_imul_imm(&b, mid, ENTRY_BYTES), HEADER_BYTES + 32));
         nir_def *take = nir_uge(&b, group, first);
         lo = nir_bcsel(&b, take, mid, lo);
         hi = nir_bcsel(&b, take, hi, mid);
      }
      nir_def *entry = nir_iadd_imm(&b, nir_imul_imm(&b, lo, ENTRY_BYTES),
                                    HEADER_BYTES);
      nir_def *e0 = load_params(&b, 4, entry);
      nir_def *e1 = load_params(&b, 4, nir_iadd_imm(&b, entry, 16));
      nir_def *e2 = load_params(&b, 1, nir_iadd_imm(&b, entry, 32));
      nir_def *width = nir_channel(&b, e0, 2);
      nir_def *height = nir_channel(&b, e0, 3);
      nir_def *groups_x = nir_channel(&b, e1, 3);
      nir_def *local = nir_isub(&b, group, e2);
      nir_def *block_x = nir_umod(&b, local, groups_x);
      nir_def *block_y = nir_udiv(&b, local, groups_x);
      nir_def *lid = nir_load_local_invocation_id(&b);
      nir_def *x = nir_iadd(&b, nir_imul_imm(&b, block_x, GROUP_PIXELS),
                            nir_imul_imm(&b, nir_channel(&b, lid, 0),
                                         PIXELS_PER_INVOCATION));
      nir_def *y = nir_iadd(&b, nir_imul_imm(&b, block_y, BLOCK_HEIGHT),
                            nir_channel(&b, lid, 1));

      nir_push_if(&b, nir_iand(&b, nir_ult(&b, x, width), nir_ult(&b, y, height)));
      {
         nir_def *last = nir_iadd_imm(&b, width, -1);
         nir_def *row = nir_iadd(&b, nir_channel(&b, e0, 1), y);
         nir_def *pixels[PIXELS_PER_INVOCATION];
         for (int k = 0; k < PIXELS_PER_INVOCATION; k++) {
            /* past the copy's end: fetched in it, never stored */
            nir_def *column = nir_umin(&b, nir_iadd_imm(&b, x, k), last);
            nir_def *coord = nir_vec2(&b,
               nir_iadd(&b, nir_channel(&b, e0, 0), column), row);
            nir_def *texel = nir_txf(&b, coord, .lod = zero, .texture_index = 0,
                                     .sampler_index = 0,
                                     .dim = GLSL_SAMPLER_DIM_2D,
                                     .dest_type = nir_type_float32);
            nir_def *bytes = nir_f2u32(&b, nir_fround_even(&b,
               nir_fmul_imm(&b, nir_fsat(&b, texel), 255.0)));
            /* B, G, R, A in memory: the screen's B_RGB32 */
            pixels[k] = nir_ior(&b,
               nir_ior(&b, nir_channel(&b, bytes, 2),
                           nir_ishl_imm(&b, nir_channel(&b, bytes, 1), 8)),
               nir_ior(&b, nir_ishl_imm(&b, nir_channel(&b, bytes, 0), 16),
                           nir_ishl_imm(&b, nir_channel(&b, bytes, 3), 24)));
         }
         nir_def *all = nir_vec(&b, pixels, PIXELS_PER_INVOCATION);
         nir_def *whole = nir_ult(&b, nir_iadd_imm(&b, x, PIXELS_PER_INVOCATION - 1),
                                  width);
         nir_def *start = nir_iadd(&b, nir_channel(&b, e1, 0),
            nir_iadd(&b, nir_imul(&b, y, nir_channel(&b, e1, 1)),
                     nir_imul_imm(&b, x, 4)));
         nir_push_if(&b, nir_ieq_imm(&b, nir_channel(&b, e1, 2), 0));
         store_pixels(&b, 0, start, all, pixels, whole, x, width);
         nir_push_else(&b, NULL);
         store_pixels(&b, 1, start, all, pixels, whole, x, width);
         nir_pop_if(&b, NULL);
      }
      nir_pop_if(&b, NULL);
   }
   nir_pop_if(&b, NULL);

   nir_validate_shader(b.shader, "haiku panfrost present");
   struct pipe_compute_state cs = {
      .ir_type = PIPE_SHADER_IR_NIR,
      .prog = b.shader,
   };
   /* Panfrost takes the NIR and frees it. */
   return pipe->create_compute_state(pipe, &cs);
}


/* A rectangle (frame buffer pixels, both edges included) where the image's
 * top left texel is at origin, clipped to the image and a surface of
 * width x height: the pixel it starts at and its size. */
static bool
clip_rect(const int32_t *r, int32_t origin_x, int32_t origin_y,
          unsigned image_width, unsigned image_height, uint32_t width,
          uint32_t height, int32_t out[4])
{
   int64_t left = MAX2(MAX2((int64_t)r[0], origin_x), 0);
   int64_t top = MAX2(MAX2((int64_t)r[1], origin_y), 0);
   int64_t right = MIN2(MIN2((int64_t)r[2] + 1, (int64_t)origin_x + image_width),
                        (int64_t)width);
   int64_t bottom = MIN2(MIN2((int64_t)r[3] + 1, (int64_t)origin_y + image_height),
                         (int64_t)height);
   if (right <= left || bottom <= top)
      return false;
   out[0] = left;
   out[1] = top;
   out[2] = right - left;
   out[3] = bottom - top;
   return true;
}


static void
add_copy(unsigned target, const int32_t *c, int32_t origin_x, int32_t origin_y,
         uint32_t stride, uint32_t *total_groups)
{
   struct copy_header *header = &state.params.header;
   const uint32_t groups_x = DIV_ROUND_UP(c[2], GROUP_PIXELS);
   const uint32_t groups = groups_x * DIV_ROUND_UP(c[3], BLOCK_HEIGHT);
   state.params.copies[header->count++] = (struct copy_entry) {
      .src_x = c[0] - origin_x,
      .src_y = c[1] - origin_y,
      .width = c[2],
      .height = c[3],
      .offset = (uint32_t)c[1] * stride + (uint32_t)c[0] * 4,
      .stride = stride,
      .target = target,
      .groups_x = groups_x,
      .first_group = *total_groups,
   };
   *total_groups += groups;
}


static void release_at_exit(void);


int
haiku_panfrost_present(struct st_context *st, struct pipe_resource *image,
                       const struct summit_haiku_present *p)
{
   struct pipe_context *pipe = st->pipe;
   struct pipe_screen *screen = st->screen;
   if (image == NULL || p == NULL || p->version < 1 || p->version > 2
       || (p->version >= 2 && p->flags != 0)
       || (p->rect_count > 0 && p->rects == NULL))
      return -1;
   if (!panfrost_haiku_can_import(screen))
      return -10;
   /* Any four-channel colour image the shader can fetch as floats. */
   if (image->target != PIPE_TEXTURE_2D && image->target != PIPE_TEXTURE_RECT)
      return -2;
   if (image->nr_samples > 1 || util_format_is_compressed(image->format)
       || util_format_is_pure_integer(image->format)
       || util_format_is_depth_or_stencil(image->format))
      return -2;

   pthread_mutex_lock(&state.lock);
   if (state.screen != NULL && state.screen != screen) {
      pthread_mutex_unlock(&state.lock);
      return -1;
   }
   state.screen = screen;
   if (!state.thread_started) {
      atexit(release_at_exit);
      if (pthread_create(&state.thread, NULL, completion_thread, NULL) != 0) {
         pthread_mutex_unlock(&state.lock);
         return -1;
      }
      state.thread_started = true;
   }
   unsigned in_flight = p->frames_in_flight ? p->frames_in_flight : 2;
   while (state.submitted - state.finished >= in_flight
          || state.queue_count == MAX_QUEUED)
      pthread_cond_wait(&state.changed, &state.lock);

   int result = 0;
   bool has_front = false, has_back = false;
   if (p->front_bytes_per_row != 0) {
      has_front = acquire_front(p->front_bytes_per_row);
      if (!has_front)
         result = -3;
   }
   if (result == 0 && p->back_bits != NULL) {
      uint64_t size = (uint64_t)p->back_bytes_per_row * p->back_height;
      if (p->back_width == 0 || p->back_height == 0
          || p->back_bytes_per_row < p->back_width * 4
          || !acquire_back(p->back_bits, size))
         result = -4;
      else
         has_back = true;
   }
   if (result != 0 || (!has_front && !has_back) || p->rect_count == 0) {
      pthread_mutex_unlock(&state.lock);
      if (result == 0 && p->completed != NULL)
         p->completed(p->cookie);
      return result;
   }

   /* The copies, rectangle by rectangle and target by target. Offsets are
    * 32-bit in the shader: a frame buffer is far smaller. */
   unsigned tail = (state.queue_head + state.queue_count) % MAX_QUEUED;
   struct completion *entry = &state.queue[tail];
   entry->back_count = 0;
   entry->back_bits = NULL;
   state.params.header = (struct copy_header) { 0 };
   uint32_t total_groups = 0;
   const unsigned image_width = image->width0, image_height = image->height0;
   for (uint32_t i = 0; i < p->rect_count && i < MAX_RECTS; i++) {
      const int32_t *r = p->rects + i * 4;
      int32_t front[4], back[4];
      if (has_front && clip_rect(r, p->origin_x, p->origin_y, image_width,
                                 image_height, state.front_width,
                                 state.front_height, front)) {
         add_copy(0, front, p->origin_x, p->origin_y, state.front_bytes_per_row,
                  &total_groups);
      }
      if (has_back && clip_rect(r, p->origin_x, p->origin_y, image_width,
                                image_height, p->back_width, p->back_height,
                                back)) {
         add_copy(1, back, p->origin_x, p->origin_y, p->back_bytes_per_row,
                  &total_groups);
         entry->back_ranges[entry->back_count++] = (struct byte_range) {
            .offset = (uint32_t)back[1] * p->back_bytes_per_row + (uint32_t)back[0] * 4,
            .bytes = (uint32_t)back[2] * 4,
            .rows = (uint32_t)back[3],
            .stride = p->back_bytes_per_row,
         };
      }
   }
   if (state.params.header.count == 0) {
      pthread_mutex_unlock(&state.lock);
      return -5;
   }
   const uint32_t grid_x = MIN2(total_groups, MAX_GRID_X);
   const uint32_t grid_y = DIV_ROUND_UP(total_groups, grid_x);
   state.params.header.grid_x = grid_x;
   state.params.header.total_groups = total_groups;

   if (state.shader == NULL || state.shader_pipe != pipe) {
      /* A shader of a context that may be gone stays behind. */
      state.shader = create_shader(pipe);
      state.shader_pipe = pipe;
      if (state.shader == NULL) {
         pthread_mutex_unlock(&state.lock);
         return -1;
      }
   }

   /* What the CPU wrote there must not be written back over the copies
    * later. */
   bigtime_t before = system_time();
   if (entry->back_count != 0 && state.back_cached) {
      entry->back_bits = state.back_bits;
      sync_cpu_cache(entry->back_bits, entry->back_ranges, entry->back_count);
   }
   bigtime_t dispatched = system_time();
   trace_add(&trace.before, dispatched - before);

   struct cso_context *cso = st->cso_context;
   cso_save_compute_state(cso, CSO_BIT_COMPUTE_SHADER | CSO_BIT_COMPUTE_SAMPLERS);
   cso_set_compute_shader_handle(cso, state.shader);

   struct pipe_sampler_view templ;
   u_sampler_view_default_template(&templ, image, util_format_linear(image->format));
   struct pipe_sampler_view *view = pipe->create_sampler_view(pipe, image, &templ);
   if (view == NULL) {
      cso_restore_compute_state(cso);
      pthread_mutex_unlock(&state.lock);
      return -1;
   }
   pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 0, 1, 0, &view);
   struct pipe_sampler_state sampler = {
      .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_img_filter = PIPE_TEX_FILTER_NEAREST,
      .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
   };
   const struct pipe_sampler_state *samplers[1] = { &sampler };
   cso_set_samplers(cso, MESA_SHADER_COMPUTE, 1, samplers);

   const unsigned params_bytes = HEADER_BYTES + state.params.header.count * ENTRY_BYTES;
   struct pipe_constant_buffer cb = {
      .buffer_size = params_bytes,
      .user_buffer = &state.params,
   };
   pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, 0, &cb);

   /* A copy into one target only still has both bound. */
   struct pipe_resource *front_buffer = has_front ? state.front : state.back;
   struct pipe_resource *back_buffer = has_back ? state.back : state.front;
   struct pipe_shader_buffer buffers[2] = {
      { .buffer = front_buffer, .buffer_size = front_buffer->width0 },
      { .buffer = back_buffer, .buffer_size = back_buffer->width0 },
   };
   pipe->set_shader_buffers(pipe, MESA_SHADER_COMPUTE, 0, 2, buffers, 0x3);

   struct pipe_grid_info info = {
      .block = { BLOCK_WIDTH, BLOCK_HEIGHT, 1 },
      .grid = { grid_x, grid_y, 1 },
   };
   pipe->launch_grid(pipe, &info);

   pipe->sampler_view_release(pipe, view);
   cso_restore_compute_state(cso);
   /* Unbound by hand: st/mesa would not, if its own shader does not use
    * them. */
   pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 0, 0,
                           MAX2(st->state.num_sampler_views[MESA_SHADER_COMPUTE], 1),
                           NULL);
   st->state.num_sampler_views[MESA_SHADER_COMPUTE] = 0;
   pipe->set_shader_buffers(pipe, MESA_SHADER_COMPUTE, 0, 2, NULL, 0);
   pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, 0, NULL);
   ST_SET_STATE4(st->ctx->NewDriverState, ST_NEW_CS_CONSTANTS,
                 ST_NEW_CS_SSBOS, ST_NEW_CS_SAMPLER_VIEWS,
                 ST_NEW_CS_SAMPLERS);
   ST_SET_STATE(st->ctx->NewDriverState, ST_NEW_CS_UBOS);

   /* Panfrost submitted the dispatch; the fence is that submission's. */
   entry->fence = NULL;
   pipe->flush(pipe, &entry->fence, 0);
   entry->submitted = system_time();
   trace_add(&trace.dispatch, entry->submitted - dispatched);
   entry->completed = p->completed;
   entry->cookie = p->cookie;
   state.queue_count++;
   state.submitted++;
   pthread_cond_broadcast(&state.changed);
   pthread_mutex_unlock(&state.lock);
   return 0;
}


void
haiku_panfrost_present_wait_idle(void)
{
   pthread_mutex_lock(&state.lock);
   wait_idle_locked();
   pthread_mutex_unlock(&state.lock);
}


void
haiku_panfrost_present_release(struct pipe_screen *screen)
{
   pthread_mutex_lock(&state.lock);
   if (state.screen != NULL && (screen == NULL || state.screen == screen)) {
      drop_back();
      drop_front();
      if (state.display >= 0) {
         close(state.display);
         state.display = -1;
      }
      if (screen != NULL) {
         state.screen = NULL;
         state.shader = NULL;
         state.shader_pipe = NULL;
      }
   }
   pthread_mutex_unlock(&state.lock);
}


static void
release_at_exit(void)
{
   haiku_panfrost_present_release(NULL);
}
