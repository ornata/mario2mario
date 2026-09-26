#include "src/oracle/trace.h"

static int executed(const Oracle *o, uint32_t word) {
  return (o->exec_bits[word >> 3] >> (word & 7u)) & 1u;
}

uint32_t trace_exec_words(const Oracle *o) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < HW_RDRAM_SIZE / 4 / 8; i++)
    n += (uint32_t)__builtin_popcount(o->exec_bits[i]);
  return n;
}

void trace_write(FILE *f, const Oracle *o) {
  const HwState *hw = o->hw;
  fprintf(f, "# m2m oracle trace v1\nicount %llu\n",
          (unsigned long long)o->cpu.icount);
  for (uint32_t i = 0; i < hw->dma_count; i++) {
    const HwDmaRecord *d = &hw->dma_log[i];
    fprintf(f, "dma %llX %X %X %X\n", (unsigned long long)d->icount,
            d->rom_offset, d->ram_addr, d->length);
  }
  for (uint32_t i = 0; i < hw->task_count; i++) {
    const HwTaskRecord *t = &hw->task_log[i];
    fprintf(f, "task %llX %X %X %X %016llX\n", (unsigned long long)t->icount,
            t->task[OSTASK_TYPE], t->task[OSTASK_DATA_PTR],
            t->task[OSTASK_DATA_SIZE], (unsigned long long)t->data_hash);
  }
  const uint32_t words = HW_RDRAM_SIZE / 4;
  for (uint32_t i = 0; i < words;) {
    if (!executed(o, i)) {
      i++;
      continue;
    }
    uint32_t start = i;
    while (i < words && executed(o, i))
      i++;
    fprintf(f, "exec %08X %08X\n", 0x80000000u + start * 4,
            0x80000000u + i * 4);
  }
}
