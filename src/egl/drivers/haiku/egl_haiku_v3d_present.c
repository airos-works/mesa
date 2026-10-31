/*
 * Copyright 2026, air/OS. SPDX-License-Identifier: MIT
 *
 * Presenting a finished frame straight into the screen with the V3D of the
 * Raspberry Pi 4 (air/OS haiku#49), for Summit's web process: the same
 * summit_haiku_present_framebuffer() as Zink's (Summit's Mesa patch 06,
 * struct summit_haiku_present), where it otherwise reads every frame back.
 *
 * The frame buffer the accelerant scans out is rpi_display's, cloned into
 * this team and imported as a V3D buffer; app_server's copy of the screen is
 * the window's drawing_bits_area as the caller mapped it, imported the same
 * way. A compute shader copies each visible rectangle of the framebuffer
 * object's colour attachment into both, pixel by pixel: render target stores
 * write whole tile rows, and outside the rectangles the frame buffer holds
 * other windows. app_server's copy is cached memory: its lines are written
 * back before the copy and invalidated once the GPU is done (ranged
 * CPU_PREPARE), on a helper thread that then reports the present complete.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_sampler.h"
#include "main/mtypes.h"
#include "state_tracker/st_atom.h"
#include "state_tracker/st_context.h"

#include "v3d/drm/v3d_drm_public.h"

#include "egl_haiku_v3d_present.h"

#define MAX_RECTS 256
#define MAX_QUEUED 16
#define BLOCK_WIDTH 16
#define BLOCK_HEIGHT 4

/* rpi_display's interface (headers/private/graphics/rpi_display in the
 * air/OS tree), as far as this needs it. */
#define RPI_DISPLAY_PATH "/dev/graphics/rpi_display/0"
#define RPI_DISPLAY_CLONE_FRAME_BUFFER (B_DEVICE_OP_CODES_END + 3)
#define RPI_DISPLAY_GET_STATE (B_DEVICE_OP_CODES_END + 4)

struct rpi_display_output {
   uint32_t id, flags;
   uint16_t native_width, native_height;
   int32_t x, y;
   uint16_t width, height, mode_width, mode_height, scale, render_scale;
   uint32_t edid_length;
   uint8_t edid[256];
};

struct rpi_display_shared_info {
   uint32_t version, output_count;
   struct rpi_display_output outputs[2];
   uint32_t width, height, bytes_per_row, generation;
   uint64_t physical_address;
};

_Static_assert(sizeof(struct rpi_display_shared_info) == 616,
               "rpi_display_shared_info as the driver has it");

/* The parameters of one copy, in the order the shader reads them. */
struct copy_params {
   uint32_t src_x, src_y, width, height;
   uint32_t front_offset, front_stride, back_offset, back_stride;
   uint32_t targets, pad[3];  /* 1: front, 2: back */
};

struct completion {
   int token;
   void (*completed)(void *cookie);
   void *cookie;
   unsigned back_count;
   struct v3d_haiku_byte_range back_ranges[MAX_RECTS];
};

static struct {
   pthread_mutex_t lock;
   pthread_cond_t changed;
   struct pipe_screen *screen;

   /* The compute shader, for the context it was made with. */
   struct pipe_context *shader_pipe;
   void *shader;

   /* rpi_display's frame buffer. */
   int display;
   struct pipe_resource *front;
   uint32_t front_width, front_height, front_bytes_per_row, front_generation;
   int64_t front_refused_at;

   /* app_server's copy of the screen. */
   void *back_bits;
   uint64_t back_size;
   struct pipe_resource *back;
   bool back_cached;

   struct completion queue[MAX_QUEUED];
   unsigned queue_head, queue_count;
   uint64_t submitted, finished;
   bool thread_started;
   pthread_t thread;
} state = {
   .lock = PTHREAD_MUTEX_INITIALIZER,
   .changed = PTHREAD_COND_INITIALIZER,
   .display = -1,
};


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
      struct pipe_resource *back = state.back;
      pthread_mutex_unlock(&state.lock);

      if (entry->token >= 0)
         v3d_haiku_wait_job_token(screen, entry->token, -1);
      /* What the CPU may hold of the rectangles the GPU just wrote goes;
       * later presents may already be writing elsewhere. */
      if (entry->back_count != 0 && back != NULL)
         v3d_haiku_prepare_ranges(back, entry->back_ranges,
                                  entry->back_count, true);
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
   pipe_resource_reference(&state.front, NULL);
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


/* The frame buffer rpi_display scans out, as a V3D buffer; a mode change
 * replaces it (its generation tells). */
static bool
acquire_front(uint32_t bytes_per_row)
{
   if (state.display < 0) {
      state.display = open(RPI_DISPLAY_PATH, O_RDWR | O_CLOEXEC);
      if (state.display < 0)
         return false;
   }
   struct rpi_display_shared_info info;
   if (ioctl(state.display, RPI_DISPLAY_GET_STATE, &info, sizeof(info)) != 0)
      return false;
   if (state.front != NULL && state.front_generation == info.generation
       && state.front_bytes_per_row == bytes_per_row)
      return true;
   if (state.front != NULL)
      drop_front();
   if (info.bytes_per_row != bytes_per_row)
      return false;

   /* Asking costs a round trip and an import; after a refusal, later. */
   const bigtime_t now = system_time();
   if (state.front_refused_at != 0 && now - state.front_refused_at < 1000000)
      return false;

   area_info clone;
   if (ioctl(state.display, RPI_DISPLAY_CLONE_FRAME_BUFFER, &clone,
             sizeof(clone)) != 0) {
      state.front_refused_at = now;
      return false;
   }
   /* Replaced in between: the clone is of the old one. */
   struct rpi_display_shared_info after;
   uint64_t size = (uint64_t)info.bytes_per_row * info.height;
   bool cached = false;
   if (ioctl(state.display, RPI_DISPLAY_GET_STATE, &after, sizeof(after)) == 0
       && after.generation == info.generation && size <= clone.size) {
      state.front = v3d_haiku_import_buffer(state.screen, clone.address,
                                            align64(size, B_PAGE_SIZE),
                                            &cached);
   }
   /* The driver holds the pages now; this team needs no mapping of them. */
   delete_area(clone.area);
   if (state.front == NULL) {
      state.front_refused_at = now;
      return false;
   }
   state.front_width = info.width;
   state.front_height = info.height;
   state.front_bytes_per_row = info.bytes_per_row;
   state.front_generation = info.generation;
   state.front_refused_at = 0;
   return true;
}


static bool
acquire_back(void *bits, uint64_t size)
{
   if (state.back_bits == bits && state.back_size >= size)
      return true;
   drop_back();
   if (((uintptr_t)bits & (B_PAGE_SIZE - 1)) != 0)
      return false;
   size = align64(size, B_PAGE_SIZE);
   state.back = v3d_haiku_import_buffer(state.screen, bits, size,
                                        &state.back_cached);
   if (state.back == NULL)
      return false;
   state.back_bits = bits;
   state.back_size = size;
   return true;
}


/* gl_GlobalInvocationID.xy within width x height: texel (src_x + x,
 * src_y + y) as a B, G, R, A pixel into each target asked for, at
 * offset + y * stride + x * 4. */
static void *
create_shader(struct pipe_context *pipe)
{
   struct pipe_screen *screen = pipe->screen;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      screen->nir_options[MESA_SHADER_COMPUTE], "haiku present");
   b.shader->info.workgroup_size[0] = BLOCK_WIDTH;
   b.shader->info.workgroup_size[1] = BLOCK_HEIGHT;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ubos = 1;
   b.shader->info.num_ssbos = 2;
   b.shader->info.num_textures = 1;
   BITSET_SET(b.shader->info.textures_used, 0);
   BITSET_SET(b.shader->info.textures_used_by_txf, 0);
   BITSET_SET(b.shader->info.samplers_used, 0);
   nir_variable *front = nir_variable_create(b.shader, nir_var_mem_ssbo,
      glsl_array_type(glsl_uint_type(), 0, 4), "front");
   front->data.binding = 0;
   nir_variable *back = nir_variable_create(b.shader, nir_var_mem_ssbo,
      glsl_array_type(glsl_uint_type(), 0, 4), "back");
   back->data.binding = 1;

   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *p0 = nir_load_ubo(&b, 4, 32, zero, nir_imm_int(&b, 0),
                              .align_mul = 16, .align_offset = 0, .range = ~0);
   nir_def *p1 = nir_load_ubo(&b, 4, 32, zero, nir_imm_int(&b, 16),
                              .align_mul = 16, .align_offset = 0, .range = ~0);
   nir_def *p2 = nir_load_ubo(&b, 4, 32, zero, nir_imm_int(&b, 32),
                              .align_mul = 16, .align_offset = 0, .range = ~0);

   nir_def *block = nir_imm_ivec3(&b, BLOCK_WIDTH, BLOCK_HEIGHT, 1);
   nir_def *id = nir_iadd(&b, nir_imul(&b, nir_load_workgroup_id(&b), block),
                          nir_load_local_invocation_id(&b));
   nir_def *x = nir_channel(&b, id, 0);
   nir_def *y = nir_channel(&b, id, 1);

   nir_push_if(&b, nir_iand(&b, nir_ult(&b, x, nir_channel(&b, p0, 2)),
                                nir_ult(&b, y, nir_channel(&b, p0, 3))));
   {
      nir_def *coord = nir_vec2(&b, nir_iadd(&b, nir_channel(&b, p0, 0), x),
                                    nir_iadd(&b, nir_channel(&b, p0, 1), y));
      nir_def *texel = nir_txf(&b, coord, .lod = zero, .texture_index = 0,
                               .sampler_index = 0,
                               .dim = GLSL_SAMPLER_DIM_2D,
                               .dest_type = nir_type_float32);
      nir_def *bytes = nir_f2u32(&b, nir_fround_even(&b,
         nir_fmul_imm(&b, nir_fsat(&b, texel), 255.0)));
      /* B, G, R, A in memory: the screen's B_RGB32 */
      nir_def *pixel = nir_ior(&b,
         nir_ior(&b, nir_channel(&b, bytes, 2),
                     nir_ishl_imm(&b, nir_channel(&b, bytes, 1), 8)),
         nir_ior(&b, nir_ishl_imm(&b, nir_channel(&b, bytes, 0), 16),
                     nir_ishl_imm(&b, nir_channel(&b, bytes, 3), 24)));
      nir_def *column = nir_imul_imm(&b, x, 4);
      nir_def *targets = nir_channel(&b, p2, 0);

      nir_push_if(&b, nir_i2b(&b, nir_iand_imm(&b, targets, 1)));
      nir_store_ssbo(&b, pixel, nir_imm_int(&b, 0),
                     nir_iadd(&b, nir_channel(&b, p1, 0),
                              nir_iadd(&b, nir_imul(&b, y, nir_channel(&b, p1, 1)),
                                       column)),
                     .write_mask = 0x1, .align_mul = 4);
      nir_pop_if(&b, NULL);

      nir_push_if(&b, nir_i2b(&b, nir_iand_imm(&b, targets, 2)));
      nir_store_ssbo(&b, pixel, nir_imm_int(&b, 1),
                     nir_iadd(&b, nir_channel(&b, p1, 2),
                              nir_iadd(&b, nir_imul(&b, y, nir_channel(&b, p1, 3)),
                                       column)),
                     .write_mask = 0x1, .align_mul = 4);
      nir_pop_if(&b, NULL);
   }
   nir_pop_if(&b, NULL);

   nir_validate_shader(b.shader, "haiku present");
   if (screen->finalize_nir)
      screen->finalize_nir(screen, b.shader);
   struct pipe_compute_state cs = {
      .ir_type = PIPE_SHADER_IR_NIR,
      .prog = b.shader,
   };
   return pipe->create_compute_state(pipe, &cs);
}


/* A rectangle (frame buffer pixels, both edges included) where the image's
 * top left texel is at origin, clipped to the image and a surface of
 * width x height: the texel and pixel it starts at and its size. */
static bool
clip_rect(const int32_t *r, int32_t origin_x, int32_t origin_y,
          unsigned image_width, unsigned image_height, uint32_t width,
          uint32_t height, int32_t out[4])
{
   int32_t left = MAX2(MAX2(r[0], origin_x), 0);
   int32_t top = MAX2(MAX2(r[1], origin_y), 0);
   int32_t right = MIN2(MIN2(r[2] + 1, origin_x + (int32_t)image_width),
                        (int32_t)width);
   int32_t bottom = MIN2(MIN2(r[3] + 1, origin_y + (int32_t)image_height),
                         (int32_t)height);
   if (right <= left || bottom <= top)
      return false;
   out[0] = left;
   out[1] = top;
   out[2] = right - left;
   out[3] = bottom - top;
   return true;
}


static void
dispatch(struct pipe_context *pipe, const struct copy_params *params)
{
   struct pipe_constant_buffer cb = {
      .buffer_size = sizeof(*params),
      .user_buffer = params,
   };
   pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, 0, &cb);
   struct pipe_grid_info info = {
      .block = { BLOCK_WIDTH, BLOCK_HEIGHT, 1 },
      .grid = { DIV_ROUND_UP(params->width, BLOCK_WIDTH),
                DIV_ROUND_UP(params->height, BLOCK_HEIGHT), 1 },
   };
   pipe->launch_grid(pipe, &info);
}


static void release_at_exit(void);


int
haiku_v3d_present(struct st_context *st, struct pipe_resource *image,
                  const struct summit_haiku_present *p)
{
   struct pipe_context *pipe = st->pipe;
   struct pipe_screen *screen = st->screen;
   if (image == NULL || p == NULL || p->version < 1 || p->version > 2
       || (p->version >= 2 && p->flags != 0)
       || (p->rect_count > 0 && p->rects == NULL))
      return -1;
   if (!v3d_haiku_can_import(screen))
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
      if (p->back_bytes_per_row < p->back_width * 4
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

   if (state.shader == NULL || state.shader_pipe != pipe) {
      /* A shader of a context that may be gone stays behind. */
      state.shader = create_shader(pipe);
      state.shader_pipe = pipe;
      if (state.shader == NULL) {
         pthread_mutex_unlock(&state.lock);
         return -1;
      }
   }

   /* The copies, rectangle by rectangle, into both targets at once where
    * both clip it alike. */
   static struct copy_params copies[2 * MAX_RECTS];
   unsigned copy_count = 0;
   unsigned tail = (state.queue_head + state.queue_count) % MAX_QUEUED;
   struct completion *entry = &state.queue[tail];
   entry->back_count = 0;
   const unsigned image_width = image->width0, image_height = image->height0;
   for (uint32_t i = 0; i < p->rect_count && i < MAX_RECTS; i++) {
      const int32_t *r = p->rects + i * 4;
      int32_t front[4], back[4];
      bool to_front = has_front && clip_rect(r, p->origin_x, p->origin_y,
         image_width, image_height, state.front_width, state.front_height,
         front);
      bool to_back = has_back && clip_rect(r, p->origin_x, p->origin_y,
         image_width, image_height, p->back_width, p->back_height, back);
      if (to_back) {
         entry->back_ranges[entry->back_count++] = (struct v3d_haiku_byte_range) {
            .offset = (uint64_t)back[1] * p->back_bytes_per_row + back[0] * 4,
            .bytes = back[2] * 4,
            .rows = back[3],
            .stride = p->back_bytes_per_row,
         };
      }
      bool together = to_front && to_back && memcmp(front, back,
                                                    sizeof(front)) == 0;
      for (int target = 0; target < 2; target++) {
         const int32_t *c = target == 0 ? front : back;
         if (!(target == 0 ? to_front : to_back) || (target == 1 && together))
            continue;
         copies[copy_count++] = (struct copy_params) {
            .src_x = c[0] - p->origin_x,
            .src_y = c[1] - p->origin_y,
            .width = c[2],
            .height = c[3],
            .front_offset = c[1] * state.front_bytes_per_row + c[0] * 4,
            .front_stride = state.front_bytes_per_row,
            .back_offset = c[1] * p->back_bytes_per_row + c[0] * 4,
            .back_stride = p->back_bytes_per_row,
            .targets = together ? 3 : target == 0 ? 1 : 2,
         };
      }
   }
   if (copy_count == 0) {
      pthread_mutex_unlock(&state.lock);
      return -5;
   }

   /* What the CPU wrote there must not be written back over the copies
    * later. */
   if (entry->back_count != 0 && state.back_cached)
      v3d_haiku_prepare_ranges(state.back, entry->back_ranges,
                               entry->back_count, true);
   if (!state.back_cached)
      entry->back_count = 0;

   struct cso_context *cso = st->cso_context;
   cso_save_compute_state(cso, CSO_BIT_COMPUTE_SHADER
                               | CSO_BIT_COMPUTE_SAMPLERS);
   cso_set_compute_shader_handle(cso, state.shader);

   struct pipe_sampler_view templ;
   u_sampler_view_default_template(&templ, image,
                                   util_format_linear(image->format));
   struct pipe_sampler_view *view = pipe->create_sampler_view(pipe, image,
                                                              &templ);
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

   /* A copy into one target only still has both bound. */
   struct pipe_resource *front_buffer = has_front ? state.front : state.back;
   struct pipe_resource *back_buffer = has_back ? state.back : state.front;
   struct pipe_shader_buffer buffers[2] = {
      { .buffer = front_buffer, .buffer_size = front_buffer->width0 },
      { .buffer = back_buffer, .buffer_size = back_buffer->width0 },
   };
   pipe->set_shader_buffers(pipe, MESA_SHADER_COMPUTE, 0, 2, buffers, 0x3);

   for (unsigned i = 0; i < copy_count; i++)
      dispatch(pipe, &copies[i]);

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
   ST_SET_STATE3(st->ctx->NewDriverState, ST_NEW_CS_CONSTANTS,
                 ST_NEW_CS_SSBOS, ST_NEW_CS_SAMPLER_VIEWS);

   /* The compute jobs were submitted as they were dispatched. */
   entry->token = v3d_haiku_last_job_token(pipe);
   entry->completed = p->completed;
   entry->cookie = p->cookie;
   state.queue_count++;
   state.submitted++;
   pthread_cond_broadcast(&state.changed);
   pthread_mutex_unlock(&state.lock);
   return 0;
}


void
haiku_v3d_present_wait_idle(void)
{
   pthread_mutex_lock(&state.lock);
   wait_idle_locked();
   pthread_mutex_unlock(&state.lock);
}


void
haiku_v3d_present_release(struct pipe_screen *screen)
{
   pthread_mutex_lock(&state.lock);
   if (state.screen != NULL && (screen == NULL || state.screen == screen)) {
      drop_back();
      drop_front();
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
   haiku_v3d_present_release(NULL);
}
