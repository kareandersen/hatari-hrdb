/*
  Hatari - cpumeter.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  CPU meter for the status overlay, fed by the NF_CPUMETER native
  feature: a program tells which section of its work the CPU time from
  now on goes to - 0 for free time, 1 and up for the parts of its frame,
  such as music, a scroller or sprites - and the meter shows each frame,
  VBL to VBL, as those sections in the order they ran.

  That shows how much of a frame is still free, how large a part of it
  each section takes and where in the frame it runs, and - since a
  section carries on over a VBL until the program switches away from it
  - how many frames an effect really takes, when it takes more than one:
  its work runs from leaving section 0 until returning to it.

  Besides the native feature, the section can be switched by writing it
  to the byte register at $FFC125, next to Hatari's $FFC123 debugger
  trigger: one move.b instead of a native feature call, which costs far
  more than the meter can afford to leave out of the measurement.
  Reading the register gives the current section, so an interrupt can
  save and put back the one it interrupted.

  For scripts, every complete frame can also be logged to a file, one
  line each: the frame's VBL number, its length in CPU cycles, and its
  parts in the order they ran as section:cycles.
*/
const char CpuMeter_fileid[] = "Hatari cpumeter.c";

#include "main.h"
#include "configuration.h"
#include "cycles.h"
#include "video.h"
#include "ioMem.h"
#include "m68000.h"
#include "cpumeter.h"

#define MAX_PARTS 64		/* section switches recorded per frame */

static bool bActive;		/* a program has used the meter */
static uint32_t nSection;	/* where the CPU time goes now */
static uint64_t FrameStart;	/* clock at the VBL starting this frame */

/* the frame being recorded */
static cpumeter_part_t Parts[MAX_PARTS];
static int nParts;

/* the last complete frame, for the overlay */
static cpumeter_part_t ShownParts[MAX_PARTS];
static int nShownParts;
static uint32_t nShownLength;
static uint32_t nShownFree;	/* cycles in section 0 */
static uint32_t nShownLine;	/* CPU cycles per scanline */

/* the current work run, from leaving section 0, and the last one done */
static uint64_t RunStart;
static float fRunFrames;

static FILE *LogFile;		/* every complete frame goes here, if open */


/**
 * Give the CPU time from now on to 'section', 0 meaning free time.
 * Returns the section it went to before, so that e.g. an interrupt
 * handler can put that back when it is done.
 */
uint32_t CpuMeter_SetSection(uint32_t section)
{
	uint64_t now = CyclesGlobalClockCounter;
	uint32_t prev = nSection;

	if (!bActive)
	{
		/* the frame so far counts as free */
		bActive = true;
		Parts[0].start = 0;
		Parts[0].section = 0;
		nParts = 1;
		prev = nSection = 0;
	}
	if (section == nSection)
		return prev;

	if (nParts < MAX_PARTS)
	{
		Parts[nParts].start = now - FrameStart;
		Parts[nParts].section = section;
		nParts++;
	}
	else
	{
		/* out of room: the rest of the frame goes to the last part */
		Parts[MAX_PARTS - 1].section = section;
	}

	/* a work run is done when it gets back to free time, and as long
	 * as it took, in frames as long as the last one */
	if (nSection == 0)
		RunStart = now;
	else if (section == 0 && nShownLength)
		fRunFrames = (float)(now - RunStart) / nShownLength;

	nSection = section;
	return prev;
}

/**
 * The section the CPU time goes to now.
 */
uint32_t CpuMeter_GetSection(void)
{
	return nSection;
}

/**
 * $FFC125 written: switch to the section in the byte written.
 */
void CpuMeter_WriteByte(void)
{
	CpuMeter_SetSection(IoMem_ReadByte(CPUMETER_REGISTER));
}

/**
 * $FFC125 read: the current section.
 */
void CpuMeter_ReadByte(void)
{
	IoMem_WriteByte(CPUMETER_REGISTER, nSection);
}

/**
 * A new frame starts at 'frame_start': the one before it is complete.
 * The section the CPU is in carries on into the new frame.
 */
void CpuMeter_VBL(uint64_t frame_start)
{
	uint32_t length, free, end;
	int i;

	if (bActive && frame_start > FrameStart)
	{
		length = frame_start - FrameStart;
		free = 0;
		for (i = 0; i < nParts; i++)
		{
			end = (i + 1 < nParts) ? Parts[i + 1].start : length;
			if (Parts[i].section == 0 && end > Parts[i].start)
				free += end - Parts[i].start;
			ShownParts[i] = Parts[i];
		}
		nShownParts = nParts;
		nShownLength = length;
		nShownFree = free;
		nShownLine = nCyclesPerLine << nCpuFreqShift;

		if (LogFile)
		{
			/* nVBLs is still the number of the frame that ended */
			fprintf(LogFile, "%d %u", nVBLs, length);
			for (i = 0; i < nParts; i++)
			{
				end = (i + 1 < nParts) ? Parts[i + 1].start : length;
				if (end > Parts[i].start)
					fprintf(LogFile, " %u:%u", Parts[i].section,
						end - Parts[i].start);
			}
			fputc('\n', LogFile);
		}

		Parts[0].start = 0;
		Parts[0].section = nSection;
		nParts = 1;
	}
	FrameStart = frame_start;
}

/**
 * Log every complete frame from now on to 'path', or with NULL, stop
 * logging. Returns false if the file could not be opened.
 *
 * The log needs no closing on exit: exit() flushes and closes it.
 */
bool CpuMeter_SetLog(const char *path)
{
	if (LogFile)
	{
		fclose(LogFile);
		LogFile = NULL;
	}
	if (!path)
		return true;
	LogFile = fopen(path, "w");
	return LogFile != NULL;
}

/**
 * Forget it all, e.g. on reset, until a program uses the meter again.
 * Logging goes on: the frames simply start over.
 */
void CpuMeter_Reset(void)
{
	bActive = false;
	nSection = 0;
	nParts = nShownParts = 0;
	nShownLength = 0;
	fRunFrames = 0;
}

/**
 * The last complete frame: its parts, its length and its free time in
 * CPU cycles, the CPU cycles per scanline in it, and how many frames
 * the last work run took.
 * Returns false while no program uses the meter.
 */
bool CpuMeter_GetFrame(const cpumeter_part_t **parts, int *count,
		       uint32_t *length, uint32_t *free_cycles,
		       uint32_t *line_cycles, float *run_frames)
{
	if (!bActive || !nShownParts)
		return false;
	*parts = ShownParts;
	*count = nShownParts;
	*length = nShownLength;
	*free_cycles = nShownFree;
	*line_cycles = nShownLine ? nShownLine : 1;
	*run_frames = fRunFrames;
	return true;
}
