/* Display-list HLE on hand-built F3D command lists (no GL, no ROM).
 * Encodings follow the F3D macro layouts; expected geometry is worked
 * out by hand. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/hw/hw.h"
#include "src/rcp/gfx.h"
#include "src/rcp/texture.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static uint8_t *ram;

static void put16(uint32_t a, uint16_t v) {
  ram[a] = (uint8_t)(v >> 8);
  ram[a + 1] = (uint8_t)v;
}

static uint32_t cmd(uint32_t pc, uint32_t w0, uint32_t w1) {
  hw_put_be32(ram + pc, w0);
  hw_put_be32(ram + pc + 4, w1);
  return pc + 8;
}

/* Identity Mtx: integer parts 1 on the diagonal, fractions 0. */
static void put_identity(uint32_t a) {
  memset(ram + a, 0, 64);
  for (int i = 0; i < 4; i++)
    put16(a + 2u * (uint32_t)(i * 4 + i), 1);
}

static void put_vtx(uint32_t a, int16_t x, int16_t y, int16_t z,
                    uint32_t rgba) {
  memset(ram + a, 0, 16);
  put16(a, (uint16_t)x);
  put16(a + 2, (uint16_t)y);
  put16(a + 4, (uint16_t)z);
  hw_put_be32(ram + a + 12, rgba);
}

static int near(float a, float b) {
  return fabsf(a - b) < 1e-4f;
}

static void test_triangle(Gfx *g) {
  memset(ram, 0, HW_RDRAM_SIZE);
  put_identity(0x1000);
  put_vtx(0x2000, 0, 0, 0, 0xFF0000FFu);
  put_vtx(0x2010, 1, 0, 0, 0x00FF00FFu);
  put_vtx(0x2020, 0, 1, 0, 0x0000FFFFu);
  uint32_t pc = 0x100;
  /* segment 1 -> 0x2000 via G_MOVEWORD(G_MW_SEGMENT, offset 4) */
  pc = cmd(pc, 0xBC000406u, 0x00002000u);
  /* G_MTX projection|load, then modelview|load */
  pc = cmd(pc, 0x01030040u, 0x00001000u);
  pc = cmd(pc, 0x01020040u, 0x00001000u);
  /* G_VTX: 3 vertices ((n-1) << 4 | v0 = 0x20) from segment 1 */
  pc = cmd(pc, 0x04200030u, 0x01000000u);
  /* G_TRI1 v0, v1, v2 (indices * 10) */
  pc = cmd(pc, 0xBF000000u, 0x00000A14u);
  pc = cmd(pc, 0xB8000000u, 0);
  gfx_run_task(g, ram, 0x100);
  CHECK(g->ndraws == 1 && g->nverts == 3);
  CHECK(g->unknown == 0);
  /* Default viewport covers 320x240, so clip coordinates pass through. */
  CHECK(near(g->verts[1].x, 1.0f) && near(g->verts[1].y, 0.0f));
  CHECK(near(g->verts[2].y, 1.0f) && near(g->verts[2].w, 1.0f));
  CHECK(near(g->verts[0].r, 1.0f) && near(g->verts[1].g, 1.0f) &&
        near(g->verts[2].b, 1.0f));
  uint64_t h1 = g->dl_hash;
  gfx_run_task(g, ram, 0x100);
  CHECK(g->dl_hash == h1);
  ram[0x2000 + 13] = 0x80; /* change a vertex colour: hash must change */
  gfx_run_task(g, ram, 0x100);
  CHECK(g->dl_hash != h1);
}

static void test_nested_and_cull(Gfx *g) {
  memset(ram, 0, HW_RDRAM_SIZE);
  put_identity(0x1000);
  put_vtx(0x2000, 0, 0, 0, 0xFFFFFFFFu);
  put_vtx(0x2010, 1, 0, 0, 0xFFFFFFFFu);
  put_vtx(0x2020, 0, 1, 0, 0xFFFFFFFFu);
  uint32_t sub = 0x400;
  sub = cmd(sub, 0x04200030u, 0x00002000u);
  sub = cmd(sub, 0xBF000000u, 0x00000A14u); /* counter-clockwise on screen */
  sub = cmd(sub, 0xBF000000u, 0x0000140Au); /* clockwise */
  sub = cmd(sub, 0xB8000000u, 0);
  uint32_t pc = 0x100;
  pc = cmd(pc, 0x01030040u, 0x00001000u);
  pc = cmd(pc, 0x01020040u, 0x00001000u);
  pc = cmd(pc, 0xB7000000u, 0x00002000u); /* G_CULL_BACK */
  pc = cmd(pc, 0x06000000u, 0x00000400u); /* G_DL push */
  pc = cmd(pc, 0xB6000000u, 0x00002000u); /* clear culling */
  pc = cmd(pc, 0x06000000u, 0x00000400u);
  pc = cmd(pc, 0xB8000000u, 0);
  gfx_run_task(g, ram, 0x100);
  /* First call keeps one of the two triangles, second keeps both. */
  CHECK(g->nverts == 9);
}

static void test_texture_decode(void) {
  uint8_t src[8] = {0xF8, 0x01, 0x07, 0xC1, 0x12, 0x34, 0x00, 0x3E};
  uint8_t out[16];
  memset(ram, 0, HW_RDRAM_SIZE);
  memcpy(ram + 0x3000, src, 8);
  /* RGBA16: F801 = red, opaque; 07C1 = green; 0000 = transparent; 003E =
   * blue, transparent (alpha bit clear). */
  tex_decode(out, ram, HW_RDRAM_SIZE, 0x3000, 8, 4, 1, TX_RGBA, TX_16B, NULL, 0,
             0);
  CHECK(out[0] == 255 && out[1] == 0 && out[2] == 0 && out[3] == 255);
  CHECK(out[4] == 0 && out[5] == 255 && out[6] == 0 && out[7] == 255);
  CHECK(out[14] == 255 && out[15] == 0); /* 0x1F -> 0xF8 | 0x07 */
  /* IA8 0x12: I = 1*17, A = 2*17. */
  tex_decode(out, ram, HW_RDRAM_SIZE, 0x3004, 1, 1, 1, TX_IA, TX_8B, NULL, 0,
             0);
  CHECK(out[0] == 17 && out[3] == 34);
  /* CI4 via a 16-entry palette bank 1. */
  uint16_t tlut[256] = {0};
  tlut[16 + 1] = 0xF801;
  tlut[16 + 2] = 0x07C1;
  ram[0x3100] = 0x12;
  tex_decode(out, ram, HW_RDRAM_SIZE, 0x3100, 1, 2, 1, TX_CI, TX_4B, tlut, 0,
             1);
  CHECK(out[0] == 255 && out[4] == 0 && out[5] == 255);
}

int main(void) {
  ram = calloc(HW_RDRAM_SIZE, 1);
  Gfx g;
  gfx_init(&g);
  test_triangle(&g);
  test_nested_and_cull(&g);
  test_texture_decode();
  gfx_free(&g);
  free(ram);
  printf("%d failures\n", failures);
  return failures ? 1 : 0;
}
