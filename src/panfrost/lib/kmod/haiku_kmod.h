/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "drm-uapi/panthor_drm.h"
#include "haiku_csf_native.h"
struct pan_kmod_dev;
struct pan_kmod_vm;
struct pan_kmod_bo;
struct pan_kmod_allocator;
struct pan_kmod_dev *haiku_kmod_dev_create(int, uint32_t, const struct pan_kmod_allocator *);
void *haiku_kmod_bo_mmap(struct pan_kmod_bo *, off_t, size_t, int, int, void *);
int haiku_kmod_bo_munmap(struct pan_kmod_bo *, void *, size_t);
int haiku_kmod_bo_attach_sync_point(struct pan_kmod_bo *, uint32_t, uint64_t, bool);
int haiku_kmod_bo_get_sync_point(struct pan_kmod_bo *, uint32_t *, uint64_t *, bool);
uint32_t haiku_kmod_vm_sync_handle(struct pan_kmod_vm *);
uint64_t haiku_kmod_vm_sync_lock(struct pan_kmod_vm *);
void haiku_kmod_vm_sync_unlock(struct pan_kmod_vm *, uint64_t);
uint32_t haiku_kmod_get_flush_id(const struct pan_kmod_dev *);
const struct drm_panthor_csif_info *haiku_kmod_get_csif_props(const struct pan_kmod_dev *);
int haiku_kmod_group_create(struct pan_kmod_dev *, struct drm_panthor_group_create *);
int haiku_kmod_group_destroy(struct pan_kmod_dev *, struct drm_panthor_group_destroy *);
int haiku_kmod_group_submit(struct pan_kmod_dev *, struct drm_panthor_group_submit *);
int haiku_kmod_group_get_state(struct pan_kmod_dev *, struct drm_panthor_group_get_state *);
int haiku_kmod_group_get_reset_info(struct pan_kmod_dev *, uint32_t,
   struct pan_haiku_reset_info *);
int haiku_kmod_heap_create(struct pan_kmod_dev *, struct drm_panthor_tiler_heap_create *);
int haiku_kmod_heap_destroy(struct pan_kmod_dev *, struct drm_panthor_tiler_heap_destroy *);
