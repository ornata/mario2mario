/* //oracle:run -- boot the ROM under the oracle interpreter.
 *
 *   run [--rom PATH] [--max-insns N] [--trace PATH]
 *       [--checkpoints PATH] [--checkpoint-stride N] [--history N]
 *       [--replay F.rec] [--task-hashes F.txt]
 *
 * --replay feeds controller polls from a .rec recording (src/rcp/input.h).
 * --task-hashes walks every graphics task's display list with the
 * GL-free Fast3D HLE and writes one hex dl_hash per line.
 * --history N prints the last N (<= 256) executed PCs at the end.
 * --rom defaults to $M2M_ROM. Relative paths are resolved against the
 * directory bazel was invoked from. Prints a summary of milestones. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/hw/hw.h"
#include "src/oracle/boot.h"
#include "src/oracle/cpu.h"
#include "src/oracle/trace.h"
#include "src/rcp/gfx.h"
#include "src/rcp/input.h"
#include "src/tools/z64.h"

static const char *resolve(const char *path, char *buf, size_t cap) {
  const char *wd = getenv("BUILD_WORKING_DIRECTORY");
  if (!path || path[0] == '/' || !wd || !*wd)
    return path;
  snprintf(buf, cap, "%s/%s", wd, path);
  return buf;
}

typedef struct {
  Gfx gfx;
  InputState input;
  FILE *hashes;
} Hooks;

static void on_task(void *user, HwState *hw, HwTaskRecord *r) {
  Hooks *h = user;
  if (!h->hashes || r->task[OSTASK_TYPE] != OSTASK_M_GFXTASK)
    return;
  gfx_run_task(&h->gfx, hw->rdram, r->task[OSTASK_DATA_PTR] & 0x1FFFFFFFu);
  r->dl_hash = h->gfx.dl_hash;
  fprintf(h->hashes, "%016llX\n", (unsigned long long)r->dl_hash);
}

static void on_pad(void *user, HwState *hw, unsigned port) {
  Hooks *h = user;
  if (h->input.replay)
    input_pad_hook(&h->input, hw, port);
}

static const char *const stop_names[] = {"none", "budget", "exception"};

int main(int argc, char **argv) {
  const char *rom_path = getenv("M2M_ROM"), *trace_path = NULL,
             *ckpt_path = NULL, *replay = NULL, *hashes = NULL;
  unsigned long long max_insns = 100000000ull, stride = 1;
  unsigned history = 0;
  for (int i = 1; i < argc; i += 2) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!v) {
      fprintf(stderr, "missing value for %s\n", a);
      return 2;
    }
    if (!strcmp(a, "--rom"))
      rom_path = v;
    else if (!strcmp(a, "--max-insns"))
      max_insns = strtoull(v, NULL, 0);
    else if (!strcmp(a, "--trace"))
      trace_path = v;
    else if (!strcmp(a, "--checkpoints"))
      ckpt_path = v;
    else if (!strcmp(a, "--checkpoint-stride"))
      stride = strtoull(v, NULL, 0);
    else if (!strcmp(a, "--replay"))
      replay = v;
    else if (!strcmp(a, "--task-hashes"))
      hashes = v;
    else if (!strcmp(a, "--history"))
      history = (unsigned)strtoul(v, NULL, 0);
    else {
      fprintf(stderr,
              "usage: %s [--rom PATH] [--max-insns N] [--trace PATH] "
              "[--checkpoints PATH] [--checkpoint-stride N] [--history N] "
              "[--replay F.rec] [--task-hashes F.txt]\n",
              argv[0]);
      return 2;
    }
  }
  char b1[4096], b2[4096], b3[4096], b4[4096], b5[4096];
  replay = resolve(replay, b4, sizeof(b4));
  hashes = resolve(hashes, b5, sizeof(b5));
  rom_path = resolve(rom_path, b1, sizeof(b1));
  trace_path = resolve(trace_path, b2, sizeof(b2));
  ckpt_path = resolve(ckpt_path, b3, sizeof(b3));
  if (!rom_path) {
    fprintf(stderr, "no ROM: pass --rom or set M2M_ROM\n");
    return 2;
  }

  Z64Rom rom;
  Z64Status st = z64_open(rom_path, &rom);
  if (st != Z64_OK) {
    fprintf(stderr, "%s: %s\n", rom_path, z64_status_str(st));
    return 1;
  }
  HwState *hw = malloc(sizeof(HwState));
  hw_init(hw, rom.data, (uint32_t)rom.size);
  Oracle *o = malloc(sizeof(Oracle));
  oracle_init(o, hw);
  o->entry_pc = rom.header.entry_pc;
  if (ckpt_path) {
    o->checkpoints = fopen(ckpt_path, "wb");
    if (!o->checkpoints) {
      perror(ckpt_path);
      return 1;
    }
    o->checkpoint_stride = stride ? stride : 1;
  }

  Hooks *hooks = calloc(1, sizeof(Hooks));
  Rec rec = {0};
  gfx_init(&hooks->gfx);
  if (replay) {
    if (!rec_load(&rec, replay)) {
      fprintf(stderr, "%s: not a .rec file\n", replay);
      return 1;
    }
    hooks->input.replay = &rec;
  }
  if (hashes && !(hooks->hashes = fopen(hashes, "w"))) {
    perror(hashes);
    return 1;
  }
  hw->task_hook = on_task;
  hw->pad_hook = on_pad;
  hw->hook_user = hooks;

  boot_pif_hle(o);
  clock_t t0 = clock();
  StopReason r = oracle_run(o, max_insns);
  double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;

  const CpuState *c = &o->cpu;
  uint32_t gfx = 0, aud = 0, first_gfx = UINT32_MAX;
  for (uint32_t i = 0; i < hw->task_count; i++) {
    if (hw->task_log[i].task[OSTASK_TYPE] == OSTASK_M_GFXTASK) {
      if (first_gfx == UINT32_MAX)
        first_gfx = i;
      gfx++;
    } else if (hw->task_log[i].task[OSTASK_TYPE] == OSTASK_M_AUDTASK) {
      aud++;
    }
  }
  printf("stop=%s icount=%llu pc=%08X (%.1f M insn/s)\n", stop_names[r],
         (unsigned long long)c->icount, c->pc,
         secs > 0 ? (double)c->icount / secs / 1e6 : 0.0);
  printf("entry %08X reached at icount %llu; first eret after entry at %llu\n",
         o->entry_pc, (unsigned long long)o->entry_icount,
         (unsigned long long)o->first_eret_icount);
  printf("vi=%llu interrupts=%llu erets=%llu si_reads=%llu si_writes=%llu "
         "pi_dmas=%u ai_buffers=%llu unmapped=%llu\n",
         (unsigned long long)hw->vi_count, (unsigned long long)o->interrupts,
         (unsigned long long)o->eret_count, (unsigned long long)hw->si_reads,
         (unsigned long long)hw->si_writes, hw->dma_count,
         (unsigned long long)hw->ai_buffers,
         (unsigned long long)hw->unmapped_accesses);
  printf("rsp tasks=%u (gfx=%u audio=%u)\n", hw->task_count, gfx, aud);
  if (first_gfx != UINT32_MAX) {
    const HwTaskRecord *t = &hw->task_log[first_gfx];
    printf(
        "first gfx task at icount %llu: data_ptr=%08X size=%X hash=%016llX\n",
        (unsigned long long)t->icount, t->task[OSTASK_DATA_PTR],
        t->task[OSTASK_DATA_SIZE], (unsigned long long)t->data_hash);
  }
  for (int i = 1; i < 32; i++)
    if (o->exceptions[i])
      printf("exception code %d taken %llu times\n", i,
             (unsigned long long)o->exceptions[i]);
  printf("tlb-mapped accesses=%llu tlb exceptions=%llu\n",
         (unsigned long long)o->tlb_mapped_accesses,
         (unsigned long long)o->tlb_exceptions);
  printf("executed RDRAM words: %u\n", trace_exec_words(o));
  if (history > 256)
    history = 256;
  for (unsigned i = 0; i < history; i++)
    printf("%s%08X",
           i % 8 ? " "
           : i   ? "\n  "
                 : "last PCs:\n  ",
           o->history[(o->history_pos - history + i) & 255u]);
  if (history)
    printf("\n");

  if (trace_path) {
    FILE *f = fopen(trace_path, "w");
    if (!f) {
      perror(trace_path);
      return 1;
    }
    trace_write(f, o);
    fclose(f);
  }
  if (o->checkpoints)
    fclose(o->checkpoints);
  if (hooks->hashes)
    fclose(hooks->hashes);
  gfx_free(&hooks->gfx);
  rec_free(&rec);
  free(hooks);
  oracle_free(o);
  hw_free(hw);
  free(o);
  free(hw);
  z64_close(&rom);
  return 0;
}
