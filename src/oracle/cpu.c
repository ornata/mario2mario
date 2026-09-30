/* Integer core, COP0, memory access and the run loop. */
#include "src/oracle/cpu.h"

#include <stdlib.h>
#include <string.h>

#include "src/oracle/checkpoint.h"

#define RS(w)   (((w) >> 21) & 31u)
#define RT(w)   (((w) >> 16) & 31u)
#define RD(w)   (((w) >> 11) & 31u)
#define SA(w)   (((w) >> 6) & 31u)
#define IMM(w)  ((int64_t)(int16_t)((w) & 0xFFFFu))
#define UIMM(w) ((uint64_t)((w) & 0xFFFFu))

void oracle_init(Oracle *o, HwState *hw) {
  memset(o, 0, sizeof(*o));
  o->hw = hw;
  o->exec_bits = calloc(HW_RDRAM_SIZE / 4 / 8, 1);
  o->checkpoint_stride = 1;
  o->entry_pc = 0x80246000u;
}

void oracle_free(Oracle *o) {
  free(o->exec_bits);
  o->exec_bits = NULL;
}

/* --- Exceptions --------------------------------------------------------- */

void cpu_exception(Oracle *o, unsigned code, unsigned ce) {
  cpu_exception_at(o, code, ce, 0x180);
}

void cpu_exception_at(Oracle *o, unsigned code, unsigned ce, uint32_t offset) {
  CpuState *c = &o->cpu;
  uint64_t status = c->cop0[CP0_STATUS];
  uint64_t cause = c->cop0[CP0_CAUSE];
  cause &= ~(uint64_t)(0x7Cu | (3u << 28));
  cause |= (uint64_t)code << 2 | (uint64_t)(ce & 3u) << 28;
  if (!(status & SR_EXL)) {
    uint32_t epc = c->cur_delay ? c->cur_pc - 4 : c->cur_pc;
    c->cop0[CP0_EPC] = sext32(epc);
    cause = c->cur_delay ? cause | CAUSE_BD : cause & ~(uint64_t)CAUSE_BD;
  }
  c->cop0[CP0_CAUSE] = (uint32_t)cause;
  c->cop0[CP0_STATUS] = status | SR_EXL;
  uint32_t vector = (status & SR_BEV ? 0xBFC00200u : 0x80000000u) + offset;
  c->pc = vector;
  c->next_pc = vector + 4;
  c->in_delay = 0;
  c->boundary = 1;
  o->exceptions[code & 31]++;
  if (o->stop_on_exception)
    o->stop = STOP_EXCEPTION;
}

static void address_error(Oracle *o, uint32_t vaddr, int store) {
  o->cpu.cop0[CP0_BADVADDR] = sext32(vaddr);
  cpu_exception(o, store ? EXC_ADES : EXC_ADEL, 0);
}

/* --- Memory ------------------------------------------------------------- */

/* TLB exception: refill (miss) uses the 0x000 vector unless EXL is set;
 * invalid and modify use the general vector. */
static void tlb_exception(Oracle *o, uint32_t va, unsigned code, int refill) {
  CpuState *c = &o->cpu;
  c->cop0[CP0_BADVADDR] = sext32(va);
  c->cop0[CP0_CONTEXT] =
      (c->cop0[CP0_CONTEXT] & ~0x7FFFF0ull) | (uint64_t)((va >> 13) << 4);
  c->cop0[CP0_ENTRYHI] =
      sext32((va & 0xFFFFE000u) | ((uint32_t)c->cop0[CP0_ENTRYHI] & 0xFFu));
  o->tlb_exceptions++;
  cpu_exception_at(o, code, 0,
                   refill && !(c->cop0[CP0_STATUS] & SR_EXL) ? 0x000 : 0x180);
}

/* 32-bit mode translation. KSEG0/KSEG1 are direct-mapped; KUSEG, KSSEG
 * and KSEG3 go through the 32-entry TLB. Returns 1 on success, 0 after
 * raising a TLB exception. */
static int translate(Oracle *o, uint64_t vaddr, uint32_t *paddr, int store) {
  uint32_t va = (uint32_t)vaddr;
  if ((va & 0xC0000000u) == 0x80000000u) {
    *paddr = va & 0x1FFFFFFFu;
    return 1;
  }
  CpuState *c = &o->cpu;
  o->tlb_mapped_accesses++;
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
    if (!(lo & 2u)) { /* not valid */
      tlb_exception(o, va, store ? 3 : 2, 0);
      return 0;
    }
    if (store && !(lo & 4u)) { /* not dirty (writable) */
      tlb_exception(o, va, 1, 0);
      return 0;
    }
    uint32_t base = (uint32_t)((lo >> 6) & 0xFFFFFu) << 12;
    *paddr = ((base & ~offmask) | (va & offmask)) & 0x1FFFFFFFu;
    return 1;
  }
  tlb_exception(o, va, store ? 3 : 2, 1);
  return 0;
}

static uint32_t phys_read32(Oracle *o, uint32_t pa) {
  HwState *hw = o->hw;
  if (pa < HW_RDRAM_SIZE)
    return hw_be32(hw->rdram + pa);
  if ((pa & 0xFFFFE000u) == 0x04000000u)
    return hw_be32(hw->sp_mem + (pa & 0x1FFFu));
  hw->now = o->cpu.icount;
  return hw_read32(hw, pa);
}

static void phys_write32(Oracle *o, uint32_t pa, uint32_t v) {
  HwState *hw = o->hw;
  if (pa < HW_RDRAM_SIZE) {
    hw_put_be32(hw->rdram + pa, v);
    return;
  }
  if ((pa & 0xFFFFE000u) == 0x04000000u) {
    hw_put_be32(hw->sp_mem + (pa & 0x1FFFu), v);
    return;
  }
  hw->now = o->cpu.icount;
  hw_write32(hw, pa, v);
}

/* Byte-addressable memories (RDRAM, SP DMEM/IMEM), or NULL for MMIO. */
static uint8_t *direct(Oracle *o, uint32_t pa) {
  if (pa < HW_RDRAM_SIZE)
    return o->hw->rdram + pa;
  if ((pa & 0xFFFFE000u) == 0x04000000u)
    return o->hw->sp_mem + (pa & 0x1FFFu);
  return NULL;
}

/* Loads `size` (1, 2, 4, 8) bytes, big-endian, zero-extended. */
static int load(Oracle *o, uint64_t vaddr, unsigned size, uint64_t *out) {
  uint32_t va = (uint32_t)vaddr, pa;
  if (va & (size - 1)) {
    address_error(o, va, 0);
    return 0;
  }
  if (!translate(o, vaddr, &pa, 0))
    return 0;
  uint8_t *p = direct(o, pa);
  uint64_t v = 0;
  if (p) {
    for (unsigned i = 0; i < size; i++)
      v = v << 8 | p[i];
  } else if (size == 8) {
    v = (uint64_t)phys_read32(o, pa) << 32 | phys_read32(o, pa + 4);
  } else {
    uint32_t word = phys_read32(o, pa & ~3u);
    unsigned shift = 8 * (4 - size - (pa & 3u));
    v = size == 4 ? word : (word >> shift) & ((1u << (8 * size)) - 1u);
  }
  *out = v;
  return 1;
}

/* Stores the low `size` bytes of v. Sub-word MMIO stores are written as
 * the containing word with the value shifted into its byte lane. */
static int store(Oracle *o, uint64_t vaddr, unsigned size, uint64_t v) {
  uint32_t va = (uint32_t)vaddr, pa;
  if (va & (size - 1)) {
    address_error(o, va, 1);
    return 0;
  }
  if (!translate(o, vaddr, &pa, 1))
    return 0;
  uint8_t *p = direct(o, pa);
  if (p) {
    for (unsigned i = 0; i < size; i++)
      p[i] = (uint8_t)(v >> (8 * (size - 1 - i)));
  } else if (size == 8) {
    phys_write32(o, pa, (uint32_t)(v >> 32));
    phys_write32(o, pa + 4, (uint32_t)v);
  } else {
    unsigned shift = 8 * (4 - size - (pa & 3u));
    phys_write32(o, pa & ~3u, (uint32_t)v << shift);
  }
  return 1;
}

/* --- COP0 --------------------------------------------------------------- */

static uint64_t cop0_read(Oracle *o, unsigned r) {
  CpuState *c = &o->cpu;
  switch (r) {
  case CP0_COUNT:
    return (uint32_t)(c->icount + c->count_offset);
  case CP0_CAUSE:
    return (c->cop0[CP0_CAUSE] & ~(uint64_t)CAUSE_IP2) |
           (hw_irq(o->hw) ? CAUSE_IP2 : 0);
  case CP0_RANDOM: {
    /* Deterministic: counts down from 31 to Wired, one per instruction. */
    uint32_t wired = (uint32_t)c->cop0[CP0_WIRED] & 31u;
    return 31u - (uint32_t)(c->icount % (32u - wired));
  }
  default:
    return c->cop0[r];
  }
}

static void cop0_write(Oracle *o, unsigned r, uint64_t v) {
  CpuState *c = &o->cpu;
  switch (r) {
  case CP0_COUNT:
    c->count_offset = (uint32_t)v - (uint32_t)c->icount;
    return;
  case CP0_COMPARE:
    c->cop0[r] = (uint32_t)v;
    c->cop0[CP0_CAUSE] &= ~(uint64_t)CAUSE_IP7;
    return;
  case CP0_CAUSE: /* only the software interrupt bits are writable */
    c->cop0[r] = (c->cop0[r] & ~0x300ull) | (v & 0x300u);
    return;
  case CP0_STATUS:
    c->cop0[r] = (uint32_t)v;
    return;
  case CP0_RANDOM:
  case CP0_PRID:
    return;
  default:
    c->cop0[r] = v;
    return;
  }
}

static void tlb_op(Oracle *o, unsigned funct) {
  CpuState *c = &o->cpu;
  TlbEntry *t;
  switch (funct) {
  case 1: /* tlbr */
    t = &c->tlb[c->cop0[CP0_INDEX] & 31u];
    c->cop0[CP0_PAGEMASK] = t->page_mask;
    c->cop0[CP0_ENTRYHI] = t->entry_hi;
    c->cop0[CP0_ENTRYLO0] = t->entry_lo0;
    c->cop0[CP0_ENTRYLO1] = t->entry_lo1;
    return;
  case 2: /* tlbwi */
  case 6: /* tlbwr */
    t = &c->tlb[(funct == 2 ? c->cop0[CP0_INDEX] : cop0_read(o, CP0_RANDOM)) &
                31u];
    t->page_mask = c->cop0[CP0_PAGEMASK];
    t->entry_hi = c->cop0[CP0_ENTRYHI];
    t->entry_lo0 = c->cop0[CP0_ENTRYLO0];
    t->entry_lo1 = c->cop0[CP0_ENTRYLO1];
    return;
  case 8: /* tlbp */
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
    return;
  }
}

static void eret(Oracle *o) {
  CpuState *c = &o->cpu;
  uint32_t target;
  if (c->cop0[CP0_STATUS] & SR_ERL) {
    target = (uint32_t)c->cop0[CP0_ERROREPC];
    c->cop0[CP0_STATUS] &= ~(uint64_t)SR_ERL;
  } else {
    target = (uint32_t)c->cop0[CP0_EPC];
    c->cop0[CP0_STATUS] &= ~(uint64_t)SR_EXL;
  }
  c->llbit = 0;
  c->pc = target;
  c->next_pc = target + 4;
  c->in_delay = 0;
  c->boundary = 1;
  o->eret_count++;
  if (o->entry_icount && !o->first_eret_icount)
    o->first_eret_icount = c->icount + 1;
}

/* Returns 1 if the instruction completed. */
static int exec_cop0(Oracle *o, uint32_t w) {
  CpuState *c = &o->cpu;
  switch (RS(w)) {
  case 0: /* mfc0 */
    c->gpr[RT(w)] = sext32((uint32_t)cop0_read(o, RD(w)));
    return 1;
  case 1: /* dmfc0 */
    c->gpr[RT(w)] = cop0_read(o, RD(w));
    return 1;
  case 4: /* mtc0 */
    cop0_write(o, RD(w), sext32((uint32_t)c->gpr[RT(w)]));
    return 1;
  case 5: /* dmtc0 */
    cop0_write(o, RD(w), c->gpr[RT(w)]);
    return 1;
  default:
    if (RS(w) >= 16) {
      unsigned f = w & 63u;
      if (f == 1 || f == 2 || f == 6 || f == 8) {
        tlb_op(o, f);
        return 1;
      }
      if (f == 24) {
        eret(o);
        return 1;
      }
    }
    cpu_exception(o, EXC_RI, 0);
    return 0;
  }
}

/* --- Unaligned load/store merges (big-endian) --------------------------- */

static int lwl_lwr(Oracle *o, uint32_t w, int left) {
  CpuState *c = &o->cpu;
  uint64_t addr = c->gpr[RS(w)] + (uint64_t)IMM(w), word;
  if (!load(o, addr & ~3ull, 4, &word))
    return 0;
  unsigned k = (uint32_t)addr & 3u;
  uint32_t rt = (uint32_t)c->gpr[RT(w)], m;
  if (left) {
    m = (uint32_t)word << (8 * k) | (rt & ((1u << (8 * k)) - 1u));
    c->gpr[RT(w)] = sext32(m);
  } else {
    unsigned s = 8 * (3 - k);
    m = (uint32_t)word >> s | (rt & ~(0xFFFFFFFFu >> s));
    /* 32-bit addressing mode: only the low word is merged. */
    c->gpr[RT(w)] = (c->gpr[RT(w)] & 0xFFFFFFFF00000000ull) | m;
  }
  return 1;
}

static int swl_swr(Oracle *o, uint32_t w, int left) {
  CpuState *c = &o->cpu;
  uint64_t addr = c->gpr[RS(w)] + (uint64_t)IMM(w), word;
  if (!load(o, addr & ~3ull, 4, &word))
    return 0;
  unsigned k = (uint32_t)addr & 3u;
  uint32_t rt = (uint32_t)c->gpr[RT(w)], m, wd = (uint32_t)word;
  if (left)
    m = (wd & ~(0xFFFFFFFFu >> (8 * k))) | rt >> (8 * k);
  else
    m = (wd & ~(0xFFFFFFFFu << (8 * (3 - k)))) | rt << (8 * (3 - k));
  return store(o, addr & ~3ull, 4, m);
}

static int ldl_ldr(Oracle *o, uint32_t w, int left) {
  CpuState *c = &o->cpu;
  uint64_t addr = c->gpr[RS(w)] + (uint64_t)IMM(w), d;
  if (!load(o, addr & ~7ull, 8, &d))
    return 0;
  unsigned k = (uint32_t)addr & 7u;
  uint64_t rt = c->gpr[RT(w)];
  if (left)
    c->gpr[RT(w)] = d << (8 * k) | (k ? rt & ((1ull << (8 * k)) - 1u) : 0);
  else
    c->gpr[RT(w)] =
        d >> (8 * (7 - k)) | (k == 7 ? 0 : rt & ~(~0ull >> (8 * (7 - k))));
  return 1;
}

static int sdl_sdr(Oracle *o, uint32_t w, int left) {
  CpuState *c = &o->cpu;
  uint64_t addr = c->gpr[RS(w)] + (uint64_t)IMM(w), d;
  if (!load(o, addr & ~7ull, 8, &d))
    return 0;
  unsigned k = (uint32_t)addr & 7u;
  uint64_t rt = c->gpr[RT(w)], m;
  if (left)
    m = (d & ~(~0ull >> (8 * k))) | rt >> (8 * k);
  else
    m = (d & ~(~0ull << (8 * (7 - k)))) | rt << (8 * (7 - k));
  return store(o, addr & ~7ull, 8, m);
}

/* --- Integer execution -------------------------------------------------- */

static int coprocessor_usable(Oracle *o, unsigned n) {
  if (o->cpu.cop0[CP0_STATUS] & (1u << (28 + n)))
    return 1;
  cpu_exception(o, EXC_CPU, n);
  return 0;
}

static void branch(CpuState *c, int taken, uint32_t w) {
  if (taken)
    c->next_pc = c->cur_pc + 4 + (uint32_t)(IMM(w) * 4);
  c->in_delay = 1;
}

static void branch_likely(CpuState *c, int taken, uint32_t w) {
  if (taken) {
    c->next_pc = c->cur_pc + 4 + (uint32_t)(IMM(w) * 4);
    c->in_delay = 1;
  } else { /* nullify the delay slot */
    c->pc = c->next_pc;
    c->next_pc = c->pc + 4;
    c->boundary = 1;
  }
}

static void jump(CpuState *c, uint32_t target) {
  c->next_pc = target;
  c->in_delay = 1;
}

static int trap(Oracle *o, int cond) {
  if (cond) {
    cpu_exception(o, EXC_TR, 0);
    return 0;
  }
  return 1;
}

static int exec_special(Oracle *o, uint32_t w) {
  CpuState *c = &o->cpu;
  uint64_t s = c->gpr[RS(w)], t = c->gpr[RT(w)];
  uint64_t *d = &c->gpr[RD(w)];
  unsigned sa = SA(w);
  switch (w & 63u) {
  case 0:
    *d = sext32((uint32_t)t << sa);
    return 1; /* sll */
  case 2:
    *d = sext32((uint32_t)t >> sa);
    return 1; /* srl */
  case 3:
    *d = sext32((uint32_t)((int64_t)t >> sa));
    return 1; /* sra */
  case 4:
    *d = sext32((uint32_t)t << (s & 31));
    return 1; /* sllv */
  case 6:
    *d = sext32((uint32_t)t >> (s & 31));
    return 1; /* srlv */
  case 7:
    *d = sext32((uint32_t)((int64_t)t >> (s & 31)));
    return 1; /* srav */
  case 8:
    jump(c, (uint32_t)s);
    return 1; /* jr */
  case 9:     /* jalr */
    *d = sext32(c->cur_pc + 8);
    jump(c, (uint32_t)s);
    return 1;
  case 12:
    cpu_exception(o, EXC_SYS, 0);
    return 0; /* syscall */
  case 13:
    cpu_exception(o, EXC_BP, 0);
    return 0; /* break */
  case 15:
    return 1; /* sync */
  case 16:
    *d = c->hi;
    return 1;
  case 17:
    c->hi = s;
    return 1;
  case 18:
    *d = c->lo;
    return 1;
  case 19:
    c->lo = s;
    return 1;
  case 20:
    *d = t << (s & 63);
    return 1; /* dsllv */
  case 22:
    *d = t >> (s & 63);
    return 1; /* dsrlv */
  case 23:
    *d = (uint64_t)((int64_t)t >> (s & 63));
    return 1; /* dsrav */
  case 24: {  /* mult */
    int64_t p = (int64_t)(int32_t)s * (int64_t)(int32_t)t;
    c->lo = sext32((uint32_t)p);
    c->hi = sext32((uint32_t)((uint64_t)p >> 32));
    return 1;
  }
  case 25: { /* multu */
    uint64_t p = (uint64_t)(uint32_t)s * (uint32_t)t;
    c->lo = sext32((uint32_t)p);
    c->hi = sext32((uint32_t)(p >> 32));
    return 1;
  }
  case 26: { /* div */
    int32_t n = (int32_t)s, q = (int32_t)t;
    if (q == 0) {
      c->lo = n < 0 ? 1 : ~0ull;
      c->hi = sext32((uint32_t)n);
    } else if (n == INT32_MIN && q == -1) {
      c->lo = sext32((uint32_t)INT32_MIN);
      c->hi = 0;
    } else {
      c->lo = sext32((uint32_t)(n / q));
      c->hi = sext32((uint32_t)(n % q));
    }
    return 1;
  }
  case 27: { /* divu */
    uint32_t n = (uint32_t)s, q = (uint32_t)t;
    c->lo = q ? sext32(n / q) : ~0ull;
    c->hi = sext32(q ? n % q : n);
    return 1;
  }
  case 28: { /* dmult */
    __int128 p = (__int128)(int64_t)s * (int64_t)t;
    c->lo = (uint64_t)p;
    c->hi = (uint64_t)((unsigned __int128)p >> 64);
    return 1;
  }
  case 29: { /* dmultu */
    unsigned __int128 p = (unsigned __int128)s * t;
    c->lo = (uint64_t)p;
    c->hi = (uint64_t)(p >> 64);
    return 1;
  }
  case 30: { /* ddiv */
    int64_t n = (int64_t)s, q = (int64_t)t;
    if (q == 0) {
      c->lo = n < 0 ? 1 : ~0ull;
      c->hi = (uint64_t)n;
    } else if (n == INT64_MIN && q == -1) {
      c->lo = (uint64_t)n;
      c->hi = 0;
    } else {
      c->lo = (uint64_t)(n / q);
      c->hi = (uint64_t)(n % q);
    }
    return 1;
  }
  case 31: /* ddivu */
    c->lo = t ? s / t : ~0ull;
    c->hi = t ? s % t : s;
    return 1;
  case 32: { /* add */
    int32_t r;
    if (__builtin_add_overflow((int32_t)s, (int32_t)t, &r)) {
      cpu_exception(o, EXC_OV, 0);
      return 0;
    }
    *d = sext32((uint32_t)r);
    return 1;
  }
  case 33:
    *d = sext32((uint32_t)s + (uint32_t)t);
    return 1; /* addu */
  case 34: {  /* sub */
    int32_t r;
    if (__builtin_sub_overflow((int32_t)s, (int32_t)t, &r)) {
      cpu_exception(o, EXC_OV, 0);
      return 0;
    }
    *d = sext32((uint32_t)r);
    return 1;
  }
  case 35:
    *d = sext32((uint32_t)s - (uint32_t)t);
    return 1; /* subu */
  case 36:
    *d = s & t;
    return 1;
  case 37:
    *d = s | t;
    return 1;
  case 38:
    *d = s ^ t;
    return 1;
  case 39:
    *d = ~(s | t);
    return 1;
  case 42:
    *d = (int64_t)s < (int64_t)t;
    return 1;
  case 43:
    *d = s < t;
    return 1;
  case 44: { /* dadd */
    int64_t r;
    if (__builtin_add_overflow((int64_t)s, (int64_t)t, &r)) {
      cpu_exception(o, EXC_OV, 0);
      return 0;
    }
    *d = (uint64_t)r;
    return 1;
  }
  case 45:
    *d = s + t;
    return 1; /* daddu */
  case 46: {  /* dsub */
    int64_t r;
    if (__builtin_sub_overflow((int64_t)s, (int64_t)t, &r)) {
      cpu_exception(o, EXC_OV, 0);
      return 0;
    }
    *d = (uint64_t)r;
    return 1;
  }
  case 47:
    *d = s - t;
    return 1; /* dsubu */
  case 48:
    return trap(o, (int64_t)s >= (int64_t)t);
  case 49:
    return trap(o, s >= t);
  case 50:
    return trap(o, (int64_t)s < (int64_t)t);
  case 51:
    return trap(o, s < t);
  case 52:
    return trap(o, s == t);
  case 54:
    return trap(o, s != t);
  case 56:
    *d = t << sa;
    return 1;
  case 58:
    *d = t >> sa;
    return 1;
  case 59:
    *d = (uint64_t)((int64_t)t >> sa);
    return 1;
  case 60:
    *d = t << (sa + 32);
    return 1;
  case 62:
    *d = t >> (sa + 32);
    return 1;
  case 63:
    *d = (uint64_t)((int64_t)t >> (sa + 32));
    return 1;
  default:
    cpu_exception(o, EXC_RI, 0);
    return 0;
  }
}

static int exec_regimm(Oracle *o, uint32_t w) {
  CpuState *c = &o->cpu;
  int64_t s = (int64_t)c->gpr[RS(w)], imm = IMM(w);
  unsigned rt = RT(w);
  if (rt >= 16 && rt <= 19) /* linking forms always write $ra */
    c->gpr[31] = sext32(c->cur_pc + 8);
  switch (rt) {
  case 0:
  case 16:
    branch(c, s < 0, w);
    return 1;
  case 1:
  case 17:
    branch(c, s >= 0, w);
    return 1;
  case 2:
  case 18:
    branch_likely(c, s < 0, w);
    return 1;
  case 3:
  case 19:
    branch_likely(c, s >= 0, w);
    return 1;
  case 8:
    return trap(o, s >= imm);
  case 9:
    return trap(o, (uint64_t)s >= (uint64_t)imm);
  case 10:
    return trap(o, s < imm);
  case 11:
    return trap(o, (uint64_t)s < (uint64_t)imm);
  case 12:
    return trap(o, s == imm);
  case 14:
    return trap(o, s != imm);
  default:
    cpu_exception(o, EXC_RI, 0);
    return 0;
  }
}

static int load_to(Oracle *o, uint32_t w, unsigned size, int sign) {
  CpuState *c = &o->cpu;
  uint64_t v;
  if (!load(o, c->gpr[RS(w)] + (uint64_t)IMM(w), size, &v))
    return 0;
  if (sign && size < 8) {
    unsigned sh = 64 - 8 * size;
    v = (uint64_t)((int64_t)(v << sh) >> sh);
  }
  c->gpr[RT(w)] = v;
  return 1;
}

static int store_from(Oracle *o, uint32_t w, unsigned size) {
  CpuState *c = &o->cpu;
  return store(o, c->gpr[RS(w)] + (uint64_t)IMM(w), size, c->gpr[RT(w)]);
}

static int load_linked(Oracle *o, uint32_t w, unsigned size) {
  CpuState *c = &o->cpu;
  uint64_t addr = c->gpr[RS(w)] + (uint64_t)IMM(w);
  if (!load_to(o, w, size, 1))
    return 0;
  c->llbit = 1;
  c->cop0[CP0_LLADDR] = ((uint32_t)addr & 0x1FFFFFFFu) >> 4;
  return 1;
}

static int store_conditional(Oracle *o, uint32_t w, unsigned size) {
  CpuState *c = &o->cpu;
  if (c->llbit && !store_from(o, w, size))
    return 0;
  c->gpr[RT(w)] = c->llbit;
  return 1;
}

static int fpu_load(Oracle *o, uint32_t w, unsigned size) {
  CpuState *c = &o->cpu;
  uint64_t v;
  if (!coprocessor_usable(o, 1) ||
      !load(o, c->gpr[RS(w)] + (uint64_t)IMM(w), size, &v))
    return 0;
  if (size == 4)
    fpr_set32(c, RT(w), (uint32_t)v);
  else
    fpr_set64(c, RT(w), v);
  return 1;
}

static int fpu_store(Oracle *o, uint32_t w, unsigned size) {
  CpuState *c = &o->cpu;
  if (!coprocessor_usable(o, 1))
    return 0;
  uint64_t v = size == 4 ? fpr_get32(c, RT(w)) : fpr_get64(c, RT(w));
  return store(o, c->gpr[RS(w)] + (uint64_t)IMM(w), size, v);
}

static int exec_cop1(Oracle *o, uint32_t w) {
  CpuState *c = &o->cpu;
  if (!coprocessor_usable(o, 1))
    return 0;
  if (RS(w) == 8) { /* bc1f / bc1t / bc1fl / bc1tl */
    int cond = (c->fcr31 & FCR31_C) != 0;
    int want = (w >> 16) & 1u;
    if (w & (1u << 17))
      branch_likely(c, cond == want, w);
    else
      branch(c, cond == want, w);
    return 1;
  }
  return fpu_exec(o, w);
}

static int exec(Oracle *o, uint32_t w) {
  CpuState *c = &o->cpu;
  uint64_t s = c->gpr[RS(w)], t = c->gpr[RT(w)];
  uint64_t *rt = &c->gpr[RT(w)];
  switch (w >> 26) {
  case 0:
    return exec_special(o, w);
  case 1:
    return exec_regimm(o, w);
  case 2: /* j */
    jump(c, ((c->cur_pc + 4) & 0xF0000000u) | (w & 0x03FFFFFFu) << 2);
    return 1;
  case 3: /* jal */
    c->gpr[31] = sext32(c->cur_pc + 8);
    jump(c, ((c->cur_pc + 4) & 0xF0000000u) | (w & 0x03FFFFFFu) << 2);
    return 1;
  case 4:
    branch(c, s == t, w);
    return 1;
  case 5:
    branch(c, s != t, w);
    return 1;
  case 6:
    branch(c, (int64_t)s <= 0, w);
    return 1;
  case 7:
    branch(c, (int64_t)s > 0, w);
    return 1;
  case 8: { /* addi */
    int32_t r;
    if (__builtin_add_overflow((int32_t)s, (int32_t)IMM(w), &r)) {
      cpu_exception(o, EXC_OV, 0);
      return 0;
    }
    *rt = sext32((uint32_t)r);
    return 1;
  }
  case 9:
    *rt = sext32((uint32_t)s + (uint32_t)IMM(w));
    return 1;
  case 10:
    *rt = (int64_t)s < IMM(w);
    return 1;
  case 11:
    *rt = s < (uint64_t)IMM(w);
    return 1;
  case 12:
    *rt = s & UIMM(w);
    return 1;
  case 13:
    *rt = s | UIMM(w);
    return 1;
  case 14:
    *rt = s ^ UIMM(w);
    return 1;
  case 15:
    *rt = sext32((uint32_t)(w << 16));
    return 1;
  case 16:
    return exec_cop0(o, w);
  case 17:
    return exec_cop1(o, w);
  case 18: /* COP2: the VR4300 has none */
    if (coprocessor_usable(o, 2))
      cpu_exception(o, EXC_RI, 0);
    return 0;
  case 20:
    branch_likely(c, s == t, w);
    return 1;
  case 21:
    branch_likely(c, s != t, w);
    return 1;
  case 22:
    branch_likely(c, (int64_t)s <= 0, w);
    return 1;
  case 23:
    branch_likely(c, (int64_t)s > 0, w);
    return 1;
  case 24: { /* daddi */
    int64_t r;
    if (__builtin_add_overflow((int64_t)s, IMM(w), &r)) {
      cpu_exception(o, EXC_OV, 0);
      return 0;
    }
    *rt = (uint64_t)r;
    return 1;
  }
  case 25:
    *rt = s + (uint64_t)IMM(w);
    return 1;
  case 26:
    return ldl_ldr(o, w, 1);
  case 27:
    return ldl_ldr(o, w, 0);
  case 32:
    return load_to(o, w, 1, 1);
  case 33:
    return load_to(o, w, 2, 1);
  case 34:
    return lwl_lwr(o, w, 1);
  case 35:
    return load_to(o, w, 4, 1);
  case 36:
    return load_to(o, w, 1, 0);
  case 37:
    return load_to(o, w, 2, 0);
  case 38:
    return lwl_lwr(o, w, 0);
  case 39:
    return load_to(o, w, 4, 0);
  case 40:
    return store_from(o, w, 1);
  case 41:
    return store_from(o, w, 2);
  case 42:
    return swl_swr(o, w, 1);
  case 43:
    return store_from(o, w, 4);
  case 44:
    return sdl_sdr(o, w, 1);
  case 45:
    return sdl_sdr(o, w, 0);
  case 46:
    return swl_swr(o, w, 0);
  case 47:
    return 1; /* cache: no cache model */
  case 48:
    return load_linked(o, w, 4);
  case 49:
    return fpu_load(o, w, 4);
  case 52:
    return load_linked(o, w, 8);
  case 53:
    return fpu_load(o, w, 8);
  case 55:
    return load_to(o, w, 8, 0);
  case 56:
    return store_conditional(o, w, 4);
  case 57:
    return fpu_store(o, w, 4);
  case 60:
    return store_conditional(o, w, 8);
  case 61:
    return fpu_store(o, w, 8);
  case 63:
    return store_from(o, w, 8);
  default:
    cpu_exception(o, EXC_RI, 0);
    return 0;
  }
}

/* --- Executed-word capture ---------------------------------------------- */

void exec_words_add(ExecWords *e, uint32_t vaddr, uint32_t word,
                    uint64_t icount) {
  uint64_t key = (uint64_t)vaddr << 32 | word;
  if (!key)
    key = 1; /* (0, nop) cannot be stored as 0 */
  if (e->n * 2 >= e->cap) {
    uint32_t old_cap = e->cap;
    uint64_t *ok = e->keys, *of = e->first;
    e->cap = old_cap ? old_cap * 2 : 1u << 16;
    e->keys = calloc(e->cap, sizeof(uint64_t));
    e->first = calloc(e->cap, sizeof(uint64_t));
    e->n = 0;
    for (uint32_t i = 0; i < old_cap; i++)
      if (ok[i]) {
        uint32_t h =
            (uint32_t)(ok[i] * 0x9E3779B97F4A7C15ull >> 40) & (e->cap - 1);
        while (e->keys[h])
          h = (h + 1) & (e->cap - 1);
        e->keys[h] = ok[i];
        e->first[h] = of[i];
        e->n++;
      }
    free(ok);
    free(of);
  }
  uint32_t h = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 40) & (e->cap - 1);
  while (e->keys[h]) {
    if (e->keys[h] == key)
      return;
    h = (h + 1) & (e->cap - 1);
  }
  e->keys[h] = key;
  e->first[h] = icount;
  e->n++;
}

/* --- Run loop ----------------------------------------------------------- */

static int interrupt_pending(Oracle *o) {
  const CpuState *c = &o->cpu;
  uint64_t sr = c->cop0[CP0_STATUS];
  if (!(sr & SR_IE) || (sr & (SR_EXL | SR_ERL)))
    return 0;
  uint64_t ip = cop0_read(o, CP0_CAUSE) & 0xFF00u;
  return (ip & sr & 0xFF00u) != 0;
}

static void at_boundary(Oracle *o) {
  CpuState *c = &o->cpu;
  o->boundaries++;
  if (o->checkpoints && o->boundaries % o->checkpoint_stride == 0 &&
      c->icount >= o->checkpoint_from)
    checkpoint_write(o->checkpoints, c, checkpoint_hash(c));
  if (interrupt_pending(o)) {
    c->cur_pc = c->pc;
    c->cur_delay = 0;
    cpu_exception(o, EXC_INT, 0);
    o->interrupts++;
  }
}

StopReason oracle_run(Oracle *o, uint64_t until) {
  CpuState *c = &o->cpu;
  HwState *hw = o->hw;
  o->stop = STOP_NONE; /* resumable: each call runs until the new limit */
  fpu_sync_host(c);    /* host rounding mode follows FCR31 */
  while (o->stop == STOP_NONE) {
    if (c->icount >= until) {
      o->stop = STOP_BUDGET;
      break;
    }
    if (c->boundary && !c->in_delay) {
      c->boundary = 0;
      at_boundary(o);
      /* An interrupt taken here makes the vector a boundary of its own
       * (timebase.h: "right after exception entry"): process it before
       * fetching the vector's first instruction. */
      continue;
    }

    uint32_t pc = c->pc, pa, w;
    o->history[o->history_pos++ & 255u] = pc;
    c->cur_pc = pc;
    c->cur_delay = c->in_delay;
    if (pc & 3u) {
      address_error(o, pc, 0);
      continue;
    }
    if (!translate(o, sext32(pc), &pa, 0))
      continue;
    if (pa < HW_RDRAM_SIZE) {
      w = hw_be32(hw->rdram + pa);
      o->exec_bits[pa >> 5] |= (uint8_t)(1u << ((pa >> 2) & 7u));
    } else {
      w = phys_read32(o, pa);
    }
    if (o->exec_words)
      exec_words_add(o->exec_words, pc, w, c->icount);
    if (pc == o->entry_pc && !o->entry_icount)
      o->entry_icount = c->icount;

    c->pc = c->next_pc;
    c->next_pc = c->pc + 4;
    c->in_delay = 0;
    int done = exec(o, w);
    c->gpr[0] = 0;
    if (!done)
      continue; /* exception entry (or stop); nothing retired */

    c->icount++;
    if (c->cur_delay)
      c->boundary = 1;
    if ((uint32_t)(c->icount + c->count_offset) ==
        (uint32_t)c->cop0[CP0_COMPARE])
      c->cop0[CP0_CAUSE] |= CAUSE_IP7;
    if (c->icount >= hw->next_event)
      hw_run_events(hw, c->icount);
  }
  return o->stop;
}
