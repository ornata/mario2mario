/* Per-opcode stub harness (PLAN.md Phase 5.1).
 *
 * For every opcode form in the translated units, stubgen extracted one
 * representative LLM-translated group. Each is run in isolation over
 * fixed-seed randomized register, FPU and memory states and compared with
 * the oracle executing the same MIPS word from the same state: all GPRs,
 * HI/LO, FPRs, FCR31, the exception-visible COP0 registers, the retired
 * count and a 4 KB data window. Grows automatically with coverage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/hw/hw.h"
#include "src/native/native.h"
#include "src/oracle/cpu.h"
#include "src/tools/mips.h"

typedef struct {
  const void *code;
  uint32_t pc, word;
} Stub;

extern const Stub m2m_stub_table[];
extern const uint32_t m2m_stub_count;
void m2m_enter(NativeState *ns, const void *code);

#define WINDOW 0x100000u
#define TRIALS 64

static uint64_t rng;
static uint64_t rnd(void) {
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return rng;
}

static uint32_t normal_float_bits(void) {
  uint32_t sign = (uint32_t)rnd() & 0x80000000u;
  uint32_t exp = 100u + (uint32_t)(rnd() % 54u); /* 2^-27 .. 2^26 */
  return sign | exp << 23 | ((uint32_t)rnd() & 0x7FFFFFu);
}

static uint64_t normal_double_bits(void) {
  uint64_t sign = rnd() & 0x8000000000000000ull;
  uint64_t exp = 990u + rnd() % 60u;
  return sign | exp << 52 | (rnd() & 0xFFFFFFFFFFFFFull);
}

static void random_state(CpuState *c, int doubles) {
  memset(c, 0, sizeof(*c));
  for (int i = 1; i < 32; i++)
    c->gpr[i] = rnd() & 1 ? sext32((uint32_t)rnd()) : rnd();
  c->hi = rnd();
  c->lo = rnd();
  for (int i = 0; i < 32; i += 2)
    c->fpr[i] = doubles
                    ? normal_double_bits()
                    : (uint64_t)normal_float_bits() << 32 | normal_float_bits();
  c->fcr31 = 0x01000000u | ((uint32_t)rnd() & 3u);
  c->cop0[CP0_STATUS] = 0x3000FF00u; /* CU1 | CU0, FR = 0, IE = 0 */
  c->cop0[CP0_COMPARE] = (uint32_t)rnd();
}

int main(void) {
  mips_init();
  HwState *hwo = malloc(sizeof(HwState)), *hwn = malloc(sizeof(HwState));
  hw_init(hwo, NULL, 0);
  hw_init(hwn, NULL, 0);
  Oracle *o = malloc(sizeof(Oracle));
  NativeState *ns = malloc(sizeof(NativeState));
  oracle_init(o, hwo);
  native_init(ns, hwn);
  int failures = 0;
  uint32_t trials = 0;
  for (uint32_t s = 0; s < m2m_stub_count; s++) {
    const Stub *st = &m2m_stub_table[s];
    uint16_t op = mips_decode(st->word);
    char text[MIPS_TEXT_MAX];
    mips_format(st->word, op, st->pc, text);
    int bad = 0;
    for (int t = 0; t < TRIALS && !bad; t++) {
      rng = 0x9E3779B97F4A7C15ull ^ ((uint64_t)s << 20) ^ (uint64_t)t;
      CpuState init;
      random_state(&init, t & 1);
      /* Memory instructions address the data window. */
      if (op != MIPS_OP_INVALID &&
          (mips_operand_bits(op) & 0x03E0FFFFu) == 0x03E0FFFFu) {
        unsigned rs = (st->word >> 21) & 31u;
        int32_t imm = (int16_t)(st->word & 0xFFFFu);
        uint32_t off = 0x400u + ((uint32_t)rnd() & 0x7F8u) +
                       (t & 4 ? (uint32_t)t & 7u : 0);
        if (rs)
          init.gpr[rs] = sext32(0x80000000u + WINDOW + off - (uint32_t)imm);
      }
      for (uint32_t i = 0; i < 4096; i++)
        hwo->rdram[WINDOW + i] = hwn->rdram[WINDOW + i] = (uint8_t)rnd();
      hw_put_be32(hwo->rdram + (st->pc & 0x7FFFFFu), st->word);

      o->cpu = init;
      o->cpu.pc = st->pc;
      o->cpu.next_pc = st->pc + 4;
      o->stop_on_exception = 1;
      oracle_run(o, o->cpu.icount + 1);

      ns->cpu = init;
      ns->cpu.pc = st->pc;
      ns->stop = NSTOP_NONE;
      ns->budget = UINT64_MAX;
      ns->attn = UINT64_MAX;
      ns->boundary_left = UINT64_MAX;
      ns->trap_log = NULL;
      m2m_enter(ns, st->code);

      const CpuState *a = &o->cpu, *b = &ns->cpu;
      const char *what = NULL;
      for (int i = 1; i < 32 && !what; i++)
        if (a->gpr[i] != b->gpr[i])
          what = "gpr";
      if (!what && (a->hi != b->hi || a->lo != b->lo))
        what = "hi/lo";
      for (int i = 0; i < 32 && !what; i++)
        if (a->fpr[i] != b->fpr[i])
          what = "fpr";
      if (!what && a->fcr31 != b->fcr31)
        what = "fcr31";
      if (!what &&
          (a->cop0[CP0_EPC] != b->cop0[CP0_EPC] ||
           (a->cop0[CP0_CAUSE] & 0x7C) != (b->cop0[CP0_CAUSE] & 0x7C) ||
           a->cop0[CP0_STATUS] != b->cop0[CP0_STATUS] ||
           a->cop0[CP0_BADVADDR] != b->cop0[CP0_BADVADDR]))
        what = "cop0";
      if (!what && a->icount != b->icount)
        what = "icount";
      if (!what && memcmp(hwo->rdram + WINDOW, hwn->rdram + WINDOW, 4096))
        what = "memory";
      if (what) {
        fprintf(stderr, "%08X %s: %s differs (trial %d)\n", st->pc, text, what,
                t);
        bad = 1;
        failures++;
      }
      trials++;
    }
  }
  printf("%u opcode forms, %u trials, %d failing forms\n", m2m_stub_count,
         trials, failures);
  return failures ? 1 : 0;
}
