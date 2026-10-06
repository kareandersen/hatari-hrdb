/*
  Hatari - cpumeter.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef HATARI_CPUMETER_H
#define HATARI_CPUMETER_H

/* a part of a frame given to one section, from 'start' CPU cycles after
 * the frame's VBL to the start of the next part */
typedef struct
{
	uint32_t start;
	uint32_t section;
} cpumeter_part_t;

/* byte register that switches the section when written to */
#define CPUMETER_REGISTER	0xffc125

extern uint32_t CpuMeter_SetSection(uint32_t section);
extern uint32_t CpuMeter_GetSection(void);
extern void CpuMeter_WriteByte(void);
extern void CpuMeter_ReadByte(void);
extern void CpuMeter_VBL(uint64_t frame_start);
extern void CpuMeter_Reset(void);
extern bool CpuMeter_GetFrame(const cpumeter_part_t **parts, int *count,
			      uint32_t *length, uint32_t *free_cycles,
			      uint32_t *line_cycles, float *run_frames);

#endif
