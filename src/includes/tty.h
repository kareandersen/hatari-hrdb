/*
  Hatari - tty.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Helpers for serial emulation that is pointed at a host tty device:
  keep Hatari from reprogramming its own controlling terminal, and
  restore the settings of any other tty it reconfigured when it exits.
*/

#ifndef HATARI_TTY_H
#define HATARI_TTY_H

#include <stdbool.h>

extern bool Tty_IsOwnTerminal(int fd);
extern bool Tty_CanConfigure(int fd);
extern void Tty_SaveSettings(int fd);
extern void Tty_RestoreSettings(int fd);

#endif /* HATARI_TTY_H */
