/* Checkpoint hash stream: the lockstep record both engines must produce.
 *
 * At every block boundary (src/hw/timebase.h), before any interrupt is
 * taken, the CPU state is folded with hw_hash_word() (src/hw/hw.h),
 * starting from HW_HASH_INIT, over these 64-bit words in this order:
 *
 *   pc (zero-extended), gpr[0..31], hi, lo, fpr[0..31] (raw 64-bit
 *   storage; FR=0 pairs live in the even register), fcr31, Status,
 *   Cause (stored bits: IP2 excluded since it is a live MI line),
 *   EPC, ErrorEPC, BadVAddr, Count (as read by mfc0), Compare, llbit,
 *   icount.
 *
 * Every `stride`-th boundary (1 = all) is written as a 24-byte
 * little-endian record: u64 icount, u32 pc, u32 zero, u64 hash. */
#ifndef M2M_ORACLE_CHECKPOINT_H
#define M2M_ORACLE_CHECKPOINT_H

#include <stdint.h>
#include <stdio.h>

#include "src/oracle/cpu.h"

uint64_t checkpoint_hash(const CpuState *c);
void checkpoint_write(FILE *f, const CpuState *c, uint64_t hash);

#endif
