/*
 * Presenting a finished frame straight into the screen on Haiku with a
 * Gallium driver on amdgpu_drm (radeonsi), for a program that composites
 * with OpenGL ES through EGL (Summit's web process); what zink_haiku_present
 * does through Vulkan.
 *
 * The frame is copied by the GPU into the rectangles of the screen the
 * window system says are visible, in the frame buffer the card scans out
 * (amdgpu_drm's DRM_HAIKU_IOCTL_SCANOUT, imported as a linear texture), and
 * nothing is read back to the processor. The window system's own copy of
 * the screen is not written: a present that names it is refused (-4), and
 * the program keeps that copy current by other frames.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HAIKU_DRM_PRESENT_H
#define HAIKU_DRM_PRESENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;
struct pipe_resource;
struct pipe_screen;

#ifndef SUMMIT_HAIKU_PRESENT_VERSION
/* As in zink_haiku_present.h, which has the details. */
#define SUMMIT_HAIKU_PRESENT_VERSION 1

struct summit_haiku_present {
   uint32_t version;
   int32_t origin_x, origin_y;
   uint32_t rect_count;
   const int32_t *rects;
   uint32_t front_bytes_per_row;
   void *back_bits;
   uint32_t back_bytes_per_row;
   uint32_t back_width, back_height;
   uint32_t frames_in_flight;
   void (*completed)(void *cookie);
   void *cookie;
};
#endif

/* Returns 0 when the copies were submitted, else as zink_haiku_present():
 * -1 bad arguments, -3 no frame buffer to be had or not the one described,
 * -4 the window system's copy was named, -5 nothing visible. */
int haiku_drm_present(struct pipe_context *pipe, struct pipe_resource *image,
                      const struct summit_haiku_present *present);

/* Waits until every present submitted so far has finished on the GPU. */
void haiku_drm_present_wait_idle(void);

/* Waits for the GPU, then lets go of the frame buffer, for the given screen
 * or any (NULL). Presenting again takes it again. */
void haiku_drm_present_release(struct pipe_screen *screen);

#ifdef __cplusplus
}
#endif

#endif /* HAIKU_DRM_PRESENT_H */
