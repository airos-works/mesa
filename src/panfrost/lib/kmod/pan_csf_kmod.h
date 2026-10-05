/* SPDX-License-Identifier: MIT */
#pragma once
#include "pan_kmod.h"
#include "drm-uapi/panthor_drm.h"
#ifdef __HAIKU__
#include "haiku_kmod.h"
#define pan_csf_bo_attach_sync_point haiku_kmod_bo_attach_sync_point
#define pan_csf_bo_get_sync_point haiku_kmod_bo_get_sync_point
#define pan_csf_vm_sync_handle haiku_kmod_vm_sync_handle
#define pan_csf_vm_sync_lock haiku_kmod_vm_sync_lock
#define pan_csf_vm_sync_unlock haiku_kmod_vm_sync_unlock
#define pan_csf_get_flush_id haiku_kmod_get_flush_id
#define pan_csf_get_csif_props haiku_kmod_get_csif_props
#define pan_csf_group_create haiku_kmod_group_create
#define pan_csf_group_destroy haiku_kmod_group_destroy
#define pan_csf_group_submit haiku_kmod_group_submit
#define pan_csf_group_get_state haiku_kmod_group_get_state
#define pan_csf_heap_create haiku_kmod_heap_create
#define pan_csf_heap_destroy haiku_kmod_heap_destroy
#else
#include "panthor_kmod.h"
#define pan_csf_bo_attach_sync_point panthor_kmod_bo_attach_sync_point
#define pan_csf_bo_get_sync_point panthor_kmod_bo_get_sync_point
#define pan_csf_vm_sync_handle panthor_kmod_vm_sync_handle
#define pan_csf_vm_sync_lock panthor_kmod_vm_sync_lock
#define pan_csf_vm_sync_unlock panthor_kmod_vm_sync_unlock
#define pan_csf_get_flush_id panthor_kmod_get_flush_id
#define pan_csf_get_csif_props panthor_kmod_get_csif_props
static inline int pan_csf_group_create(struct pan_kmod_dev *dev, struct drm_panthor_group_create *req)
{ return pan_kmod_ioctl(dev->fd, DRM_IOCTL_PANTHOR_GROUP_CREATE, req); }
static inline int pan_csf_group_destroy(struct pan_kmod_dev *dev, struct drm_panthor_group_destroy *req)
{ return pan_kmod_ioctl(dev->fd, DRM_IOCTL_PANTHOR_GROUP_DESTROY, req); }
static inline int pan_csf_group_submit(struct pan_kmod_dev *dev, struct drm_panthor_group_submit *req)
{ return pan_kmod_ioctl(dev->fd, DRM_IOCTL_PANTHOR_GROUP_SUBMIT, req); }
static inline int pan_csf_group_get_state(struct pan_kmod_dev *dev, struct drm_panthor_group_get_state *req)
{ return pan_kmod_ioctl(dev->fd, DRM_IOCTL_PANTHOR_GROUP_GET_STATE, req); }
static inline int pan_csf_heap_create(struct pan_kmod_dev *dev, struct drm_panthor_tiler_heap_create *req)
{ return pan_kmod_ioctl(dev->fd, DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE, req); }
static inline int pan_csf_heap_destroy(struct pan_kmod_dev *dev, struct drm_panthor_tiler_heap_destroy *req)
{ return pan_kmod_ioctl(dev->fd, DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY, req); }
#endif
