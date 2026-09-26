/* Per-opcode tests for the tricky parts of the interpreter.
 *
 * Programs are assembled with the listing tool's encoder (itself checked
 * by golden encodings); expected results come from hand-worked constants
 * and from reference models written differently from the interpreter
 * (byte-by-byte memory merges, 16-bit-limb multiplication), fuzzed with
 * a fixed-seed xorshift generator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/hw/hw.h"
#include "src/oracle/cpu.h"
#include "src/tools/mips.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    uint64_t a_ = (uint64_t)(a), b_ = (uint64_t)(b);                           \
    if (a_ != b_) {                                                            \
      fprintf(stderr, "%s:%d: %s = %016llX, want %016llX\n", __FILE__,         \
              __LINE__, #a, (unsigned long long)a_, (unsigned long long)b_);   \
      failures++;                                                              \
    }                                                                          \
  } while (0)

#define CODE 0x80001000u
#define DATA 0x80002000u

static HwState *hw;
static Oracle o;
static CpuState *c;

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

/* Fresh CPU; the hardware keeps its memory (tests initialise what they
 * read) but loses any pending interrupts. */
static void reset(void) {
  hw->mi[MI_INTR] = 0;
  hw->mi[MI_INTR_MASK] = 0;
  oracle_free(&o);
  oracle_init(&o, hw);
  c = &o.cpu;
  c->cop0[CP0_STATUS] = 0x30000000u; /* CU1 | CU0, FR = 0, IE = 0 */
  c->fcr31 = 0;
  c->pc = CODE;
  c->next_pc = CODE + 4;
  o.stop_on_exception = 1;
}

/* Assembles `prog` (NULL-terminated) at CODE and resets the PC. */
static void load_prog(const char *const *prog) {
  uint32_t addr = CODE;
  for (int i = 0; prog[i]; i++, addr += 4) {
    uint32_t w;
    if (!mips_assemble(prog[i], addr, &w)) {
      fprintf(stderr, "cannot assemble \"%s\"\n", prog[i]);
      exit(1);
    }
    hw_put_be32(hw->rdram + (addr & 0x1FFFFFFFu), w);
  }
  c->pc = CODE;
  c->next_pc = CODE + 4;
  c->in_delay = 0;
  o.stop = STOP_NONE;
}

static void run(uint64_t n) {
  o.stop = STOP_NONE;
  oracle_run(&o, c->icount + n);
}

static uint8_t *mem(uint32_t vaddr) {
  return hw->rdram + (vaddr & 0x1FFFFFFFu);
}

/* --- lwl / lwr / swl / swr and the doubleword forms ------------------- */

/* Reference: register as big-endian bytes, merged byte by byte. */
static uint64_t ref_lwl(const uint8_t *m, unsigned k, uint64_t rt) {
  uint8_t r[4] = {(uint8_t)(rt >> 24), (uint8_t)(rt >> 16), (uint8_t)(rt >> 8),
                  (uint8_t)rt};
  for (unsigned i = k; i < 4; i++)
    r[i - k] = m[i];
  uint32_t v = (uint32_t)r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3];
  return (uint64_t)(int64_t)(int32_t)v;
}

static uint64_t ref_lwr(const uint8_t *m, unsigned k, uint64_t rt) {
  uint8_t r[4] = {(uint8_t)(rt >> 24), (uint8_t)(rt >> 16), (uint8_t)(rt >> 8),
                  (uint8_t)rt};
  for (unsigned i = 0; i <= k; i++)
    r[3 - k + i] = m[i];
  uint32_t v = (uint32_t)r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3];
  return (rt & 0xFFFFFFFF00000000ull) | v; /* 32-bit mode: low word only */
}

static void ref_swl(uint8_t *m, unsigned k, uint64_t rt) {
  for (unsigned i = k; i < 4; i++)
    m[i] = (uint8_t)(rt >> (8 * (3 - (i - k))));
}

static void ref_swr(uint8_t *m, unsigned k, uint64_t rt) {
  for (unsigned i = 0; i <= k; i++)
    m[i] = (uint8_t)(rt >> (8 * (k - i)));
}

static uint64_t ref_ldl(const uint8_t *m, unsigned k, uint64_t rt) {
  uint8_t r[8];
  for (int i = 0; i < 8; i++)
    r[i] = (uint8_t)(rt >> (56 - 8 * i));
  for (unsigned i = k; i < 8; i++)
    r[i - k] = m[i];
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v = v << 8 | r[i];
  return v;
}

static uint64_t ref_ldr(const uint8_t *m, unsigned k, uint64_t rt) {
  uint8_t r[8];
  for (int i = 0; i < 8; i++)
    r[i] = (uint8_t)(rt >> (56 - 8 * i));
  for (unsigned i = 0; i <= k; i++)
    r[7 - k + i] = m[i];
  uint64_t v = 0;
  for (int i = 0; i < 8; i++)
    v = v << 8 | r[i];
  return v;
}

static void ref_sdl(uint8_t *m, unsigned k, uint64_t rt) {
  for (unsigned i = k; i < 8; i++)
    m[i] = (uint8_t)(rt >> (8 * (7 - (i - k))));
}

static void ref_sdr(uint8_t *m, unsigned k, uint64_t rt) {
  for (unsigned i = 0; i <= k; i++)
    m[i] = (uint8_t)(rt >> (8 * (k - i)));
}

static const char *const unaligned_ops[] = {"lwl", "lwr", "swl", "swr",
                                            "ldl", "ldr", "sdl", "sdr"};

static void test_unaligned(void) {
  /* Hand-worked: memory 11 22 33 44, $a1 = AABBCCDD. */
  static const struct {
    const char *op;
    unsigned k;
    uint64_t want; /* register result (loads) or memory word (stores) */
  } hand[] = {
      {"lwl", 0, 0x11223344ull},         {"lwl", 1, 0x223344DDull},
      {"lwl", 3, 0x44BBCCDDull},         {"lwr", 0, 0xFFFFFFFFAABBCC11ull},
      {"lwr", 2, 0xFFFFFFFFAA112233ull}, {"lwr", 3, 0xFFFFFFFF11223344ull},
      {"swl", 0, 0xAABBCCDDull},         {"swl", 2, 0x1122AABBull},
      {"swr", 0, 0xDD223344ull},         {"swr", 1, 0xCCDD3344ull},
      {"swr", 3, 0xAABBCCDDull},
  };
  for (size_t i = 0; i < sizeof(hand) / sizeof(hand[0]); i++) {
    reset();
    char text[64];
    snprintf(text, sizeof(text), "%s $a1, 0x%X($a0)", hand[i].op, hand[i].k);
    const char *prog[] = {text, NULL};
    load_prog(prog);
    hw_put_be32(mem(DATA), 0x11223344u);
    c->gpr[4] = (uint64_t)(int64_t)(int32_t)DATA;
    c->gpr[5] = 0xFFFFFFFFAABBCCDDull;
    run(1);
    if (hand[i].op[0] == 'l')
      CHECK_EQ(c->gpr[5], hand[i].want);
    else
      CHECK_EQ(hw_be32(mem(DATA)), hand[i].want);
  }

  /* Fuzz all eight ops over every byte offset. */
  for (int iter = 0; iter < 400; iter++) {
    unsigned opi = (unsigned)(rnd() % 8), dbl = opi >= 4;
    unsigned k = (unsigned)(rnd() % (dbl ? 8 : 4));
    reset();
    char text[64];
    snprintf(text, sizeof(text), "%s $a1, 0x%X($a0)", unaligned_ops[opi], k);
    const char *prog[] = {text, NULL};
    load_prog(prog);
    uint8_t before[8], want_mem[8];
    for (int i = 0; i < 8; i++)
      before[i] = mem(DATA)[i] = (uint8_t)rnd();
    memcpy(want_mem, before, 8);
    uint64_t rt = rnd(), want_reg = rt;
    if (!dbl)
      rt = (uint64_t)(int64_t)(int32_t)rt; /* 32-bit values are sext */
    want_reg = rt;
    switch (opi) {
    case 0:
      want_reg = ref_lwl(before, k, rt);
      break;
    case 1:
      want_reg = ref_lwr(before, k, rt);
      break;
    case 2:
      ref_swl(want_mem, k, rt);
      break;
    case 3:
      ref_swr(want_mem, k, rt);
      break;
    case 4:
      want_reg = ref_ldl(before, k, rt);
      break;
    case 5:
      want_reg = ref_ldr(before, k, rt);
      break;
    case 6:
      ref_sdl(want_mem, k, rt);
      break;
    case 7:
      ref_sdr(want_mem, k, rt);
      break;
    }
    c->gpr[4] = (uint64_t)(int64_t)(int32_t)DATA;
    c->gpr[5] = rt;
    run(1);
    if (c->gpr[5] != want_reg || memcmp(mem(DATA), want_mem, 8) != 0) {
      fprintf(stderr, "%s: reg %016llX want %016llX, mem mismatch=%d\n", text,
              (unsigned long long)c->gpr[5], (unsigned long long)want_reg,
              memcmp(mem(DATA), want_mem, 8) != 0);
      failures++;
    }
  }
}

/* --- mult / div -------------------------------------------------------- */

static void run_hilo(const char *op, uint64_t a, uint64_t b) {
  reset();
  char text[64];
  snprintf(text, sizeof(text), "%s $a0, $a1", op);
  const char *prog[] = {text, NULL};
  load_prog(prog);
  c->gpr[4] = a;
  c->gpr[5] = b;
  run(1);
}

#define S32(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))

static void test_div(void) {
  static const struct {
    const char *op;
    uint64_t a, b, lo, hi;
  } hand[] = {
      {"div", 7, 2, 3, 1},
      {"div", S32(-7), 2, S32(-3), S32(-1)},
      {"div", 7, S32(-2), S32(-3), 1},
      {"div", 5, 0, S32(-1), 5},       /* by zero, dividend >= 0 */
      {"div", S32(-5), 0, 1, S32(-5)}, /* by zero, dividend < 0 */
      {"div", S32(0x80000000), S32(-1), S32(0x80000000), 0},
      {"divu", S32(0xFFFFFFFF), 2, S32(0x7FFFFFFF), 1},
      {"divu", 9, 0, S32(0xFFFFFFFF), 9},
      {"divu", S32(0x80000000), 0, S32(0xFFFFFFFF), S32(0x80000000)},
      {"ddiv", 0x8000000000000000ull, ~0ull, 0x8000000000000000ull, 0},
      {"ddiv", 5, 0, ~0ull, 5},
      {"ddiv", (uint64_t)-5, 0, 1, (uint64_t)-5},
      {"ddiv", (uint64_t)-100, 7, (uint64_t)-14, (uint64_t)-2},
      {"ddivu", 100, 0, ~0ull, 100},
      {"ddivu", ~0ull, 16, 0x0FFFFFFFFFFFFFFFull, 15},
      {"mult", S32(0x7FFFFFFF), S32(0x7FFFFFFF), 1, S32(0x3FFFFFFF)},
      {"mult", S32(-1), 1, S32(-1), S32(-1)},
      {"mult", S32(0x80000000), S32(0x80000000), 0, S32(0x40000000)},
      {"multu", S32(0xFFFFFFFF), S32(0xFFFFFFFF), 1, S32(0xFFFFFFFE)},
      {"multu", S32(0x80000000), 2, 0, 1},
      {"dmult", (uint64_t)-1, (uint64_t)-1, 1, 0},
      {"dmult", 0x8000000000000000ull, 2, 0, ~0ull},
      {"dmultu", ~0ull, ~0ull, 1, 0xFFFFFFFFFFFFFFFEull},
  };
  for (size_t i = 0; i < sizeof(hand) / sizeof(hand[0]); i++) {
    run_hilo(hand[i].op, hand[i].a, hand[i].b);
    if (c->lo != hand[i].lo || c->hi != hand[i].hi) {
      fprintf(stderr, "%s %016llX, %016llX: lo %016llX hi %016llX\n",
              hand[i].op, (unsigned long long)hand[i].a,
              (unsigned long long)hand[i].b, (unsigned long long)c->lo,
              (unsigned long long)c->hi);
      failures++;
    }
  }

  /* dmultu fuzz against 16-bit-limb schoolbook multiplication. */
  for (int iter = 0; iter < 500; iter++) {
    uint64_t a = rnd(), b = rnd();
    uint32_t limbs[8] = {0};
    for (int i = 0; i < 4; i++) {
      uint64_t carry = 0;
      for (int j = 0; j < 4; j++) {
        uint64_t t =
            (uint64_t)((a >> (16 * i)) & 0xFFFF) * ((b >> (16 * j)) & 0xFFFF) +
            limbs[i + j] + carry;
        limbs[i + j] = (uint32_t)(t & 0xFFFF);
        carry = t >> 16;
      }
      for (int k = i + 4; carry && k < 8; k++) {
        uint64_t t = limbs[k] + carry;
        limbs[k] = (uint32_t)(t & 0xFFFF);
        carry = t >> 16;
      }
    }
    uint64_t lo = 0, hi = 0;
    for (int i = 3; i >= 0; i--) {
      lo = lo << 16 | limbs[i];
      hi = hi << 16 | limbs[i + 4];
    }
    run_hilo("dmultu", a, b);
    CHECK_EQ(c->lo, lo);
    CHECK_EQ(c->hi, hi);
  }

  /* div fuzz: quotient * divisor + remainder == dividend, |rem| < |div|,
   * remainder has the dividend's sign. */
  for (int iter = 0; iter < 500; iter++) {
    int32_t a = (int32_t)rnd(), b = (int32_t)(rnd() >> (rnd() % 32));
    if (b == 0 || (a == INT32_MIN && b == -1))
      continue;
    run_hilo("div", S32(a), S32(b));
    int64_t q = (int32_t)c->lo, r = (int32_t)c->hi;
    CHECK(q * b + r == a);
    CHECK((r >= 0 ? r : -r) < (b >= 0 ? (int64_t)b : -(int64_t)b));
    CHECK(r == 0 || (r < 0) == (a < 0));
    CHECK(c->lo == S32((uint32_t)q) && c->hi == S32((uint32_t)r));
  }
}

/* --- FPU rounding ------------------------------------------------------ */

static uint32_t fbits(float f) {
  uint32_t b;
  memcpy(&b, &f, 4);
  return b;
}

static void run_fp(const char *op, uint32_t fcr31, uint64_t src_bits,
                   int src_double) {
  reset();
  char text[64];
  snprintf(text, sizeof(text), "%s $f0, $f2", op);
  const char *prog[] = {text, NULL};
  load_prog(prog);
  c->fcr31 = fcr31;
  if (src_double)
    fpr_set64(c, 2, src_bits);
  else
    fpr_set32(c, 2, (uint32_t)src_bits);
  run(1);
}

static void test_fpu_rounding(void) {
  static const float in[] = {2.5f, 3.5f, -2.5f, 1.2f, -1.2f, 1.7f};
  /* Rows: RN, RZ, RP, RM (FCR31.RM = 0..3). */
  static const int32_t want[4][6] = {
      {2, 4, -2, 1, -1, 2},
      {2, 3, -2, 1, -1, 1},
      {3, 4, -2, 2, -1, 2},
      {2, 3, -3, 1, -2, 1},
  };
  for (unsigned rm = 0; rm < 4; rm++)
    for (unsigned i = 0; i < 6; i++) {
      run_fp("cvt.w.s", rm, fbits(in[i]), 0);
      if ((int32_t)fpr_get32(c, 0) != want[rm][i]) {
        fprintf(stderr, "cvt.w.s(%g) rm=%u: got %d want %d\n", in[i], rm,
                (int32_t)fpr_get32(c, 0), want[rm][i]);
        failures++;
      }
      /* All six inputs are inexact: I in cause and flags. */
      CHECK((c->fcr31 >> FCR31_CAUSE_SH) == FPX_I);
      CHECK(c->fcr31 & (FPX_I << FCR31_FLAGS_SH));
    }
  /* Fixed-mode conversions ignore FCR31.RM (run under RM = RP). */
  static const struct {
    const char *op;
    float x;
    int32_t want;
  } fixed[] = {
      {"round.w.s", 2.5f, 2},   {"round.w.s", 3.5f, 4},
      {"trunc.w.s", -1.7f, -1}, {"ceil.w.s", -1.7f, -1},
      {"floor.w.s", -1.2f, -2}, {"floor.w.s", 1.7f, 1},
  };
  for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
    run_fp(fixed[i].op, 2, fbits(fixed[i].x), 0);
    CHECK_EQ((uint64_t)(int64_t)(int32_t)fpr_get32(c, 0),
             (uint64_t)(int64_t)fixed[i].want);
  }
  /* Exact conversion: no cause bits. */
  run_fp("cvt.w.s", 0, fbits(-8.0f), 0);
  CHECK_EQ(fpr_get32(c, 0), (uint32_t)-8);
  CHECK_EQ(c->fcr31 >> FCR31_CAUSE_SH, 0);

  /* cvt.s.d of 1 + 2^-24 (exact tie between 1 and 1 + 2^-23). */
  uint64_t tie = 0x3FF0000010000000ull;
  static const uint32_t want_s[4] = {0x3F800000u, 0x3F800000u, 0x3F800001u,
                                     0x3F800000u};
  for (unsigned rm = 0; rm < 4; rm++) {
    run_fp("cvt.s.d", rm, tie, 1);
    CHECK_EQ(fpr_get32(c, 0), want_s[rm]);
  }
  /* 1 + 3*2^-24 ties up to 1 + 2^-22 under RN (even mantissa). */
  run_fp("cvt.s.d", 0, 0x3FF0000030000000ull, 1);
  CHECK_EQ(fpr_get32(c, 0), 0x3F800002u);

  /* NaN / out-of-range conversion: Unimplemented -> FPE exception. */
  run_fp("cvt.w.s", 0, 0x7F800000u /* +inf */, 0);
  CHECK_EQ(o.exceptions[EXC_FPE], 1);
  CHECK_EQ((c->fcr31 >> FCR31_CAUSE_SH) & FPX_E, FPX_E);
  CHECK_EQ(c->pc, 0x80000180u);
  CHECK_EQ((uint32_t)c->cop0[CP0_EPC], CODE);

  /* Invalid (0/0) with EV enabled traps; disabled gives the MIPS NaN. */
  reset();
  const char *divp[] = {"div.s $f0, $f2, $f2", NULL};
  load_prog(divp);
  fpr_set32(c, 2, 0);
  run(1);
  CHECK_EQ(fpr_get32(c, 0), 0x7FBFFFFFu);
  CHECK(c->fcr31 & (FPX_V << FCR31_FLAGS_SH));
  reset();
  load_prog(divp);
  c->fcr31 = FPX_V << FCR31_ENABLE_SH;
  fpr_set32(c, 2, 0);
  run(1);
  CHECK_EQ(o.exceptions[EXC_FPE], 1);

  /* FR=0 pairing: ldc1 into $f2 fills $f2 (low) and $f3 (high). */
  reset();
  const char *pair[] = {"ldc1 $f2, 0x0($a0)", "mfc1 $t0, $f2", "mfc1 $t1, $f3",
                        NULL};
  load_prog(pair);
  hw_put_be32(mem(DATA), 0x40090000u);
  hw_put_be32(mem(DATA + 4), 0x00000001u);
  c->gpr[4] = (uint64_t)(int64_t)(int32_t)DATA;
  run(3);
  CHECK_EQ(c->gpr[8], 1);
  CHECK_EQ(c->gpr[9], 0x40090000u);
}

/* --- Branches, delay slots, exceptions --------------------------------- */

static void test_branch_likely(void) {
  /* beql taken: delay slot runs. */
  static const char *const taken[] = {
      "beql $zero, $zero, 0x8000100C", "addiu $t0, $zero, 0x1",
      "addiu $t1, $zero, 0x2", "addiu $t2, $zero, 0x3", NULL};
  reset();
  load_prog(taken);
  run(3);
  CHECK_EQ(c->gpr[8], 1);
  CHECK_EQ(c->gpr[9], 0);
  CHECK_EQ(c->gpr[10], 3);
  CHECK_EQ(c->icount, 3);

  /* bnel not taken: delay slot nullified and not counted. */
  static const char *const not_taken[] = {
      "bnel $zero, $zero, 0x8000100C", "addiu $t0, $zero, 0x1",
      "addiu $t1, $zero, 0x2", "addiu $t2, $zero, 0x3", NULL};
  reset();
  load_prog(not_taken);
  run(2);
  CHECK_EQ(c->gpr[8], 0);
  CHECK_EQ(c->gpr[9], 2);
  CHECK_EQ(c->pc, CODE + 12);
  CHECK_EQ(c->icount, 2);

  /* bltzall not taken: links anyway, nullifies the slot. */
  static const char *const link[] = {"bltzall $zero, 0x80001100",
                                     "addiu $t0, $zero, 0x1", NULL};
  reset();
  load_prog(link);
  run(1);
  CHECK_EQ(c->gpr[31], (uint64_t)(int64_t)(int32_t)(CODE + 8));
  CHECK_EQ(c->pc, CODE + 8);
  CHECK_EQ(c->gpr[8], 0);

  /* Normal branch not taken still executes the delay slot. */
  static const char *const plain[] = {"bne $zero, $zero, 0x80001100",
                                      "addiu $t0, $zero, 0x1", NULL};
  reset();
  load_prog(plain);
  run(2);
  CHECK_EQ(c->gpr[8], 1);
  CHECK_EQ(c->pc, CODE + 8);
}

static void test_exceptions(void) {
  /* Overflow in a delay slot: EPC = branch, BD set, no write-back. */
  static const char *const ov[] = {"beq $zero, $zero, 0x80001100",
                                   "add $t0, $a0, $a0", NULL};
  reset();
  load_prog(ov);
  c->gpr[4] = 0x7FFFFFFF;
  c->gpr[8] = 77;
  run(2);
  CHECK_EQ(o.exceptions[EXC_OV], 1);
  CHECK_EQ(c->gpr[8], 77);
  CHECK_EQ((uint32_t)c->cop0[CP0_EPC], CODE);
  CHECK(c->cop0[CP0_CAUSE] & CAUSE_BD);
  CHECK_EQ((c->cop0[CP0_CAUSE] >> 2) & 31, EXC_OV);
  CHECK_EQ(c->pc, 0x80000180u);
  CHECK(c->cop0[CP0_STATUS] & SR_EXL);
  CHECK_EQ(c->icount, 1); /* the faulting add did not retire */

  /* Unaligned lw: AdEL with BadVAddr. */
  static const char *const ade[] = {"lw $t0, 0x2($a0)", NULL};
  reset();
  load_prog(ade);
  c->gpr[4] = (uint64_t)(int64_t)(int32_t)DATA;
  run(1);
  CHECK_EQ(o.exceptions[EXC_ADEL], 1);
  CHECK_EQ((uint32_t)c->cop0[CP0_BADVADDR], DATA + 2);

  /* Trap: teq taken. */
  static const char *const tr[] = {"teq $a0, $a0", NULL};
  reset();
  load_prog(tr);
  run(1);
  CHECK_EQ(o.exceptions[EXC_TR], 1);

  /* Coprocessor unusable: COP1 op with CU1 clear, CE = 1. */
  static const char *const cpu[] = {"mfc1 $t0, $f0", NULL};
  reset();
  load_prog(cpu);
  c->cop0[CP0_STATUS] = 0x10000000u;
  run(1);
  CHECK_EQ(o.exceptions[EXC_CPU], 1);
  CHECK_EQ((c->cop0[CP0_CAUSE] >> 28) & 3, 1);
}

static void test_count_compare(void) {
  /* mtc0 Count = 100, then two more instructions: Count reads 102. */
  static const char *const prog[] = {"mtc0 $a0, $Count", "nop",
                                     "mfc0 $t0, $Count", NULL};
  reset();
  load_prog(prog);
  c->gpr[4] = 100;
  run(3);
  CHECK_EQ(c->gpr[8], 102);

  /* Compare = Count + 3 fires IP7 after the third following retire. */
  static const char *const cmp[] = {
      "mtc0 $zero, $Count", "mtc0 $a0, $Compare", "nop", "nop", "nop", NULL};
  reset();
  load_prog(cmp);
  c->gpr[4] = 4;
  run(3);
  CHECK(!(c->cop0[CP0_CAUSE] & CAUSE_IP7)); /* Count == 3 */
  run(1);
  CHECK(c->cop0[CP0_CAUSE] & CAUSE_IP7); /* Count == 4 */
}

static void test_interrupt_sampling(void) {
  /* An MI interrupt pending before straight-line code is only taken at
   * the next block boundary (after the jump's delay slot). */
  static const char *const prog[] = {"addiu $t0, $zero, 0x1",
                                     "addiu $t1, $zero, 0x2", "j 0x80001100",
                                     "addiu $t2, $zero, 0x3", NULL};
  reset();
  load_prog(prog);
  c->cop0[CP0_STATUS] = 0x30000401u; /* IE, IM2 */
  hw->mi[MI_INTR_MASK] = MI_INTR_VI;
  hw->mi[MI_INTR] = MI_INTR_VI;
  c->boundary = 0;
  run(4);
  CHECK_EQ(c->gpr[10], 3);
  CHECK_EQ(o.interrupts, 0);
  run(1); /* boundary: interrupt taken instead of executing 0x80001100 */
  CHECK_EQ(o.interrupts, 1);
  CHECK_EQ((uint32_t)c->cop0[CP0_EPC], 0x80001100u);
  CHECK(!(c->cop0[CP0_CAUSE] & CAUSE_BD));
}

static void test_tlb(void) {
  /* Map virtual 0x04000000 (64 KB even page) to physical 0x00100000. */
  static const char *const prog[] = {"lw $t0, 0x650($a0)", "sw $t1, 0x654($a0)",
                                     NULL};
  reset();
  load_prog(prog);
  c->tlb[0].page_mask = 0x1E000;
  c->tlb[0].entry_hi = 0x04000000;
  c->tlb[0].entry_lo0 = (0x00100000u >> 12) << 6 | 0x6 | 1; /* D V G */
  c->tlb[0].entry_lo1 = 1;                                  /* G */
  hw_put_be32(mem(0x80100650u), 0xCAFEF00Du);
  c->gpr[4] = 0x04000000;
  c->gpr[9] = 0x12345678;
  run(2);
  CHECK_EQ(c->gpr[8], 0xFFFFFFFFCAFEF00Dull);
  CHECK_EQ(hw_be32(mem(0x80100654u)), 0x12345678u);

  /* Unmapped KUSEG access: TLB refill (vector 0x80000000), TLBL. */
  static const char *const miss[] = {"lw $t0, 0x0($a0)", NULL};
  reset();
  load_prog(miss);
  c->gpr[4] = 0x00400000;
  run(1);
  CHECK_EQ(c->pc, 0x80000000u);
  CHECK_EQ((c->cop0[CP0_CAUSE] >> 2) & 31, 2);
  CHECK_EQ((uint32_t)c->cop0[CP0_BADVADDR], 0x00400000u);
}

int main(void) {
  mips_init();
  hw = malloc(sizeof(HwState));
  hw_init(hw, NULL, 0);
  oracle_init(&o, hw);
  test_unaligned();
  test_div();
  test_fpu_rounding();
  test_branch_likely();
  test_exceptions();
  test_count_compare();
  test_interrupt_sampling();
  test_tlb();
  oracle_free(&o);
  hw_free(hw);
  free(hw);
  printf("%d failures\n", failures);
  return failures ? 1 : 0;
}
