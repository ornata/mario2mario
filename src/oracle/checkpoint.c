#include "src/oracle/checkpoint.h"

static uint32_t count_now(const CpuState *c) {
  return (uint32_t)(c->icount + c->count_offset);
}

uint64_t checkpoint_hash(const CpuState *c) {
  uint64_t h = HW_HASH_INIT;
  h = hw_hash_word(h, c->pc);
  for (int i = 0; i < 32; i++)
    h = hw_hash_word(h, c->gpr[i]);
  h = hw_hash_word(h, c->hi);
  h = hw_hash_word(h, c->lo);
  for (int i = 0; i < 32; i++)
    h = hw_hash_word(h, c->fpr[i]);
  h = hw_hash_word(h, c->fcr31);
  h = hw_hash_word(h, c->cop0[CP0_STATUS]);
  h = hw_hash_word(h, c->cop0[CP0_CAUSE] & ~(uint64_t)CAUSE_IP2);
  h = hw_hash_word(h, c->cop0[CP0_EPC]);
  h = hw_hash_word(h, c->cop0[CP0_ERROREPC]);
  h = hw_hash_word(h, c->cop0[CP0_BADVADDR]);
  h = hw_hash_word(h, count_now(c));
  h = hw_hash_word(h, c->cop0[CP0_COMPARE]);
  h = hw_hash_word(h, c->llbit);
  h = hw_hash_word(h, c->icount);
  return h;
}

static void put_le(uint8_t *p, uint64_t v, int n) {
  for (int i = 0; i < n; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

void checkpoint_write(FILE *f, const CpuState *c, uint64_t hash) {
  uint8_t rec[24];
  put_le(rec, c->icount, 8);
  put_le(rec + 8, c->pc, 4);
  put_le(rec + 12, 0, 4);
  put_le(rec + 16, hash, 8);
  fwrite(rec, 1, sizeof(rec), f);
}
