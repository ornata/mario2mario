/* Lockstep: the native engine (LLM-translated units) against the oracle
 * over the first BUDGET instructions of gameplay.rec (argv[2]). Requires
 * byte-identical checkpoint streams (stride 1000, src/oracle/checkpoint.h)
 * and identical graphics-task display-list hash sequences. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/hw/hw.h"
#include "src/native/native.h"
#include "src/oracle/boot.h"
#include "src/oracle/cpu.h"
#include "src/rcp/gfx.h"
#include "src/rcp/input.h"
#include "src/tools/rom_env.h"
#include "src/tools/z64.h"

#define STRIDE    1000u
#define MAX_TASKS 16384u

typedef struct {
  Gfx gfx;
  InputState input;
  uint64_t hashes[MAX_TASKS];
  uint32_t n;
} Hooks;

static void on_task(void *user, HwState *hw, HwTaskRecord *r) {
  Hooks *h = user;
  if (r->task[OSTASK_TYPE] != OSTASK_M_GFXTASK || h->n == MAX_TASKS)
    return;
  gfx_run_task(&h->gfx, hw->rdram, r->task[OSTASK_DATA_PTR] & 0x1FFFFFFFu);
  h->hashes[h->n++] = h->gfx.dl_hash;
}

static void on_pad(void *user, HwState *hw, unsigned port) {
  input_pad_hook(&((Hooks *)user)->input, hw, port);
}

static HwState *machine(const Z64Rom *rom, Hooks *h, Rec *rec) {
  HwState *hw = malloc(sizeof(HwState));
  hw_init(hw, rom->data, (uint32_t)rom->size);
  gfx_init(&h->gfx);
  h->input.replay = rec;
  hw->task_hook = on_task;
  hw->pad_hook = on_pad;
  hw->hook_user = h;
  return hw;
}

static char *slurp(FILE *f, long *n) {
  *n = ftell(f);
  rewind(f);
  char *buf = malloc((size_t)*n + 1);
  *n = (long)fread(buf, 1, (size_t)*n, f);
  return buf;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s gameplay.rec budget\n", argv[0]);
    return 2;
  }
  const uint64_t BUDGET = strtoull(argv[2], NULL, 0);
  Z64Rom rom;
  if (!rom_env_open(&rom))
    return 1;
  Rec rec_o, rec_n;
  if (!rec_load(&rec_o, argv[1]) || !rec_load(&rec_n, argv[1])) {
    fprintf(stderr, "%s: not a .rec file\n", argv[1]);
    return 1;
  }

  Hooks *ho = calloc(1, sizeof(Hooks)), *hn = calloc(1, sizeof(Hooks));
  FILE *fo = tmpfile(), *fn = tmpfile();

  HwState *hwo = machine(&rom, ho, &rec_o);
  Oracle *o = malloc(sizeof(Oracle));
  oracle_init(o, hwo);
  o->checkpoints = fo;
  o->checkpoint_stride = STRIDE;
  boot_pif_hle(&o->cpu, hwo);
  oracle_run(o, BUDGET);

  HwState *hwn = machine(&rom, hn, &rec_n);
  NativeState *ns = malloc(sizeof(NativeState));
  native_init(ns, hwn);
  ns->checkpoints = fn;
  ns->stride = STRIDE;
  ns->budget = BUDGET;
  boot_pif_hle(&ns->cpu, hwn);
  NativeStop stop = native_run(ns);

  long na, nn;
  char *a = slurp(fo, &na), *b = slurp(fn, &nn);
  int failures = 0;
  if (stop != NSTOP_BUDGET) {
    fprintf(stderr, "native stopped early at %08X: %s\n", ns->stop_pc,
            ns->stop_why ? ns->stop_why : "?");
    failures++;
  }
  if (na != nn || memcmp(a, b, (size_t)na) != 0) {
    long i = 0;
    while (i < na && i < nn && a[i] == b[i])
      i++;
    fprintf(stderr,
            "checkpoint streams differ at record %ld (%ld vs %ld "
            "bytes)\n",
            i / 24, na, nn);
    failures++;
  }
  if (ho->n != hn->n ||
      memcmp(ho->hashes, hn->hashes, sizeof(uint64_t) * ho->n) != 0) {
    fprintf(stderr, "graphics-task hashes differ (%u vs %u tasks)\n", ho->n,
            hn->n);
    failures++;
  }
  printf("%llu instructions: %ld checkpoint records, %u graphics tasks, "
         "%u units; %d failures\n",
         (unsigned long long)BUDGET, na / 24, ho->n, m2m_unit_count, failures);
  return failures ? 1 : 0;
}
