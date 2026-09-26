/* Deterministic timebase: the lockstep contract between the oracle
 * interpreter and the native runtime. Both engines MUST implement these
 * rules identically; any drift breaks lockstep.
 *
 * Retired instruction count (icount)
 *   A 64-bit counter, 0 at reset (PC = 0xA4000040). It increments by 1
 *   for every instruction that completes. It does NOT increment for:
 *   an instruction that raises an exception (it does not complete), a
 *   branch-likely delay slot that is nullified, or interrupt entry.
 *
 * COP0 Count
 *   Count = (uint32_t)(icount + count_offset). mtc0 Count, v sets
 *   count_offset = v - icount (after the mtc0 itself retires, Count reads
 *   back v + 1 on the next instruction). One tick per instruction models
 *   the VR4300 Count rate (half the 93.75 MHz pipeline clock) at an
 *   average CPI of 2.
 *
 * COP0 Compare / timer interrupt
 *   After each retired instruction, if Count == Compare then Cause.IP7 is
 *   set. mtc0 Compare clears Cause.IP7.
 *
 * VI interrupt
 *   Raised (MI_INTR.VI) when icount reaches a multiple of HW_VI_PERIOD:
 *   46,875,000 Count ticks per second / 60 fields per second. VI_V_CURRENT
 *   reads as ((icount % HW_VI_PERIOD) * 525 / HW_VI_PERIOD) & ~1 (NTSC
 *   half-lines, even field).
 *
 * Device completion
 *   PI and SI DMA complete within the store that starts them, and raise
 *   their MI interrupt immediately. An RSP task started by clearing
 *   SP_STATUS.HALT completes HW_SP_TASK_LATENCY retired instructions
 *   later (HLE; see hw.c).
 *
 * Interrupt sampling points (block boundaries)
 *   Interrupts are only taken at dynamic block boundaries, which are:
 *   right after the delay slot of any branch or jump completes (taken or
 *   not), right after a branch-likely whose delay slot was nullified,
 *   right after eret, and right after exception entry. At a boundary the
 *   checkpoint hash (see src/oracle/checkpoint.h) is emitted first, then
 *   a pending, enabled interrupt is taken. */
#ifndef M2M_HW_TIMEBASE_H
#define M2M_HW_TIMEBASE_H

#define HW_VI_PERIOD       781250u /* retired instructions per VI field */
#define HW_VI_HALF_LINES   525u
#define HW_SP_TASK_LATENCY 20000u /* retired instructions per RSP task */

#endif
