/* //oracle:play and //native:play -- SM64 in a real window, under the
 * oracle interpreter or (built with M2M_NATIVE_ENGINE) the native
 * runtime running translated code. Everything else is shared.
 *
 *   play [--rom PATH] [--scale N] [--record F.rec] [--replay F.rec]
 *        [--headless] [--frames N] [--png OUT.png]
 *        [--png-every K --png-prefix PATH] [--task-hashes F.txt]
 *        [--fields N]
 *
 * Interactive: one VI field is emulated per 1/60 s of wall clock and the
 * window shows each graphics task as it is submitted. Keys:
 *   arrows / WASD  analog stick (ramped)   X  A      Z  B
 *   Space / Shift  Z trigger               Enter  Start
 *   I J K L        C-up/left/down/right    Q  L      E  R
 *   Esc            quit
 * --headless renders into a hidden window without pacing and stops after
 * --frames graphics tasks, writing the last frame to --png (320x240 x
 * scale). --png-every K dumps every K-th frame to PREFIX_NNNNN.png.
 * --fields N quits after N emulated VI fields (N/60 s when paced). */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/hw/hw.h"
#include "src/oracle/boot.h"
#include "src/oracle/cpu.h"
#ifdef M2M_NATIVE_ENGINE
#include "src/native/native.h"
#endif
#include "src/rcp/gfx.h"
#include "src/rcp/gl.h"
#include "src/rcp/input.h"
#include "src/rcp/png.h"
#include "src/tools/z64.h"

typedef struct {
  HwState *hw;
#ifdef M2M_NATIVE_ENGINE
  NativeState *ns;
#else
  Oracle *o;
#endif
  Gfx gfx;
  GlRenderer gl;
  SDL_Window *win;
  InputState input;
  int headless;
  uint64_t frames, frame_limit, png_every;
  const char *png_prefix;
  FILE *task_hashes;
  float stick_x, stick_y;
} App;

static const char *resolve(const char *path, char *buf, size_t cap) {
  const char *wd = getenv("BUILD_WORKING_DIRECTORY");
  if (!path || path[0] == '/' || !wd || !*wd)
    return path;
  snprintf(buf, cap, "%s/%s", wd, path);
  return buf;
}

static void dump_png(App *a, const char *path) {
  uint8_t *px = malloc((size_t)a->gl.width * a->gl.height * 4);
  gl_read_rgba(&a->gl, px);
  if (!png_write(path, px, (uint32_t)a->gl.width, (uint32_t)a->gl.height))
    fprintf(stderr, "cannot write %s\n", path);
  free(px);
}

static void on_task(void *user, HwState *hw, HwTaskRecord *r) {
  App *a = user;
  if (r->task[OSTASK_TYPE] != OSTASK_M_GFXTASK)
    return;
  gfx_run_task(&a->gfx, hw->rdram, r->task[OSTASK_DATA_PTR] & 0x1FFFFFFFu);
  r->dl_hash = a->gfx.dl_hash;
  if (a->task_hashes)
    fprintf(a->task_hashes, "%016llX\n", (unsigned long long)r->dl_hash);
  gl_render(&a->gl, &a->gfx);
  a->frames++;
  if (a->png_every && a->frames % a->png_every == 0) {
    char path[4096];
    snprintf(path, sizeof(path), "%s_%05llu.png", a->png_prefix,
             (unsigned long long)a->frames);
    dump_png(a, path);
  }
  if (!a->headless)
    SDL_GL_SwapWindow(a->win);
}

/* Keyboard -> pad, with the stick ramping toward its target. */
static void read_keyboard(App *a) {
  const Uint8 *k = SDL_GetKeyboardState(NULL);
  uint16_t b = 0;
  if (k[SDL_SCANCODE_X])
    b |= BTN_A;
  if (k[SDL_SCANCODE_Z])
    b |= BTN_B;
  if (k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_LSHIFT])
    b |= BTN_Z;
  if (k[SDL_SCANCODE_RETURN])
    b |= BTN_START;
  if (k[SDL_SCANCODE_I])
    b |= BTN_CU;
  if (k[SDL_SCANCODE_K])
    b |= BTN_CD;
  if (k[SDL_SCANCODE_J])
    b |= BTN_CL;
  if (k[SDL_SCANCODE_L])
    b |= BTN_CR;
  if (k[SDL_SCANCODE_Q])
    b |= BTN_L;
  if (k[SDL_SCANCODE_E])
    b |= BTN_R;
  float tx = 0, ty = 0;
  if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A])
    tx -= 1;
  if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D])
    tx += 1;
  if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W])
    ty += 1;
  if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S])
    ty -= 1;
  if (tx && ty) {
    tx *= 0.7071f;
    ty *= 0.7071f;
  }
  tx *= 80;
  ty *= 80;
  const float ramp = 20; /* stick units per VI field */
  a->stick_x += tx > a->stick_x + ramp   ? ramp
                : tx < a->stick_x - ramp ? -ramp
                                         : tx - a->stick_x;
  a->stick_y += ty > a->stick_y + ramp   ? ramp
                : ty < a->stick_y - ramp ? -ramp
                                         : ty - a->stick_y;
  a->input.live = (RecFrame){b, (int8_t)a->stick_x, (int8_t)a->stick_y};
}

static void on_pad(void *user, HwState *hw, unsigned port) {
  App *a = user;
  input_pad_hook(&a->input, hw, port);
}

int main(int argc, char **argv) {
  const char *rom_path = getenv("M2M_ROM"), *record = NULL, *replay = NULL,
             *png = NULL, *png_prefix = "frame", *hashes = NULL;
  int scale = 2, headless = 0;
  unsigned long long frames = 0, png_every = 0, max_fields = 0;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(arg, "--headless")) {
      headless = 1;
      continue;
    }
    if (!v) {
      fprintf(stderr, "missing value for %s\n", arg);
      return 2;
    }
    if (!strcmp(arg, "--rom"))
      rom_path = v;
    else if (!strcmp(arg, "--scale"))
      scale = atoi(v);
    else if (!strcmp(arg, "--record"))
      record = v;
    else if (!strcmp(arg, "--replay"))
      replay = v;
    else if (!strcmp(arg, "--frames"))
      frames = strtoull(v, NULL, 0);
    else if (!strcmp(arg, "--png"))
      png = v;
    else if (!strcmp(arg, "--png-every"))
      png_every = strtoull(v, NULL, 0);
    else if (!strcmp(arg, "--png-prefix"))
      png_prefix = v;
    else if (!strcmp(arg, "--task-hashes"))
      hashes = v;
    else if (!strcmp(arg, "--fields"))
      max_fields = strtoull(v, NULL, 0);
    else {
      fprintf(stderr, "unknown option %s\n", arg);
      return 2;
    }
    i++;
  }
  if (scale < 1 || scale > 8)
    scale = 2;
  char b1[4096], b2[4096], b3[4096], b4[4096], b5[4096], b6[4096];
  rom_path = resolve(rom_path, b1, sizeof(b1));
  record = resolve(record, b2, sizeof(b2));
  replay = resolve(replay, b3, sizeof(b3));
  png = resolve(png, b4, sizeof(b4));
  png_prefix = resolve(png_prefix, b5, sizeof(b5));
  hashes = resolve(hashes, b6, sizeof(b6));
  if (!rom_path) {
    fprintf(stderr, "no ROM: pass --rom or set M2M_ROM\n");
    return 2;
  }
  if (headless && !frames) {
    fprintf(stderr, "--headless needs --frames N\n");
    return 2;
  }

  Z64Rom rom;
  Z64Status st = z64_open(rom_path, &rom);
  if (st != Z64_OK) {
    fprintf(stderr, "%s: %s\n", rom_path, z64_status_str(st));
    return 1;
  }

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  App *a = calloc(1, sizeof(App));
  a->headless = headless;
  a->frame_limit = frames;
  a->png_every = png_every;
  a->png_prefix = png_prefix;
  int w = 320 * scale, h = 240 * scale;
  a->win = SDL_CreateWindow(

#ifdef M2M_NATIVE_ENGINE
      "mario2mario (native)"
#else
      "mario2mario (oracle)"
#endif
      ,
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
      SDL_WINDOW_OPENGL | (headless ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN));
  if (!a->win || !SDL_GL_CreateContext(a->win)) {
    fprintf(stderr, "window/GL: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetSwapInterval(0);
  int dw, dh;
  SDL_GL_GetDrawableSize(a->win, &dw, &dh);
  if (!gl_init(&a->gl, dw, dh))
    return 1;
  gfx_init(&a->gfx);

  Rec rec_in = {0}, rec_out = {0};
  if (replay) {
    if (!rec_load(&rec_in, replay)) {
      fprintf(stderr, "%s: not a .rec file\n", replay);
      return 1;
    }
    a->input.replay = &rec_in;
  }
  if (record)
    a->input.record = &rec_out;
  if (hashes && !(a->task_hashes = fopen(hashes, "w"))) {
    perror(hashes);
    return 1;
  }

  a->hw = malloc(sizeof(HwState));
  hw_init(a->hw, rom.data, (uint32_t)rom.size);
  a->hw->task_hook = on_task;
  a->hw->pad_hook = on_pad;
  a->hw->hook_user = a;
#ifdef M2M_NATIVE_ENGINE
  a->ns = malloc(sizeof(NativeState));
  native_init(a->ns, a->hw);
  a->ns->trap_log = "native_traps.txt";
  CpuState *cpu = &a->ns->cpu;
#else
  a->o = malloc(sizeof(Oracle));
  oracle_init(a->o, a->hw);
  a->o->entry_pc = rom.header.entry_pc;
  CpuState *cpu = &a->o->cpu;
#endif
  boot_pif_hle(cpu, a->hw);

  Uint64 t0 = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency();
  int quit = 0;
  while (!quit) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
      if (e.type == SDL_QUIT ||
          (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE))
        quit = 1;
    if (!headless)
      read_keyboard(a);
    uint64_t field = cpu->icount / HW_VI_PERIOD + 1;
#ifdef M2M_NATIVE_ENGINE
    a->ns->budget = field * HW_VI_PERIOD;
    if (native_run(a->ns) != NSTOP_BUDGET) {
      fprintf(stderr, "native stop at %08X: %s\n", a->ns->stop_pc,
              a->ns->stop_why ? a->ns->stop_why : "?");
      break;
    }
#else
    oracle_run(a->o, field * HW_VI_PERIOD);
#endif
    if (max_fields && field >= max_fields)
      break;
    if (headless) {
      if (a->frames >= a->frame_limit)
        break;
      continue;
    }
    /* Pace emulated VI fields to 60 per second of wall clock. */
    double due = (double)field / 60.0;
    double now = (double)(SDL_GetPerformanceCounter() - t0) / (double)hz;
    if (due > now)
      SDL_Delay((Uint32)((due - now) * 1000.0));
  }

  if (png)
    dump_png(a, png);
  if (record && !rec_save(&rec_out, record))
    fprintf(stderr, "cannot write %s\n", record);
  double secs = (double)(SDL_GetPerformanceCounter() - t0) / (double)hz;
  printf("frames=%llu fields=%llu icount=%llu polls=%llu textures=%u "
         "wall=%.2fs\n",
         (unsigned long long)a->frames,
         (unsigned long long)(cpu->icount / HW_VI_PERIOD),
         (unsigned long long)cpu->icount, (unsigned long long)a->input.polls,
         a->gfx.ntextures, secs);
  if (a->task_hashes)
    fclose(a->task_hashes);
  gl_free(&a->gl);
  gfx_free(&a->gfx);
  rec_free(&rec_in);
  rec_free(&rec_out);
#ifdef M2M_NATIVE_ENGINE
  native_free(a->ns);
#else
  oracle_free(a->o);
#endif
  hw_free(a->hw);
  SDL_Quit();
  z64_close(&rom);
  return 0;
}
