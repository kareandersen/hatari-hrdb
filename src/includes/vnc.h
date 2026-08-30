/*
  Hatari - vnc.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef HATARI_VNC_H
#define HATARI_VNC_H

#include "config.h"
#include <SDL.h>

#ifdef HAVE_VNCSERVER

extern void Vnc_Init(void);
extern void Vnc_UnInit(void);
extern void Vnc_SetSurface(SDL_Surface *surf);
extern void Vnc_RecordFrame(void);
extern void Vnc_RecordAudio(void);
extern void Vnc_Update(void);
extern bool Vnc_HasClients(void);
extern bool Vnc_IsActive(void);
extern bool Vnc_AudioStreaming(void);
extern void Vnc_SetGuiMode(bool bEnter);

#else /* !HAVE_VNCSERVER */

static inline void Vnc_Init(void) { }
static inline void Vnc_UnInit(void) { }
static inline void Vnc_SetSurface(SDL_Surface *surf) { (void)surf; }
static inline void Vnc_RecordFrame(void) { }
static inline void Vnc_RecordAudio(void) { }
static inline void Vnc_Update(void) { }
static inline bool Vnc_HasClients(void) { return false; }
static inline bool Vnc_IsActive(void) { return false; }
static inline bool Vnc_AudioStreaming(void) { return false; }
static inline void Vnc_SetGuiMode(bool bEnter) { (void)bEnter; }

#endif /* HAVE_VNCSERVER */

#endif /* HATARI_VNC_H */
