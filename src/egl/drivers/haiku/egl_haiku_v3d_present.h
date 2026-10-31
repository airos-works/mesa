/*
 * Copyright 2026, air/OS. SPDX-License-Identifier: MIT
 *
 * Summit's direct present on the V3D (egl_haiku_v3d_present.c).
 */
#ifndef EGL_HAIKU_V3D_PRESENT_H
#define EGL_HAIKU_V3D_PRESENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_resource;
struct pipe_screen;
struct st_context;

/* As Summit's Mesa patches 06 and 07 have it for Zink (zink_haiku_present.h,
 * the struct summit_haiku_present Summit's web process passes). Version 1
 * is taken, and version 2 without flags: there is no deferred present here
 * (summit_haiku_present_version() says 1). */
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
   /* version 2 */
   uint32_t flags;
   int (*before_copy)(void *cookie, uint32_t attempt);
};

/* Copies the visible rectangles of image, the colour attachment of the
 * framebuffer object the caller composited into, into rpi_display's frame
 * buffer and app_server's copy of the screen. The results of
 * summit_haiku_present_framebuffer(): 0 when the copies were submitted, -1
 * bad arguments, -2 an image the copy cannot take, -3 no frame buffer to be
 * had or not the one described, -4 app_server's copy could not be imported,
 * -5 nothing visible, -10 not a V3D that imports memory. */
int haiku_v3d_present(struct st_context *st, struct pipe_resource *image,
                      const struct summit_haiku_present *present);
void haiku_v3d_present_wait_idle(void);
/* Waits for the GPU, then lets go of both targets (of the screen's, or of
 * any for NULL). */
void haiku_v3d_present_release(struct pipe_screen *screen);

#ifdef __cplusplus
}
#endif

#endif
