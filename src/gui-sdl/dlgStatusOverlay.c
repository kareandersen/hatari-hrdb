/*
  Hatari - dlgStatusOverlay.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Item selection for the configurable status overlay panel.
*/
const char DlgStatusOverlay_fileid[] = "Hatari dlgStatusOverlay.c";

#include "main.h"
#include "configuration.h"
#include "dialog.h"
#include "sdlgui.h"
#include "statusbar.h"

#define DLGSOVL_SHOW      2
#define DLGSOVL_DRIVES    4
#define DLGSOVL_FDC       5
#define DLGSOVL_JOY       6
#define DLGSOVL_FSKIP     7
#define DLGSOVL_REC       8
#define DLGSOVL_MSG       9
#define DLGSOVL_YM        10
#define DLGSOVL_DMA       11
#define DLGSOVL_EXIT      13

/* The status overlay dialog: */
static SGOBJ overlaydlg[] =
{
	{ SGBOX,      0, 0,  0, 0, 38,16, NULL },
	{ SGTEXT,     0, 0, 12, 1, 14,1, "Status overlay" },
	{ SGCHECKBOX, 0, 0,  2, 3, 15,1, "Show _overlay" },
	{ SGTEXT,     0, 0,  2, 5,  6,1, "Items:" },
	{ SGCHECKBOX, 0, 0,  4, 7, 13,1, "_Drive leds" },
	{ SGCHECKBOX, 0, 0,  4, 8, 13,1, "_FDC info" },
	{ SGCHECKBOX, 0, 0,  4, 9, 13,1, "_Joysticks" },
	{ SGCHECKBOX, 0, 0,  4,10, 13,1, "F_rame skip" },
	{ SGCHECKBOX, 0, 0, 21, 7, 14,1, "R_ecording" },
	{ SGCHECKBOX, 0, 0, 21, 8, 14,1, "_Messages" },
	{ SGCHECKBOX, 0, 0, 21, 9, 14,1, "_YM levels" },
	{ SGCHECKBOX, 0, 0, 21,10, 14,1, "DM_A levels" },
	{ SGTEXT,     0, 0,  2,12, 34,1, "Toggle with the StatusOverlay key" },
	{ SGBUTTON, SG_DEFAULT, 0, 9,14, 20,1, "Back to main menu" },
	{ SGSTOP, 0, 0, 0,0, 0,0, NULL }
};

static const struct {
	int obj;
	bool *pValue;
} overlayItems[] = {
	{ DLGSOVL_SHOW,   &ConfigureParams.Screen.bShowStatusOverlay },
	{ DLGSOVL_DRIVES, &ConfigureParams.Screen.bOverlayDriveLeds },
	{ DLGSOVL_FDC,    &ConfigureParams.Screen.bOverlayFdc },
	{ DLGSOVL_JOY,    &ConfigureParams.Screen.bOverlayJoysticks },
	{ DLGSOVL_FSKIP,  &ConfigureParams.Screen.bOverlayFrameSkips },
	{ DLGSOVL_REC,    &ConfigureParams.Screen.bOverlayRec },
	{ DLGSOVL_MSG,    &ConfigureParams.Screen.bOverlayMessages },
	{ DLGSOVL_YM,     &ConfigureParams.Screen.bOverlayYm },
	{ DLGSOVL_DMA,    &ConfigureParams.Screen.bOverlayDma },
};

/**
 * Show and process the status overlay dialog.
 */
void Dialog_StatusOverlayDlg(void)
{
	unsigned int i;
	int but;

	SDLGui_CenterDlg(overlaydlg);

	for (i = 0; i < ARRAY_SIZE(overlayItems); i++)
	{
		if (*overlayItems[i].pValue)
			overlaydlg[overlayItems[i].obj].state |= SG_SELECTED;
		else
			overlaydlg[overlayItems[i].obj].state &= ~SG_SELECTED;
	}

	do
	{
		but = SDLGui_DoDialog(overlaydlg);
	}
	while (but != DLGSOVL_EXIT && but != SDLGUI_QUIT
	       && but != SDLGUI_ERROR && !bQuitProgram);

	for (i = 0; i < ARRAY_SIZE(overlayItems); i++)
		*overlayItems[i].pValue = (overlaydlg[overlayItems[i].obj].state & SG_SELECTED) != 0;

	Statusbar_PanelReset();
}
