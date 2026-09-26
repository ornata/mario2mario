/* Boot milestone: the ROM's own IPL3 and game code under the oracle.
 *
 * (a) IPL3 completes: its 1 MB PI DMA of ROM 0x1000 lands at 0x246000
 *     and control reaches the header entry point 0x80246000.
 * (b) Threads start: the scheduler's first eret (thread dispatch) after
 *     entry, and interrupts are being serviced.
 * (c) The first RSP graphics task is submitted: OSTask at DMEM 0xFC0 with
 *     a display-list pointer inside RDRAM and a non-trivial hash.
 * Exit criterion: over a late 100M-instruction window (450M..550M, after
 * the title-screen head has loaded) the game keeps
 * submitting graphics tasks and polling SI every frame.
 * Determinism: two runs produce byte-identical checkpoint streams. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/hw/hw.h"
#include "src/oracle/boot.h"
#include "src/oracle/cpu.h"
#include "src/tools/rom_env.h"
#include "src/tools/z64.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static uint32_t count_gfx(const HwState *hw, uint32_t from) {
  uint32_t n = 0;
  for (uint32_t i = from; i < hw->task_count; i++)
    n += hw->task_log[i].task[OSTASK_TYPE] == OSTASK_M_GFXTASK;
  return n;
}

/* Runs a fresh machine to `until` with checkpoints into `ckpt` (or none). */
static void boot(const Z64Rom *rom, HwState *hw, Oracle *o, FILE *ckpt,
                 uint64_t until) {
  hw_init(hw, rom->data, (uint32_t)rom->size);
  oracle_init(o, hw);
  o->entry_pc = rom->header.entry_pc;
  o->checkpoints = ckpt;
  o->checkpoint_stride = 97;
  boot_pif_hle(o);
  oracle_run(o, until);
}

static int same_stream(const Z64Rom *rom, HwState *hw, Oracle *o) {
  FILE *a = tmpfile(), *b = tmpfile();
  boot(rom, hw, o, a, 20000000);
  oracle_free(o);
  hw_free(hw);
  boot(rom, hw, o, b, 20000000);
  oracle_free(o);
  hw_free(hw);
  long na = ftell(a), nb = ftell(b);
  int same = na == nb && na > 0;
  rewind(a);
  rewind(b);
  for (int ca, cb; same && (ca = fgetc(a)) != EOF;) {
    cb = fgetc(b);
    same = ca == cb;
  }
  fclose(a);
  fclose(b);
  printf("checkpoint stream: %ld bytes, identical=%d\n", na, same);
  return same;
}

int main(void) {
  Z64Rom rom;
  if (!rom_env_open(&rom))
    return 1;
  HwState *hw = malloc(sizeof(HwState));
  Oracle *o = malloc(sizeof(Oracle));

  CHECK(same_stream(&rom, hw, o));

  boot(&rom, hw, o, NULL, 450000000);
  /* (a) IPL3 hand-off. */
  CHECK(hw->dma_count > 0);
  CHECK(hw->dma_log[0].rom_offset == 0x1000 &&
        hw->dma_log[0].ram_addr == 0x246000 &&
        hw->dma_log[0].length == 0x100000);
  CHECK(o->entry_icount > 0);
  /* (b) Threads dispatched, interrupts serviced. */
  CHECK(o->first_eret_icount > o->entry_icount);
  CHECK(o->interrupts > 100);
  /* (c) First graphics task. */
  uint32_t first = UINT32_MAX;
  for (uint32_t i = 0; i < hw->task_count && first == UINT32_MAX; i++)
    if (hw->task_log[i].task[OSTASK_TYPE] == OSTASK_M_GFXTASK)
      first = i;
  CHECK(first != UINT32_MAX);
  if (first != UINT32_MAX) {
    const HwTaskRecord *t = &hw->task_log[first];
    uint32_t ptr = t->task[OSTASK_DATA_PTR] & 0x1FFFFFFFu;
    CHECK(ptr < HW_RDRAM_SIZE &&
          ptr + t->task[OSTASK_DATA_SIZE] <= HW_RDRAM_SIZE);
    CHECK(t->task[OSTASK_DATA_SIZE] > 0);
    CHECK(t->data_hash != 0 && t->data_hash != HW_HASH_INIT);
    printf("entry at icount %llu, first eret %llu, first gfx task at %llu "
           "(data %08X, %u bytes, hash %016llX)\n",
           (unsigned long long)o->entry_icount,
           (unsigned long long)o->first_eret_icount,
           (unsigned long long)t->icount, t->task[OSTASK_DATA_PTR],
           t->task[OSTASK_DATA_SIZE], (unsigned long long)t->data_hash);
  }

  /* Exit criterion over [450M, 550M): 128 VI fields = 64 frames at 30 fps. */
  uint32_t tasks0 = hw->task_count;
  uint64_t si0 = hw->si_reads, vi0 = hw->vi_count;
  oracle_run(o, 550000000);
  uint32_t gfx = count_gfx(hw, tasks0);
  uint64_t si = hw->si_reads - si0, vi = hw->vi_count - vi0;
  printf("window: %llu VI fields, %u gfx tasks, %llu SI reads\n",
         (unsigned long long)vi, gfx, (unsigned long long)si);
  CHECK(vi == 128);
  CHECK(gfx >= vi / 2 - 2); /* a frame per two fields */
  CHECK(si >= vi / 2 - 2);  /* controllers polled every frame */

  /* Only the exceptions a healthy boot takes. */
  for (int i = 1; i < 32; i++)
    if (o->exceptions[i] && i != EXC_CPU) {
      fprintf(stderr, "unexpected exception code %d x%llu\n", i,
              (unsigned long long)o->exceptions[i]);
      failures++;
    }
  CHECK(o->tlb_exceptions == 0);
  CHECK(hw->unmapped_accesses == 0);

  oracle_free(o);
  hw_free(hw);
  free(o);
  free(hw);
  z64_close(&rom);
  printf("%d failures\n", failures);
  return failures ? 1 : 0;
}
