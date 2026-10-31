/* Copyright 2026, air/OS. SPDX-License-Identifier: MIT */
#ifndef V3D_HAIKU_CACHE_H
#define V3D_HAIKU_CACHE_H

#ifdef __HAIKU__
/* Private ownership API, version 2. Query before using the allocation flag.
 * Zero CPU_PREPARE flags permit writes; READ_ONLY avoids a redundant clean.
 * A prepare is required for each CPU interval, even with an existing map.
 */
#define HAIKU_V3D_CPU_PREPARE (HAIKU_V3D_SYNC_TRANSFER + 1)
#define HAIKU_V3D_PARAM_CACHEABLE_BO 0x10000
#define HAIKU_V3D_BO_CACHEABLE 0x80000000u
#define HAIKU_V3D_CPU_READ_ONLY 0x1

/* Imports (air/OS haiku#49): pages of this team as a buffer object, and
 * CPU_PREPARE for some bytes only (headers/private/graphics/v3d/v3d_haiku.h).
 */
#define HAIKU_V3D_IMPORT_BO (HAIKU_V3D_SYNC_TRANSFER + 2)
#define HAIKU_V3D_CPU_PREPARE_RANGES (HAIKU_V3D_SYNC_TRANSFER + 3)
#define HAIKU_V3D_PARAM_IMPORT_BO 0x10001
#define HAIKU_V3D_IMPORT_CACHED 0x1
#define HAIKU_V3D_RANGES_NO_WAIT 0x2

struct haiku_v3d_import_bo {
   uint64_t address;
   uint64_t size;
   uint32_t flags;
   uint32_t handle;
   uint32_t offset;
   uint32_t pad;
};

struct haiku_v3d_cpu_ranges {
   uint32_t handle;
   uint32_t flags;
   uint64_t ranges;   /* struct v3d_haiku_byte_range[count] */
   uint32_t count;
   uint32_t pad;
};
#endif

#endif
