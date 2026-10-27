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
#endif

#endif
