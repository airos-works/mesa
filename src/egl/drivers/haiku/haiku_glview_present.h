/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Between BGLView (libGL) and the EGL driver that renders for it (Mesa's
 * EGL Haiku driver, libEGL_mesa), for putting a frame straight into the
 * screen when the view is in a direct window.
 *
 * BGLView knows where its window is on the screen and which parts of it can
 * be seen (BDirectWindow::DirectConnected); the EGL driver has the frame. At
 * each swap the driver asks the view where the frame may go, copies it there
 * with the GPU, and says what it did. The window is held where it is from the
 * question to the answer, and the view does not let the window system move
 * it again until the GPU has finished the copies.
 *
 * libGL exports haiku_glview_api(); the driver finds it with dlsym(), so an
 * older libGL (or another BitmapHook) simply means no direct presents. The
 * same file is in libglvnd (src/HGL) and in Mesa (src/egl/drivers/haiku);
 * keep the two alike.
 */
#ifndef HAIKU_GLVIEW_PRESENT_H
#define HAIKU_GLVIEW_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAIKU_GLVIEW_API_VERSION 1
#define HAIKU_GLVIEW_MAX_RECTS 64

/* Where a frame goes, in frame buffer pixels. */
struct haiku_glview_target {
	/* the frame's top left */
	int32_t origin_x, origin_y;
	/* the size it covers on the screen: larger than the frame on a desktop
	 * drawn at a higher density, where it is enlarged on the way, filtered
	 * unless the scale is a whole number */
	uint32_t dest_width, dest_height;
	uint32_t filter;
	/* the frame buffer's row length, to recognise it */
	uint32_t front_bytes_per_row;
	/* the visible parts: left, top, right, bottom each, both edges
	 * included, within the frame's place */
	uint32_t rect_count;
	int32_t rects[HAIKU_GLVIEW_MAX_RECTS * 4];
};

/* What a present did, for presented(). */
enum {
	HAIKU_GLVIEW_NOT_PRESENTED = 0,
	/* the copies are on the GPU; completed() follows once they are done */
	HAIKU_GLVIEW_PRESENTED_ASYNC = 1,
	/* the copies are done */
	HAIKU_GLVIEW_PRESENTED = 2,
};

struct haiku_glview_api {
	uint32_t version;

	/* The driver's surface for the view whose BitmapHook is window, and how
	 * to put its last frame back into the screen: represent() is called
	 * with the view's context current on the calling thread (the view takes
	 * its GL lock first), when the window is uncovered after the program
	 * stopped drawing. Returns false for a BitmapHook that is not a
	 * BGLView's. */
	bool (*attach)(void *window, void *surface,
		void (*represent)(void *surface));
	/* The surface is going away; nothing is presented for it after this. */
	void (*detach)(void *window);

	/* At a swap, with interval the surface's swap interval: whether the
	 * frame can go into the screen, and where. Waits for the display's
	 * blank first when interval is above 0. True holds the window where it
	 * is until presented(). */
	bool (*target)(void *window, int interval,
		struct haiku_glview_target *target);
	/* After target() returned true. */
	void (*presented)(void *window, int result);
	/* The GPU finished the copies of a HAIKU_GLVIEW_PRESENTED_ASYNC
	 * present. Any thread. */
	void (*completed)(void *window);
};

/* Exported by libGL. */
const struct haiku_glview_api *haiku_glview_api(void);

#ifdef __cplusplus
}
#endif

#endif /* HAIKU_GLVIEW_PRESENT_H */
