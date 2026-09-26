/* Native runtime: translated AArch64 code + the shared hardware model.
 *
 * NativeState extends the oracle's CpuState (same layout at offset 0, so
 * checkpoint_hash() and the boot code are shared verbatim). The fields
 * after CpuState that translated code touches have frozen offsets that
 * src/native/m2m.inc repeats; static asserts in runtime.c keep the two in
 * sync. Everything else is runtime-private.
 *
 * Register contract while translated code runs (see PROMPT-translate.md):
 *   x1..x15  = MIPS $1..$15     x19..x25 = MIPS $19..$25    x29 = MIPS $29
 *   MIPS $16,$17,$18,$26,$27,$28,$30,$31 live in cpu.gpr[] (memory)
 *   x26 = host address of RDRAM   x27 = NativeState*   x28 = icount
 *   x0, x16, x17, x30 = scratch   x18 = never touched (Apple platform)
 *   d0..d7 = FP scratch (preserved by runtime macros)
 */
#ifndef M2M_NATIVE_NATIVE_H
#define M2M_NATIVE_NATIVE_H

#include <stdint.h>
#include <stdio.h>

#include "src/hw/hw.h"
#include "src/oracle/cpu.h"

/* One translated unit (from gen/registry.c). */
typedef struct {
  const char *id;
  uint32_t n;             /* MIPS instructions present in the unit */
  const uint32_t *pcs;    /* their virtual addresses, ascending */
  const uint32_t *words;  /* expected instruction words */
  const uint8_t *code;    /* unit start symbol */
  const int32_t *entries; /* per pc: offset from code, or -1 (delay slot) */
} M2mUnit;

extern const M2mUnit m2m_units[];
extern const uint32_t m2m_unit_count;

typedef enum {
  NSTOP_NONE = 0,
  NSTOP_BUDGET = 1,
  NSTOP_UNTRANSLATED = 3, /* dispatch to code with no matching unit */
  NSTOP_UNSUPPORTED = 4,  /* outside the contract (TLB fetch, FR=1 FPU, ..) */
} NativeStop;

#define NATIVE_SP_ENTRIES       (HW_SP_MEM_SIZE / 4u)
#define NATIVE_DISPATCH_ENTRIES (HW_RDRAM_SIZE / 4u + NATIVE_SP_ENTRIES)

typedef struct NativeState {
  CpuState cpu; /* offset 0, 1848 bytes */

  /* ---- frozen offsets (m2m.inc) ---- */
  uint64_t boundary_left; /* 1848: boundaries until the next checkpoint */
  uint64_t attn;          /* 1856: icount at which a boundary goes slow */
  void **dispatch;        /* 1864: code pointer per physical word */
  uint8_t *codemap;       /* 1872: 1 per RDRAM word that is linked code */
  uint32_t branch_taken;  /* 1880: delay-slot branch condition */
  uint32_t stop;          /* 1884: NativeStop, checked on redirect */
  uint8_t *rdram;         /* 1888: == hw->rdram */
  uint64_t tmp;           /* 1896: scratch that survives macro calls */

  /* ---- runtime-private ---- */
  HwState *hw;
  uint64_t budget;          /* stop at the first boundary at/after this */
  uint64_t stride;          /* checkpoint every stride-th boundary */
  uint64_t boundaries;      /* boundaries so far (matches the oracle) */
  FILE *checkpoints;        /* NULL: none */
  uint64_t compare_checked; /* IP7 settled for icount <= this */
  uint32_t *unitmap;        /* per physical word: linked unit + 1, or 0 */
  uint32_t *pc_index;       /* all (unit, pc) pairs sorted by pc: unit id */
  uint32_t *pc_sorted;      /* ... and the pcs */
  uint32_t *pc_slot;        /* ... and the index within the unit */
  uint32_t npcs;
  uint32_t dma_seen;    /* PI DMA records already processed */
  const char *trap_log; /* where untranslated entries are logged */
  uint32_t stop_pc;     /* diagnostics for NSTOP_* */
  const char *stop_why;
  uint64_t links, unlinks, exceptions, interrupts;
  uint8_t started;
} NativeState;

/* Sets up `ns` over `hw` (already initialised); boot state must be put in
 * ns->cpu by the caller (boot_pif_hle). */
void native_init(NativeState *ns, HwState *hw);
void native_free(NativeState *ns);

/* Runs translated code until a stop; returns the NativeStop reason. After
 * NSTOP_BUDGET, raise ns->budget and call again to continue exactly. */
NativeStop native_run(NativeState *ns);

#endif
