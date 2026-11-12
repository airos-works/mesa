/*
 * Copyright 2006-2012, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jérôme Duval, korli@users.berlios.de
 * 		Philippe Houdoin, philippe.houdoin@free.fr
 * 		Artur Wyszynski, harakash@gmail.com
 *		Alexander von Gluck IV, kallisti5@unixzen.com
 */
#ifndef ZINKRENDERER_H
#define ZINKRENDERER_H


#include <kernel/image.h>

#include <DirectWindow.h>
#include <Screen.h>

#include "GLRenderer.h"
#include "GalliumContext.h"


class ZinkRenderer : public BGLRenderer, public HGLWinsysContext {
public:
								ZinkRenderer(BGLView *view,
									ulong bgl_options);
	virtual						~ZinkRenderer();

			void				LockGL();
			void				UnlockGL();

			void				Display(BBitmap* bitmap, BRect* updateRect);
			bool				ScanoutTarget(
									struct hgl_scanout_target* target);
			void				PresentedIntoScanout(bool presented);
			void				ReleaseScanoutTarget();

			void				SwapBuffers(bool vsync = false);
			void				Draw(BRect updateRect);
			status_t			CopyPixelsOut(BPoint source, BBitmap *dest);
			status_t			CopyPixelsIn(BBitmap *source, BPoint dest);
			void				FrameResized(float width, float height);

			void				EnableDirectMode(bool enabled);
			void				DirectConnected(direct_buffer_info *info);

private:
			GalliumContext*		fContextObj;
			context_id			fContextID;

			bool				fDirectModeEnabled;
			direct_buffer_info*	fInfo;
			BLocker				fInfoLocker;
			ulong				fOptions;
			GLuint				fWidth;
			GLuint				fHeight;
			color_space			fColorSpace;
			// set while the renderer is writing the screen itself, so that a
			// redraw does not put a stale bitmap over the frame
			bool				fPresentedIntoScanout;
			// whether ScanoutTarget took fInfoLocker and still holds it
			bool				fScanoutTargetHeld;
			// how many frames went into the screen since the window system
			// last said anything, reported under ZINK_SCANOUT_TRACE. It is
			// what shows whether a frame can be drawn at a place the window
			// has already left.
			int32				fPresentCount = 0;
			// when the window last moved or its clipping changed
			bigtime_t			fLastPlacementChange = 0;

			// The window is a BDirectWindow that was asked for
			// B_DIRECT_DEVICE_PIXELS, and fInfo is what the window system
			// said, in frame buffer pixels (window_bounds and clip_list are
			// the whole window's); device_scale says what one view pixel
			// covers. Then the view's own frame, in its window's
			// coordinates, places it.
			BDirectWindow*		fDirectWindow = NULL;
			bool				fAskedForDevicePixels = false;
			bool				fDevicePixels = false;
			BRect				fViewFrame;
			// The rectangles the last frame went into, for telling the window
			// system that its own copy of them is out of date.
			clipping_rect		fPresentedClips[HGL_MAX_SCANOUT_CLIPS];
			uint32				fPresentedClipCount = 0;
			bool				fPresentedClipsChanged = false;
			bigtime_t			fLastStaleMark = 0;
			// What LockGL and SwapBuffers asked the window system for every
			// time, kept for a while instead.
			screen_id			fScreenID;
			bigtime_t			fScreenChecked = 0;
			// A bitmap handed to the window system that it may still be
			// drawing.
			bool				fDrawPending = false;
			// The thread that puts the last frame back a second time, a
			// little after the window system said something.
			sem_id				fRepresentSem = -1;
			thread_id			fRepresentWorker = -1;
			int64				fRepresentAt = 0;
			int32				fRepresentPending = 0;
			int32				fRepresentQuit = 0;

	static	status_t			_RepresentWorker(void* data);
			void				_Represent();

			void				_UpdateScreen();
			void				_MarkStale();
			void				_ReadWindowSystemInfo(
									direct_buffer_info* info);
};

#endif	// ZINKRENDERER_H
