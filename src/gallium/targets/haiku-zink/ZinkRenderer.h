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
};

#endif	// ZINKRENDERER_H
