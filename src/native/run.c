/* //native:run -- boot the ROM with translated code (headless).
 *
 *   run [--rom PATH] [--max-insns N] [--checkpoints F] [--checkpoint-stride N]
 *       [--checkpoint-from I]
 *       [--replay F.rec] [--task-hashes F.txt] [--trap-log F]
 *
 * Mirrors //oracle:run: same boot, hardware model, input replay and hash
 * outputs, so checkpoint streams and task-hash files are byte-comparable.
 * Exit status: 0 budget reached, 3 untranslated code reached (logged to
 * --trap-log, default out/native_traps.txt, git-ignored), 4 outside the
 * contract. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "src/hw/hw.h"
#include "src/native/native.h"
#include "src/oracle/boot.h"
#include "src/rcp/gfx.h"
#include "src/rcp/input.h"
#include "src/tools/z64.h"

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

static const char *resolve(const char *path, char *buf, size_t cap) {
  const char *wd = getenv("BUILD_WORKING_DIRECTORY");
  if (!path || path[0] == '/' || !wd || !*wd)
    return path;
  snprintf(buf, cap, "%s/%s", wd, path);
  return buf;
}

int main(int argc, char **argv) {
  const char *rom_path = getenv("M2M_ROM"), *ckpt = NULL, *replay = NULL,
             *hashes = NULL, *traps = "out/native_traps.txt";
  unsigned long long max_insns = 100000000ull, stride = 1, ckpt_from = 0;
  for (int i = 1; i + 1 < argc; i += 2) {
    const char *a = argv[i], *v = argv[i + 1];
    if (!strcmp(a, "--rom"))
      rom_path = v;
    else if (!strcmp(a, "--max-insns"))
      max_insns = strtoull(v, NULL, 0);
    else if (!strcmp(a, "--checkpoints"))
      ckpt = v;
    else if (!strcmp(a, "--checkpoint-stride"))
      stride = strtoull(v, NULL, 0);
    else if (!strcmp(a, "--checkpoint-from"))
      ckpt_from = strtoull(v, NULL, 0);
    else if (!strcmp(a, "--replay"))
      replay = v;
    else if (!strcmp(a, "--task-hashes"))
      hashes = v;
    else if (!strcmp(a, "--trap-log"))
      traps = v;
    else {
      fprintf(stderr, "unknown option %s\n", a);
      return 2;
    }
  }
  if (argc % 2 == 0) {
    fprintf(stderr, "options take values\n");
    return 2;
  }
  char b[5][4096];
  rom_path = resolve(rom_path, b[0], sizeof(b[0]));
  ckpt = resolve(ckpt, b[1], sizeof(b[1]));
  replay = resolve(replay, b[2], sizeof(b[2]));
  hashes = resolve(hashes, b[3], sizeof(b[3]));
  traps = resolve(traps, b[4], sizeof(b[4]));
  if (!strncmp(traps + strlen(traps) - strlen("out/native_traps.txt"),
               "out/native_traps.txt", strlen("out/native_traps.txt"))) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%.*s", (int)(strlen(traps) - 17), traps);
    mkdir(dir, 0755); /* default location: create out/ if needed */
  }
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

  NativeState *ns = malloc(sizeof(NativeState));
  native_init(ns, hw);
  ns->budget = max_insns;
  ns->trap_log = traps;
  if (ckpt) {
    if (!(ns->checkpoints = fopen(ckpt, "wb"))) {
      perror(ckpt);
      return 1;
    }
    ns->stride = stride ? stride : 1;
    ns->checkpoint_from = ckpt_from;
  }
  boot_pif_hle(&ns->cpu, hw);

  clock_t t0 = clock();
  NativeStop r = native_run(ns);
  double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
  printf("stop=%d icount=%llu pc=%08X (%.1f M insn/s) units=%u links=%llu "
         "unlinks=%llu\n",
         r, (unsigned long long)ns->cpu.icount, ns->cpu.pc,
         secs > 0 ? (double)ns->cpu.icount / secs / 1e6 : 0.0, m2m_unit_count,
         (unsigned long long)ns->links, (unsigned long long)ns->unlinks);
  if (r != NSTOP_BUDGET)
    printf("  stopped at %08X: %s\n", ns->stop_pc,
           ns->stop_why ? ns->stop_why : "?");
  printf("vi=%llu interrupts=%llu exceptions=%llu si_reads=%llu pi_dmas=%u "
         "rsp tasks=%u\n",
         (unsigned long long)hw->vi_count, (unsigned long long)ns->interrupts,
         (unsigned long long)ns->exceptions, (unsigned long long)hw->si_reads,
         hw->dma_count, hw->task_count);
  if (ns->checkpoints)
    fclose(ns->checkpoints);
  if (hooks->hashes)
    fclose(hooks->hashes);
  native_free(ns);
  gfx_free(&hooks->gfx);
  rec_free(&rec);
  hw_free(hw);
  z64_close(&rom);
  return r == NSTOP_BUDGET ? 0 : (int)r;
}
