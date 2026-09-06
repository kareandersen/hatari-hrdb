/*
  Hatari - statusbar.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Code to draw statusbar area, floppy leds etc.

  Use like this:
  - Before screen surface is (re-)created Statusbar_SetHeight()
    has to be called with the new screen height. Add the returned
    value to screen height (zero means no statusbar).  After this,
    Statusbar_GetHeight() can be used to retrieve the statusbar size
  - After screen surface is (re-)created, call Statusbar_Init()
    to re-initialize / re-draw the statusbar
  - Call Statusbar_SetFloppyLed() to set floppy drive led ON/OFF,
    or call Statusbar_EnableHDLed() to enabled HD led for a while
  - Whenever screen is redrawn, call Statusbar_Update() to update
    statusbar contents and find out whether and what screen area
    needs to be updated (outside of screen locking)
  - If screen redraws can be partial, Statusbar_OverlayRestore()
    needs to be called before locking the screen for drawing and
    Statusbar_OverlayBackup() needs to be called after screen unlocking,
    but before calling Statusbar_Update().  These are needed for
    hiding the overlay drive led (= restoring the area that was below
    them before LED was shown) when drive leds are turned OFF.
  - If other information shown by Statusbar (TOS version etc) changes,
    call Statusbar_UpdateInfo()

  TODO:
  - re-calculate colors on each update to make sure they're
    correct in Falcon & TT 8-bit palette modes?
  - call Statusbar_AddMessage() from log.c?
*/
const char Statusbar_fileid[] = "Hatari statusbar.c";

#include <assert.h>
#include <stdlib.h>
#include "main.h"
#include "configuration.h"
#include "screenSnapShot.h"
#include "sdlgui.h"
#include "statusbar.h"
#include "tos.h"
#include "screen.h"
#include "video.h"
#include "wavFormat.h"
#include "ymFormat.h"
#include "avi_record.h"
#include "vdi.h"
#include "fdc.h"
#include "stMemory.h"
#include "blitter.h"
#include "str.h"
#include "lilo.h"
#include "sound.h"
#include "dmaSnd.h"

#define DEBUG 0
#if DEBUG
# include <execinfo.h>
# define DEBUGPRINT(x) printf x
#else
# define DEBUGPRINT(x)
#endif

/* space needed for FDC information */
#define FDC_MSG_MAX_LEN 20

#define STATUSBAR_LINES 2
#define MAX_DRIVE_LEDS (DRIVE_LED_HD + 1)

/* whether drive leds should be ON and their previous shown state */
static struct {
	drive_led_t state;
	drive_led_t oldstate;
	Uint32 expire;	/* when to disable led, valid only if >0 && state=TRUE */
	int offset;	/* led x-pos on screen */
} Led[MAX_DRIVE_LEDS];


#define PANEL_METER_W 6

/* Panel item colors, mapped against the surface being drawn into: that
 * is the emulation frame buffer or the overlay's own surface, which do
 * not have the same pixel format. */
#define PANEL_RGB_METER		158, 176, 158
#define PANEL_RGB_METER_FRAME	 56,  56,  56
#define PANEL_RGB_LED_ON	124, 168, 124
#define PANEL_RGB_LED_OFF	 48,  48,  48

/* Configurable status overlay panel: a strip drawn over the bottom of
 * the emulation area (no surface resize), item set chosen in the GUI,
 * toggled with the StatusOverlay shortcut. */
static SDL_Rect PanelRect;		/* the full strip (backup/restore unit) */
static SDL_Surface *PanelUnderside;	/* screen contents under the strip */
static bool bPanelDrawn;		/* strip currently painted into surf */
static int PanelFontW, PanelFontH;
static uint8_t PanelYmShown[3];		/* decaying meter levels, 0-31 */
static int PanelDmaShownL, PanelDmaShownR;

/* needs to be enough for all messages, but <= MessageRect width / font width */
#define MAX_MESSAGE_LEN 63
typedef struct msg_item {
	struct msg_item *next;
	char msg[MAX_MESSAGE_LEN+1];
	Uint32 timeout;	/* msecs, zero=no timeout */
	Uint32 expire;  /* when to expire message */
	bool shown;
} msg_item_t;

static msg_item_t DefaultMessage;
static msg_item_t *MessageList = &DefaultMessage;

/* screen height above statusbar and height of statusbar below screen */
static int ScreenHeight;
static int StatusbarHeight;


/*-----------------------------------------------------------------------*/
/**
 * Return statusbar height for given width and height
 */
int Statusbar_GetHeightForSize(int width, int height)
{
	/* the old statusbar was replaced by the overlay panel, which is
	 * drawn inside the emulation area: no rows are reserved */
	(void)width; (void)height;
	return 0;
}

/*-----------------------------------------------------------------------*/
/**
 * Set screen height used for statusbar height calculation.
 *
 * Return height of statusbar that should be added to the screen
 * height when screen is (re-)created, or zero if statusbar will
 * not be shown
 */
int Statusbar_SetHeight(int width, int height)
{
#if DEBUG
	/* find out from where the set height is called */
	void *addr[8];
	int count = backtrace(addr, sizeof(addr)/sizeof(*addr));
	backtrace_symbols_fd(addr, count, fileno(stderr));
#endif
	ScreenHeight = height;
	StatusbarHeight = Statusbar_GetHeightForSize(width, height);
	DEBUGPRINT(("Statusbar_SetHeight(%d, %d) -> %d\n", width, height, StatusbarHeight));
	return StatusbarHeight;
}

/*-----------------------------------------------------------------------*/
/**
 * Return height of statusbar set with Statusbar_SetHeight()
 */
int Statusbar_GetHeight(void)
{
	return StatusbarHeight;
}


/*-----------------------------------------------------------------------*/
/**
 * Enable HD drive led, it will be automatically disabled after a while.
 */
void Statusbar_EnableHDLed(drive_led_t state)
{
	/* leds are shown for 1/2 sec after enabling */
	Led[DRIVE_LED_HD].expire = SDL_GetTicks() + 1000/2;
	Led[DRIVE_LED_HD].state = state;
}

/*-----------------------------------------------------------------------*/
/**
 * Set given floppy drive led state, anything enabling led with this
 * needs also to take care of disabling it.
 */
void Statusbar_SetFloppyLed(drive_index_t drive, drive_led_t state)
{
	assert(drive == DRIVE_LED_A || drive == DRIVE_LED_B);
	Led[drive].state = state;
}


/*-----------------------------------------------------------------------*/
/**
 * Return a text corresponding to the status of each emulated joystick
 * - : off   J : real joystick   K : keyboard emulation
 */
static void Statusbar_JoysticksGetText ( char *buf )
{
	int i;

	for (i = 0; i < JOYSTICK_COUNT; i++)
	{
		switch (ConfigureParams.Joysticks.Joy[i].nJoystickMode)
		{
		case JOYSTICK_DISABLED:
			*buf++ = '-';
			break;
		case JOYSTICK_REALSTICK:
			*buf++ = 'J';
			break;
		case JOYSTICK_KEYBOARD:
			*buf++ = 'K';
			break;
		}
	}
	*buf = '\0';
}



/*-----------------------------------------------------------------------*/
/**
 * (Re-)compute the status overlay panel geometry for the given
 * surface, dropping an incompatible backup surface.
 */
static void Statusbar_PanelInit(SDL_Surface *surf)
{
	SDLGui_GetFontSize(&PanelFontW, &PanelFontH);
	PanelRect.x = 0;
	PanelRect.w = surf->w;
	PanelRect.h = PanelFontH + 4;
	PanelRect.y = surf->h - StatusbarHeight - PanelRect.h;
	if (PanelRect.y < 0)
		PanelRect.y = 0;

	if (PanelUnderside && (
	    PanelUnderside->w != PanelRect.w ||
	    PanelUnderside->h != PanelRect.h ||
	    PanelUnderside->format->BitsPerPixel != surf->format->BitsPerPixel))
	{
		SDL_FreeSurface(PanelUnderside);
		PanelUnderside = NULL;
	}
	bPanelDrawn = false;
}

/**
 * Forget the panel's backup state, e.g. after toggling it or before a
 * full screen repaint; the caller triggers the repaint itself.
 */
void Statusbar_PanelReset(void)
{
	bPanelDrawn = false;
}

/**
 * Restore the screen contents under the panel strip (before the next
 * frame conversion, so partial updates find unmodified pixels).
 */
static void Statusbar_PanelRestore(SDL_Surface *surf)
{
	if (bPanelDrawn && PanelUnderside)
	{
		SDL_BlitSurface(PanelUnderside, NULL, surf, &PanelRect);
		bPanelDrawn = false;
	}
}

/**
 * Draw one small vertical meter bar, level 0-31.
 */
static void Statusbar_PanelMeter(SDL_Surface *surf, int x, int y, int h, int level)
{
	SDL_Rect r;
	int fill = (level * (h - 2) + 30) / 31;

	r.x = x; r.y = y; r.w = PANEL_METER_W; r.h = h;
	SDL_FillRect(surf, &r, SDL_MapRGB(surf->format, PANEL_RGB_METER_FRAME));
	if (fill > 0)
	{
		r.x = x + 1;
		r.w = PANEL_METER_W - 2;
		r.y = y + (h - 1 - fill);
		r.h = fill;
		SDL_FillRect(surf, &r, SDL_MapRGB(surf->format, PANEL_RGB_METER));
	}
}

/**
 * Draw a small LED box; returns width used.
 */
static int Statusbar_PanelLed(SDL_Surface *surf, int x, int y, bool bOn)
{
	SDL_Rect r;
	r.x = x; r.y = y + 1; r.w = PanelFontW; r.h = PanelFontH - 2;
	SDL_FillRect(surf, &r, bOn ? SDL_MapRGB(surf->format, PANEL_RGB_LED_ON)
				   : SDL_MapRGB(surf->format, PANEL_RGB_LED_OFF));
	return r.w;
}

/**
 * Merge the panel rect with another update rect (or return the panel
 * rect alone). Uses a static, like the other statusbar rects.
 */
/**
 * Walk the enabled panel items left to right from x position xoff,
 * with the strip starting at ytop, drawing them when bDraw is set;
 * returns the x position after the last item. Called twice per frame:
 * once to measure the content width for centering, once to draw.
 */
static int Statusbar_PanelWalk(SDL_Surface *surf, int xoff, int ytop, bool bDraw)
{
	char buf[MAX_MESSAGE_LEN+1];
	int ytext = ytop + 2;
	int ymeter = ytop + 1;
	int meterh = PanelRect.h - 2;
	int i;

	if (ConfigureParams.Screen.bOverlayDriveLeds)
	{
		static const char *names[MAX_DRIVE_LEDS] = { "A", "B", "H" };
		for (i = 0; i < MAX_DRIVE_LEDS; i++)
		{
			if (bDraw)
			{
				SDLGui_TextShadow(xoff, ytext, names[i]);
				Statusbar_PanelLed(surf, xoff + PanelFontW + 1, ytext,
				                   Led[i].state != LED_STATE_OFF);
			}
			xoff += 2*PanelFontW + 1 + PanelFontW;
		}
	}
	if (ConfigureParams.Screen.bOverlayFdc)
	{
		FDC_Get_Statusbar_Text(buf, FDC_MSG_MAX_LEN);
		if (bDraw)
			SDLGui_TextShadow(xoff, ytext, buf);
		xoff += (strlen(buf) + 1) * PanelFontW;
	}
	if (ConfigureParams.Screen.bOverlayJoysticks)
	{
		char jbuf[JOYSTICK_COUNT+1];
		Statusbar_JoysticksGetText(jbuf);
		if (bDraw)
			SDLGui_TextShadow(xoff, ytext, jbuf);
		xoff += (JOYSTICK_COUNT + 1) * PanelFontW;
	}
	if (ConfigureParams.Screen.bOverlayFrameSkips)
	{
		/* fixed width so toggling fast forward doesn't shift the layout */
		snprintf(buf, sizeof(buf), "FS:%-2d%s", nFrameSkips,
		         ConfigureParams.System.bFastForward ? ">>" : "  ");
		if (bDraw)
			SDLGui_TextShadow(xoff, ytext, buf);
		xoff += 8 * PanelFontW;
	}
	if (ConfigureParams.Screen.bOverlayRec &&
	    (bRecordingYM || bRecordingWav || Avi_AreWeRecording()))
	{
		if (bDraw)
		{
			SDLGui_TextShadow(xoff, ytext, "REC");
			Statusbar_PanelLed(surf, xoff + 3*PanelFontW + 1, ytext, true);
		}
		xoff += 4*PanelFontW + 1 + PanelFontW;
	}
	if (ConfigureParams.Screen.bOverlayYm)
	{
		if (bDraw)
		{
			uint8_t levels[3];
			Sound_GetYmChannelLevels(levels);
			SDLGui_TextShadow(xoff, ytext, "YM");
		}
		xoff += 2*PanelFontW + 2;
		for (i = 0; i < 3; i++)
		{
			if (bDraw)
			{
				uint8_t levels[3];
				Sound_GetYmChannelLevels(levels);
				if (levels[i] >= PanelYmShown[i])
					PanelYmShown[i] = levels[i];
				else if (PanelYmShown[i] >= 2)
					PanelYmShown[i] -= 2;
				else
					PanelYmShown[i] = 0;
				Statusbar_PanelMeter(surf, xoff, ymeter, meterh, PanelYmShown[i]);
			}
			xoff += PANEL_METER_W + 2;
		}
		xoff += PanelFontW;
	}
	if (ConfigureParams.Screen.bOverlayDma &&
	    (Config_IsMachineSTE() || Config_IsMachineTT()))
	{
		if (bDraw)
			SDLGui_TextShadow(xoff, ytext, "DMA");
		xoff += 3*PanelFontW + 2;
		if (bDraw)
		{
			bool playing;
			int16_t left, right, lvl;
			DmaSnd_GetActivity(&playing, &left, &right);

			/* FrameLeft/Right hold 8 bit STE samples (-128..127) */
			lvl = playing ? (abs(left) * 31) / 127 : 0;
			if (lvl > 31)
				lvl = 31;
			if (lvl >= PanelDmaShownL) PanelDmaShownL = lvl;
			else if (PanelDmaShownL >= 2) PanelDmaShownL -= 2;
			else PanelDmaShownL = 0;
			Statusbar_PanelMeter(surf, xoff, ymeter, meterh, PanelDmaShownL);

			lvl = playing ? (abs(right) * 31) / 127 : 0;
			if (lvl > 31)
				lvl = 31;
			if (lvl >= PanelDmaShownR) PanelDmaShownR = lvl;
			else if (PanelDmaShownR >= 2) PanelDmaShownR -= 2;
			else PanelDmaShownR = 0;
			Statusbar_PanelMeter(surf, xoff + PANEL_METER_W + 2, ymeter, meterh, PanelDmaShownR);
		}
		xoff += 2*(PANEL_METER_W + 2) + PanelFontW;
	}
	if (ConfigureParams.Screen.bOverlayMessages && MessageList->msg[0])
	{
		int len = strlen(MessageList->msg);
		if (len > 48)
			len = 48;
		if (bDraw)
		{
			Str_Copy(buf, MessageList->msg, len + 1);
			SDLGui_TextShadow(xoff, ytext, buf);
		}
		xoff += len * PanelFontW;
	}
	return xoff;
}

/**
 * Draw the enabled panel items, centered. Called once per frame from
 * Statusbar_Update(); returns the panel rect when drawn, NULL when the
 * panel is disabled.
 *
 * The panel goes into the window's bottom padding when the screen is
 * being scaled and Screen_GetOverlaySurface() hands out a surface for
 * it. Otherwise it is drawn into the emulation frame buffer itself,
 * over the bottom of the Atari screen, which needs the pixels below it
 * saved so they can be put back before the next frame.
 */
static SDL_Rect* Statusbar_PanelDraw(SDL_Surface *surf)
{
	SDL_Surface *overlay, *target, *prev;
	int width, xstart, ytop;

	if (!ConfigureParams.Screen.bShowStatusOverlay)
		return NULL;

	/* genconv path has no separate restore hook: restore here */
	Statusbar_PanelRestore(surf);

	overlay = Screen_GetOverlaySurface(PanelRect.w, PanelRect.h);
	if (overlay)
	{
		target = overlay;
		ytop = 0;
	}
	else
	{
		if (!PanelUnderside)
		{
			SDL_PixelFormat *fmt = surf->format;
			PanelUnderside = SDL_CreateRGBSurface(surf->flags,
				PanelRect.w, PanelRect.h, fmt->BitsPerPixel,
				fmt->Rmask, fmt->Gmask, fmt->Bmask, fmt->Amask);
			if (!PanelUnderside)
				return NULL;
		}
		SDL_BlitSurface(surf, &PanelRect, PanelUnderside, NULL);
		bPanelDrawn = true;

		target = surf;
		ytop = PanelRect.y;
	}

	prev = SDLGui_SwapSurface(target);
	width = Statusbar_PanelWalk(target, 0, ytop, false);
	xstart = (target->w - width) / 2;
	if (xstart < PanelFontW / 2)
		xstart = PanelFontW / 2;

	/* no background box: the items are drawn straight onto whatever
	 * is behind them, the text carrying its own drop shadow
	 */
	Statusbar_PanelWalk(target, xstart, ytop, true);
	SDLGui_SwapSurface(prev);

	/* the caller uses this only to force the frame to be shown, which
	 * the overlay needs too for its meters to keep moving
	 */
	return &PanelRect;
}

/*-----------------------------------------------------------------------*/
/**
 * (re-)initialize statusbar internal variables for given screen surface
 * (sizes&colors may need to be re-calculated for the new SDL surface)
 * and draw the statusbar background.
 */
void Statusbar_Init(SDL_Surface *surf)
{
	int i;

	assert(surf);

	/* disable leds */
	for (i = 0; i < MAX_DRIVE_LEDS; i++)
	{
		Led[i].state = Led[i].oldstate = LED_STATE_OFF;
		Led[i].expire = 0;
	}

	StatusbarHeight = 0;
	ScreenHeight = surf->h;

	/* the overlay panel needs fonts and its geometry for this surface */
	SDLGui_Init();
	SDLGui_SetScreen(surf);
	Statusbar_PanelInit(surf);
}


/*-----------------------------------------------------------------------*/
/**
 * Queue new statusbar message 'msg' to be shown for 'msecs' milliseconds
 */
void Statusbar_AddMessage(const char *msg, Uint32 msecs)
{
	msg_item_t *item;

	item = calloc(1, sizeof(msg_item_t));
	assert(item);

	item->next = MessageList;
	MessageList = item;

	Str_Copy(item->msg, msg, sizeof(item->msg));
	DEBUGPRINT(("Add message: '%s'\n", item->msg));

	if (msecs)
	{
		item->timeout = msecs;
	}
	else
	{
		/* show items by default for 2.5 secs */
		item->timeout = 2500;
	}
	item->shown = false;
}

/*-----------------------------------------------------------------------*/
/**
 * Write given 'more' string to 'buffer' and return new end of 'buffer'
 */
static char *Statusbar_AddString(char *buffer, const char *more)
{
	if (!more)
		return buffer;
	while (*more)
		*buffer++ = *more++;
	return buffer;
}

/*-----------------------------------------------------------------------*/
/**
 * Retrieve/update default statusbar information
 */
void Statusbar_UpdateInfo(void)
{
	int size;
	char buffer[200];				/* large enough for any message */
	char *end;

	end = buffer;

	/* CPU MHz */
	if (ConfigureParams.System.nCpuFreq > 9)
	{
		*end++ = '0' + ConfigureParams.System.nCpuFreq / 10;
	}
	*end++ = '0' + ConfigureParams.System.nCpuFreq % 10;
	end = Statusbar_AddString(end, "MHz");

	/* CPU type */
	if(ConfigureParams.System.nCpuLevel > 0)
	{
		*end++ = '/';
		*end++ = '0';
		if ( ConfigureParams.System.nCpuLevel == 5 )	/* Special case : 68060 has nCpuLevel=5 */
			*end++ = '0' + 6;
		else
			*end++ = '0' + ConfigureParams.System.nCpuLevel % 10;
		*end++ = '0';
	}

	const char *mode = NULL;
	bool cache = ConfigureParams.System.bCpuDataCache && ConfigureParams.System.nCpuLevel > 2;
	/* Prefetch / cycle exact mode and data cache? */
	if ( ConfigureParams.System.bCycleExactCpu )
		mode = cache ? "(CED)" : "(CE)";
	else if ( ConfigureParams.System.bCompatibleCpu )
		mode = cache ? "(PFD)" : "(PF)";
	else if (cache)
		mode = "(D)";
	end = Statusbar_AddString(end, mode);

	/* additional WinUAE CPU/FPU info */
	*end++ = '/';
	switch (ConfigureParams.System.n_FPUType)
	{
	case FPU_68881:
		end = Statusbar_AddString(end, "68881");
		break;
	case FPU_68882:
		end = Statusbar_AddString(end, "68882");
		break;
	case FPU_CPU:
		end = ( ConfigureParams.System.nCpuLevel == 5 ? Statusbar_AddString(end, "060") : Statusbar_AddString(end, "040") );
		break;
	default:
		*end++ = '-';
	}
	if (ConfigureParams.System.bSoftFloatFPU && ConfigureParams.System.n_FPUType != FPU_NONE)
	{
		end = Statusbar_AddString(end, "(SF)");
	}
	if (ConfigureParams.System.bMMU)
	{
		end = Statusbar_AddString(end, "/MMU");
	}

	/* amount of memory in MB */
	*end++ = ' ';
	size = ConfigureParams.Memory.STRamSize_KB;
	end += sprintf(end, "%d", size / 1024);
	if ( size % 1024 == 256 )
		end += sprintf(end, ".25");
	else if ( size % 1024 == 512 )
		end += sprintf(end, ".5");

	if (TTmemory && ConfigureParams.Memory.TTRamSize_KB)
	{
		end += sprintf(end, "/%i", ConfigureParams.Memory.TTRamSize_KB/1024);
	}
	end = Statusbar_AddString(end, "MB ");

	/* machine type */
	switch (ConfigureParams.System.nMachineType)
	{
	case MACHINE_ST:
		end = Statusbar_AddString(end, "ST(");
		end = Statusbar_AddString(end, Video_GetTimings_Name());
		*end++ = ')';
		break;
	case MACHINE_MEGA_ST:
		end = Statusbar_AddString(end, "MegaST");
		break;
	case MACHINE_STE:
		end = Statusbar_AddString(end, "STE");
		break;
	case MACHINE_MEGA_STE:
		end = Statusbar_AddString(end, "MegaSTE");
		break;
	case MACHINE_TT:
		end = Statusbar_AddString(end, "TT");
		break;
	case MACHINE_FALCON:
		end = Statusbar_AddString(end, "Falcon");
		break;
	default:
		end = Statusbar_AddString(end, "???");
	}

	/* TOS type/version */
	end = Statusbar_AddString(end, ", ");
	if (bIsEmuTOS)
	{
		if (EmuTosVersion > 0)
		{
			char str[20];
			snprintf(str, sizeof(str), "EmuTOS %d.%d.%d",
			         EmuTosVersion >> 24, (EmuTosVersion >> 16) & 0xff,
			         (EmuTosVersion >> 8) & 0xff);
			end = Statusbar_AddString(end, str);
			if (EmuTosVersion & 0xff)
				end = Statusbar_AddString(end, "+");
		}
		else
		{
			end = Statusbar_AddString(end, "EmuTOS");
		}
	}
	else if (bUseLilo)
	{
		end = Statusbar_AddString(end, "Linux");
	}
	else
	{
		end = Statusbar_AddString(end, "TOS ");
		*end++ = '0' + ((TosVersion & 0xf00) >> 8);
		*end++ = '.';
		*end++ = '0' + ((TosVersion & 0xf0) >> 4);
		*end++ = '0' + (TosVersion & 0xf);
	}

	/* monitor type */
	end = Statusbar_AddString(end, ", ");
	if (bUseVDIRes)
	{
		end = Statusbar_AddString(end, "VDI");
	}
	else
	{
		switch (ConfigureParams.Screen.nMonitorType)
		{
		case MONITOR_TYPE_MONO:
			end = Statusbar_AddString(end, "MONO");
			break;
		case MONITOR_TYPE_RGB:
			end = Statusbar_AddString(end, "RGB");
			break;
		case MONITOR_TYPE_VGA:
			/* There were no VGA monitors for the ST/STE */
			if (Config_IsMachineST() || Config_IsMachineSTE())
				end = Statusbar_AddString(end, "RGB");
			else
				end = Statusbar_AddString(end, "VGA");
			break;
		case MONITOR_TYPE_TV:
			end = Statusbar_AddString(end, "TV");
			break;
		default:
			*end++ = '?';
		}
		end += sprintf(end, " %d Hz" , nScreenRefreshRate);
	}

	*end = '\0';

	Str_Copy(DefaultMessage.msg, buffer, MAX_MESSAGE_LEN);
	DEBUGPRINT(("Set default message: '%s'\n", DefaultMessage.msg));
	/* make sure default message gets (re-)drawn when next checked */
	DefaultMessage.shown = false;
}

/*-----------------------------------------------------------------------*/
/**
 * Save the area that will be left under overlay led
 */
void Statusbar_OverlayBackup(SDL_Surface *surf)
{
	/* the overlay panel saves its own underside in Statusbar_Update() */
	(void)surf;
}

/*-----------------------------------------------------------------------*/
/**
 * Restore the area left under overlay led
 * 
 * State machine for overlay led handling will return from
 * Statusbar_Update() call the area that is restored (if any)
 */
void Statusbar_OverlayRestore(SDL_Surface *surf)
{
	if (surf)
		Statusbar_PanelRestore(surf);
}

/*-----------------------------------------------------------------------*/
/**
 * Update statusbar information (leds etc) if/when needed.
 * 
 * May not be called when screen is locked (SDL limitation).
 * 
 * Return updated area, or NULL if nothing is drawn.
 */
/**
 * Advance the message queue: expire the shown message and move on to
 * the next one. The overlay panel draws whatever is at the queue head.
 */
static void Statusbar_MessageTick(Uint32 ticks)
{
	msg_item_t *next;

	if (MessageList->shown)
	{
		if (!MessageList->expire || MessageList->expire > ticks)
			return;
		assert(MessageList->next); /* last message shouldn't end here */
		next = MessageList->next;
		free(MessageList);
		MessageList = next;
	}
	MessageList->shown = true;
	if (MessageList->timeout && !MessageList->expire)
		MessageList->expire = ticks + MessageList->timeout;
}

SDL_Rect* Statusbar_Update(SDL_Surface *surf, bool do_update)
{
	Uint32 currentticks;
	SDL_Rect *last_rect;
	int i;

	/* Don't update anything on screen if video output is disabled */
	if ( ConfigureParams.Screen.DisableVideo )
		return NULL;
	assert(surf);

	currentticks = SDL_GetTicks();
	Statusbar_MessageTick(currentticks);
	for (i = 0; i < MAX_DRIVE_LEDS; i++)
	{
		if (Led[i].expire && Led[i].expire < currentticks)
		{
			Led[i].state = LED_STATE_OFF;
			Led[i].expire = 0;
		}
	}

	last_rect = Statusbar_PanelDraw(surf);
	if (do_update && last_rect)
	{
		Screen_UpdateRects(surf, 1, last_rect);
		last_rect = NULL;
	}
	return last_rect;
}
