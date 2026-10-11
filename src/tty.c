/*
  Hatari - tty.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The RS232 (MFP) and SCC emulation can be pointed at host tty devices.
  When such a device is the terminal Hatari itself was started from
  (e.g. an output file of /dev/stderr or /dev/tty), putting it into raw
  mode or changing its baud rate breaks the user's shell: ^C and ^Z stop
  working once Hatari exits, because ISIG was cleared on the terminal.

  These helpers refuse to configure Hatari's own terminal, and remember
  the original settings of any other tty so they can be restored on exit.
*/
const char Tty_fileid[] = "Hatari tty.c";

#include "main.h"
#include "tty.h"

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#if HAVE_TERMIOS_H && HAVE_TCSETATTR
# include <termios.h>
# define TTY_HAVE_TERMIOS 1
#else
# define TTY_HAVE_TERMIOS 0
#endif

#if TTY_HAVE_TERMIOS

#define TTY_MAX_SAVED	8

typedef struct {
	int fd;
	struct termios attr;
} TTY_SAVED;

static TTY_SAVED TtySaved[TTY_MAX_SAVED];
static int nTtySaved;

static int Tty_FindSaved(int fd)
{
	int i;
	for (i = 0; i < nTtySaved; i++)
		if (TtySaved[i].fd == fd)
			return i;
	return -1;
}

/**
 * atexit() handler: restore every tty that is still reconfigured, so that
 * exit() paths which bypass the normal *_UnInit() functions (errors,
 * --run-vbls, ...) do not leave devices in raw mode either.
 */
static void Tty_RestoreAll(void)
{
	while (nTtySaved > 0)
		Tty_RestoreSettings(TtySaved[nTtySaved - 1].fd);
}

#endif /* TTY_HAVE_TERMIOS */


/**
 * Return true if the file descriptor refers to the terminal Hatari itself
 * is running on: its controlling terminal, or the device behind
 * stdin/stdout/stderr (so that e.g. "/dev/stderr" is recognized too).
 */
bool Tty_IsOwnTerminal(int fd)
{
	const int ownfds[3] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
	struct stat st, own;
	int i;

	if (fd < 0 || !isatty(fd))
		return false;

#if TTY_HAVE_TERMIOS
	/* tcgetpgrp() only succeeds on the controlling terminal of the caller */
	if (tcgetpgrp(fd) >= 0)
		return true;
#endif

	if (fstat(fd, &st) != 0)
		return false;

	for (i = 0; i < 3; i++)
	{
		if (!isatty(ownfds[i]) || fstat(ownfds[i], &own) != 0)
			continue;
		if (own.st_dev == st.st_dev && own.st_ino == st.st_ino
		    && own.st_rdev == st.st_rdev)
			return true;
	}
	return false;
}


/**
 * Return true if it is OK to change the terminal settings (speed, raw
 * mode etc) of the given file descriptor: it must be a tty, and it must
 * not be Hatari's own terminal.
 */
bool Tty_CanConfigure(int fd)
{
#if TTY_HAVE_TERMIOS
	return fd >= 0 && isatty(fd) && !Tty_IsOwnTerminal(fd);
#else
	return false;
#endif
}


/**
 * Remember the current settings of a tty so that they can be restored
 * with Tty_RestoreSettings(). Does nothing for descriptors that are not
 * configurable (see Tty_CanConfigure()).
 */
void Tty_SaveSettings(int fd)
{
#if TTY_HAVE_TERMIOS
	struct termios attr;

	if (!Tty_CanConfigure(fd) || Tty_FindSaved(fd) >= 0)
		return;
	if (nTtySaved >= TTY_MAX_SAVED)
		return;
	if (tcgetattr(fd, &attr) != 0)
		return;

	if (nTtySaved == 0)
	{
		static bool bAtExitRegistered;
		if (!bAtExitRegistered)
		{
			atexit(Tty_RestoreAll);
			bAtExitRegistered = true;
		}
	}
	TtySaved[nTtySaved].fd = fd;
	TtySaved[nTtySaved].attr = attr;
	nTtySaved++;
#endif
}


/**
 * Restore the settings saved for a tty with Tty_SaveSettings() and
 * forget them. Call this before closing the descriptor.
 */
void Tty_RestoreSettings(int fd)
{
#if TTY_HAVE_TERMIOS
	int i = Tty_FindSaved(fd);

	if (i < 0)
		return;

	tcsetattr(fd, TCSADRAIN, &TtySaved[i].attr);

	nTtySaved--;
	if (i != nTtySaved)
		TtySaved[i] = TtySaved[nTtySaved];
#endif
}
