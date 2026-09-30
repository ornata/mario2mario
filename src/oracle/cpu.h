/* R4300i reference interpreter (the test oracle).
 *
 * Decodes raw instruction bits itself (independently of the listing
 * decoder in src/tools, so a table bug there cannot hide here) and
 * executes one instruction at a time against the shared HwState.
 * Timing, interrupt sampling and Count follow src/hw/timebase.h. */
#ifndef M2M_ORACLE_CPU_H
#define M2M_ORACLE_CPU_H

#include <stdint.h>
#include <stdio.h>

#include "src/hw/hw.h"

/* COP0 register numbers. */
enum {
  CP0_INDEX = 0,
  CP0_RANDOM = 1,
  CP0_ENTRYLO0 = 2,
  CP0_ENTRYLO1 = 3,
  CP0_CONTEXT = 4,
  CP0_PAGEMASK = 5,
  CP0_WIRED = 6,
  CP0_BADVADDR = 8,
  CP0_COUNT = 9,
  CP0_ENTRYHI = 10,
  CP0_COMPARE = 11,
  CP0_STATUS = 12,
  CP0_CAUSE = 13,
  CP0_EPC = 14,
  CP0_PRID = 15,
  CP0_CONFIG = 16,
  CP0_LLADDR = 17,
  CP0_XCONTEXT = 20,
  CP0_TAGLO = 28,
  CP0_TAGHI = 29,
  CP0_ERROREPC = 30,
};

/* Status bits. */
#define SR_IE  (1u << 0)
#define SR_EXL (1u << 1)
#define SR_ERL (1u << 2)
#define SR_BEV (1u << 22)
#define SR_FR  (1u << 26)
#define SR_CU1 (1u << 29)

/* Cause bits. */
#define CAUSE_IP2 (1u << 10)
#define CAUSE_IP7 (1u << 15)
#define CAUSE_BD  (1u << 31)

/* Exception codes. */
enum {
  EXC_INT = 0,
  EXC_ADEL = 4,
  EXC_ADES = 5,
  EXC_SYS = 8,
  EXC_BP = 9,
  EXC_RI = 10,
  EXC_CPU = 11,
  EXC_OV = 12,
  EXC_TR = 13,
  EXC_FPE = 15,
};

/* FCR31 fields. Cause/enable/flag groups share the bit order
 * I, U, O, Z, V (and E, cause only). */
#define FCR31_RM        3u
#define FCR31_FLAGS_SH  2
#define FCR31_ENABLE_SH 7
#define FCR31_CAUSE_SH  12
#define FCR31_C         (1u << 23)
#define FCR31_FS        (1u << 24)
#define FCR31_MASK      0x0183FFFFu
enum { FPX_I = 1, FPX_U = 2, FPX_O = 4, FPX_Z = 8, FPX_V = 16, FPX_E = 32 };

typedef struct {
  uint64_t page_mask, entry_hi, entry_lo0, entry_lo1;
} TlbEntry;

typedef struct {
  uint64_t gpr[32];
  uint64_t hi, lo;
  uint64_t fpr[32]; /* FR=0: even/odd pairs share fpr[even] (lo/hi half) */
  uint32_t fcr0, fcr31;
  uint64_t cop0[32]; /* Count is derived, see timebase.h */
  uint32_t count_offset;
  uint32_t pc, next_pc; /* address of the next instruction / the one after */
  uint32_t cur_pc;      /* instruction being executed */
  uint8_t in_delay;     /* instruction at pc is a delay slot */
  uint8_t cur_delay;    /* instruction being executed is a delay slot */
  uint8_t boundary;     /* a block boundary precedes the instruction at pc */
  uint8_t llbit;
  uint64_t icount; /* retired instructions */
  TlbEntry tlb[32];
} CpuState;

typedef enum {
  STOP_NONE = 0,
  STOP_BUDGET,    /* instruction budget exhausted */
  STOP_EXCEPTION, /* exception entered and stop_on_exception is set */
} StopReason;

/* Optional capture of every distinct (vaddr, instruction word) executed,
 * with the icount of its first execution: an open-addressed hash set. */
typedef struct {
  uint64_t *keys;  /* vaddr << 32 | word, 0 = empty */
  uint64_t *first; /* icount of first execution */
  uint32_t n, cap;
} ExecWords;

typedef struct {
  CpuState cpu;
  HwState *hw;
  ExecWords *exec_words; /* NULL unless capturing */

  /* Trace: one bit per RDRAM word that has executed. */
  uint8_t *exec_bits;

  /* Checkpoint stream (optional): every `stride`-th block boundary. */
  FILE *checkpoints;
  uint64_t checkpoint_stride, boundaries;
  uint64_t checkpoint_from; /* no records before this icount (0: all) */

  /* Last 256 executed PCs (ring, for diagnostics). */
  uint32_t history[256];
  uint32_t history_pos;

  StopReason stop;
  uint8_t stop_on_exception; /* stop right after entering any exception */

  /* Milestones and counters. */
  uint32_t entry_pc;
  uint64_t entry_icount;      /* first time pc == entry_pc (0: never) */
  uint64_t first_eret_icount; /* first eret after entry (0: never) */
  uint64_t eret_count, interrupts, exceptions[32];
  uint64_t tlb_mapped_accesses, tlb_exceptions;
} Oracle;

void oracle_init(Oracle *o, HwState *hw);
void oracle_free(Oracle *o);

/* Runs until icount reaches `until` or execution stops; may be called
 * again to continue. */
StopReason oracle_run(Oracle *o, uint64_t until);

/* Records (vaddr, word) in the capture set (first execution wins). */
void exec_words_add(ExecWords *e, uint32_t vaddr, uint32_t word,
                    uint64_t icount);

/* Enters the exception vector for `code` (EPC/BD from cur_pc/cur_delay).
 * `ce` is the coprocessor number for EXC_CPU. */
void cpu_exception(Oracle *o, unsigned code, unsigned ce);
/* As cpu_exception, with an explicit vector offset (0x000: TLB refill). */
void cpu_exception_at(Oracle *o, unsigned code, unsigned ce, uint32_t offset);

/* COP1: executes a COP1-opcode word other than BC1. Returns 1 if the
 * instruction completed, 0 if it raised an exception. */
int fpu_exec(Oracle *o, uint32_t w);
/* Writes FCR31 (ctc1 $31), updating the host rounding mode. Returns 0 if
 * the write raised a floating-point exception. */
int fpu_write_fcr31(Oracle *o, uint32_t v);
/* Makes the host rounding mode follow FCR31.RM. */
void fpu_sync_host(const CpuState *c);

static inline uint64_t sext32(uint32_t v) {
  return (uint64_t)(int64_t)(int32_t)v;
}

/* FPR access honouring Status.FR. */
static inline uint32_t fpr_get32(const CpuState *c, unsigned i) {
  if (c->cop0[CP0_STATUS] & SR_FR)
    return (uint32_t)c->fpr[i];
  return i & 1 ? (uint32_t)(c->fpr[i & ~1u] >> 32) : (uint32_t)c->fpr[i];
}

static inline void fpr_set32(CpuState *c, unsigned i, uint32_t v) {
  if (!(c->cop0[CP0_STATUS] & SR_FR) && (i & 1)) {
    uint64_t *r = &c->fpr[i & ~1u];
    *r = (*r & 0xFFFFFFFFull) | (uint64_t)v << 32;
  } else {
    c->fpr[i] = (c->fpr[i] & 0xFFFFFFFF00000000ull) | v;
  }
}

static inline uint64_t fpr_get64(const CpuState *c, unsigned i) {
  return c->fpr[c->cop0[CP0_STATUS] & SR_FR ? i : i & ~1u];
}

static inline void fpr_set64(CpuState *c, unsigned i, uint64_t v) {
  c->fpr[c->cop0[CP0_STATUS] & SR_FR ? i : i & ~1u] = v;
}

#endif
