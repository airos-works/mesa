/*
 * Copyright 2006-2012, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jérôme Duval, korli@users.berlios.de
 *		Philippe Houdoin, philippe.houdoin@free.fr
 *		Artur Wyszynski, harakash@gmail.com
 *		Alexander von Gluck IV, kallisti5@unixzen.com
 */


#include "ZinkRenderer.h"

#include <Autolock.h>
#include <interface/DirectWindowPrivate.h>
#include <interface/ColorConversion.h>
#include <GraphicsDefs.h>
#include <Screen.h>
#include <stdio.h>
#include <sys/time.h>
#include <new>


#ifdef DEBUG
#	define TRACE(x...) printf("ZinkRenderer: " x)
#	define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#	define TRACE(x...)
#	define CALLED()
#endif
#define ERROR(x...)	printf("ZinkRenderer: " x)


extern const char* color_space_name(color_space space);


extern "C" _EXPORT BGLRenderer*
instantiate_gl_renderer(BGLView *view, ulong opts)
{
	return new ZinkRenderer(view, opts);
}

struct RasBuffer
{
	int32 width, height, stride;
	int32 orgX, orgY;
	void *colors;
	color_space pixel_format;
	int32 pixel_size;	

	RasBuffer(int32 width, int32 height, int32 stride, int32 orgX, int32 orgY, void *colors):
		width(width), height(height), stride(stride), orgX(orgX), orgY(orgY), colors(colors)
	{}

	RasBuffer(BBitmap *bmp)
	{
		width  = bmp->Bounds().IntegerWidth()  + 1;
		height = bmp->Bounds().IntegerHeight() + 1;
		stride = bmp->BytesPerRow();
		orgX   = 0;
		orgY   = 0;
		pixel_format = bmp->ColorSpace();
		pixel_size = stride / width;
		colors = bmp->Bits();
	}

	RasBuffer(direct_buffer_info *info)
	{
		width  = 0x7fffffff;
		height = 0x7fffffff;
		stride = info->bytes_per_row;
		orgX   = 0;
		orgY   = 0;
		pixel_format = info->pixel_format;
		pixel_size = info->bits_per_pixel / 8;
		colors = info->bits;
	}

	void ClipSize(int32 x, int32 y, int32 w, int32 h)
	{
		if (x < 0) {w += x; x = 0;}
		if (y < 0) {h += y; y = 0;}
		if (x + w > width) {w = width  - x;}
		if (y + h > height) {h = height - y;}
		if ((w > 0) && (h > 0)) {
			colors += (y * stride) + (x * pixel_size);
			width  = w;
			height = h;
		} else {
			width = 0; height = 0; colors = NULL;
		}
		if (x + orgX > 0) {orgX += x;} else {orgX = 0;}
		if (y + orgY > 0) {orgY += y;} else {orgY = 0;}
	}

	void ClipRect(int32 l, int32 t, int32 r, int32 b)
	{
		ClipSize(l, t, r - l, b - t);
	}

	void Shift(int32 dx, int32 dy)
	{
		orgX += dx;
		orgY += dy;
	}

	void Blit(RasBuffer src)
	{
		RasBuffer dst = *this;
		int32 x, y;
		x = src.orgX - orgX;
		y = src.orgY - orgY;
		dst.ClipSize(x, y, src.width, src.height);
		src.ClipSize(-x, -y, width, height);
		for (; dst.height > 0; dst.height--) {
			if (src.pixel_format == dst.pixel_format) {
				memcpy(dst.colors, src.colors, src.width * src.pixel_size);
			} else {
				BPrivate::ConvertBits(src.colors, dst.colors, src.stride, dst.stride,
					src.stride, dst.stride, src.pixel_format, dst.pixel_format, src.width, 1);
			}
			dst.colors += dst.stride;
			src.colors += src.stride;
		}
	}
};

ZinkRenderer::ZinkRenderer(BGLView *view, ulong options)
	:
	BGLRenderer(view, options),
	fDirectModeEnabled(false),
	fInfo(NULL),
	fInfoLocker("info locker"),
	fOptions(options),
	fColorSpace(B_NO_COLOR_SPACE),
	fPresentedIntoScanout(false)
{
	CALLED();

	// Initialize the "Haiku Software GL Pipe"
	time_t beg;
	time_t end;
	beg = time(NULL);
	fContextObj = new GalliumContext(options);
	end = time(NULL);
	TRACE("Haiku Software GL Pipe initialization time: %f.\n",
		difftime(end, beg));

	BRect b = view->Bounds();
	fColorSpace = BScreen(view->Window()).ColorSpace();
	TRACE("%s: Colorspace:\t%s\n", __func__, color_space_name(fColorSpace));

	fWidth = (GLint)b.IntegerWidth();
	fHeight = (GLint)b.IntegerHeight();

	// Initialize the first "Haiku Software GL Pipe" context
	beg = time(NULL);
	fContextID = fContextObj->CreateContext(this);
	end = time(NULL);

	if (fContextID < 0)
		ERROR("%s: There was an error creating the context!\n", __func__);
	else {
		TRACE("%s: Haiku Software GL Pipe context creation time: %f.\n",
			__func__, difftime(end, beg));
	}

	if (!fContextObj->GetCurrentContext())
		LockGL();
}


ZinkRenderer::~ZinkRenderer()
{
	CALLED();

	if (fContextObj)
		delete fContextObj;
}


void
ZinkRenderer::LockGL()
{
//	CALLED();
	BGLRenderer::LockGL();

	color_space cs = BScreen(GLView()->Window()).ColorSpace();

	{
		BAutolock lock(fInfoLocker);
		if (fDirectModeEnabled && fInfo != NULL) {
			fWidth = fInfo->window_bounds.right - fInfo->window_bounds.left;
			fHeight = fInfo->window_bounds.bottom - fInfo->window_bounds.top;
		}

		fContextObj->Validate(fWidth, fHeight);
		fColorSpace = cs;
	}

	// do not hold fInfoLocker here to avoid deadlock
	fContextObj->SetCurrentContext(true, fContextID);
}


void
ZinkRenderer::UnlockGL()
{
//	CALLED();
	if ((fOptions & BGL_DOUBLE) == 0) {
		SwapBuffers();
	}
	fContextObj->SetCurrentContext(false, fContextID);
	BGLRenderer::UnlockGL();
}


// Where the window is on the screen, so that the renderer can put the frame
// there itself. Haiku hands us this in direct mode and keeps it up to date as
// the window moves and is covered.
bool
ZinkRenderer::ScanoutTarget(struct hgl_scanout_target *target)
{
	BAutolock lock(fInfoLocker);
	if (!fDirectModeEnabled || fInfo == NULL)
		return false;
	if ((fInfo->buffer_state & B_DIRECT_MODE_MASK) == B_DIRECT_STOP)
		return false;
	// Nothing of the window can be seen - the screen saver has covered it, or
	// it has more pieces than we can copy in one go.
	if (fInfo->clip_list_count == 0
		|| fInfo->clip_list_count > HGL_MAX_SCANOUT_CLIPS) {
		return false;
	}

	target->origin_x = fInfo->window_bounds.left;
	target->origin_y = fInfo->window_bounds.top;
	target->bytes_per_row = fInfo->bytes_per_row;
	target->num_clips = fInfo->clip_list_count;
	for (uint32 i = 0; i < fInfo->clip_list_count; i++) {
		target->clips[i].left = fInfo->clip_list[i].left;
		target->clips[i].top = fInfo->clip_list[i].top;
		target->clips[i].right = fInfo->clip_list[i].right;
		target->clips[i].bottom = fInfo->clip_list[i].bottom;
	}
	return true;
}


void
ZinkRenderer::PresentedIntoScanout(bool presented)
{
	fPresentedIntoScanout = presented;
}


void
ZinkRenderer::Display(BBitmap *bitmap, BRect *updateRect)
{
//	CALLED();

	if (fPresentedIntoScanout) {
		// the frame is already on the screen
		return;
	}

	if (!fDirectModeEnabled) {
		// TODO: avoid timeout
		if (GLView()->LockLooperWithTimeout(1000) == B_OK) {
			GLView()->DrawBitmap(bitmap, B_ORIGIN);
			GLView()->UnlockLooper();
		}
	} else {
		BAutolock lock(fInfoLocker);
		if (fInfo != NULL) {
			RasBuffer srcBuf(bitmap);
			RasBuffer dstBuf(fInfo);
			for (uint32 i = 0; i < fInfo->clip_list_count; i++) {
				clipping_rect *clip = &fInfo->clip_list[i];
				RasBuffer dstClip = dstBuf;
				dstClip.ClipRect(clip->left, clip->top, clip->right + 1, clip->bottom + 1);
				dstClip.Shift(-fInfo->window_bounds.left, -fInfo->window_bounds.top);
				dstClip.Blit(srcBuf);
			}
		}
	}
}


void
ZinkRenderer::SwapBuffers(bool vsync)
{
	BScreen screen(GLView()->Window());
	fContextObj->SwapBuffers(fContextID);
	fContextObj->Validate(fWidth, fHeight);
	if (vsync)
		screen.WaitForRetrace();
}

void
ZinkRenderer::Draw(BRect updateRect)
{
//	CALLED();
	// When the frame went into the screen itself the window system's bitmap
	// has nothing in it, so the renderer puts the last frame back instead of
	// showing the bitmap; it knows which of the two this is.
	fContextObj->Draw(fContextID, updateRect);
}


status_t
ZinkRenderer::CopyPixelsOut(BPoint location, BBitmap *bitmap)
{
	CALLED();

	// TODO: implement
	return B_ERROR;
}


status_t
ZinkRenderer::CopyPixelsIn(BBitmap *bitmap, BPoint location)
{
	CALLED();

	// TODO: implement
	return B_ERROR;
}


void
ZinkRenderer::EnableDirectMode(bool enabled)
{
	fDirectModeEnabled = enabled;
}


void
ZinkRenderer::DirectConnected(direct_buffer_info *info)
{
//	CALLED();
	bool repaint = false;
	{
		BAutolock lock(fInfoLocker);
		if (info) {
			if (!fInfo) {
				fInfo = (direct_buffer_info *)calloc(1,
					DIRECT_BUFFER_INFO_AREA_SIZE);
			}
			memcpy(fInfo, info, DIRECT_BUFFER_INFO_AREA_SIZE);
			repaint = fPresentedIntoScanout
				&& (info->buffer_state & B_DIRECT_MODE_MASK) != B_DIRECT_STOP;
		} else if (fInfo) {
			free(fInfo);
			fInfo = NULL;
			fPresentedIntoScanout = false;
		}
	}

	// This is how a direct window is told that it has been uncovered or moved:
	// the window system clears what it exposed and expects the window to paint
	// it again. Nothing is in the window system's bitmap when the frame went
	// into the screen itself, so the renderer puts the last one back - which
	// it only does once the application has stopped drawing, since while it
	// draws its own next frame repairs the window.
	if (getenv("ZINK_SCANOUT_TRACE") != NULL) {
		fprintf(stderr, "zink: DirectConnected: state %#x, repaint %d, "
			"%" B_PRIu32 " clips, window %d,%d-%d,%d, bits %p, bpr %" B_PRIu32 "\n",
			info != NULL ? info->buffer_state : 0, repaint,
			info != NULL ? info->clip_list_count : 0,
			info != NULL ? (int)info->window_bounds.left : 0,
			info != NULL ? (int)info->window_bounds.top : 0,
			info != NULL ? (int)info->window_bounds.right : 0,
			info != NULL ? (int)info->window_bounds.bottom : 0,
			info != NULL ? info->bits : NULL,
			info != NULL ? info->bytes_per_row : 0);
	}
	if (repaint)
		fContextObj->Draw(fContextID, BRect());
}


void
ZinkRenderer::FrameResized(float width, float height)
{
	TRACE("%s: %f x %f\n", __func__, width, height);

	BAutolock lock(fInfoLocker);
	fWidth = (GLuint)width;
	fHeight = (GLuint)height;
}
