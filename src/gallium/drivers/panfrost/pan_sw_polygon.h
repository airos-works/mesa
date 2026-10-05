/* SPDX-License-Identifier: MIT */
#ifndef PAN_SW_POLYGON_H
#define PAN_SW_POLYGON_H

#include "util/compiler.h"
#include "compiler/shader_enums.h"

struct panfrost_context;
struct pipe_draw_info;
struct pipe_draw_indirect_info;
struct pipe_draw_start_count_bias;
struct pipe_rasterizer_state;
struct pipe_vertex_element;

/* Keep legacy polygon boundaries intact until the draw pipeline handles
 * clipping, culling and edge flags. These modes must use it even when filled:
 * advertising them would otherwise send unsupported primitives to Valhall. */
static inline bool
panfrost_sw_polygon_legacy_topology(enum mesa_prim mode)
{
   return mode == MESA_PRIM_QUADS || mode == MESA_PRIM_QUAD_STRIP ||
          mode == MESA_PRIM_POLYGON;
}

bool panfrost_sw_polygon_enabled(void);
bool panfrost_sw_polygon_draw(struct panfrost_context *ctx,
                             const struct pipe_rasterizer_state *rasterizer,
                             unsigned num_elements,
                             const struct pipe_vertex_element *elements,
                             const struct pipe_draw_info *info,
                             unsigned drawid_offset,
                             const struct pipe_draw_indirect_info *indirect,
                             const struct pipe_draw_start_count_bias *draws,
                             unsigned num_draws);
void panfrost_sw_polygon_destroy(struct panfrost_context *ctx);

#endif
