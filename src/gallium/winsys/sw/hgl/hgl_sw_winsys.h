/**************************************************************************
 *
 * Copyright 2009 Artur Wyszynski <harakash@gmail.com>
 * Copyright 2013 Alexander von Gluck IV <kallisti5@unixzen.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/
#ifndef _HGL_SOFTWAREWINSYS_H
#define _HGL_SOFTWAREWINSYS_H

#include <stdbool.h>
#include <stdint.h>

#define HGL_MAX_SCANOUT_CLIPS 32

// Where a window sits on the screen and which parts of it are visible.
//
// A renderer that can write the screen's own frame buffer needs this to put a
// frame in the right place without painting over anything else. Only a window
// in direct mode has one; every other window is presented through the bitmap
// the window system gave us.
struct hgl_scanout_target {
	int32_t origin_x;
	int32_t origin_y;
	uint32_t bytes_per_row;		// of the screen, to check it is the one we hold
	uint32_t num_clips;
	struct {
		// screen coordinates, the right and bottom edges included
		int32_t left, top, right, bottom;
	} clips[HGL_MAX_SCANOUT_CLIPS];
};

#ifdef __cplusplus
class BBitmap;
class BRect;

class HGLWinsysContext {
public:
	virtual void Display(BBitmap *bitmap, BRect *updateRect) = 0;

	// Returning false means present the ordinary way.
	virtual bool ScanoutTarget(struct hgl_scanout_target *target)
		{ (void)target; return false; }

	// Whether the frame just presented went into the screen itself.
	virtual void PresentedIntoScanout(bool presented)
		{ (void)presented; }
};
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct sw_winsys;

struct sw_winsys* hgl_create_sw_winsys(void);

// The drawable handle a frontend passes to flush_frontbuffer is the window
// system context; this asks it where its window is.
bool hgl_winsys_scanout_target(void *contextPrivate,
	struct hgl_scanout_target *target);
void hgl_winsys_scanout_presented(void *contextPrivate, bool presented);

#ifdef __cplusplus
}
#endif

#endif
