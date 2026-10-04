/*
  Hatari - monitorframe.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef HATARI_MONITORFRAME_H
#define HATARI_MONITORFRAME_H

extern void MonitorFrame_Draw(SDL_Renderer *renderer, int win_w, int win_h,
			      int display_w, int display_h,
			      const SDL_Rect *screen, const SDL_Rect *atari);
extern void MonitorFrame_Free(void);

#endif
