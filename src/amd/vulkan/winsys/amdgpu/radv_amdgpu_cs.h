/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based on amdgpu winsys.
 * Copyright © 2011 Marek Olšák <maraeo@gmail.com>
 * Copyright © 2015 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_AMDGPU_CS_H
#define RADV_AMDGPU_CS_H

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "util/simple_mtx.h"
#include "radv_amdgpu_winsys.h"
#include "radv_radeon_winsys.h"

enum { MAX_RINGS_PER_TYPE = 8 };

struct radv_amdgpu_fence {
   struct amdgpu_cs_fence fence;
};

struct radv_amdgpu_ctx {
   struct radv_amdgpu_winsys *ws;
   uint32_t ctx_handle;
   struct radv_amdgpu_fence last_submission[AMDGPU_HW_IP_NUM + 1][MAX_RINGS_PER_TYPE];

   struct radeon_winsys_bo *fence_bo;

   uint32_t queue_syncobj[AMDGPU_HW_IP_NUM + 1][MAX_RINGS_PER_TYPE];
   bool queue_syncobj_wait[AMDGPU_HW_IP_NUM + 1][MAX_RINGS_PER_TYPE];

#ifdef __HAIKU__
   /* A command stream of NOPs per IP, for submissions without command buffers that have to wait
    * (radv_amdgpu_cs_submit_nop): made when first needed, under nop_cs_lock. */
   simple_mtx_t nop_cs_lock;
   struct ac_cmdbuf *nop_cs[AMDGPU_HW_IP_NUM + 1];
#endif
};

static inline struct radv_amdgpu_ctx *
radv_amdgpu_ctx(struct radeon_winsys_ctx *base)
{
   return (struct radv_amdgpu_ctx *)base;
}

void radv_amdgpu_cs_init_functions(struct radv_amdgpu_winsys *ws);

#endif /* RADV_AMDGPU_CS_H */
