/*
 * Presenting a finished frame straight into the screen on Haiku, for a
 * program that composites with OpenGL ES through EGL (Summit's web process).
 *
 * The frame is copied with the GPU into the rectangles of the screen the
 * window system says are visible: into the frame buffer the card scans out
 * (video memory, imported through NVK's VkImportScanoutMemoryHAIKU) and into
 * the window system's own copy of the screen, which it draws into first and
 * copies to the frame buffer (a direct window with B_DIRECT_DEVICE_PIXELS is
 * handed it as drawing_bits_area). Nothing is read back to the processor.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZINK_HAIKU_PRESENT_H
#define ZINK_HAIKU_PRESENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_context;
struct pipe_resource;

#ifndef SUMMIT_HAIKU_PRESENT_VERSION
/* also in haiku_drm_present.h (EGL Haiku, radeonsi) */
#define SUMMIT_HAIKU_PRESENT_VERSION 1

struct summit_haiku_present {
   uint32_t version;              /* SUMMIT_HAIKU_PRESENT_VERSION */
   /* Where the image's top left texel goes, in frame buffer pixels. */
   int32_t origin_x, origin_y;
   /* What to copy, in frame buffer pixels, both edges included (as
    * clipping_rect): left, top, right, bottom for each. Already clipped to
    * what is visible; parts outside the image or the screen are dropped. */
   uint32_t rect_count;
   const int32_t *rects;
   /* The frame buffer's row length, to recognise it; 0 skips it. */
   uint32_t front_bytes_per_row;
   /* The window system's copy of the screen as mapped in this team (page
    * aligned), or NULL. Its pages stay locked for the GPU from the first
    * present that names them until one names another copy: the mapping must
    * not go away in between, or the system can hang. */
   void *back_bits;
   uint32_t back_bytes_per_row;
   uint32_t back_width, back_height;
   /* Presents that may still be running on the GPU when another starts;
    * the call waits for older ones. 0 means 2. */
   uint32_t frames_in_flight;
   /* Called on a helper thread once the GPU has finished this present's
    * copies (or at once if there was nothing to copy). May be NULL. */
   void (*completed)(void *cookie);
   void *cookie;
};
#endif

/* Returns 0 when the copies were submitted, a negative number when this
 * present cannot go to the screen (the caller then presents some other way):
 * -1 bad arguments, -2 unsupported image format, -3 no frame buffer to be had
 * or it is not the one described, -4 the window system's copy could not be
 * imported, -5 nothing visible. */
int zink_haiku_present(struct pipe_context *pipe, struct pipe_resource *image,
                       const struct summit_haiku_present *present);

/* Waits until every present submitted so far has finished on the GPU. */
void zink_haiku_present_wait_idle(void);

/* Waits for the GPU, then lets go of the frame buffer and of the window
 * system's copy (unlocking its pages), for the given screen or any (NULL).
 * Call before the memory or the program goes away: a copy still writing
 * host memory while the driver tears the import down has hung the X399.
 * Presenting again imports them again. */
struct pipe_screen;
void zink_haiku_present_release(struct pipe_screen *screen);

#ifdef __cplusplus
}
#endif

#endif /* ZINK_HAIKU_PRESENT_H */
