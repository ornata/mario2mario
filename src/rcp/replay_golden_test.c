/* Replays a committed .rec under the oracle with the GL-free Fast3D HLE
 * and requires the sequence of graphics-task display-list hashes to
 * match a committed golden file exactly.
 *
 *   replay_golden_test REC GOLDEN FRAMES [--twice]
 *
 * --twice runs the replay a second time from reset and requires an
 * identical sequence (determinism of replay itself). The ROM comes from
 * $M2M_ROM. Regenerate goldens with:
 *   bazelisk run //oracle:run -- --replay REC --task-hashes OUT \
 *     --max-insns N && head -FRAMES OUT > GOLDEN */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/hw/hw.h"
#include "src/oracle/boot.h"
#include "src/oracle/cpu.h"
#include "src/rcp/gfx.h"
#include "src/rcp/input.h"
#include "src/tools/rom_env.h"
#include "src/tools/z64.h"

typedef struct {
  Gfx gfx;
  InputState input;
  uint64_t *hashes;
  uint32_t n, cap;
} Ctx;

static void on_task(void *user, HwState *hw, HwTaskRecord *r) {
  Ctx *c = user;
  if (r->task[OSTASK_TYPE] != OSTASK_M_GFXTASK || c->n == c->cap)
    return;
  gfx_run_task(&c->gfx, hw->rdram, r->task[OSTASK_DATA_PTR] & 0x1FFFFFFFu);
  r->dl_hash = c->gfx.dl_hash;
  c->hashes[c->n++] = r->dl_hash;
}

static void on_pad(void *user, HwState *hw, unsigned port) {
  input_pad_hook(&((Ctx *)user)->input, hw, port);
}

/* Boots and replays until `frames` graphics tasks have been hashed. */
static uint64_t *replay(const Z64Rom *rom, const char *rec_path,
                        uint32_t frames) {
  Rec rec;
  if (!rec_load(&rec, rec_path)) {
    fprintf(stderr, "%s: not a .rec file\n", rec_path);
    exit(1);
  }
  HwState *hw = malloc(sizeof(HwState));
  Oracle *o = malloc(sizeof(Oracle));
  Ctx *c = calloc(1, sizeof(Ctx));
  gfx_init(&c->gfx);
  c->input.replay = &rec;
  c->cap = frames;
  c->hashes = calloc(frames, sizeof(uint64_t));
  hw_init(hw, rom->data, (uint32_t)rom->size);
  hw->task_hook = on_task;
  hw->pad_hook = on_pad;
  hw->hook_user = c;
  oracle_init(o, hw);
  o->entry_pc = rom->header.entry_pc;
  boot_pif_hle(o);
  /* At most ~4 VI fields per frame even in heavy scenes; bail out well
   * past that so a regression cannot hang the test. */
  uint64_t limit = (uint64_t)frames * 8u * HW_VI_PERIOD + 400000000ull;
  while (c->n < frames && o->cpu.icount < limit)
    oracle_run(o, o->cpu.icount + HW_VI_PERIOD);
  uint64_t *out = c->hashes;
  if (c->n < frames) {
    fprintf(stderr, "only %u of %u graphics tasks before icount %llu\n", c->n,
            frames, (unsigned long long)o->cpu.icount);
    exit(1);
  }
  gfx_free(&c->gfx);
  oracle_free(o);
  hw_free(hw);
  rec_free(&rec);
  free(c);
  free(o);
  free(hw);
  return out;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s REC GOLDEN FRAMES [--twice]\n", argv[0]);
    return 2;
  }
  uint32_t frames = (uint32_t)strtoul(argv[3], NULL, 0);
  int twice = argc > 4 && !strcmp(argv[4], "--twice");
  Z64Rom rom;
  if (!rom_env_open(&rom))
    return 1;

  FILE *g = fopen(argv[2], "r");
  if (!g) {
    perror(argv[2]);
    return 1;
  }
  uint64_t *golden = calloc(frames, sizeof(uint64_t));
  uint32_t ng = 0;
  char line[64];
  while (ng < frames && fgets(line, sizeof(line), g))
    golden[ng++] = strtoull(line, NULL, 16);
  fclose(g);
  if (ng < frames) {
    fprintf(stderr, "golden has %u hashes, need %u\n", ng, frames);
    return 1;
  }

  uint64_t *got = replay(&rom, argv[1], frames);
  int failures = 0;
  for (uint32_t i = 0; i < frames; i++)
    if (got[i] != golden[i]) {
      fprintf(stderr, "frame %u: hash %016llX, golden %016llX\n", i,
              (unsigned long long)got[i], (unsigned long long)golden[i]);
      failures++;
      break; /* later frames diverge too */
    }
  if (twice) {
    uint64_t *again = replay(&rom, argv[1], frames);
    if (memcmp(again, got, sizeof(uint64_t) * frames) != 0) {
      fprintf(stderr, "second replay diverged\n");
      failures++;
    }
    free(again);
  }
  uint32_t unique = 0;
  for (uint32_t i = 0; i < frames; i++)
    unique += i == 0 || got[i] != got[i - 1];
  printf("%u frames matched golden (%u hash changes)%s; %d failures\n", frames,
         unique, twice ? ", replayed twice identically" : "", failures);
  free(got);
  free(golden);
  z64_close(&rom);
  return failures ? 1 : 0;
}
