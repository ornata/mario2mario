/* Native runtime services called from translated code (via entry.s).
 *
 * Every service mirrors the oracle's definition of the same behaviour
 * (src/oracle/cpu.c, fpu.c) and the normative timing in
 * src/hw/timebase.h, so both engines produce identical checkpoint
 * streams. Code identity: a unit is linked into the dispatch table only
 * when RAM holds exactly its instruction words (content-keyed, which
 * covers PI DMA overlays and CPU-copied code alike); stores to linked
 * code words and PI DMAs over them unlink the unit again. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "src/native/native.h"
#include "src/oracle/checkpoint.h"

_Static_assert(offsetof(NativeState, boundary_left) == 1848, "m2m.inc");
_Static_assert(offsetof(NativeState, attn) == 1856, "m2m.inc");
_Static_assert(offsetof(NativeState, dispatch) == 1864, "m2m.inc");
_Static_assert(offsetof(NativeState, codemap) == 1872, "m2m.inc");
_Static_assert(offsetof(NativeState, branch_taken) == 1880, "m2m.inc");
_Static_assert(offsetof(NativeState, stop) == 1884, "m2m.inc");
_Static_assert(offsetof(NativeState, rdram) == 1888, "entry.s");
_Static_assert(offsetof(NativeState, tmp) == 1896, "m2m.inc");
_Static_assert(offsetof(CpuState, hi) == 256, "m2m.inc");
_Static_assert(offsetof(CpuState, lo) == 264, "m2m.inc");
_Static_assert(offsetof(CpuState, fpr) == 272, "m2m.inc");
_Static_assert(offsetof(CpuState, fcr31) == 532, "m2m.inc");
_Static_assert(offsetof(CpuState, cop0) + 8 * CP0_STATUS == 632, "m2m.inc");
_Static_assert(offsetof(CpuState, pc) == 796, "entry.s");
_Static_assert(offsetof(CpuState, llbit) == 811, "m2m.inc");
_Static_assert(offsetof(CpuState, icount) == 816, "m2m.inc");

typedef struct {
  uint64_t value, redirect;
} RtRet;

/* Assembly entry points (entry.s). */
void m2m_enter(NativeState *ns, const void *code);
void m2m_lazy(void);
void m2m_redirect(void);

#define RDRAM_WORDS (HW_RDRAM_SIZE / 4u)

/* Unit descriptors: every unit object contributes one M2mUnit to this
 * Mach-O section; the linker provides its bounds. */
extern const M2mUnit m2m_sect_start __asm("section$start$__DATA$__m2m_units");
extern const M2mUnit m2m_sect_end __asm("section$end$__DATA$__m2m_units");
const M2mUnit *m2m_units;
uint32_t m2m_unit_count;

/* ---- small helpers ---------------------------------------------------- */

static void stop(NativeState *ns, NativeStop why, uint32_t pc,
                 const char *msg) {
  if (!ns->stop) {
    ns->stop = why;
    ns->stop_pc = pc;
    ns->stop_why = msg;
  }
}

static uint32_t count_now(const NativeState *ns) {
  return (uint32_t)(ns->cpu.icount + ns->cpu.count_offset);
}

static void run_events(NativeState *ns) {
  HwState *hw = ns->hw;
  if (ns->cpu.icount >= hw->next_event)
    hw_run_events(hw, ns->cpu.icount);
  hw->now = ns->cpu.icount;
}

/* Cause.IP7 for every retired instruction up to icount (timebase.h). */
static void settle_compare(NativeState *ns) {
  CpuState *c = &ns->cpu;
  uint64_t now = c->icount, last = ns->compare_checked;
  if (now <= last)
    return;
  uint32_t d =
      (uint32_t)c->cop0[CP0_COMPARE] - (uint32_t)(last + 1 + c->count_offset);
  if ((uint64_t)d < now - last)
    c->cop0[CP0_CAUSE] |= CAUSE_IP7;
  ns->compare_checked = now;
}

static uint64_t cause_live(const NativeState *ns) {
  return (ns->cpu.cop0[CP0_CAUSE] & ~(uint64_t)CAUSE_IP2) |
         (hw_irq(ns->hw) ? CAUSE_IP2 : 0);
}

static int interrupt_pending(const NativeState *ns) {
  uint64_t sr = ns->cpu.cop0[CP0_STATUS];
  if (!(sr & SR_IE) || (sr & (SR_EXL | SR_ERL)))
    return 0;
  return (cause_live(ns) & sr & 0xFF00u) != 0;
}

static void update_attn(NativeState *ns) {
  if (interrupt_pending(ns)) {
    ns->attn = 0;
    return;
  }
  const CpuState *c = &ns->cpu;
  uint64_t a =
      ns->hw->next_event < ns->budget ? ns->hw->next_event : ns->budget;
  uint64_t last = ns->compare_checked;
  uint32_t d =
      (uint32_t)c->cop0[CP0_COMPARE] - (uint32_t)(last + 1 + c->count_offset);
  uint64_t hit = last + 1 + d;
  if (hit < a)
    a = hit;
  ns->attn = a;
}

/* Port of cpu_exception(): EPC/BD from (pc, ds), vector at `offset`. */
static void take_exception(NativeState *ns, unsigned code, unsigned ce,
                           uint32_t pc, int ds, uint32_t offset) {
  CpuState *c = &ns->cpu;
  uint64_t status = c->cop0[CP0_STATUS], cause = c->cop0[CP0_CAUSE];
  cause &= ~(uint64_t)(0x7Cu | (3u << 28));
  cause |= (uint64_t)code << 2 | (uint64_t)(ce & 3u) << 28;
  if (!(status & SR_EXL)) {
    c->cop0[CP0_EPC] = sext32(ds ? pc - 4 : pc);
    cause = ds ? cause | CAUSE_BD : cause & ~(uint64_t)CAUSE_BD;
  }
  c->cop0[CP0_CAUSE] = (uint32_t)cause;
  c->cop0[CP0_STATUS] = status | SR_EXL;
  c->pc = (status & SR_BEV ? 0xBFC00200u : 0x80000000u) + offset;
  ns->exceptions++;
}

/* Everything the oracle does at a block boundary (timebase.h), looping
 * when an interrupt makes the vector itself a boundary. `counted`: the
 * assembly fast path already consumed this boundary's count. */
static void boundary_loop(NativeState *ns, int counted) {
  CpuState *c = &ns->cpu;
  for (;;) {
    if (c->icount >= ns->budget) {
      /* Stop before this boundary; give back the count the fast path
       * took so a later native_run() processes it exactly once. */
      if (counted)
        ns->boundary_left++;
      stop(ns, NSTOP_BUDGET, c->pc, "instruction budget");
      return;
    }
    ns->boundaries++;
    if (!counted)
      ns->boundary_left--;
    counted = 0;
    settle_compare(ns);
    if (ns->boundary_left == 0) {
      if (ns->checkpoints)
        checkpoint_write(ns->checkpoints, c, checkpoint_hash(c));
      ns->boundary_left = ns->stride;
    }
    run_events(ns);
    if (!interrupt_pending(ns))
      break;
    take_exception(ns, EXC_INT, 0, c->pc, 0, 0x180);
    ns->interrupts++;
  }
  update_attn(ns);
}

/* ---- dispatch table and code identity ---------------------------------- */

static int dispatch_index(uint32_t va, uint32_t *idx) {
  if ((va & 0xC0000000u) != 0x80000000u)
    return 0;
  uint32_t pa = va & 0x1FFFFFFFu;
  if (pa < HW_RDRAM_SIZE) {
    *idx = pa >> 2;
    return 1;
  }
  if ((pa & 0xFFFFE000u) == 0x04000000u) {
    *idx = RDRAM_WORDS + ((pa & 0x1FFFu) >> 2);
    return 1;
  }
  return 0;
}

static uint32_t word_at(const NativeState *ns, uint32_t idx) {
  if (idx < RDRAM_WORDS)
    return hw_be32(ns->hw->rdram + 4u * idx);
  return hw_be32(ns->hw->sp_mem + 4u * (idx - RDRAM_WORDS));
}

static void unlink_unit(NativeState *ns, uint32_t u) {
  const M2mUnit *un = &m2m_units[u];
  for (uint64_t i = 0; i < un->n; i++) {
    uint32_t idx;
    if (!dispatch_index(un->rows[i].pc, &idx) || ns->unitmap[idx] != u + 1)
      continue;
    ns->unitmap[idx] = 0;
    ns->dispatch[idx] = (void *)m2m_lazy;
    if (idx < RDRAM_WORDS)
      ns->codemap[idx] = 0;
  }
  ns->unlinks++;
}

static void unlink_words(NativeState *ns, uint32_t first, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    uint32_t idx = first + i;
    if (idx < NATIVE_DISPATCH_ENTRIES && ns->unitmap[idx])
      unlink_unit(ns, ns->unitmap[idx] - 1);
  }
}

/* Rows [first, end) of unit u: a run of consecutive addresses. Runs are
 * validated and linked independently, since one unit may pack code that
 * reaches memory at different times (e.g. SP memory and RDRAM). */
static void run_bounds(const M2mUnit *un, uint32_t slot, uint32_t *first,
                       uint32_t *end) {
  uint32_t a = slot, b = slot + 1;
  while (a > 0 && un->rows[a - 1].pc + 4 == un->rows[a].pc)
    a--;
  while (b < un->n && un->rows[b - 1].pc + 4 == un->rows[b].pc)
    b++;
  *first = a;
  *end = b;
}

static int run_matches(const NativeState *ns, uint32_t u, uint32_t first,
                       uint32_t end) {
  const M2mUnit *un = &m2m_units[u];
  for (uint32_t i = first; i < end; i++) {
    uint32_t idx;
    if (!dispatch_index(un->rows[i].pc, &idx) ||
        word_at(ns, idx) != un->rows[i].word)
      return 0;
  }
  return 1;
}

static void link_run(NativeState *ns, uint32_t u, uint32_t first,
                     uint32_t end) {
  const M2mUnit *un = &m2m_units[u];
  /* A delay-slot row (entry -1) is not an entry point: it never takes a
   * word over from another unit, since the same instruction may also be
   * translated as an ordinary entry elsewhere. */
  for (uint32_t i = first; i < end; i++) {
    uint32_t idx;
    if (un->rows[i].entry >= 0 && dispatch_index(un->rows[i].pc, &idx) &&
        ns->unitmap[idx] && ns->unitmap[idx] != u + 1)
      unlink_unit(ns, ns->unitmap[idx] - 1);
  }
  for (uint32_t i = first; i < end; i++) {
    uint32_t idx;
    if (!dispatch_index(un->rows[i].pc, &idx))
      continue;
    if (un->rows[i].entry < 0 && ns->unitmap[idx] && ns->unitmap[idx] != u + 1)
      continue;
    ns->unitmap[idx] = u + 1;
    if (idx < RDRAM_WORDS)
      ns->codemap[idx] = 1;
    ns->dispatch[idx] = un->rows[i].entry < 0
                            ? (void *)m2m_lazy
                            : (void *)(un->code + un->rows[i].entry);
  }
  ns->links++;
}

/* Where the code at `idx` came from, for the untranslated-entry log. */
static void describe_origin(const NativeState *ns, uint32_t idx, char *buf,
                            size_t cap) {
  if (idx >= RDRAM_WORDS) {
    snprintf(buf, cap, "sp-mem");
    return;
  }
  uint32_t pa = idx * 4u;
  const HwState *hw = ns->hw;
  for (uint32_t i = hw->dma_count; i-- > 0;) {
    const HwDmaRecord *d = &hw->dma_log[i];
    if (pa >= d->ram_addr && pa < d->ram_addr + d->length) {
      snprintf(buf, cap, "pi-dma rom=0x%X ram=0x%X len=0x%X icount=%llu",
               d->rom_offset + (pa - d->ram_addr), d->ram_addr, d->length,
               (unsigned long long)d->icount);
      return;
    }
  }
  snprintf(buf, cap, "cpu-written");
}

static void *resolve(NativeState *ns, uint32_t va) {
  uint32_t idx;
  if (!dispatch_index(va, &idx)) {
    stop(ns, NSTOP_UNSUPPORTED, va,
         "instruction fetch outside KSEG0/KSEG1 RDRAM or SP memory "
         "(TLB-mapped fetch is not supported)");
    return NULL;
  }
  if (idx < RDRAM_WORDS && (va & 0xE0000000u) == 0xA0000000u) {
    stop(ns, NSTOP_UNSUPPORTED, va, "instruction fetch via KSEG1 RDRAM");
    return NULL;
  }
  if (ns->dispatch[idx] != (void *)m2m_lazy)
    return ns->dispatch[idx];
  /* Candidates: units containing `va`, found by binary search. */
  uint32_t lo = 0, hi = ns->npcs;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (ns->pc_sorted[mid] < va)
      lo = mid + 1;
    else
      hi = mid;
  }
  for (uint32_t i = lo; i < ns->npcs && ns->pc_sorted[i] == va; i++) {
    uint32_t u = ns->pc_index[i], first, end;
    if (m2m_units[u].rows[ns->pc_slot[i]].entry < 0)
      continue;
    run_bounds(&m2m_units[u], ns->pc_slot[i], &first, &end);
    if (!run_matches(ns, u, first, end))
      continue;
    link_run(ns, u, first, end);
    return ns->dispatch[idx];
  }
  char origin[256];
  describe_origin(ns, idx, origin, sizeof(origin));
  if (ns->trap_log) {
    FILE *f = fopen(ns->trap_log, "a");
    if (f) {
      fprintf(f, "%08X %06X %08X %llu %s\n", va, idx, word_at(ns, idx),
              (unsigned long long)ns->cpu.icount, origin);
      fclose(f);
    }
  }
  stop(ns, NSTOP_UNTRANSLATED, va, "no translation for this code");
  return NULL;
}

RtRet rt_dispatch(NativeState *ns, uint64_t va, uint64_t b, uint32_t pcds);
RtRet rt_dispatch(NativeState *ns, uint64_t va, uint64_t b, uint32_t pcds) {
  (void)b;
  (void)pcds;
  void *code = resolve(ns, (uint32_t)va);
  return (RtRet){(uint64_t)(uintptr_t)code, code == NULL};
}

RtRet rt_missing(NativeState *ns, uint64_t va, uint64_t b, uint32_t pcds);
RtRet rt_missing(NativeState *ns, uint64_t va, uint64_t b, uint32_t pcds) {
  (void)b;
  (void)pcds;
  if (ns->trap_log) {
    FILE *f = fopen(ns->trap_log, "a");
    if (f) {
      fprintf(f, "%08X missing-path icount=%llu\n", (uint32_t)va,
              (unsigned long long)ns->cpu.icount);
      fclose(f);
    }
  }
  stop(ns, NSTOP_UNTRANSLATED, (uint32_t)va, "path not in the translated set");
  return (RtRet){0, 1};
}

RtRet rt_link(NativeState *ns, uint64_t va, uint64_t b, uint32_t pcds);
RtRet rt_link(NativeState *ns, uint64_t va, uint64_t b, uint32_t pcds) {
  return rt_dispatch(ns, va, b, pcds);
}

/* ---- memory bus (port of the oracle's load/store) ---------------------- */

static void tlb_exception(NativeState *ns, uint32_t va, unsigned code,
                          int refill, uint32_t pc, int ds) {
  CpuState *c = &ns->cpu;
  c->cop0[CP0_BADVADDR] = sext32(va);
  c->cop0[CP0_CONTEXT] =
      (c->cop0[CP0_CONTEXT] & ~0x7FFFF0ull) | (uint64_t)((va >> 13) << 4);
  c->cop0[CP0_ENTRYHI] =
      sext32((va & 0xFFFFE000u) | ((uint32_t)c->cop0[CP0_ENTRYHI] & 0xFFu));
  take_exception(ns, code, 0, pc, ds,
                 refill && !(c->cop0[CP0_STATUS] & SR_EXL) ? 0x000 : 0x180);
}

static int translate(NativeState *ns, uint32_t va, int store, uint32_t pc,
                     int ds, uint32_t *pa) {
  if ((va & 0xC0000000u) == 0x80000000u) {
    *pa = va & 0x1FFFFFFFu;
    return 1;
  }
  CpuState *c = &ns->cpu;
  uint32_t asid = (uint32_t)c->cop0[CP0_ENTRYHI] & 0xFFu;
  for (unsigned i = 0; i < 32; i++) {
    const TlbEntry *t = &c->tlb[i];
    uint32_t span = ((uint32_t)t->page_mask & 0x01FFE000u) | 0x1FFFu;
    if (((uint32_t)t->entry_hi & ~span) != (va & ~span))
      continue;
    int global = (t->entry_lo0 & t->entry_lo1 & 1u) != 0;
    if (!global && ((uint32_t)t->entry_hi & 0xFFu) != asid)
      continue;
    uint32_t offmask = span >> 1;
    uint64_t lo = va & (offmask + 1) ? t->entry_lo1 : t->entry_lo0;
    if (!(lo & 2u)) {
      tlb_exception(ns, va, store ? 3 : 2, 0, pc, ds);
      return 0;
    }
    if (store && !(lo & 4u)) {
      tlb_exception(ns, va, 1, 0, pc, ds);
      return 0;
    }
    uint32_t base = (uint32_t)((lo >> 6) & 0xFFFFFu) << 12;
    *pa = ((base & ~offmask) | (va & offmask)) & 0x1FFFFFFFu;
    return 1;
  }
  tlb_exception(ns, va, store ? 3 : 2, 1, pc, ds);
  return 0;
}

static uint8_t *direct(NativeState *ns, uint32_t pa) {
  if (pa < HW_RDRAM_SIZE)
    return ns->hw->rdram + pa;
  if ((pa & 0xFFFFE000u) == 0x04000000u)
    return ns->hw->sp_mem + (pa & 0x1FFFu);
  return NULL;
}

static uint32_t mmio_read32(NativeState *ns, uint32_t pa) {
  run_events(ns);
  return hw_read32(ns->hw, pa);
}

static void process_hw_writes(NativeState *ns, uint32_t pa) {
  HwState *hw = ns->hw;
  for (; ns->dma_seen < hw->dma_count; ns->dma_seen++) {
    const HwDmaRecord *d = &hw->dma_log[ns->dma_seen];
    unlink_words(ns, d->ram_addr >> 2, (d->length + 3u) >> 2);
  }
  if ((pa & 0xFFF00000u) == 0x04000000u) /* SP registers: DMEM/IMEM DMA */
    unlink_words(ns, RDRAM_WORDS, NATIVE_SP_ENTRIES);
}

static void mmio_write32(NativeState *ns, uint32_t pa, uint32_t v) {
  run_events(ns);
  hw_write32(ns->hw, pa, v);
  process_hw_writes(ns, pa);
}

static RtRet fault(NativeState *ns) {
  boundary_loop(ns, 0);
  return (RtRet){0, 1};
}

RtRet rt_read(NativeState *ns, uint64_t vaddr, uint64_t size, uint32_t pcds);
RtRet rt_read(NativeState *ns, uint64_t vaddr, uint64_t size, uint32_t pcds) {
  uint32_t va = (uint32_t)vaddr, pc = pcds & ~1u, pa;
  int ds = pcds & 1;
  if (va & (size - 1)) {
    ns->cpu.cop0[CP0_BADVADDR] = sext32(va);
    take_exception(ns, EXC_ADEL, 0, pc, ds, 0x180);
    return fault(ns);
  }
  if (!translate(ns, va, 0, pc, ds, &pa))
    return fault(ns);
  uint8_t *p = direct(ns, pa);
  uint64_t v = 0;
  if (p) {
    for (unsigned i = 0; i < size; i++)
      v = v << 8 | p[i];
  } else if (size == 8) {
    v = (uint64_t)mmio_read32(ns, pa) << 32 | mmio_read32(ns, pa + 4);
  } else {
    uint32_t word = mmio_read32(ns, pa & ~3u);
    unsigned shift = 8 * (4 - (unsigned)size - (pa & 3u));
    v = size == 4 ? word : (word >> shift) & ((1u << (8 * size)) - 1u);
  }
  update_attn(ns);
  return (RtRet){v, 0};
}

static RtRet bus_write(NativeState *ns, uint64_t vaddr, uint64_t v,
                       unsigned size, uint32_t pcds) {
  uint32_t va = (uint32_t)vaddr, pc = pcds & ~1u, pa;
  int ds = pcds & 1;
  if (va & (size - 1)) {
    ns->cpu.cop0[CP0_BADVADDR] = sext32(va);
    take_exception(ns, EXC_ADES, 0, pc, ds, 0x180);
    return fault(ns);
  }
  if (!translate(ns, va, 1, pc, ds, &pa))
    return fault(ns);
  uint8_t *p = direct(ns, pa);
  if (p) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < size; i++)
      bytes[i] = (uint8_t)(v >> (8 * (size - 1 - i)));
    uint32_t idx;
    if (memcmp(p, bytes, size) != 0 && dispatch_index(0x80000000u | pa, &idx))
      unlink_words(ns, idx, (size + 3u) / 4u); /* code is being changed */
    memcpy(p, bytes, size);
  } else if (size == 8) {
    mmio_write32(ns, pa, (uint32_t)(v >> 32));
    mmio_write32(ns, pa + 4, (uint32_t)v);
  } else {
    unsigned shift = 8 * (4 - size - (pa & 3u));
    mmio_write32(ns, pa & ~3u, (uint32_t)v << shift);
  }
  update_attn(ns);
  return (RtRet){0, 0};
}

#define WRITE_FN(n, size)                                                      \
  RtRet rt_write##n(NativeState *ns, uint64_t va, uint64_t v, uint32_t pcds);  \
  RtRet rt_write##n(NativeState *ns, uint64_t va, uint64_t v, uint32_t pcds) { \
    return bus_write(ns, va, v, size, pcds);                                   \
  }
WRITE_FN(8, 1)
WRITE_FN(16, 2)
WRITE_FN(32, 4)
WRITE_FN(64, 8)

/* ---- COP0 / TLB / exceptions (ports of the oracle) --------------------- */

RtRet rt_mfc0(NativeState *ns, uint64_t a, uint64_t reg, uint32_t pcds);
RtRet rt_mfc0(NativeState *ns, uint64_t a, uint64_t reg, uint32_t pcds) {
  (void)a;
  (void)pcds;
  CpuState *c = &ns->cpu;
  uint64_t v;
  switch (reg & 31u) {
  case CP0_COUNT:
    v = count_now(ns);
    break;
  case CP0_CAUSE:
    run_events(ns);
    settle_compare(ns);
    v = cause_live(ns);
    break;
  case CP0_RANDOM: {
    uint32_t wired = (uint32_t)c->cop0[CP0_WIRED] & 31u;
    v = 31u - (uint32_t)(c->icount % (32u - wired));
    break;
  }
  default:
    v = c->cop0[reg & 31u];
  }
  return (RtRet){v, 0};
}

RtRet rt_mtc0(NativeState *ns, uint64_t v, uint64_t reg, uint32_t pcds);
RtRet rt_mtc0(NativeState *ns, uint64_t v, uint64_t reg, uint32_t pcds) {
  (void)pcds;
  CpuState *c = &ns->cpu;
  unsigned r = reg & 31u;
  switch (r) {
  case CP0_COUNT:
    settle_compare(ns);
    c->count_offset = (uint32_t)v - (uint32_t)c->icount;
    ns->compare_checked = c->icount;
    break;
  case CP0_COMPARE:
    settle_compare(ns);
    c->cop0[r] = (uint32_t)v;
    c->cop0[CP0_CAUSE] &= ~(uint64_t)CAUSE_IP7;
    ns->compare_checked = c->icount;
    break;
  case CP0_CAUSE:
    c->cop0[r] = (c->cop0[r] & ~0x300ull) | (v & 0x300u);
    break;
  case CP0_STATUS:
    c->cop0[r] = (uint32_t)v;
    break;
  case CP0_RANDOM:
  case CP0_PRID:
    break;
  default:
    c->cop0[r] = v;
  }
  update_attn(ns);
  return (RtRet){0, 0};
}

RtRet rt_tlb(NativeState *ns, uint64_t a, uint64_t op, uint32_t pcds);
RtRet rt_tlb(NativeState *ns, uint64_t a, uint64_t op, uint32_t pcds) {
  (void)a;
  (void)pcds;
  CpuState *c = &ns->cpu;
  TlbEntry *t;
  switch (op) {
  case 1:
    t = &c->tlb[c->cop0[CP0_INDEX] & 31u];
    c->cop0[CP0_PAGEMASK] = t->page_mask;
    c->cop0[CP0_ENTRYHI] = t->entry_hi;
    c->cop0[CP0_ENTRYLO0] = t->entry_lo0;
    c->cop0[CP0_ENTRYLO1] = t->entry_lo1;
    break;
  case 2:
  case 6: {
    uint32_t wired = (uint32_t)c->cop0[CP0_WIRED] & 31u;
    uint64_t i = op == 2 ? c->cop0[CP0_INDEX]
                         : 31u - (uint32_t)(c->icount % (32u - wired));
    t = &c->tlb[i & 31u];
    t->page_mask = c->cop0[CP0_PAGEMASK];
    t->entry_hi = c->cop0[CP0_ENTRYHI];
    t->entry_lo0 = c->cop0[CP0_ENTRYLO0];
    t->entry_lo1 = c->cop0[CP0_ENTRYLO1];
    break;
  }
  case 8:
    c->cop0[CP0_INDEX] = 0x80000000u;
    for (unsigned i = 0; i < 32; i++) {
      t = &c->tlb[i];
      uint64_t mask = ~(t->page_mask | 0x1FFFull) & 0xFFFFFFE000ull;
      int global = (t->entry_lo0 & t->entry_lo1 & 1u) != 0;
      if ((t->entry_hi & mask) == (c->cop0[CP0_ENTRYHI] & mask) &&
          (global || (t->entry_hi & 0xFF) == (c->cop0[CP0_ENTRYHI] & 0xFF))) {
        c->cop0[CP0_INDEX] = i;
        break;
      }
    }
    break;
  }
  return (RtRet){0, 0};
}

RtRet rt_eret(NativeState *ns, uint64_t a, uint64_t b, uint32_t pcds);
RtRet rt_eret(NativeState *ns, uint64_t a, uint64_t b, uint32_t pcds) {
  (void)a;
  (void)b;
  (void)pcds;
  CpuState *c = &ns->cpu;
  if (c->cop0[CP0_STATUS] & SR_ERL) {
    c->pc = (uint32_t)c->cop0[CP0_ERROREPC];
    c->cop0[CP0_STATUS] &= ~(uint64_t)SR_ERL;
  } else {
    c->pc = (uint32_t)c->cop0[CP0_EPC];
    c->cop0[CP0_STATUS] &= ~(uint64_t)SR_EXL;
  }
  c->llbit = 0;
  boundary_loop(ns, 0);
  return (RtRet){0, 1};
}

RtRet rt_raise(NativeState *ns, uint64_t a, uint64_t code, uint32_t pcds);
RtRet rt_raise(NativeState *ns, uint64_t a, uint64_t code, uint32_t pcds) {
  (void)a;
  take_exception(ns, (unsigned)code, 0, pcds & ~1u, pcds & 1, 0x180);
  return fault(ns);
}

/* ---- COP1 ---------------------------------------------------------------- */

static void set_host_fpcr(const CpuState *c) {
  static const uint64_t rmode[4] = {0, 3, 1, 2}; /* RN, RZ, RP, RM */
  uint64_t fpcr = rmode[c->fcr31 & 3u] << 22;
  if (c->fcr31 & FCR31_FS)
    fpcr |= 1u << 24; /* FZ */
  __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
}

RtRet rt_cop1(NativeState *ns, uint64_t a, uint64_t b, uint32_t pcds);
RtRet rt_cop1(NativeState *ns, uint64_t a, uint64_t b, uint32_t pcds) {
  (void)a;
  (void)b;
  if (!(ns->cpu.cop0[CP0_STATUS] & SR_CU1)) {
    take_exception(ns, EXC_CPU, 1, pcds & ~1u, pcds & 1, 0x180);
    return fault(ns);
  }
  stop(ns, NSTOP_UNSUPPORTED, pcds & ~1u,
       "FPR access with Status.FR = 1 (contract assumes FR = 0)");
  return (RtRet){0, 1};
}

static RtRet fpe_or_ok(NativeState *ns, unsigned cause, uint32_t pcds) {
  CpuState *c = &ns->cpu;
  unsigned enables = (c->fcr31 >> FCR31_ENABLE_SH) & 31u;
  c->fcr31 = (c->fcr31 & ~(0x3Fu << FCR31_CAUSE_SH)) | cause << FCR31_CAUSE_SH;
  if ((cause & enables) || (cause & FPX_E)) {
    take_exception(ns, EXC_FPE, 0, pcds & ~1u, pcds & 1, 0x180);
    return fault(ns);
  }
  c->fcr31 |= (cause & 31u) << FCR31_FLAGS_SH;
  return (RtRet){0, 0};
}

RtRet rt_fpu(NativeState *ns, uint64_t a, uint64_t fpsr, uint32_t pcds);
RtRet rt_fpu(NativeState *ns, uint64_t a, uint64_t fpsr, uint32_t pcds) {
  (void)a;
  unsigned cause = (fpsr & 0x10 ? FPX_I : 0) | (fpsr & 0x08 ? FPX_U : 0) |
                   (fpsr & 0x04 ? FPX_O : 0) | (fpsr & 0x02 ? FPX_Z : 0) |
                   (fpsr & 0x01 ? FPX_V : 0);
  return fpe_or_ok(ns, cause, pcds);
}

RtRet rt_cfc1(NativeState *ns, uint64_t a, uint64_t fs, uint32_t pcds);
RtRet rt_cfc1(NativeState *ns, uint64_t a, uint64_t fs, uint32_t pcds) {
  (void)a;
  (void)pcds;
  const CpuState *c = &ns->cpu;
  uint64_t v = fs == 31 ? sext32(c->fcr31) : fs == 0 ? sext32(c->fcr0) : 0;
  return (RtRet){v, 0};
}

RtRet rt_ctc1(NativeState *ns, uint64_t v, uint64_t fs, uint32_t pcds);
RtRet rt_ctc1(NativeState *ns, uint64_t v, uint64_t fs, uint32_t pcds) {
  CpuState *c = &ns->cpu;
  if (fs != 31)
    return (RtRet){0, 0};
  c->fcr31 = (uint32_t)v & FCR31_MASK;
  set_host_fpcr(c);
  unsigned cause = (c->fcr31 >> FCR31_CAUSE_SH) & 0x3Fu;
  unsigned enables = (c->fcr31 >> FCR31_ENABLE_SH) & 31u;
  if ((cause & enables) || (cause & FPX_E)) {
    take_exception(ns, EXC_FPE, 0, pcds & ~1u, pcds & 1, 0x180);
    return fault(ns);
  }
  return (RtRet){0, 0};
}

/* ---- boundary ------------------------------------------------------------ */

RtRet rt_boundary(NativeState *ns, uint64_t pc, uint64_t b, uint32_t pcds);
RtRet rt_boundary(NativeState *ns, uint64_t pc, uint64_t b, uint32_t pcds) {
  (void)b;
  (void)pcds;
  ns->cpu.pc = (uint32_t)pc;
  boundary_loop(ns, 1);
  return (RtRet){ns->cpu.pc, ns->stop || ns->cpu.pc != (uint32_t)pc};
}

/* ---- setup and run --------------------------------------------------- */

typedef struct {
  uint32_t pc, unit, slot;
} PcRow;

static int cmp_pcrow(const void *a, const void *b) {
  const PcRow *x = a, *y = b;
  if (x->pc != y->pc)
    return x->pc < y->pc ? -1 : 1;
  return x->unit < y->unit ? -1 : x->unit > y->unit;
}

void native_init(NativeState *ns, HwState *hw) {
  memset(ns, 0, sizeof(*ns));
  ns->hw = hw;
  ns->rdram = hw->rdram;
  ns->budget = UINT64_MAX;
  ns->stride = 1;
  ns->boundary_left = UINT64_MAX;
  ns->dispatch = malloc(sizeof(void *) * NATIVE_DISPATCH_ENTRIES);
  for (uint32_t i = 0; i < NATIVE_DISPATCH_ENTRIES; i++)
    ns->dispatch[i] = (void *)m2m_lazy;
  ns->codemap = calloc(RDRAM_WORDS + 1, 1);
  ns->unitmap = calloc(NATIVE_DISPATCH_ENTRIES, sizeof(uint32_t));

  m2m_units = &m2m_sect_start;
  m2m_unit_count = (uint32_t)(&m2m_sect_end - &m2m_sect_start);
  uint32_t total = 0;
  for (uint32_t u = 0; u < m2m_unit_count; u++)
    total += (uint32_t)m2m_units[u].n;
  PcRow *rows = malloc(sizeof(PcRow) * (total ? total : 1));
  uint32_t n = 0;
  for (uint32_t u = 0; u < m2m_unit_count; u++)
    for (uint32_t i = 0; i < (uint32_t)m2m_units[u].n; i++)
      rows[n++] = (PcRow){m2m_units[u].rows[i].pc, u, i};
  qsort(rows, n, sizeof(PcRow), cmp_pcrow);
  ns->npcs = n;
  ns->pc_sorted = malloc(sizeof(uint32_t) * (n ? n : 1));
  ns->pc_index = malloc(sizeof(uint32_t) * (n ? n : 1));
  ns->pc_slot = malloc(sizeof(uint32_t) * (n ? n : 1));
  for (uint32_t i = 0; i < n; i++) {
    ns->pc_sorted[i] = rows[i].pc;
    ns->pc_index[i] = rows[i].unit;
    ns->pc_slot[i] = rows[i].slot;
  }
  free(rows);
}

void native_free(NativeState *ns) {
  free(ns->dispatch);
  free(ns->codemap);
  free(ns->unitmap);
  free(ns->pc_sorted);
  free(ns->pc_index);
  free(ns->pc_slot);
}

NativeStop native_run(NativeState *ns) {
  if (!ns->started) {
    ns->boundary_left = ns->checkpoints ? ns->stride : UINT64_MAX;
    ns->compare_checked = ns->cpu.icount;
    ns->started = 1;
  }
  set_host_fpcr(&ns->cpu);
  ns->stop = NSTOP_NONE;
  /* Runs start (and budget stops resume) at a block boundary. */
  boundary_loop(ns, 0);
  if (!ns->stop)
    m2m_enter(ns, (const void *)m2m_redirect);
  return (NativeStop)ns->stop;
}
