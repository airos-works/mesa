/* SPDX-License-Identifier: MIT
 * Experimental software geometry with native Panfrost rasterization.
 * Opt-in while shader/resource coverage and native rendering are qualified.
 */
#include "pan_context.h"
#include "pan_sw_polygon.h"

#include "draw/draw_context.h"
#include "draw/draw_fs.h"
#include "draw/draw_pipe.h"
#include "draw/draw_private.h"
#include "draw/draw_vs.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_prim.h"
#include "util/u_simple_shaders.h"
#include "util/u_upload_mgr.h"

#include <math.h>
#include <stdio.h>

#define POLYGON_BATCH_VERTICES 2048

DEBUG_GET_ONCE_BOOL_OPTION(haiku_sw_polygon, "HAIKU_PAN_SW_POLYGON", true)
DEBUG_GET_ONCE_BOOL_OPTION(haiku_sw_polygon_trace, "HAIKU_PAN_SW_POLYGON_TRACE", false)

bool
panfrost_sw_polygon_enabled(void)
{
   return debug_get_option_haiku_sw_polygon();
}

struct panfrost_sw_polygon {
   /* The final draw stage is embedded and freed with this object. */
   struct draw_stage stage;
   struct panfrost_context *ctx;
   struct draw_context *draw;
   struct draw_vertex_shader *vs;
   struct draw_fragment_shader *fs;
   nir_shader *fs_nir;
   void *native_vs;
   void *native_elements;
   unsigned char vs_sha1[20], fs_sha1[20];
   bool cache_valid;

   unsigned num_attributes;
   unsigned source_slot[PIPE_MAX_ATTRIBS];
   unsigned position_attribute;
   unsigned stride;
   float *vertices;
   unsigned vertex_count;
   enum mesa_prim mode;
   struct pipe_viewport_state viewport;
   bool failed;
   uint64_t draws, batches, emitted_vertices, failures;
   bool trace_draw, trace_vertices, trace_shader_valid;
   unsigned char trace_vs_sha1[20];
   void (*trace_run_linear)(struct draw_vertex_shader *, const float (*)[4],
                           float (*)[4], const struct draw_buffer_info *,
                           unsigned, unsigned, unsigned, const unsigned *);
};

static void
trace_vertex_attributes(struct panfrost_sw_polygon *sw, const char *phase,
                        const float (*data)[4], unsigned count,
                        unsigned stride, unsigned slots)
{
   for (unsigned v = 0; v < count; ++v) {
      const float (*attributes)[4] = (const void *)
         ((const char *)data + (size_t)v * stride);
      for (unsigned a = 0; a < slots; ++a) {
         uint32_t bits[4];
         memcpy(bits, attributes[a], sizeof(bits));
         fprintf(stderr, "HAIKU_PAN_GEOMETRY_VS_%s draw=%llu vertex=%u slot=%u bits=%08x,%08x,%08x,%08x\n",
                 phase, (unsigned long long)sw->draws, v, a,
                 bits[0], bits[1], bits[2], bits[3]);
      }
   }
}

static void
trace_run_vertex_shader(struct draw_vertex_shader *shader,
                         const float (*input)[4], float (*output)[4],
                         const struct draw_buffer_info *constants,
                         unsigned count, unsigned input_stride,
                         unsigned output_stride, const unsigned *fetch_elts)
{
   struct panfrost_context *ctx = (void *)shader->draw->pipe;
   struct panfrost_sw_polygon *sw = ctx->sw_polygon;
   bool trace = sw->trace_draw && count <= 8 &&
                shader->info.num_inputs <= PIPE_MAX_ATTRIBS &&
                shader->info.num_outputs <= PIPE_MAX_ATTRIBS;
   if (trace) {
      fprintf(stderr, "HAIKU_PAN_GEOMETRY_VS_BEGIN draw=%llu count=%u input_stride=%u output_stride=%u inputs=%u outputs=%u\n",
              (unsigned long long)sw->draws, count, input_stride, output_stride,
              shader->info.num_inputs, shader->info.num_outputs);
      trace_vertex_attributes(sw, "INPUT", input, count, input_stride,
                              shader->info.num_inputs);
   }
   sw->trace_run_linear(shader, input, output, constants, count,
                         input_stride, output_stride, fetch_elts);
   if (trace) {
      trace_vertex_attributes(sw, "OUTPUT", (const float (*)[4])output, count,
                              output_stride, shader->info.num_outputs);
      fprintf(stderr, "HAIKU_PAN_GEOMETRY_VS_END draw=%llu\n",
              (unsigned long long)sw->draws);
   }
}

static void
emit_batch(struct panfrost_sw_polygon *sw)
{
   if (!sw->vertex_count || sw->failed) {
      sw->vertex_count = 0;
      return;
   }

   struct pipe_context *pipe = &sw->ctx->base;
   struct pipe_resource *buffer = NULL;
   unsigned offset = 0;
   u_upload_data_ref(pipe->stream_uploader, 0,
                     sw->vertex_count * sw->stride, 64, sw->vertices,
                     &offset, &buffer);
   u_upload_unmap(pipe->stream_uploader);
   if (!buffer) {
      sw->failed = true;
      sw->vertex_count = 0;
      return;
   }

   struct pipe_vertex_buffer vb = {
      .buffer_offset = offset,
      .buffer.resource = buffer,
   };
   pipe->set_vertex_buffers(pipe, 1, &vb);
   struct pipe_draw_info info = {
      .mode = sw->mode,
      .instance_count = 1,
   };
   struct pipe_draw_start_count_bias draw = {.count = sw->vertex_count};
   assert(sw->ctx->sw_polygon_active);
   pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
   sw->batches++;
   sw->emitted_vertices += sw->vertex_count;
   pipe_resource_reference(&buffer, NULL);
   sw->vertex_count = 0;
}

static void
emit_primitive(struct draw_stage *stage, const struct prim_header *prim,
               enum mesa_prim mode, unsigned count)
{
   struct panfrost_sw_polygon *sw = (struct panfrost_sw_polygon *)stage;
   if (sw->failed)
      return;
   if (sw->trace_draw && !sw->trace_vertices) {
      const struct draw_context *draw = sw->draw;
      unsigned pos = draw_current_shader_position_output(draw);
      unsigned edge = draw->vs.edgeflag_output;
      fprintf(stderr, "HAIKU_PAN_GEOMETRY_VERTICES draw=%llu count=%u stride=%u pos=%u edge=%u\n",
              (unsigned long long)sw->draws, draw->pipeline.vertex_count,
              draw->pipeline.vertex_stride, pos, edge);
      for (unsigned i = 0; draw->pipeline.verts && i < MIN2(draw->pipeline.vertex_count, 8); ++i) {
         const struct vertex_header *v = (const void *)
            ((const char *)draw->pipeline.verts + i * draw->pipeline.vertex_stride);
         fprintf(stderr, "HAIKU_PAN_GEOMETRY_VERTEX draw=%llu vertex=%u edgeflag=%u clipmask=%u pos=%g,%g,%g,%g clip=%g,%g,%g,%g\n",
                 (unsigned long long)sw->draws, i, v->edgeflag, v->clipmask,
                 v->data[pos][0], v->data[pos][1], v->data[pos][2], v->data[pos][3],
                 v->clip_pos[0], v->clip_pos[1], v->clip_pos[2], v->clip_pos[3]);
      }
      sw->trace_vertices = true;
   }
   if (sw->mode != mode || sw->vertex_count + count > POLYGON_BATCH_VERTICES)
      emit_batch(sw);
   if (sw->failed)
      return;
   sw->mode = mode;

   for (unsigned v = 0; v < count; ++v) {
      const struct vertex_header *src = prim->v[v];
      float *dst = sw->vertices + (sw->vertex_count + v) * sw->num_attributes * 4;
      for (unsigned a = 0; a < sw->num_attributes; ++a)
         memcpy(dst + a * 4, src->data[sw->source_slot[a]], 4 * sizeof(float));

      /* draw has performed the viewport transform. Reconstruct homogeneous
       * position so native interpolation retains the original reciprocal W.
       * Use retained clip coordinates when a viewport scale is zero.
       */
      float *pos = dst + sw->position_attribute * 4;
      float w = src->clip_pos[3];
      if (!isfinite(w) || w == 0.0f) {
         sw->failed = true;
         return;
      }
      for (unsigned c = 0; c < 3; ++c) {
         if (sw->viewport.scale[c] != 0.0f)
            pos[c] = (pos[c] - sw->viewport.translate[c]) / sw->viewport.scale[c] * w;
         else
            pos[c] = src->clip_pos[c];
      }
      pos[3] = w;
   }
   sw->vertex_count += count;
}

static void polygon_point(struct draw_stage *s, struct prim_header *p)
{
   emit_primitive(s, p, MESA_PRIM_POINTS, 1);
}

static void polygon_line(struct draw_stage *s, struct prim_header *p)
{
   emit_primitive(s, p, MESA_PRIM_LINES, 2);
}

static void polygon_tri(struct draw_stage *s, struct prim_header *p)
{
   emit_primitive(s, p, MESA_PRIM_TRIANGLES, 3);
}

static void polygon_flush(struct draw_stage *s, unsigned flags)
{
   (void)flags;
   emit_batch((struct panfrost_sw_polygon *)s);
}

static void polygon_reset_stipple(struct draw_stage *s)
{
   /* Line stipple is excluded by the current experimental entry gate. */
   (void)s;
}

static void polygon_stage_destroy(struct draw_stage *s)
{
   /* The enclosing context owns the embedded stage. */
   (void)s;
}

static void
delete_shader_cache(struct panfrost_sw_polygon *sw)
{
   struct pipe_context *pipe = &sw->ctx->base;
   draw_bind_vertex_shader(sw->draw, NULL);
   draw_bind_fragment_shader(sw->draw, NULL);
   if (sw->vs)
      draw_delete_vertex_shader(sw->draw, sw->vs);
   if (sw->fs)
      draw_delete_fragment_shader(sw->draw, sw->fs);
   if (sw->native_vs)
      pipe->delete_vs_state(pipe, sw->native_vs);
   if (sw->native_elements)
      pipe->delete_vertex_elements_state(pipe, sw->native_elements);
   ralloc_free(sw->fs_nir);
   sw->vs = NULL;
   sw->fs = NULL;
   sw->fs_nir = NULL;
   sw->native_vs = NULL;
   sw->native_elements = NULL;
   sw->cache_valid = false;
}

static bool
float32_type(const struct glsl_type *type)
{
   if (glsl_type_is_array(type))
      return float32_type(glsl_get_array_element(type));
   if (glsl_type_is_struct(type) || glsl_type_is_interface(type)) {
      for (unsigned i = 0; i < glsl_get_length(type); ++i)
         if (!float32_type(glsl_get_struct_field(type, i)))
            return false;
      return true;
   }
   return glsl_type_is_float(type);
}

static bool
float32_fragment_inputs(nir_shader *shader)
{
   nir_foreach_variable_with_modes(var, shader, nir_var_shader_in) {
      if (!float32_type(var->type))
         return false;
   }
   nir_foreach_function_impl(impl, shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_input &&
                intr->intrinsic != nir_intrinsic_load_interpolated_input)
               continue;
            nir_alu_type type = nir_intrinsic_dest_type(intr);
            if (intr->def.bit_size != 32 || (type && type != nir_type_float32))
               return false;
         }
      }
   }
   return true;
}

static bool
prepare_shader_cache(struct panfrost_sw_polygon *sw)
{
   struct pipe_context *pipe = &sw->ctx->base;
   struct panfrost_uncompiled_shader *vs = sw->ctx->uncompiled[MESA_SHADER_VERTEX];
   struct panfrost_uncompiled_shader *fs = sw->ctx->uncompiled[MESA_SHADER_FRAGMENT];
   if (!vs || !fs || !vs->sw_polygon_nir || !fs->sw_polygon_nir)
      return false;
   if (sw->cache_valid && memcmp(sw->vs_sha1, vs->nir_sha1, 20) == 0 &&
       memcmp(sw->fs_sha1, fs->nir_sha1, 20) == 0)
      return true;

   delete_shader_cache(sw);
   const shader_info *info = &vs->sw_polygon_nir->info;
   /* No null software sampler/image callbacks or untracked shader writes. */
   if (info->num_textures || info->num_images || info->num_ssbos ||
       info->writes_memory || vs->stream_output.num_outputs ||
       (info->outputs_written & (VARYING_BIT_LAYER | VARYING_BIT_VIEWPORT)) ||
       !float32_fragment_inputs(fs->sw_polygon_nir))
      return false;

   sw->fs_nir = nir_shader_clone(NULL, fs->sw_polygon_nir);
   if (!sw->fs_nir)
      return false;
   struct pipe_shader_state fs_state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = sw->fs_nir,
   };
   sw->fs = draw_create_fragment_shader(sw->draw, &fs_state);
   if (!sw->fs || sw->fs->info.uses_frontface || sw->fs->info.uses_primid)
      return false;

   struct pipe_shader_state vs_state = {
      .type = PIPE_SHADER_IR_NIR,
      .ir.nir = nir_shader_clone(NULL, vs->sw_polygon_nir),
   };
   if (!vs_state.ir.nir)
      return false;
   sw->vs = draw_create_vertex_shader(sw->draw, &vs_state);
   if (!sw->vs)
      return false;
   if (debug_get_option_haiku_sw_polygon_trace()) {
      sw->trace_run_linear = sw->vs->run_linear;
      sw->vs->run_linear = trace_run_vertex_shader;
   }
   draw_bind_vertex_shader(sw->draw, sw->vs);
   draw_bind_fragment_shader(sw->draw, sw->fs);

   enum tgsi_semantic names[PIPE_MAX_ATTRIBS];
   unsigned indices[PIPE_MAX_ATTRIBS];
   sw->num_attributes = 0;
   sw->position_attribute = ~0u;
   for (unsigned i = 0; i < sw->vs->info.num_outputs; ++i) {
      enum tgsi_semantic name = sw->vs->info.output_semantic_name[i];
      if (name == TGSI_SEMANTIC_EDGEFLAG || name == TGSI_SEMANTIC_CLIPDIST ||
          name == TGSI_SEMANTIC_CLIPVERTEX)
         continue;
      unsigned a = sw->num_attributes++;
      if (a >= PIPE_MAX_ATTRIBS)
         return false;
      sw->source_slot[a] = i;
      names[a] = name;
      indices[a] = sw->vs->info.output_semantic_index[i];
      if (name == TGSI_SEMANTIC_POSITION)
         sw->position_attribute = a;
   }
   if (sw->position_attribute == ~0u)
      return false;
   sw->stride = sw->num_attributes * 4 * sizeof(float);
   sw->native_vs = util_make_vertex_passthrough_shader(pipe, sw->num_attributes,
                                                       names, indices, false);
   struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS] = {0};
   for (unsigned a = 0; a < sw->num_attributes; ++a) {
      elements[a].src_offset = a * 4 * sizeof(float);
      elements[a].src_stride = sw->stride;
      elements[a].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   }
   sw->native_elements = pipe->create_vertex_elements_state(pipe, sw->num_attributes, elements);
   if (!sw->native_vs || !sw->native_elements)
      return false;
   memcpy(sw->vs_sha1, vs->nir_sha1, 20);
   memcpy(sw->fs_sha1, fs->nir_sha1, 20);
   sw->cache_valid = true;
   return true;
}

static struct panfrost_sw_polygon *
create_polygon_context(struct panfrost_context *ctx)
{
   struct panfrost_sw_polygon *sw = CALLOC_STRUCT(panfrost_sw_polygon);
   if (!sw)
      return NULL;
   sw->ctx = ctx;
   sw->draw = draw_create_no_llvm(&ctx->base);
   sw->vertices = calloc(POLYGON_BATCH_VERTICES,
                         PIPE_MAX_ATTRIBS * 4 * sizeof(float));
   if (!sw->draw || !sw->vertices) {
      if (sw->draw)
         draw_destroy(sw->draw);
      free(sw->vertices);
      free(sw);
      return NULL;
   }
   sw->stage.draw = sw->draw;
   sw->stage.name = "panfrost software polygon geometry";
   sw->stage.point = polygon_point;
   sw->stage.line = polygon_line;
   sw->stage.tri = polygon_tri;
   sw->stage.flush = polygon_flush;
   sw->stage.reset_stipple_counter = polygon_reset_stipple;
   sw->stage.destroy = polygon_stage_destroy;
   draw_set_rasterize_stage(sw->draw, &sw->stage);
   draw_set_driver_clipping(sw->draw, false, false, false, false);
   /* Native rasterization owns legal line/point widths and point sprites.
    * Avoid optional draw stages that introduce extra shader attributes.
    */
   draw_wide_line_threshold(sw->draw, 1000000.0f);
   draw_wide_point_threshold(sw->draw, 1000000.0f);
   draw_wide_point_sprites(sw->draw, false);
   draw_enable_point_sprites(sw->draw, false);
   return sw;
}

void
panfrost_sw_polygon_destroy(struct panfrost_context *ctx)
{
   struct panfrost_sw_polygon *sw = ctx->sw_polygon;
   if (!sw)
      return;
   fprintf(stderr, "HAIKU_PAN_SW_POLYGON draws=%llu batches=%llu vertices=%llu failures=%llu\n",
           (unsigned long long)sw->draws, (unsigned long long)sw->batches,
           (unsigned long long)sw->emitted_vertices, (unsigned long long)sw->failures);
   delete_shader_cache(sw);
   draw_destroy(sw->draw);
   free(sw->vertices);
   free(sw);
   ctx->sw_polygon = NULL;
}

bool
panfrost_sw_polygon_draw(struct panfrost_context *ctx,
                        const struct pipe_rasterizer_state *rasterizer,
                        unsigned num_elements,
                        const struct pipe_vertex_element *elements,
                        const struct pipe_draw_info *info,
                        unsigned drawid_offset,
                        const struct pipe_draw_indirect_info *indirect,
                        const struct pipe_draw_start_count_bias *draws,
                        unsigned num_draws)
{
   /* Development scope: direct vertex/element draws and float32 varyings, no
    * shader memory effects, transform feedback, primitive-ID/front-face FS
    * inputs, layered viewports, separate near/far clipping or stippling.
    * Unsupported draws fail explicitly instead of using invalid callbacks.
    */
   if (indirect || ctx->streamout.num_targets || rasterizer->line_stipple_enable ||
       rasterizer->poly_stipple_enable ||
       rasterizer->depth_clip_near != rasterizer->depth_clip_far ||
       ((ctx->pipe_viewport.scale[2] == 0.0f || rasterizer->offset_units_unscaled) &&
        (rasterizer->offset_line || rasterizer->offset_point || rasterizer->offset_tri)))
      return false;
   if (!ctx->sw_polygon)
      ctx->sw_polygon = create_polygon_context(ctx);
   struct panfrost_sw_polygon *sw = ctx->sw_polygon;
   if (!sw)
      return false;

   struct pipe_context *pipe = &ctx->base;
   struct pipe_transfer *vb_maps[PIPE_MAX_ATTRIBS] = {0};
   struct pipe_transfer *cb_maps[PIPE_MAX_CONSTANT_BUFFERS] = {0};
   struct pipe_transfer *ib_map = NULL;
   const void *vertex_maps[PIPE_MAX_ATTRIBS] = {0};
   size_t vertex_map_sizes[PIPE_MAX_ATTRIBS] = {0};
   const void *index_map = NULL;
   size_t index_map_size = 0;
   struct pipe_vertex_buffer saved_buffers[PIPE_MAX_ATTRIBS] = {0};
   unsigned num_buffers = util_last_bit(ctx->vb_mask);
   void *saved_rasterizer = ctx->rasterizer;
   void *saved_elements = ctx->vertex;
   void *saved_vs = ctx->uncompiled[MESA_SHADER_VERTEX];
   uint64_t saved_primitives = ctx->prims_generated;
   uint64_t saved_draw_calls = ctx->draw_calls;
   void *native_rasterizer = NULL;
   bool rebound = false;
   bool success = false;

   for (unsigned i = 0; i < num_buffers; ++i) {
      saved_buffers[i].buffer_offset = ctx->vertex_buffers[i].buffer_offset;
      saved_buffers[i].is_user_buffer = ctx->vertex_buffers[i].is_user_buffer;
      if (saved_buffers[i].is_user_buffer)
         saved_buffers[i].buffer.user = ctx->vertex_buffers[i].buffer.user;
      else
         pipe_resource_reference(&saved_buffers[i].buffer.resource,
                                  ctx->vertex_buffers[i].buffer.resource);
   }

   ctx->sw_polygon_active = true;
   sw->draws++;
   sw->trace_draw = debug_get_option_haiku_sw_polygon_trace() &&
                    sw->draws <= 64 && num_draws == 1 && draws[0].count <= 4;
   sw->trace_vertices = false;
   sw->failed = false;
   sw->vertex_count = 0;
   sw->viewport = ctx->pipe_viewport;
   if (!prepare_shader_cache(sw))
      goto cleanup;
   draw_set_vertex_buffers(sw->draw, num_buffers, saved_buffers);
   draw_set_vertex_elements(sw->draw, num_elements, elements);
   draw_set_viewport_states(sw->draw, 0, 1, &sw->viewport);
   draw_set_rasterizer_state(sw->draw, rasterizer, saved_rasterizer);
   struct pipe_clip_state clip = {0};
   draw_set_clip_state(sw->draw, &clip);
   draw_set_zs_format(sw->draw, ctx->pipe_framebuffer.zsbuf.texture
                        ? ctx->pipe_framebuffer.zsbuf.format : PIPE_FORMAT_NONE);

   for (unsigned i = 0; i < num_buffers; ++i) {
      const struct pipe_vertex_buffer *vb = &saved_buffers[i];
      const void *map = NULL;
      size_t size = 0;
      if (vb->is_user_buffer) {
         map = vb->buffer.user;
         size = SIZE_MAX;
      } else if (vb->buffer.resource) {
         size = vb->buffer.resource->width0;
         map = pipe_buffer_map(pipe, vb->buffer.resource, PIPE_MAP_READ, &vb_maps[i]);
         if (!map)
            goto cleanup;
      }
      draw_set_mapped_vertex_buffer(sw->draw, i, map, size);
      vertex_maps[i] = map;
      vertex_map_sizes[i] = size;
   }
   if (info->index_size) {
      const void *map;
      unsigned size;
      if (info->has_user_indices) {
         map = info->index.user;
         size = ~0u;
      } else {
         if (!info->index.resource)
            goto cleanup;
         map = pipe_buffer_map(pipe, info->index.resource, PIPE_MAP_READ, &ib_map);
         size = info->index.resource->width0;
      }
      if (!map)
         goto cleanup;
      draw_set_indexes(sw->draw, map, info->index_size, size);
      index_map = map;
      index_map_size = size;
   } else {
      draw_set_indexes(sw->draw, NULL, 0, 0);
   }
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; ++i) {
      const struct pipe_constant_buffer *cb = &ctx->constant_buffer[MESA_SHADER_VERTEX].cb[i];
      const void *map = NULL;
      unsigned size = cb->buffer_size;
      if (size && cb->buffer) {
         if (cb->buffer_offset > cb->buffer->width0 ||
             size > cb->buffer->width0 - cb->buffer_offset)
            goto cleanup;
         map = pipe_buffer_map_range(pipe, cb->buffer, cb->buffer_offset, size,
                                      PIPE_MAP_READ, &cb_maps[i]);
         if (!map)
            goto cleanup;
      } else if (size && cb->user_buffer) {
         map = (const uint8_t *)cb->user_buffer + cb->buffer_offset;
      } else {
         size = 0;
      }
      draw_set_mapped_constant_buffer(sw->draw, MESA_SHADER_VERTEX, i, map, size);
   }

   if (sw->trace_draw) {
      for (unsigned b = 0; b < num_buffers; ++b) {
         fprintf(stderr, "HAIKU_PAN_GEOMETRY_BUFFER draw=%llu buffer=%u offset=%u bytes=%zu user=%u\n",
                 (unsigned long long)sw->draws, b,
                 saved_buffers[b].buffer_offset, vertex_map_sizes[b],
                 saved_buffers[b].is_user_buffer);
      }
      fprintf(stderr, "HAIKU_PAN_GEOMETRY_INPUT draw=%llu mode=%u start=%u count=%u index_size=%u bias=%d front=%u back=%u cull=%u ccw=%u first=%u clip=%u elements=%u\n",
              (unsigned long long)sw->draws, info->mode, draws[0].start,
              draws[0].count, info->index_size, draws[0].index_bias,
              rasterizer->fill_front, rasterizer->fill_back,
              rasterizer->cull_face, rasterizer->front_ccw,
              rasterizer->flatshade_first, rasterizer->clip_plane_enable, num_elements);
      for (unsigned a = 0; a < num_elements; ++a) {
         const struct pipe_vertex_element *e = &elements[a];
         unsigned b = e->vertex_buffer_index;
         fprintf(stderr, "HAIKU_PAN_GEOMETRY_ELEMENT draw=%llu element=%u buffer=%u offset=%u stride=%u format=%u divisor=%u\n",
                 (unsigned long long)sw->draws, a, b, e->src_offset,
                 e->src_stride, e->src_format, e->instance_divisor);
         if (b >= num_buffers || !vertex_maps[b])
            continue;
         for (unsigned i = 0; i < draws[0].count; ++i) {
            int64_t index = draws[0].start + i;
            if (info->index_size) {
               uint64_t index_offset = (uint64_t)index * info->index_size;
               if (info->index_size > sizeof(uint32_t) || index_offset > index_map_size ||
                   info->index_size > index_map_size - index_offset)
                  continue;
               uint32_t value = 0;
               memcpy(&value, (const char *)index_map + index_offset,
                      info->index_size);
               index = (int64_t)value + draws[0].index_bias;
            }
            if (index < 0 || e->instance_divisor)
               continue;
            uint64_t offset = saved_buffers[b].buffer_offset + e->src_offset +
                              (uint64_t)index * e->src_stride;
            unsigned bytes = MIN2(util_format_get_blocksize(e->src_format), 16);
            if (offset > vertex_map_sizes[b] || bytes > vertex_map_sizes[b] - offset)
               continue;
            const uint8_t *data = (const uint8_t *)vertex_maps[b] + offset;
            fprintf(stderr, "HAIKU_PAN_GEOMETRY_ATTRIBUTE draw=%llu element=%u vertex=%u index=%lld bytes=",
                    (unsigned long long)sw->draws, a, i, (long long)index);
            for (unsigned j = 0; j < bytes; ++j)
               fprintf(stderr, "%02x", data[j]);
            fprintf(stderr, "\n");
         }
      }
      if (!sw->trace_shader_valid || memcmp(sw->trace_vs_sha1, sw->vs_sha1, 20)) {
         fprintf(stderr, "HAIKU_PAN_GEOMETRY_SHADER_BEGIN draw=%llu\n", (unsigned long long)sw->draws);
         nir_print_shader(((struct panfrost_uncompiled_shader *)saved_vs)->sw_polygon_nir, stderr);
         fprintf(stderr, "HAIKU_PAN_GEOMETRY_SHADER_END draw=%llu\n", (unsigned long long)sw->draws);
         memcpy(sw->trace_vs_sha1, sw->vs_sha1, 20);
         sw->trace_shader_valid = true;
      }
   }

   struct pipe_rasterizer_state native = *rasterizer;
   native.fill_front = native.fill_back = PIPE_POLYGON_MODE_FILL;
   native.cull_face = PIPE_FACE_NONE;
   native.light_twoside = false;
   native.offset_line = native.offset_point = native.offset_tri = false;
   native.clip_plane_enable = 0;
   native_rasterizer = pipe->create_rasterizer_state(pipe, &native);
   if (!native_rasterizer)
      goto cleanup;
   pipe->bind_rasterizer_state(pipe, native_rasterizer);
   pipe->bind_vertex_elements_state(pipe, sw->native_elements);
   pipe->bind_vs_state(pipe, sw->native_vs);
   rebound = true;
   draw_vbo(sw->draw, info, drawid_offset, NULL, draws, num_draws, 0);
   draw_flush(sw->draw);
   success = !sw->failed;
   if (sw->trace_draw)
      fprintf(stderr, "HAIKU_PAN_GEOMETRY_END draw=%llu batches=%llu vertices=%llu success=%u\n",
              (unsigned long long)sw->draws, (unsigned long long)sw->batches,
              (unsigned long long)sw->emitted_vertices, success);

cleanup:
   sw->vertex_count = 0;
   if (rebound) {
      pipe->bind_rasterizer_state(pipe, saved_rasterizer);
      pipe->bind_vertex_elements_state(pipe, saved_elements);
      pipe->set_vertex_buffers(pipe, num_buffers, saved_buffers);
      pipe->bind_vs_state(pipe, saved_vs);
   }
   if (native_rasterizer)
      pipe->delete_rasterizer_state(pipe, native_rasterizer);
   draw_set_indexes(sw->draw, NULL, 0, 0);
   if (ib_map)
      pipe_buffer_unmap(pipe, ib_map);
   for (unsigned i = 0; i < PIPE_MAX_ATTRIBS; ++i) {
      draw_set_mapped_vertex_buffer(sw->draw, i, NULL, 0);
      if (vb_maps[i])
         pipe_buffer_unmap(pipe, vb_maps[i]);
   }
   draw_set_vertex_buffers(sw->draw, 0, NULL);
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; ++i) {
      draw_set_mapped_constant_buffer(sw->draw, MESA_SHADER_VERTEX, i, NULL, 0);
      if (cb_maps[i])
         pipe_buffer_unmap(pipe, cb_maps[i]);
   }
   for (unsigned i = 0; i < num_buffers; ++i)
      if (!saved_buffers[i].is_user_buffer)
         pipe_resource_reference(&saved_buffers[i].buffer.resource, NULL);

   /* Internal line batches are implementation details, not application draws.
    * Preserve this driver's existing direct-draw primitive-query convention.
    */
   ctx->prims_generated = saved_primitives;
   if (success && ctx->active_queries)
      for (unsigned i = 0; i < num_draws; ++i)
         ctx->prims_generated += u_prims_for_vertices(info->mode, draws[i].count);
   ctx->draw_calls = saved_draw_calls + 1;
   ctx->sw_polygon_active = false;
   if (!success)
      sw->failures++;
   return success;
}
