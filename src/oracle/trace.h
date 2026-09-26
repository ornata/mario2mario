/* Coverage trace for the translation pipeline (text, one record/line):
 *
 *   # m2m oracle trace v1
 *   icount <retired instructions>
 *   dma <icount> <rom_offset> <ram_addr> <length>      (hex, PI DMAs)
 *   task <icount> <type> <data_ptr> <data_size> <hash> (hex, RSP tasks)
 *   exec <start> <end>                                 (hex KSEG0, [start,end))
 *
 * `exec` ranges are maximal runs of RDRAM words that executed at least
 * once. They are physical RDRAM locations; the dma lines say which ROM
 * bytes occupied them over time. */
#ifndef M2M_ORACLE_TRACE_H
#define M2M_ORACLE_TRACE_H

#include <stdio.h>

#include "src/oracle/cpu.h"

void trace_write(FILE *f, const Oracle *o);

/* Number of executed RDRAM words. */
uint32_t trace_exec_words(const Oracle *o);

#endif
