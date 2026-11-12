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
#include <GLView.h>
#include <Screen.h>
#include <Window.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
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


// BGLView keeps what the window system last said about a direct window, and
// hands the renderer a copy cut down to the view. On a desktop drawn at a
// higher density, with B_DIRECT_DEVICE_PIXELS, the window system speaks in
// frame buffer pixels while the view's frame is in its own, and the cut is
// made with the two mixed up: a quarter of the view at 200 %, in the wrong
// place. The renderer reads the original instead, from BGLView's
// glview_direct_info (GLView.cpp of the same Mesa), and cuts it itself.
//
// The member is private; explicit instantiation of a template is allowed to
// name it, which is how its offset is had without a copy of the layout.
struct glview_direct_info;

namespace {

struct GLViewDirectInfo {
	direct_buffer_info* direct_info;
	bool direct_connected;
	bool enable_direct_mode;
};

template<typename Tag, typename Tag::type Member>
struct PrivateMember {
	friend typename Tag::type Get(Tag) { return Member; }
};

struct GLViewClipInfo {
	typedef glview_direct_info* BGLView::* type;
	friend type Get(GLViewClipInfo);
};

template struct PrivateMember<GLViewClipInfo, &BGLView::fClipInfo>;

}	// namespace


static direct_buffer_info*
window_system_info(BGLView* view)
{
	const GLViewDirectInfo* info
		= (const GLViewDirectInfo*)(view->*Get(GLViewClipInfo()));
	return info != NULL ? info->direct_info : NULL;
}


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
	fPresentedIntoScanout(false),
	fScanoutTargetHeld(false)
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
	fScreenID = B_MAIN_SCREEN_ID;
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

	if (fRepresentWorker >= 0) {
		atomic_set(&fRepresentQuit, 1);
		release_sem(fRepresentSem);
		status_t result;
		wait_for_thread(fRepresentWorker, &result);
	}
	if (fRepresentSem >= 0)
		delete_sem(fRepresentSem);

	if (fContextObj)
		delete fContextObj;
}


// Putting the last frame back right when the window system says the window was
// uncovered is not enough: the window system then repaints what it exposed
// itself (the view's colour) and copies that to the screen at the next blank,
// over the frame. So it is done once more a little later, from this thread, by
// which time that copy has happened.
static const bigtime_t kRepresentAgainAfter = 50000;

// Set on a thread while it puts the last frame back (ScanoutTarget then skips
// the wait after a move).
static __thread bool sRepresenting;


/*static*/ status_t
ZinkRenderer::_RepresentWorker(void* data)
{
	ZinkRenderer* self = (ZinkRenderer*)data;
	while (acquire_sem(self->fRepresentSem) == B_OK) {
		if (atomic_get(&self->fRepresentQuit) != 0)
			break;
		bigtime_t at;
		while ((at = atomic_get64(&self->fRepresentAt)) > system_time())
			snooze_until(at, B_SYSTEM_TIMEBASE);
		atomic_set(&self->fRepresentPending, 0);
		if (atomic_get(&self->fRepresentQuit) != 0)
			break;
		self->_Represent();
	}
	return B_OK;
}


// Puts the last frame back into the screen, when it went there and the
// application has stopped drawing (zink_represent_into_scanout decides that).
void
ZinkRenderer::_Represent()
{
	if (!fPresentedIntoScanout)
		return;
	sRepresenting = true;
	fContextObj->Draw(fContextID, BRect());
	sRepresenting = false;
	// The present back went through ScanoutTarget, which holds the placement
	// until told what happened.
	if (fScanoutTargetHeld && fInfoLocker.LockingThread() == find_thread(NULL)) {
		const bool markStale = fDevicePixels && fInfo != NULL
			&& fInfo->drawing_bits_area >= 0;
		fPresentedClipsChanged = true;
		ReleaseScanoutTarget();
		if (markStale)
			_MarkStale();
	}
}


void
ZinkRenderer::LockGL()
{
//	CALLED();
	BGLRenderer::LockGL();

	_UpdateScreen();

	{
		BAutolock lock(fInfoLocker);
		// A direct window cut down to the view by BGLView has the view's
		// size; one in frame buffer pixels has its size on the screen, and
		// the frame keeps the view's (FrameResized).
		if (fDirectModeEnabled && fInfo != NULL && !fDevicePixels) {
			fWidth = fInfo->window_bounds.right - fInfo->window_bounds.left;
			fHeight = fInfo->window_bounds.bottom - fInfo->window_bounds.top;
		}

		fContextObj->Validate(fWidth, fHeight);
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


// Which screen the window is on and how it is set up, asked of the window
// system once a second rather than twice a frame: both were synchronous round
// trips, in LockGL and SwapBuffers.
void
ZinkRenderer::_UpdateScreen()
{
	const bigtime_t now = system_time();
	if (fScreenChecked != 0 && now - fScreenChecked < 1000000)
		return;
	fScreenChecked = now;

	BScreen screen(GLView()->Window());
	if (!screen.IsValid())
		return;
	fScreenID = screen.ID();
	fColorSpace = screen.ColorSpace();
}


// Where the window is on the screen, so that the renderer can put the frame
// there itself. Haiku hands us this in direct mode and keeps it up to date as
// the window moves and is covered.
// How long after a window moves before it is drawn into the screen again. Long
// enough to outlast the window system's repaint of what the window came off,
// short enough that letting go of a dragged window feels immediate.
static const bigtime_t kSettleAfterMove = 250000;


bool
ZinkRenderer::ScanoutTarget(struct hgl_scanout_target *target)
{
	// Held until PresentedIntoScanout, so that the window cannot move between
	// its clipping being read and the copy being sent to the screen. Anything
	// that would move it goes through DirectConnected, which takes the same
	// lock.
	fInfoLocker.Lock();
	fScanoutTargetHeld = true;

	struct Failed {
		ZinkRenderer &self;
		bool ok = false;
		~Failed() { if (!ok) self.ReleaseScanoutTarget(); }
	} failed {*this};

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

	// The frame is copied into the screen a pixel at a time with no conversion,
	// so the two have to agree on what a pixel is.
	if (fInfo->pixel_format != B_RGB32 && fInfo->pixel_format != B_RGBA32)
		return false;

	// Switching to another workspace takes the window off the screen, and what
	// comes back describes no frame buffer at all: no address, and a row of
	// zero bytes. Believing it means going back to the driver for a frame
	// buffer of that shape on every frame, which gets nowhere and takes the
	// program with it.
	if (fInfo->bits == NULL || fInfo->bytes_per_row == 0)
		return false;

	// A window that has just moved leaves the strip it came off behind for the
	// window system to repaint, and a program drawing into the screen at a
	// thousand frames a second outruns that repaint: the strips linger, one
	// per move, until the window stops. Presenting through the window system's
	// bitmap instead is slower and has no such trouble, so that is what a
	// moving window gets. Only while it is moving - a window is still almost
	// all of the time.
	//
	// Putting the last frame back after the window system said something
	// (DirectConnected) is not that: the window system waits for it, and it
	// happens once.
	if (!sRepresenting
		&& system_time() - fLastPlacementChange < kSettleAfterMove)
		return false;

	target->bits = fInfo->bits;
	target->bytes_per_row = fInfo->bytes_per_row;
	target->dest_width = 0;
	target->dest_height = 0;
	target->filter = false;

	if (!fDevicePixels) {
		target->origin_x = fInfo->window_bounds.left;
		target->origin_y = fInfo->window_bounds.top;
		target->num_clips = fInfo->clip_list_count;
		for (uint32 i = 0; i < fInfo->clip_list_count; i++) {
			target->clips[i].left = fInfo->clip_list[i].left;
			target->clips[i].top = fInfo->clip_list[i].top;
			target->clips[i].right = fInfo->clip_list[i].right;
			target->clips[i].bottom = fInfo->clip_list[i].bottom;
		}
	} else {
		// The window system's own words, in frame buffer pixels: the view
		// covers this much of the screen, the way the window system
		// enlarges anything drawn in the window (DirectWindowInfo.cpp).
		const float scale = fInfo->device_scale > 100
			? fInfo->device_scale / 100.0f : 1.0f;
		const int32 left = fInfo->window_bounds.left
			+ (int32)floorf(fViewFrame.left * scale);
		const int32 top = fInfo->window_bounds.top
			+ (int32)floorf(fViewFrame.top * scale);
		const int32 right = fInfo->window_bounds.left
			+ (int32)floorf((fViewFrame.right + 1) * scale) - 1;
		const int32 bottom = fInfo->window_bounds.top
			+ (int32)floorf((fViewFrame.bottom + 1) * scale) - 1;
		if (right < left || bottom < top)
			return false;
		target->origin_x = left;
		target->origin_y = top;
		target->dest_width = right - left + 1;
		target->dest_height = bottom - top + 1;
		target->filter = (fInfo->device_scale % 100) != 0;

		uint32 count = 0;
		for (uint32 i = 0; i < fInfo->clip_list_count; i++) {
			const clipping_rect& clip = fInfo->clip_list[i];
			const int32 l = max_c(clip.left, left);
			const int32 t = max_c(clip.top, top);
			const int32 r = min_c(clip.right, right);
			const int32 b = min_c(clip.bottom, bottom);
			if (r < l || b < t)
				continue;
			if (count == HGL_MAX_SCANOUT_CLIPS)
				return false;
			target->clips[count].left = l;
			target->clips[count].top = t;
			target->clips[count].right = r;
			target->clips[count].bottom = b;
			count++;
		}
		if (count == 0)
			return false;
		target->num_clips = count;

		// for the window system, once the frame is there
		fPresentedClipsChanged = fPresentedClipsChanged
			|| count != fPresentedClipCount
			|| memcmp(fPresentedClips, target->clips,
				sizeof(clipping_rect) * count) != 0;
		memcpy(fPresentedClips, target->clips, sizeof(clipping_rect) * count);
		fPresentedClipCount = count;
	}

	failed.ok = true;
	return true;
}


void
ZinkRenderer::ReleaseScanoutTarget()
{
	if (fScanoutTargetHeld
		&& fInfoLocker.LockingThread() == find_thread(NULL)) {
		fScanoutTargetHeld = false;
		fInfoLocker.Unlock();
	}
}


void
ZinkRenderer::PresentedIntoScanout(bool presented)
{
	fPresentedIntoScanout = presented;
	if (presented)
		fPresentCount++;
	const bool markStale = presented && fDevicePixels && fInfo != NULL
		&& fInfo->drawing_bits_area >= 0;
	ReleaseScanoutTarget();
	if (markStale)
		_MarkStale();
}


// The window system draws into a copy of the screen of its own and copies
// that to the frame buffer; the frame went into the frame buffer only. Told
// so, it takes those pixels from the frame buffer when it moves the window
// or reads the screen, instead of showing what its copy has there.
//
// It is told once a display frame, or at once when the window's visible
// parts changed. Never wait for the window here: its thread may be waiting
// for this one (a view's FrameResized locking the GL context, say).
void
ZinkRenderer::_MarkStale()
{
	const bigtime_t now = system_time();
	if (!fPresentedClipsChanged && now - fLastStaleMark < 16000)
		return;
	if (fDirectWindow == NULL || fDirectWindow->LockWithTimeout(0) != B_OK)
		return;
	clipping_rect clips[HGL_MAX_SCANOUT_CLIPS];
	uint32 count;
	{
		BAutolock lock(fInfoLocker);
		count = fPresentedClipCount;
		memcpy(clips, fPresentedClips, sizeof(clipping_rect) * count);
		fPresentedClipsChanged = false;
	}
	fDirectWindow->MarkDrawingBitsStale(clips, count);
	fDirectWindow->Unlock();
	fLastStaleMark = now;
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
			// The window system copies the bitmap into the screen (enlarged
			// on a desktop drawn at a higher density) while this thread goes
			// on to the next frame, instead of the two taking turns. The
			// renderer has two bitmaps and the next frame goes into the
			// other one; it is waited for here, before this one is handed
			// over, so that the next frame is never copied into a bitmap
			// still being drawn.
			if (fDrawPending)
				GLView()->Sync();
			GLView()->DrawBitmapAsync(bitmap, B_ORIGIN);
			GLView()->Flush();
			fDrawPending = true;
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
	// When the frame goes into the screen itself, wait for the display to
	// finish the frame it is showing before starting to overwrite it. The copy
	// takes about a millisecond for a whole screen and the display takes
	// sixteen to draw one, so a copy begun at the blank stays well ahead of
	// the beam and the frame arrives whole.
	//
	// Waiting afterwards, which is what this did, only paces the application:
	// it does not stop the tearing, because the copy has already happened.
	// Never wait indefinitely: a display that has been switched off stops
	// reporting blanks, and an application should carry on drawing rather than
	// stop dead. Three frames is long enough to be sure one was missed.
	const bigtime_t kRetraceTimeout = 50000;
	const bool wait = fPresentedIntoScanout
		&& (vsync || getenv("HGL_VSYNC") != NULL);
	if (wait)
		BScreen(fScreenID).WaitForRetrace(kRetraceTimeout);

	fContextObj->SwapBuffers(fContextID);
	fContextObj->Validate(fWidth, fHeight);

	if (vsync && !wait)
		BScreen(fScreenID).WaitForRetrace(kRetraceTimeout);
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
		// Taking this waits for any present that is part way through reading
		// the window's placement and sending its copy.
		BAutolock lock(fInfoLocker);
		if (info) {
			if (!fInfo) {
				fInfo = (direct_buffer_info *)calloc(1,
					DIRECT_BUFFER_INFO_AREA_SIZE);
			}
			_ReadWindowSystemInfo(info);
			if ((info->buffer_state
					& (B_BUFFER_MOVED | B_BUFFER_RESIZED | B_CLIPPING_MODIFIED))
					!= 0) {
				fLastPlacementChange = system_time();
			}
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
		fprintf(stderr, "       presents since the last one: %" B_PRId32 "\n",
			fPresentCount);
		fPresentCount = 0;
	}
	if (repaint) {
		_Represent();

		// and again once the window system's own repaint is on the screen
		if (fRepresentSem < 0) {
			fRepresentSem = create_sem(0, "zink represent");
			if (fRepresentSem >= 0) {
				fRepresentWorker = spawn_thread(_RepresentWorker,
					"zink represent", B_DISPLAY_PRIORITY, this);
				if (fRepresentWorker >= 0)
					resume_thread(fRepresentWorker);
			}
		}
		if (fRepresentWorker >= 0) {
			atomic_set64(&fRepresentAt, system_time() + kRepresentAgainAfter);
			if (atomic_get_and_set(&fRepresentPending, 1) == 0)
				release_sem_etc(fRepresentSem, 1, B_DO_NOT_RESCHEDULE);
		}
	}
}


// What DirectConnected was given, or the window system's original of it
// when that is in frame buffer pixels (see window_system_info). fInfoLocker
// is held.
void
ZinkRenderer::_ReadWindowSystemInfo(direct_buffer_info* info)
{
	const direct_buffer_info* original = window_system_info(GLView());
	// The same announcement, as far as can be told: BGLView copies it before
	// it calls the renderer.
	if (original != NULL && original->device_scale != 0
		&& original->bits == info->bits
		&& original->bytes_per_row == info->bytes_per_row
		&& original->device_scale == info->device_scale
		&& original->buffer_state == info->buffer_state) {
		memcpy(fInfo, original, DIRECT_BUFFER_INFO_AREA_SIZE);
		fDevicePixels = true;
		return;
	}
	memcpy(fInfo, info, DIRECT_BUFFER_INFO_AREA_SIZE);
	fDevicePixels = false;
	if (info->device_scale > 100) {
		// Frame buffer pixels that cannot be placed: nothing goes into the
		// screen (ScanoutTarget sees no clipping).
		fInfo->clip_list_count = 0;
	}
}


void
ZinkRenderer::FrameResized(float width, float height)
{
	TRACE("%s: %f x %f\n", __func__, width, height);

	// The window is locked: BGLView calls this from AttachedToWindow and
	// FrameResized.
	BView* view = GLView();
	BRect frame = view->Bounds();
	for (BView* parent = view; parent != NULL; parent = parent->Parent())
		parent->ConvertToParent(&frame);

	{
		BAutolock lock(fInfoLocker);
		fWidth = (GLuint)width;
		fHeight = (GLuint)height;
		fViewFrame = frame;
	}

	// A direct window on a desktop drawn at a higher density is left
	// unconnected - nothing would go into the screen - unless it asks for
	// the frame buffer's own pixels. The frame is then enlarged on the GPU
	// (zink_screen.c). Asked once, outside fInfoLocker: the window system
	// connects the window again before SetFlags returns, which comes back
	// here through DirectConnected on another thread.
	BDirectWindow* window = dynamic_cast<BDirectWindow*>(view->Window());
	if (window != NULL && !fAskedForDevicePixels) {
		fAskedForDevicePixels = true;
		fDirectWindow = window;
		if (getenv("HGL_NO_DEVICE_PIXELS") == NULL
			&& (window->Flags() & B_DIRECT_DEVICE_PIXELS) == 0) {
			window->SetFlags(window->Flags() | B_DIRECT_DEVICE_PIXELS);
		}
	}
}
