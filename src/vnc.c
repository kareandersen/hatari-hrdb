/*
  Hatari - vnc.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Embedded VNC server. Exports the composited screen surface once per
  emulated frame and injects client keyboard/mouse events into the
  emulation, so an external application can embed Hatari's display.
  Runs libvncserver in non-threaded mode: the framebuffer copy, dirty
  rectangle marking and rfbProcessEvents() all happen on the main thread,
  so clients never observe a partially composited frame.
*/
const char Vnc_fileid[] = "Hatari vnc.c";

#include <rfb/rfb.h>
#include <rfb/keysym.h>

#include "main.h"
#include "configuration.h"
#include "audio.h"
#include "ikbd.h"
#include "keymap.h"
#include "log.h"
#include "screen.h"
#include "video.h"
#include "vnc.h"

static rfbScreenInfoPtr vncScreen;
static int fbWidth, fbHeight;

/* Pointer state: VNC sends absolute framebuffer coordinates, the IKBD
 * wants deltas. Zoom carry accumulators as in Main_HandleMouseMotion(). */
static int ptrLastX = -1, ptrLastY;
static int ptrLastMask;
static int ptrCarryX, ptrCarryY;

/* Modifier state tracked from the client's own modifier key events */
static SDL_Keymod vncMod = KMOD_NONE;

/* While the SDL GUI runs its modal dialog loop, input must arrive as SDL
 * events on the queue (the dialogs poll SDL, not the IKBD) - the depth
 * counter handles nested dialogs like the fileselector */
static int nGuiModeDepth;

void Vnc_SetGuiMode(bool bEnter)
{
	if (bEnter)
		nGuiModeDepth++;
	else if (nGuiModeDepth > 0)
		nGuiModeDepth--;
}

/**
 * Convert VNC framebuffer coordinates to window coordinates, the inverse
 * of SDLGui_ScaleMouseStateCoordinates(), since the dialogs scale event
 * coordinates from window space back to the GUI surface.
 */
static void Vnc_GuiWindowCoords(int *x, int *y)
{
	int win_w, win_h;

	if (bInFullScreen || !sdlWindow || !sdlscrn)
		return;
	SDL_GetWindowSize(sdlWindow, &win_w, &win_h);
	if (sdlscrn->w > 0 && win_w > 0)
		*x = *x * win_w / sdlscrn->w;
	if (sdlscrn->h > 0 && win_h > 0)
		*y = *y * win_h / sdlscrn->h;
}


/**
 * Map an X11/RFB keysym to an SDL keycode. The latin1 range is shared
 * between the two; everything else needs the explicit table.
 */
static SDL_Keycode Vnc_KeySymToSDL(rfbKeySym key)
{
	if (key < 0x100)
	{
		/* SDL keycodes are unshifted: fold A-Z to a-z */
		if (key >= 'A' && key <= 'Z')
			return (SDL_Keycode)(key + ('a' - 'A'));
		return (SDL_Keycode)key;
	}
	if (key >= XK_F1 && key <= XK_F12)
		return SDLK_F1 + (key - XK_F1);
	if (key >= XK_KP_0 && key <= XK_KP_9)
		return SDLK_KP_0 + (key - XK_KP_0);

	switch (key)
	{
	 case XK_Return:	return SDLK_RETURN;
	 case XK_BackSpace:	return SDLK_BACKSPACE;
	 case XK_Tab:		return SDLK_TAB;
	 case XK_Escape:	return SDLK_ESCAPE;
	 case XK_Delete:	return SDLK_DELETE;
	 case XK_Insert:	return SDLK_INSERT;
	 case XK_Home:		return SDLK_HOME;
	 case XK_End:		return SDLK_END;
	 case XK_Page_Up:	return SDLK_PAGEUP;
	 case XK_Page_Down:	return SDLK_PAGEDOWN;
	 case XK_Left:		return SDLK_LEFT;
	 case XK_Right:		return SDLK_RIGHT;
	 case XK_Up:		return SDLK_UP;
	 case XK_Down:		return SDLK_DOWN;
	 case XK_Shift_L:	return SDLK_LSHIFT;
	 case XK_Shift_R:	return SDLK_RSHIFT;
	 case XK_Control_L:	return SDLK_LCTRL;
	 case XK_Control_R:	return SDLK_RCTRL;
	 case XK_Alt_L:		return SDLK_LALT;
	 case XK_Alt_R:		return SDLK_RALT;
	 /* AltGr arrives as ISO_Level3_Shift from X11/Wayland clients */
	 case XK_ISO_Level3_Shift:	return SDLK_RALT;
	 case XK_Mode_switch:	return SDLK_MODE;
	 /* Super/meta modifiers reach Hatari's shortcut layer, never the
	  * ST itself (IsKeyTranslatable() filters them) - so meta+key
	  * combos drive the emulator's own shortcuts through VNC */
	 case XK_Super_L:	return SDLK_LGUI;
	 case XK_Meta_L:	return SDLK_LGUI;
	 case XK_Super_R:	return SDLK_RGUI;
	 case XK_Meta_R:	return SDLK_RGUI;
	 case XK_Caps_Lock:	return SDLK_CAPSLOCK;
	 case XK_KP_Enter:	return SDLK_KP_ENTER;
	 case XK_KP_Add:	return SDLK_KP_PLUS;
	 case XK_KP_Subtract:	return SDLK_KP_MINUS;
	 case XK_KP_Multiply:	return SDLK_KP_MULTIPLY;
	 case XK_KP_Divide:	return SDLK_KP_DIVIDE;
	 case XK_KP_Decimal:	return SDLK_KP_PERIOD;
	}
	return SDLK_UNKNOWN;
}

static void Vnc_TrackModifier(SDL_Keycode sym, rfbBool down)
{
	int mod;

	switch (sym)
	{
	 case SDLK_LSHIFT:	mod = KMOD_LSHIFT; break;
	 case SDLK_RSHIFT:	mod = KMOD_RSHIFT; break;
	 case SDLK_LCTRL:	mod = KMOD_LCTRL; break;
	 case SDLK_RCTRL:	mod = KMOD_RCTRL; break;
	 case SDLK_LALT:	mod = KMOD_LALT; break;
	 case SDLK_RALT:	mod = KMOD_RALT; break;
	 case SDLK_LGUI:	mod = KMOD_LGUI; break;
	 case SDLK_RGUI:	mod = KMOD_RGUI; break;
	 case SDLK_MODE:	mod = KMOD_MODE; break;
	 default:		return;
	}
	if (down)
		vncMod |= mod;
	else
		vncMod &= ~mod;
}

static void Vnc_KbdEvent(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
	SDL_Keysym sym;
	SDL_Keycode k = Vnc_KeySymToSDL(key);

	(void)cl;
	if (k == SDLK_UNKNOWN)
		return;

	Vnc_TrackModifier(k, down);

	memset(&sym, 0, sizeof(sym));
	sym.sym = k;
	sym.scancode = SDL_GetScancodeFromKey(k);
	sym.mod = vncMod;

	if (nGuiModeDepth > 0)
	{
		/* dialog loop: deliver as SDL events on the queue */
		SDL_Event ev;

		memset(&ev, 0, sizeof(ev));
		ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
		ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
		ev.key.keysym = sym;
		SDL_PushEvent(&ev);

		/* edit fields take typed characters from SDL_TEXTINPUT */
		if (down && key >= 0x20 && key <= 0x7e)
		{
			memset(&ev, 0, sizeof(ev));
			ev.type = SDL_TEXTINPUT;
			ev.text.text[0] = (char)key;
			SDL_PushEvent(&ev);
		}
		return;
	}

	if (down)
		Keymap_KeyDown(&sym);
	else
		Keymap_KeyUp(&sym);
}

static void Vnc_PtrEvent(int buttonMask, int x, int y, rfbClientPtr cl)
{
	if (nGuiModeDepth > 0)
	{
		/* dialog loop: deliver as SDL events in window coordinates */
		SDL_Event ev;
		int wx = x, wy = y;
		static const struct { int mask; int button; } btns[] = {
			{ 1, SDL_BUTTON_LEFT },
			{ 2, SDL_BUTTON_MIDDLE },
			{ 4, SDL_BUTTON_RIGHT },
		};
		int i;

		Vnc_GuiWindowCoords(&wx, &wy);

		memset(&ev, 0, sizeof(ev));
		ev.type = SDL_MOUSEMOTION;
		ev.motion.x = wx;
		ev.motion.y = wy;
		SDL_PushEvent(&ev);

		for (i = 0; i < 3; i++)
		{
			bool bNow = (buttonMask & btns[i].mask) != 0;
			bool bWas = (ptrLastMask & btns[i].mask) != 0;
			if (bNow == bWas)
				continue;
			memset(&ev, 0, sizeof(ev));
			ev.type = bNow ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
			ev.button.button = btns[i].button;
			ev.button.state = bNow ? SDL_PRESSED : SDL_RELEASED;
			ev.button.x = wx;
			ev.button.y = wy;
			SDL_PushEvent(&ev);
		}
		if ((buttonMask & 24) != (ptrLastMask & 24))
		{
			memset(&ev, 0, sizeof(ev));
			ev.type = SDL_MOUSEWHEEL;
			ev.wheel.y = (buttonMask & 8) ? 1 : -1;
			if (buttonMask & 24)
				SDL_PushEvent(&ev);
		}

		ptrLastX = x;
		ptrLastY = y;
		ptrLastMask = buttonMask;
		rfbDefaultPtrAddEvent(buttonMask, x, y, cl);
		return;
	}

	if (ptrLastX >= 0)
	{
		int dx = x - ptrLastX;
		int dy = y - ptrLastY;

		/* VNC coordinates are sdlscrn pixels, so only the emulated
		 * zoom factor applies (no host window rescale) */
		if (nScreenZoomX != 1)
		{
			dx += ptrCarryX;
			ptrCarryX = dx % nScreenZoomX;
			dx /= nScreenZoomX;
		}
		if (nScreenZoomY != 1)
		{
			dy += ptrCarryY;
			ptrCarryY = dy % nScreenZoomY;
			dy /= nScreenZoomY;
		}
		KeyboardProcessor.Mouse.dx += dx;
		KeyboardProcessor.Mouse.dy += dy;
	}
	ptrLastX = x;
	ptrLastY = y;

	/* Buttons: bit 0 = left, 1 = middle, 2 = right, 3/4 = wheel up/down */
	if ((buttonMask & 1) && !(ptrLastMask & 1))
	{
		if (Keyboard.LButtonDblClk == 0)
			Keyboard.bLButtonDown |= BUTTON_MOUSE;
	}
	else if (!(buttonMask & 1) && (ptrLastMask & 1))
	{
		Keyboard.bLButtonDown &= ~BUTTON_MOUSE;
	}
	if ((buttonMask & 4) && !(ptrLastMask & 4))
		Keyboard.bRButtonDown |= BUTTON_MOUSE;
	else if (!(buttonMask & 4) && (ptrLastMask & 4))
		Keyboard.bRButtonDown &= ~BUTTON_MOUSE;
	if ((buttonMask & 2) && !(ptrLastMask & 2))
		Keyboard.LButtonDblClk = 1;

	/* Wheel taps cursor keys, as the SDL wheel events do */
	if ((buttonMask & 8) && !(ptrLastMask & 8))
	{
		IKBD_PressSTKey(0x48, true);
		IKBD_PressSTKey(0x48, false);
	}
	if ((buttonMask & 16) && !(ptrLastMask & 16))
	{
		IKBD_PressSTKey(0x50, true);
		IKBD_PressSTKey(0x50, false);
	}
	ptrLastMask = buttonMask;

	rfbDefaultPtrAddEvent(buttonMask, x, y, cl);
}


/**
 * Configure the RFB pixel format from the SDL surface's channel masks
 * (the composited surface is XRGB8888 with the SDL renderer, but the
 * window-surface path can have other byte orders).
 */
static void Vnc_SetPixelFormat(const SDL_PixelFormat *fmt)
{
	rfbPixelFormat *sf = &vncScreen->serverFormat;

	sf->bitsPerPixel = 32;
	sf->depth = 24;
	sf->trueColour = TRUE;
	sf->bigEndian = FALSE;
	sf->redShift = fmt->Rshift;
	sf->greenShift = fmt->Gshift;
	sf->blueShift = fmt->Bshift;
	sf->redMax = fmt->Rmask >> fmt->Rshift;
	sf->greenMax = fmt->Gmask >> fmt->Gshift;
	sf->blueMax = fmt->Bmask >> fmt->Bshift;
}

/**
 * (Re)size the exported framebuffer to match the given surface.
 * Called at init and whenever the screen surface is reallocated.
 */
void Vnc_SetSurface(SDL_Surface *surf)
{
	char *newfb, *oldfb;

	if (!vncScreen || !surf)
		return;
	if (surf->format->BytesPerPixel != 4)
	{
		Log_Printf(LOG_WARN, "VNC: unsupported %d bpp surface, not exported\n",
		           surf->format->BitsPerPixel);
		return;
	}
	if (surf->w == fbWidth && surf->h == fbHeight)
		return;

	newfb = calloc(1, (size_t)surf->w * surf->h * 4);
	if (!newfb)
		return;

	oldfb = vncScreen->frameBuffer;
	rfbNewFramebuffer(vncScreen, newfb, surf->w, surf->h, 8, 3, 4);
	/* rfbNewFramebuffer resets serverFormat to its defaults */
	Vnc_SetPixelFormat(surf->format);
	free(oldfb);

	fbWidth = surf->w;
	fbHeight = surf->h;
	ptrLastX = -1;	/* pointer position is stale in the new geometry */
}

/**
 * Copy the composited frame into the exported framebuffer and mark the
 * changed scanline bands. Called once per emulated frame from the VBL
 * handler, after Video_DrawScreen(). Cheap no-op without clients.
 */
void Vnc_RecordFrame(void)
{
	const uint8_t *src;
	uint8_t *dst;
	int y, rowbytes, bandStart;
	bool bChanged = false;

	if (!vncScreen || !sdlscrn)
		return;
	if (vncScreen->clientHead == NULL)
		return;
	if (ConfigureParams.Screen.DisableVideo)
		return;

	/* the surface global may have been reallocated or reassigned */
	if (sdlscrn->w != fbWidth || sdlscrn->h != fbHeight)
	{
		Vnc_SetSurface(sdlscrn);
		if (sdlscrn->w != fbWidth || sdlscrn->h != fbHeight)
			return;
	}
	if (sdlscrn->format->BytesPerPixel != 4)
		return;

	if (SDL_MUSTLOCK(sdlscrn))
	{
		if (SDL_LockSurface(sdlscrn) < 0)
			return;
	}

	src = (const uint8_t *)sdlscrn->pixels;
	dst = (uint8_t *)vncScreen->frameBuffer;
	rowbytes = fbWidth * 4;
	bandStart = -1;

	for (y = 0; y < fbHeight; y++)
	{
		const uint8_t *srcrow = src + (size_t)y * sdlscrn->pitch;
		uint8_t *dstrow = dst + (size_t)y * rowbytes;

		if (memcmp(dstrow, srcrow, rowbytes) != 0)
		{
			memcpy(dstrow, srcrow, rowbytes);
			if (bandStart < 0)
				bandStart = y;
		}
		else if (bandStart >= 0)
		{
			rfbMarkRectAsModified(vncScreen, 0, bandStart, fbWidth, y);
			bandStart = -1;
			bChanged = true;
		}
	}
	if (bandStart >= 0)
	{
		rfbMarkRectAsModified(vncScreen, 0, bandStart, fbWidth, fbHeight);
		bChanged = true;
	}

	if (SDL_MUSTLOCK(sdlscrn))
		SDL_UnlockSurface(sdlscrn);

	/* pick up requests that arrived during the emulated frame, then
	 * answer them with this frame right away - steadier pacing than
	 * leaving them for the next event pump */
	if (bChanged)
		Vnc_Update();

	if (bChanged && ConfigureParams.Vnc.bReportFrameInfo)
	{
		/* rfbSendServerCutText() writes to every client, including
		 * ones still in the RFB handshake, corrupting their stream —
		 * hold back while any client is not yet in the normal state */
		rfbClientIteratorPtr it = rfbGetClientIterator(vncScreen);
		rfbClientPtr cl;
		bool bAllReady = true;

		while ((cl = rfbClientIteratorNext(it)))
		{
			if (cl->state != RFB_NORMAL)
				bAllReady = false;
		}
		rfbReleaseClientIterator(it);

		if (bAllReady)
		{
			char buf[32];
			int len = snprintf(buf, sizeof(buf), "frame:%d", nVBLs);
			rfbSendServerCutText(vncScreen, buf, len);
		}
	}
}

/**
 * True while at least one VNC client is connected.
 */
bool Vnc_HasClients(void)
{
	return vncScreen != NULL && vncScreen->clientHead != NULL;
}

/**
 * True when the VNC server is running (with or without clients).
 */
bool Vnc_IsActive(void)
{
	return vncScreen != NULL;
}

/**
 * Service client connections and input. Called from Main_EventHandler,
 * which also runs while the emulation is halted in the debugger.
 */
void Vnc_Update(void)
{
	static bool bHadClients;
	bool bHaveClients;
	int i;

	if (!vncScreen)
		return;

	/* audio muting and present skipping depend on whether anyone is
	 * watching - re-evaluate when the first client arrives or the
	 * last one leaves */
	bHaveClients = vncScreen->clientHead != NULL;
	if (bHaveClients != bHadClients)
	{
		bHadClients = bHaveClients;
		Audio_Recheck();
	}

	/* rfbCheckFds() handles at most one message per client per call.
	 * We are called once per frame, so drain the sockets here or a
	 * pointer motion flood queues up and input lags ever further
	 * behind. Bounded, in case a client streams without pause. */
	for (i = 0; i < 256; i++)
	{
		if (rfbCheckFds(vncScreen, 0) <= 0)
			break;
	}
	rfbProcessEvents(vncScreen, 0);
}

void Vnc_Init(void)
{
	if (ConfigureParams.Vnc.nPort == 0)
		return;
	if (!sdlscrn || sdlscrn->format->BytesPerPixel != 4)
	{
		Log_Printf(LOG_WARN, "VNC: no 32-bit screen surface, server not started\n");
		return;
	}

	fbWidth = sdlscrn->w;
	fbHeight = sdlscrn->h;

	vncScreen = rfbGetScreen(NULL, NULL, fbWidth, fbHeight, 8, 3, 4);
	if (!vncScreen)
	{
		Log_Printf(LOG_WARN, "VNC: could not create server\n");
		return;
	}
	vncScreen->frameBuffer = calloc(1, (size_t)fbWidth * fbHeight * 4);
	if (!vncScreen->frameBuffer)
	{
		rfbScreenCleanup(vncScreen);
		vncScreen = NULL;
		return;
	}
	vncScreen->desktopName = "Hatari";
	vncScreen->port = ConfigureParams.Vnc.nPort;
	vncScreen->ipv6port = ConfigureParams.Vnc.nPort;
	/* local display embedding only: never listen on external interfaces */
	vncScreen->listenInterface = htonl(INADDR_LOOPBACK);
	{
		static char lo6[] = "::1";
		vncScreen->listen6Interface = lo6;
	}
	vncScreen->alwaysShared = TRUE;
	/* updates are already paced per emulated frame, don't defer them */
	vncScreen->deferUpdateTime = 0;
	vncScreen->kbdAddEvent = Vnc_KbdEvent;
	vncScreen->ptrAddEvent = Vnc_PtrEvent;
	Vnc_SetPixelFormat(sdlscrn->format);

	rfbInitServer(vncScreen);
	Log_Printf(LOG_INFO, "VNC: display exported on localhost:%d\n",
	           ConfigureParams.Vnc.nPort);
}

void Vnc_UnInit(void)
{
	if (!vncScreen)
		return;
	rfbShutdownServer(vncScreen, TRUE);
	free(vncScreen->frameBuffer);
	vncScreen->frameBuffer = NULL;
	rfbScreenCleanup(vncScreen);
	vncScreen = NULL;
}
