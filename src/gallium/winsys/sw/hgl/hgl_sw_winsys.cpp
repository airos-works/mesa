/**************************************************************************
 *
 * Copyright 2009 Artur Wyszynski <harakash@gmail.com>
 * Copyright 2013-2014 Alexander von Gluck IV <kallisti5@unixzen.com>
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

#include <stdio.h>

#include "pipe/p_compiler.h"
#include "pipe/p_defines.h"
#include "pipe/p_format.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "frontend/api.h"
#include "frontend/sw_winsys.h"

#include "hgl_sw_winsys.h"

#include <Bitmap.h>
#include <OS.h>

#include <new>
#include <string.h>

#include "c11/threads.h"

#ifdef DEBUG
#   define TRACE(x...) printf("hgl:winsys: " x)
#   define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#   define TRACE(x...)
#   define CALLED()
#endif
#define ERROR(x...) printf("hgl:winsys: " x)


struct haiku_displaytarget
{
	enum pipe_format format;
	color_space colorSpace;

	unsigned width;
	unsigned height;
	unsigned stride;

	unsigned size;

	void* data;
	BBitmap* bitmap;
	// The memory behind the bitmap, when it has an area of its own.
	area_id area;
};


// Display targets are freed a little after the renderer lets go of them. A
// window's thread may still be showing the last frame of one (a redraw while
// the application resizes), and it holds no reference the renderer knows of;
// two resizes later nobody is.
#define HGL_DEAD_TARGETS 4
static mtx_t sDeadTargetsLock = _MTX_INITIALIZER_NP;
static struct haiku_displaytarget* sDeadTargets[HGL_DEAD_TARGETS];
static int sDeadTargetCount;


// Cast
static inline struct haiku_displaytarget*
hgl_sw_displaytarget(struct sw_displaytarget* target)
{
	return (struct haiku_displaytarget *)target;
}


static void
hgl_winsys_destroy(struct sw_winsys* winsys)
{
	FREE(winsys);
}


static bool
hgl_winsys_is_displaytarget_format_supported(struct sw_winsys* winsys,
	unsigned textureUsage, enum pipe_format format)
{
	// TODO STUB
	return true;
}

static color_space
hgl_winsys_convert_cs(enum pipe_format format)
{
	// TODO: B_RGB24, B_RGB16, B_RGB15?
	switch(format) {
		case PIPE_FORMAT_B5G6R5_UNORM:
			return B_CMAP8;
		case PIPE_FORMAT_A8B8G8R8_UNORM:
		case PIPE_FORMAT_X8B8G8R8_UNORM:
		default:
			return B_RGB32;
	}
}

static struct sw_displaytarget*
hgl_winsys_displaytarget_create(struct sw_winsys* winsys,
	unsigned textureUsage, enum pipe_format format, unsigned width,
	unsigned height, unsigned alignment, const void *front_private,
	unsigned* stride)
{
	struct haiku_displaytarget* haikuDisplayTarget
		= CALLOC_STRUCT(haiku_displaytarget);
	assert(haikuDisplayTarget);

	TRACE("%s: %d x %d\n", __func__, width, height);

	haikuDisplayTarget->colorSpace = hgl_winsys_convert_cs(format);
	haikuDisplayTarget->format = format;
	haikuDisplayTarget->width = width;
	haikuDisplayTarget->height = height;

	size_t formatStride = util_format_get_stride(format, width);
	unsigned blockSize = util_format_get_nblocksy(format, height);

	haikuDisplayTarget->stride = align(formatStride, alignment);
	haikuDisplayTarget->size = haikuDisplayTarget->stride * blockSize;

	haikuDisplayTarget->area = -1;
	if (textureUsage & PIPE_BIND_DISPLAY_TARGET) {
		haikuDisplayTarget->data = NULL;
		// Each display target gets an area of its own, so that its pixels
		// start on a page: a GPU can then be given the memory and copy the
		// frame into it (zink's present into host memory). The window
		// system's own allocator packs bitmaps one after another, so all but
		// the first of a window's two buffers started part way into a page
		// unless the frame happened to be a whole number of pages, and those
		// frames were read back and copied by the processor instead.
		//
		// Ask for the rows to be as long as the stride we report, so that a
		// driver copying into the mapped bitmap with that stride stays inside
		// it.
		const BRect bounds(0, 0, width - 1, height - 1);
		void* address = NULL;
		area_id area = create_area("hgl display target", &address,
			B_ANY_ADDRESS,
			align((size_t)haikuDisplayTarget->size, B_PAGE_SIZE),
			B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (area >= 0) {
			BBitmap* bitmap = new(std::nothrow) BBitmap(area, 0, bounds, 0,
				haikuDisplayTarget->colorSpace, haikuDisplayTarget->stride);
			if (bitmap != NULL && bitmap->InitCheck() == B_OK
				&& bitmap->BytesPerRow() == (int32)haikuDisplayTarget->stride) {
				haikuDisplayTarget->bitmap = bitmap;
				haikuDisplayTarget->area = area;
			} else {
				delete bitmap;
				delete_area(area);
			}
		}
		if (haikuDisplayTarget->bitmap == NULL) {
			// The window system's own memory, then; then report what the
			// bitmap actually settled on.
			haikuDisplayTarget->bitmap = new BBitmap(bounds, 0,
				haikuDisplayTarget->colorSpace,
				haikuDisplayTarget->stride);
		}
		haikuDisplayTarget->stride = haikuDisplayTarget->bitmap->BytesPerRow();
		haikuDisplayTarget->size = haikuDisplayTarget->stride * blockSize;
	} else {
		haikuDisplayTarget->data
			= align_malloc(haikuDisplayTarget->size, alignment);

		haikuDisplayTarget->bitmap = NULL;
	}

	*stride = haikuDisplayTarget->stride;

	// Cast to ghost sw_displaytarget type
	return (struct sw_displaytarget*)haikuDisplayTarget;
}


static void
hgl_winsys_displaytarget_destroy(struct sw_winsys* winsys,
	struct sw_displaytarget* displayTarget)
{
	struct haiku_displaytarget* haikuDisplayTarget
		= hgl_sw_displaytarget(displayTarget);

	if (!haikuDisplayTarget)
		return;

	if (haikuDisplayTarget->bitmap != NULL) {
		// Kept for a while (see sDeadTargets); the oldest one goes now.
		mtx_lock(&sDeadTargetsLock);
		struct haiku_displaytarget* oldest = NULL;
		if (sDeadTargetCount == HGL_DEAD_TARGETS) {
			oldest = sDeadTargets[0];
			memmove(&sDeadTargets[0], &sDeadTargets[1],
				sizeof(sDeadTargets[0]) * (HGL_DEAD_TARGETS - 1));
			sDeadTargetCount--;
		}
		sDeadTargets[sDeadTargetCount++] = haikuDisplayTarget;
		mtx_unlock(&sDeadTargetsLock);
		haikuDisplayTarget = oldest;
		if (haikuDisplayTarget == NULL)
			return;
	}

	if (haikuDisplayTarget->data != NULL)
		align_free(haikuDisplayTarget->data);

	if (haikuDisplayTarget->bitmap != NULL)
		delete haikuDisplayTarget->bitmap;
	// The bitmap does not own an area it was given; the window system keeps
	// its own clone of it for as long as it still needs the pixels.
	if (haikuDisplayTarget->area >= 0)
		delete_area(haikuDisplayTarget->area);

	FREE(haikuDisplayTarget);
}


static struct sw_displaytarget*
hgl_winsys_displaytarget_from_handle(struct sw_winsys* winsys,
	const struct pipe_resource* templat, struct winsys_handle* whandle,
	unsigned* stride)
{
	return NULL;
}


static bool
hgl_winsys_displaytarget_get_handle(struct sw_winsys* winsys,
	struct sw_displaytarget* displayTarget, struct winsys_handle* whandle)
{
	return false;
}


static void*
hgl_winsys_displaytarget_map(struct sw_winsys* winsys,
	struct sw_displaytarget* displayTarget, unsigned flags)
{
	struct haiku_displaytarget* haikuDisplayTarget
		= hgl_sw_displaytarget(displayTarget);

	if (haikuDisplayTarget->bitmap != NULL)
		return haikuDisplayTarget->bitmap->Bits();

	return haikuDisplayTarget->data;
}


static void
hgl_winsys_displaytarget_unmap(struct sw_winsys* winsys,
	struct sw_displaytarget* displayTarget)
{
	return;
}


static void
hgl_winsys_displaytarget_display(struct sw_winsys* winsys,
	struct sw_displaytarget* displayTarget, void* contextPrivate,
	struct pipe_box *box)
{
	assert(contextPrivate);

	struct haiku_displaytarget* haikuDisplayTarget
		= hgl_sw_displaytarget(displayTarget);

	HGLWinsysContext *context = (HGLWinsysContext*)contextPrivate;
	context->Display(haikuDisplayTarget->bitmap, NULL);
}


extern "C" bool
hgl_winsys_scanout_target(void* contextPrivate,
	struct hgl_scanout_target* target)
{
	if (contextPrivate == NULL)
		return false;

	HGLWinsysContext *context = (HGLWinsysContext*)contextPrivate;
	return context->ScanoutTarget(target);
}


extern "C" void
hgl_winsys_scanout_presented(void* contextPrivate, bool presented)
{
	if (contextPrivate == NULL)
		return;

	HGLWinsysContext *context = (HGLWinsysContext*)contextPrivate;
	context->PresentedIntoScanout(presented);
}


struct sw_winsys*
hgl_create_sw_winsys()
{
	struct sw_winsys* winsys = CALLOC_STRUCT(sw_winsys);

	if (!winsys)
		return NULL;

	// Attach winsys hooks for Haiku
	winsys->destroy = hgl_winsys_destroy;
	winsys->is_displaytarget_format_supported
		= hgl_winsys_is_displaytarget_format_supported;
	winsys->displaytarget_create = hgl_winsys_displaytarget_create;
	winsys->displaytarget_from_handle = hgl_winsys_displaytarget_from_handle;
	winsys->displaytarget_get_handle = hgl_winsys_displaytarget_get_handle;
	winsys->displaytarget_map = hgl_winsys_displaytarget_map;
	winsys->displaytarget_unmap = hgl_winsys_displaytarget_unmap;
	winsys->displaytarget_display = hgl_winsys_displaytarget_display;
	winsys->displaytarget_destroy = hgl_winsys_displaytarget_destroy;

	return winsys;
}
