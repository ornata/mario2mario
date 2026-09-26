#include "src/rcp/gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "src/hw/hw.h"
#include "src/rcp/texture.h"

#define RDRAM_MASK (HW_RDRAM_SIZE - 1u)

/* F3D opcodes (RSP). */
enum {
  G_SPNOOP = 0x00,
  G_MTX = 0x01,
  G_MOVEMEM = 0x03,
  G_VTX = 0x04,
  G_DL = 0x06,
  G_RDPHALF_CONT = 0xB2,
  G_RDPHALF_2 = 0xB3,
  G_RDPHALF_1 = 0xB4,
  G_CLEARGEOMETRYMODE = 0xB6,
  G_SETGEOMETRYMODE = 0xB7,
  G_ENDDL = 0xB8,
  G_SETOTHERMODE_L = 0xB9,
  G_SETOTHERMODE_H = 0xBA,
  G_TEXTURE = 0xBB,
  G_MOVEWORD = 0xBC,
  G_POPMTX = 0xBD,
  G_CULLDL = 0xBE,
  G_TRI1 = 0xBF,
};

/* RDP opcodes. */
enum {
  G_TEXRECT = 0xE4,
  G_TEXRECTFLIP = 0xE5,
  G_SETSCISSOR = 0xED,
  G_RDPSETOTHERMODE = 0xEF,
  G_LOADTLUT = 0xF0,
  G_SETTILESIZE = 0xF2,
  G_LOADBLOCK = 0xF3,
  G_LOADTILE = 0xF4,
  G_SETTILE = 0xF5,
  G_FILLRECT = 0xF6,
  G_SETFILLCOLOR = 0xF7,
  G_SETFOGCOLOR = 0xF8,
  G_SETBLENDCOLOR = 0xF9,
  G_SETPRIMCOLOR = 0xFA,
  G_SETENVCOLOR = 0xFB,
  G_SETCOMBINE = 0xFC,
  G_SETTIMG = 0xFD,
  G_SETZIMG = 0xFE,
  G_SETCIMG = 0xFF,
};

/* Geometry mode bits (F3D). */
#define G_ZBUFFER     0x00000001u
#define G_CULL_FRONT  0x00001000u
#define G_CULL_BACK   0x00002000u
#define G_LIGHTING    0x00020000u
#define G_TEXTURE_GEN 0x00040000u

/* Othermode fields. */
#define CYCLE_TYPE(h) (((h) >> 20) & 3u)
enum { CYC_1, CYC_2, CYC_COPY, CYC_FILL };
#define TEXTLUT(h)     (((h) >> 14) & 3u) /* 2: RGBA16, 3: IA16 */
#define TEXTFILT(h)    (((h) >> 12) & 3u) /* 0 point, 2 bilerp, 3 average */
#define RM_Z_CMP       0x0010u
#define RM_Z_UPD       0x0020u
#define RM_ZMODE_MASK  0x0C00u
#define RM_ZMODE_DEC   0x0C00u
#define RM_CVG_X_ALPHA 0x1000u
#define RM_FORCE_BL    0x4000u

enum { MAX_COMMANDS = 1u << 21, MAX_DEPTH = 18 };
#define TEXEL_BUDGET (48u << 20)

/* --- small helpers ------------------------------------------------------ */

static uint32_t rd32(const Gfx *g, uint32_t pa) {
  return hw_be32(g->rdram + (pa & RDRAM_MASK & ~3u));
}

static uint16_t rd16(const Gfx *g, uint32_t pa) {
  pa &= RDRAM_MASK & ~1u;
  return (uint16_t)(g->rdram[pa] << 8 | g->rdram[pa + 1]);
}

static uint32_t seg(const Gfx *g, uint32_t addr) {
  return (g->segments[(addr >> 24) & 15u] + (addr & 0x00FFFFFFu)) & RDRAM_MASK;
}

static void hash(Gfx *g, uint64_t w) {
  g->dl_hash = hw_hash_word(g->dl_hash, w);
}

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t elem) {
  if (need <= *cap)
    return p;
  uint32_t c = *cap ? *cap : 1024;
  while (c < need)
    c *= 2;
  *cap = c;
  return realloc(p, (size_t)c * elem);
}

static void color_from_rgba(float *o, uint32_t v) {
  for (int i = 0; i < 4; i++)
    o[i] = (float)((v >> (24 - 8 * i)) & 255u) / 255.0f;
}

static void mat_identity(float m[4][4]) {
  memset(m, 0, sizeof(float) * 16);
  for (int i = 0; i < 4; i++)
    m[i][i] = 1.0f;
}

static void mat_mul(float out[4][4], const float a[4][4], const float b[4][4]) {
  float r[4][4];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
      r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] +
                a[i][3] * b[3][j];
  memcpy(out, r, sizeof(r));
}

/* Mtx: 16 s16 integer parts then 16 u16 fractions, row-major. */
static void load_mtx(Gfx *g, uint32_t pa, float m[4][4]) {
  for (int w = 0; w < 16; w++)
    hash(g, rd32(g, pa + 4u * (uint32_t)w));
  for (int e = 0; e < 16; e++) {
    int32_t hi = (int16_t)rd16(g, pa + 2u * (uint32_t)e);
    uint32_t lo = rd16(g, pa + 32u + 2u * (uint32_t)e);
    m[e / 4][e % 4] = (float)(int32_t)((uint32_t)hi << 16 | lo) / 65536.0f;
  }
}

/* --- init --------------------------------------------------------------- */

void gfx_init(Gfx *g) {
  memset(g, 0, sizeof(*g));
  g->screen_w = 320;
  g->screen_h = 240;
  g->tex_index_cap = 8192;
  g->tex_index = calloc(g->tex_index_cap, sizeof(uint32_t));
  g->bound_texture = -1;
}

void gfx_free(Gfx *g) {
  free(g->verts);
  free(g->draws);
  free(g->states);
  free(g->textures);
  free(g->tex_index);
  free(g->texels);
  memset(g, 0, sizeof(*g));
}

static void reset_rsp(Gfx *g) {
  memset(g->segments, 0, sizeof(g->segments));
  g->mv_top = 0;
  mat_identity(g->mv[0]);
  mat_identity(g->proj);
  mat_identity(g->mvp);
  g->vp_scale[0] = 160;
  g->vp_scale[1] = 120;
  g->vp_trans[0] = 160;
  g->vp_trans[1] = 120;
  g->geometry_mode = 0;
  g->num_lights = 1;
  g->lights_dirty = 1;
  g->tex_on = 0;
  g->scissor[0] = 0;
  g->scissor[1] = 0;
  g->scissor[2] = (int16_t)g->screen_w;
  g->scissor[3] = (int16_t)g->screen_h;
}

/* --- texture cache ------------------------------------------------------ */

static void flush_textures(Gfx *g) {
  g->ntextures = 0;
  g->ntexels = 0;
  memset(g->tex_index, 0, sizeof(uint32_t) * g->tex_index_cap);
  g->cache_generation++;
  g->bound_texture = -1;
  g->texture_dirty = 1;
}

static uint8_t wrap_mode(uint8_t cm, uint8_t mask) {
  if ((cm & 2u) || mask == 0)
    return 2;
  return cm & 1u ? 1 : 0;
}

static int32_t resolve_texture(Gfx *g, unsigned tile_index) {
  const GfxTile *t = &g->tiles[tile_index & 7u];
  uint32_t w = ((uint32_t)(t->lrs - t->uls) >> 2) + 1;
  uint32_t h = ((uint32_t)(t->lrt - t->ult) >> 2) + 1;
  if (t->masks && !(t->cms & 2u) && (1u << t->masks) < w)
    w = 1u << t->masks;
  if (t->maskt && !(t->cmt & 2u) && (1u << t->maskt) < h)
    h = 1u << t->maskt;
  if (w > 1024 || h > 1024 || w == 0 || h == 0)
    return -1;
  const GfxTmemLoad *ld = &g->tmem_load[t->tmem & 511u];
  if (!ld->valid)
    return -1;
  uint32_t row_bytes = (w * tex_bits(t->siz) + 7u) / 8u;
  uint32_t stride = ld->stride ? ld->stride : t->line * 8u;
  if (!stride)
    stride = row_bytes;

  uint64_t key = HW_HASH_INIT;
  key = hw_hash_word(key, (uint64_t)ld->addr << 32 | stride);
  key = hw_hash_word(key, (uint64_t)w << 48 | (uint64_t)h << 32 |
                              (uint64_t)t->fmt << 24 | (uint64_t)t->siz << 16 |
                              (uint64_t)t->palette << 8 |
                              TEXTLUT(g->othermode_h));
  key = hw_hash_word(key, (uint64_t)t->cms << 24 | (uint64_t)t->cmt << 16 |
                              (uint64_t)t->masks << 8 | t->maskt);
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < row_bytes; x += 4)
      key = hw_hash_word(key, rd32(g, ld->addr + y * stride + x));
  if (t->fmt == TX_CI)
    key = hw_hash_word(key, g->tlut_hash);
  if (!key)
    key = 1;

  uint32_t mask = g->tex_index_cap - 1u;
  uint32_t slot = (uint32_t)(key ^ (key >> 32)) & mask;
  while (g->tex_index[slot]) {
    uint32_t i = g->tex_index[slot] - 1u;
    if (g->textures[i].key == key)
      return (int32_t)i;
    slot = (slot + 1u) & mask;
  }

  uint32_t bytes = w * h * 4u;
  if (g->ntexels + bytes > TEXEL_BUDGET ||
      g->ntextures * 2 >= g->tex_index_cap) {
    flush_textures(g);
    slot = (uint32_t)(key ^ (key >> 32)) & mask;
  }
  g->texels = grow(g->texels, &g->cap_texels, g->ntexels + bytes, 1);
  g->textures =
      grow(g->textures, &g->cap_textures, g->ntextures + 1, sizeof(GfxTexture));
  GfxTexture *tx = &g->textures[g->ntextures];
  tx->key = key;
  tx->offset = g->ntexels;
  tx->w = (uint16_t)w;
  tx->h = (uint16_t)h;
  tx->wrap_s = wrap_mode(t->cms, t->masks);
  tx->wrap_t = wrap_mode(t->cmt, t->maskt);
  tex_decode(g->texels + g->ntexels, g->rdram, HW_RDRAM_SIZE, ld->addr, stride,
             w, h, t->fmt, t->siz, g->tlut, TEXTLUT(g->othermode_h) == 3,
             t->palette);
  g->ntexels += bytes;
  g->tex_index[slot] = ++g->ntextures;
  return (int32_t)(g->ntextures - 1);
}

/* --- combiner ----------------------------------------------------------- */

static const uint8_t cc_a[16] = {CC_COMBINED, CC_TEXEL0, CC_TEXEL1, CC_PRIM,
                                 CC_SHADE,    CC_ENV,    CC_ONE,    CC_NOISE,
                                 CC_ZERO,     CC_ZERO,   CC_ZERO,   CC_ZERO,
                                 CC_ZERO,     CC_ZERO,   CC_ZERO,   CC_ZERO};
static const uint8_t cc_b[16] = {CC_COMBINED, CC_TEXEL0, CC_TEXEL1, CC_PRIM,
                                 CC_SHADE,    CC_ENV,    CC_ZERO,   CC_ZERO,
                                 CC_ZERO,     CC_ZERO,   CC_ZERO,   CC_ZERO,
                                 CC_ZERO,     CC_ZERO,   CC_ZERO,   CC_ZERO};
static const uint8_t cc_c[32] = {
    CC_COMBINED, CC_TEXEL0,     CC_TEXEL1,   CC_PRIM,     CC_SHADE,  CC_ENV,
    CC_ONE,      CC_COMBINED_A, CC_TEXEL0_A, CC_TEXEL1_A, CC_PRIM_A, CC_SHADE_A,
    CC_ENV_A,    CC_ZERO,       CC_PRIM_LOD, CC_ZERO,     CC_ZERO,   CC_ZERO,
    CC_ZERO,     CC_ZERO,       CC_ZERO,     CC_ZERO,     CC_ZERO,   CC_ZERO,
    CC_ZERO,     CC_ZERO,       CC_ZERO,     CC_ZERO,     CC_ZERO,   CC_ZERO,
    CC_ZERO,     CC_ZERO};
static const uint8_t cc_d[8] = {CC_COMBINED, CC_TEXEL0, CC_TEXEL1, CC_PRIM,
                                CC_SHADE,    CC_ENV,    CC_ONE,    CC_ZERO};
static const uint8_t ac_abd[8] = {CC_COMBINED, CC_TEXEL0, CC_TEXEL1, CC_PRIM,
                                  CC_SHADE,    CC_ENV,    CC_ONE,    CC_ZERO};
static const uint8_t ac_c[8] = {CC_ZERO,  CC_TEXEL0, CC_TEXEL1,   CC_PRIM,
                                CC_SHADE, CC_ENV,    CC_PRIM_LOD, CC_ZERO};

static void decode_combiner(uint32_t w0, uint32_t w1, uint8_t out[16]) {
  out[0] = cc_a[(w0 >> 20) & 15u];
  out[1] = cc_b[(w1 >> 28) & 15u];
  out[2] = cc_c[(w0 >> 15) & 31u];
  out[3] = cc_d[(w1 >> 15) & 7u];
  out[4] = ac_abd[(w0 >> 12) & 7u];
  out[5] = ac_abd[(w1 >> 12) & 7u];
  out[6] = ac_c[(w0 >> 9) & 7u];
  out[7] = ac_abd[(w1 >> 9) & 7u];
  out[8] = cc_a[(w0 >> 5) & 15u];
  out[9] = cc_b[(w1 >> 24) & 15u];
  out[10] = cc_c[w0 & 31u];
  out[11] = cc_d[(w1 >> 6) & 7u];
  out[12] = ac_abd[(w1 >> 21) & 7u];
  out[13] = ac_abd[(w1 >> 3) & 7u];
  out[14] = ac_c[(w1 >> 18) & 7u];
  out[15] = ac_abd[w1 & 7u];
}

static int uses_texture(const uint8_t comb[16], int two_cycle) {
  for (int i = 0; i < (two_cycle ? 16 : 8); i++)
    if (comb[i] == CC_TEXEL0 || comb[i] == CC_TEXEL1 ||
        comb[i] == CC_TEXEL0_A || comb[i] == CC_TEXEL1_A)
      return 1;
  return 0;
}

/* --- state and draw emission ------------------------------------------- */

static uint32_t intern_state(Gfx *g, const GfxState *s) {
  if (g->nstates && memcmp(&g->states[g->nstates - 1], s, sizeof(*s)) == 0)
    return g->nstates - 1;
  g->states = grow(g->states, &g->cap_states, g->nstates + 1, sizeof(GfxState));
  g->states[g->nstates] = *s;
  return g->nstates++;
}

static void emit_draw(Gfx *g, uint32_t kind, uint32_t state, uint32_t nverts) {
  if (kind == GD_TRIANGLES && g->ndraws) {
    GfxDraw *d = &g->draws[g->ndraws - 1];
    if (d->kind == GD_TRIANGLES && d->state == state &&
        d->first + d->count == g->nverts - nverts) {
      d->count += nverts;
      return;
    }
  }
  g->draws = grow(g->draws, &g->cap_draws, g->ndraws + 1, sizeof(GfxDraw));
  g->draws[g->ndraws++] = (GfxDraw){kind, g->nverts - nverts, nverts, state};
}

/* Render state for the current modes. `rect`: screen-space rectangle. */
static void build_state(Gfx *g, GfxState *s, int rect, unsigned tile) {
  memset(s, 0, sizeof(*s));
  uint32_t cyc = CYCLE_TYPE(g->othermode_h), l = g->othermode_l;
  decode_combiner(g->combine_w0, g->combine_w1, s->comb);
  if (cyc == CYC_2)
    s->flags |= GS_TWO_CYCLE;
  if (cyc == CYC_COPY) {
    static const uint8_t copy[16] = {CC_ZERO, CC_ZERO, CC_ZERO, CC_TEXEL0,
                                     CC_ZERO, CC_ZERO, CC_ZERO, CC_TEXEL0};
    memcpy(s->comb, copy, sizeof(copy));
    s->flags &= ~(uint32_t)GS_TWO_CYCLE;
    if ((l & 3u) != 0) {
      s->flags |= GS_ALPHA_TEST;
      s->alpha_ref = 0.5f;
    }
  }
  if (!rect && (g->geometry_mode & G_ZBUFFER)) {
    if (l & RM_Z_CMP)
      s->flags |= GS_DEPTH_TEST;
    if (l & RM_Z_UPD)
      s->flags |= GS_DEPTH_WRITE;
    if ((l & RM_ZMODE_MASK) == RM_ZMODE_DEC)
      s->flags |= GS_DECAL;
  }
  if (cyc < CYC_COPY) {
    if (l & RM_FORCE_BL)
      s->flags |= GS_BLEND;
    if (l & RM_CVG_X_ALPHA) {
      s->flags |= GS_ALPHA_TEST;
      s->alpha_ref = 0.5f;
    } else if ((l & 3u) == 1) {
      s->flags |= GS_ALPHA_TEST;
      s->alpha_ref = g->blend[3] > 0.0f ? g->blend[3] : 1.0f / 255.0f;
    }
  }
  if (TEXTFILT(g->othermode_h) != 0 && cyc != CYC_COPY)
    s->flags |= GS_FILTER;
  s->texture = -1;
  if (uses_texture(s->comb, cyc == CYC_2) && (rect || g->tex_on)) {
    if (g->texture_dirty || g->bound_tile != tile) {
      g->bound_texture = resolve_texture(g, tile);
      g->texture_dirty = 0;
      g->bound_tile = (uint8_t)tile;
    }
    s->texture = g->bound_texture;
    if (s->texture >= 0)
      s->flags |= GS_TEXTURED;
  }
  memcpy(s->prim, g->prim, sizeof(s->prim));
  memcpy(s->env, g->env, sizeof(s->env));
  s->prim_lod = g->prim_lod;
  memcpy(s->scissor, g->scissor, sizeof(s->scissor));
}

static float shift_coord(float c, uint8_t shift) {
  if (shift == 0)
    return c;
  if (shift <= 10)
    return c / (float)(1u << shift);
  return c * (float)(1u << (16u - shift));
}

static void emit_triangle(Gfx *g, const GfxRspVertex *v[3]) {
  /* Face culling in screen space (N64 y-down). */
  uint32_t cull = g->geometry_mode & (G_CULL_FRONT | G_CULL_BACK);
  if (cull && v[0]->pos[3] > 0 && v[1]->pos[3] > 0 && v[2]->pos[3] > 0) {
    float x0 = v[0]->pos[0] / v[0]->pos[3], y0 = v[0]->pos[1] / v[0]->pos[3];
    float x1 = v[1]->pos[0] / v[1]->pos[3], y1 = v[1]->pos[1] / v[1]->pos[3];
    float x2 = v[2]->pos[0] / v[2]->pos[3], y2 = v[2]->pos[1] / v[2]->pos[3];
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (cull == (G_CULL_FRONT | G_CULL_BACK))
      return;
    if ((cull & G_CULL_BACK) && area < 0)
      return;
    if ((cull & G_CULL_FRONT) && area > 0)
      return;
  }
  GfxState s;
  build_state(g, &s, 0, g->tex_tile);
  uint32_t st = intern_state(g, &s);
  const GfxTile *t = &g->tiles[g->tex_tile & 7u];
  float tw = 1, th = 1;
  if (s.texture >= 0) {
    tw = g->textures[s.texture].w;
    th = g->textures[s.texture].h;
  }
  g->verts = grow(g->verts, &g->cap_verts, g->nverts + 3, sizeof(GfxVertex));
  for (int i = 0; i < 3; i++) {
    GfxVertex *o = &g->verts[g->nverts++];
    o->x = v[i]->pos[0];
    o->y = v[i]->pos[1];
    o->z = v[i]->pos[2];
    o->w = v[i]->pos[3];
    o->r = v[i]->col[0];
    o->g = v[i]->col[1];
    o->b = v[i]->col[2];
    o->a = v[i]->col[3];
    o->u = (shift_coord(v[i]->u, t->shifts) - (float)t->uls / 4.0f) / tw;
    o->v = (shift_coord(v[i]->v, t->shiftt) - (float)t->ult / 4.0f) / th;
  }
  emit_draw(g, GD_TRIANGLES, st, 3);
}

/* Screen-space quad; coordinates in N64 pixels, texcoords in texels. */
static void emit_rect(Gfx *g, float x0, float y0, float x1, float y1,
                      const float uv[4][2], const float col[4], unsigned tile) {
  GfxState s;
  build_state(g, &s, 1, tile);
  uint32_t st = intern_state(g, &s);
  float tw = 1, th = 1;
  if (s.texture >= 0) {
    tw = g->textures[s.texture].w;
    th = g->textures[s.texture].h;
  }
  const GfxTile *t = &g->tiles[tile & 7u];
  float hw = g->screen_w / 2.0f, hh = g->screen_h / 2.0f;
  float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
  static const int order[6] = {0, 1, 2, 0, 2, 3};
  g->verts = grow(g->verts, &g->cap_verts, g->nverts + 6, sizeof(GfxVertex));
  for (int k = 0; k < 6; k++) {
    int i = order[k];
    GfxVertex *o = &g->verts[g->nverts++];
    o->x = xs[i] / hw - 1.0f;
    o->y = 1.0f - ys[i] / hh;
    o->z = 0;
    o->w = 1;
    o->r = col[0];
    o->g = col[1];
    o->b = col[2];
    o->a = col[3];
    o->u = uv ? (uv[i][0] - (float)t->uls / 4.0f) / tw : 0;
    o->v = uv ? (uv[i][1] - (float)t->ult / 4.0f) / th : 0;
  }
  emit_draw(g, GD_TRIANGLES, st, 6);
}

/* --- geometry ----------------------------------------------------------- */

static void update_lights(Gfx *g) {
  const float (*m)[4] = g->mv[g->mv_top];
  for (uint32_t i = 0; i <= g->num_lights && i < 8; i++) {
    float d[3], o[3];
    for (int k = 0; k < 3; k++)
      d[k] = g->light_dir[i][k] / 127.0f;
    for (int k = 0; k < 3; k++)
      o[k] = d[0] * m[k][0] + d[1] * m[k][1] + d[2] * m[k][2];
    float len = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    for (int k = 0; k < 3; k++)
      g->light_model[i][k] = len > 0 ? o[k] / len : 0;
  }
  g->lights_dirty = 0;
}

static void load_vertices(Gfx *g, uint32_t w0, uint32_t w1) {
  uint32_t n = ((w0 >> 20) & 15u) + 1, v0 = (w0 >> 16) & 15u;
  uint32_t pa = seg(g, w1);
  if (g->lights_dirty && (g->geometry_mode & G_LIGHTING))
    update_lights(g);
  float hw = g->screen_w / 2.0f, hh = g->screen_h / 2.0f;
  float ax = g->vp_scale[0] / hw, bx = g->vp_trans[0] / hw - 1.0f;
  float ay = g->vp_scale[1] / hh, by = 1.0f - g->vp_trans[1] / hh;
  float ss = g->tex_scale_s / 65536.0f, ts = g->tex_scale_t / 65536.0f;
  for (uint32_t i = 0; i < n && v0 + i < 16; i++) {
    uint32_t a = pa + 16u * i;
    for (int k = 0; k < 4; k++)
      hash(g, rd32(g, a + 4u * (uint32_t)k));
    float p[3] = {(int16_t)rd16(g, a), (int16_t)rd16(g, a + 2),
                  (int16_t)rd16(g, a + 4)};
    int16_t s = (int16_t)rd16(g, a + 8), t = (int16_t)rd16(g, a + 10);
    const uint8_t *c = g->rdram + ((a + 12) & RDRAM_MASK);
    GfxRspVertex *v = &g->vtx[v0 + i];
    float clip[4];
    for (int j = 0; j < 4; j++)
      clip[j] = p[0] * g->mvp[0][j] + p[1] * g->mvp[1][j] +
                p[2] * g->mvp[2][j] + g->mvp[3][j];
    v->pos[0] = clip[0] * ax + clip[3] * bx;
    v->pos[1] = clip[1] * ay + clip[3] * by;
    v->pos[2] = clip[2];
    v->pos[3] = clip[3];
    if (g->geometry_mode & G_LIGHTING) {
      float nrm[3] = {(int8_t)c[0] / 127.0f, (int8_t)c[1] / 127.0f,
                      (int8_t)c[2] / 127.0f};
      const uint8_t *amb = g->light_col[g->num_lights & 7u];
      float rgb[3] = {amb[0] / 255.0f, amb[1] / 255.0f, amb[2] / 255.0f};
      for (uint32_t l = 0; l < g->num_lights && l < 7; l++) {
        const float *ld = g->light_model[l];
        float d = nrm[0] * ld[0] + nrm[1] * ld[1] + nrm[2] * ld[2];
        if (d > 0)
          for (int k = 0; k < 3; k++)
            rgb[k] += d * g->light_col[l][k] / 255.0f;
      }
      for (int k = 0; k < 3; k++)
        v->col[k] = rgb[k] > 1.0f ? 1.0f : rgb[k];
      if (g->geometry_mode & G_TEXTURE_GEN) {
        /* Environment mapping from the look-at directions (as lights). */
        const float (*m)[4] = g->mv[g->mv_top];
        float dots[2];
        for (int q = 0; q < 2; q++) {
          float d[3], o[3];
          for (int k = 0; k < 3; k++)
            d[k] = g->lookat[q][k] / 127.0f;
          for (int k = 0; k < 3; k++)
            o[k] = d[0] * m[k][0] + d[1] * m[k][1] + d[2] * m[k][2];
          float len = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
          dots[q] = len > 0
                        ? (nrm[0] * o[0] + nrm[1] * o[1] + nrm[2] * o[2]) / len
                        : 0;
        }
        s = (int16_t)((dots[0] + 1.0f) / 4.0f * g->tex_scale_s);
        t = (int16_t)((dots[1] + 1.0f) / 4.0f * g->tex_scale_t);
        v->u = s / 32.0f;
        v->v = t / 32.0f;
      } else {
        v->u = s * ss / 32.0f;
        v->v = t * ts / 32.0f;
      }
    } else {
      for (int k = 0; k < 3; k++)
        v->col[k] = c[k] / 255.0f;
      v->u = s * ss / 32.0f;
      v->v = t * ts / 32.0f;
    }
    v->col[3] = c[3] / 255.0f;
  }
}

static void matrix(Gfx *g, uint32_t w0, uint32_t w1) {
  uint32_t params = (w0 >> 16) & 0xFFu;
  float m[4][4];
  load_mtx(g, seg(g, w1), m);
  if (params & 1u) { /* projection */
    if (params & 2u)
      memcpy(g->proj, m, sizeof(m));
    else
      mat_mul(g->proj, m, g->proj);
  } else {
    if ((params & 4u) && g->mv_top < 9) {
      memcpy(g->mv[g->mv_top + 1], g->mv[g->mv_top], sizeof(m));
      g->mv_top++;
    }
    if (params & 2u)
      memcpy(g->mv[g->mv_top], m, sizeof(m));
    else
      mat_mul(g->mv[g->mv_top], m, g->mv[g->mv_top]);
    g->lights_dirty = 1;
  }
  mat_mul(g->mvp, g->mv[g->mv_top], g->proj);
}

static void movemem(Gfx *g, uint32_t w0, uint32_t w1) {
  uint32_t idx = (w0 >> 16) & 0xFFu, pa = seg(g, w1);
  if (idx == 0x80) { /* viewport */
    for (int k = 0; k < 3; k++) {
      g->vp_scale[k] = (int16_t)rd16(g, pa + 2u * (uint32_t)k) / 4.0f;
      g->vp_trans[k] = (int16_t)rd16(g, pa + 8u + 2u * (uint32_t)k) / 4.0f;
    }
    if (g->vp_scale[0] < 0)
      g->vp_scale[0] = -g->vp_scale[0];
  } else if (idx == 0x82 || idx == 0x84) { /* look-at Y / X */
    int q = idx == 0x84 ? 0 : 1;
    for (int k = 0; k < 3; k++)
      g->lookat[q][k] = (int8_t)g->rdram[(pa + 8u + (uint32_t)k) & RDRAM_MASK];
  } else if (idx >= 0x86 && idx <= 0x94) { /* lights L0..L7 */
    uint32_t i = (idx - 0x86) / 2;
    for (int k = 0; k < 3; k++) {
      g->light_col[i][k] = g->rdram[(pa + (uint32_t)k) & RDRAM_MASK];
      g->light_dir[i][k] =
          (int8_t)g->rdram[(pa + 8u + (uint32_t)k) & RDRAM_MASK];
    }
    g->lights_dirty = 1;
  }
}

static void moveword(Gfx *g, uint32_t w0, uint32_t w1) {
  uint32_t index = w0 & 0xFFu, offset = (w0 >> 8) & 0xFFFFu;
  switch (index) {
  case 0x02: /* G_MW_NUMLIGHT: ((n + 1) * 32) | 0x80000000 */
    g->num_lights = (((w1 & 0x7FFFFFFFu) >> 5) - 1u) & 7u;
    g->lights_dirty = 1;
    break;
  case 0x06: /* G_MW_SEGMENT */
    g->segments[(offset >> 2) & 15u] = w1 & 0x1FFFFFFFu;
    break;
  default:
    break; /* fog, clip ratio, perspective normalisation: not modelled */
  }
}

/* --- RDP commands ------------------------------------------------------- */

static void set_tile(Gfx *g, uint32_t w0, uint32_t w1) {
  GfxTile *t = &g->tiles[(w1 >> 24) & 7u];
  t->fmt = (w0 >> 21) & 7u;
  t->siz = (w0 >> 19) & 3u;
  t->line = (w0 >> 9) & 0x1FFu;
  t->tmem = w0 & 0x1FFu;
  t->palette = (w1 >> 20) & 15u;
  t->cmt = (w1 >> 18) & 3u;
  t->maskt = (w1 >> 14) & 15u;
  t->shiftt = (w1 >> 10) & 15u;
  t->cms = (w1 >> 8) & 3u;
  t->masks = (w1 >> 4) & 15u;
  t->shifts = w1 & 15u;
  g->texture_dirty = 1;
}

static void set_tile_size(Gfx *g, uint32_t w0, uint32_t w1) {
  GfxTile *t = &g->tiles[(w1 >> 24) & 7u];
  t->uls = (w0 >> 12) & 0xFFFu;
  t->ult = w0 & 0xFFFu;
  t->lrs = (w1 >> 12) & 0xFFFu;
  t->lrt = w1 & 0xFFFu;
  g->texture_dirty = 1;
}

static void load(Gfx *g, uint32_t w0, uint32_t w1, int block) {
  GfxTile *t = &g->tiles[(w1 >> 24) & 7u];
  uint32_t uls = ((w0 >> 12) & 0xFFFu) >> (block ? 0 : 2);
  uint32_t ult = (w0 & 0xFFFu) >> (block ? 0 : 2);
  uint32_t bpp = tex_bits(g->timg_siz);
  GfxTmemLoad *ld = &g->tmem_load[t->tmem & 511u];
  ld->addr =
      (g->timg_addr + (ult * g->timg_width + uls) * bpp / 8u) & RDRAM_MASK;
  ld->stride = block ? 0 : g->timg_width * bpp / 8u;
  ld->valid = 1;
  if (!block) {
    t->uls = (uint16_t)((w0 >> 12) & 0xFFFu);
    t->ult = (uint16_t)(w0 & 0xFFFu);
    t->lrs = (uint16_t)((w1 >> 12) & 0xFFFu);
    t->lrt = (uint16_t)(w1 & 0xFFFu);
  }
  g->texture_dirty = 1;
}

static void load_tlut(Gfx *g, uint32_t w1) {
  const GfxTile *t = &g->tiles[(w1 >> 24) & 7u];
  uint32_t count = ((w1 >> 14) & 0x3FFu) + 1u, base = t->tmem - 256u;
  for (uint32_t i = 0; i < count && i < 256; i++)
    g->tlut[(base + i) & 255u] = rd16(g, g->timg_addr + 2u * i);
  uint64_t h = HW_HASH_INIT;
  for (int i = 0; i < 256; i += 4)
    h = hw_hash_word(h, (uint64_t)g->tlut[i] << 48 |
                            (uint64_t)g->tlut[i + 1] << 32 |
                            (uint64_t)g->tlut[i + 2] << 16 | g->tlut[i + 3]);
  g->tlut_hash = h;
  g->texture_dirty = 1;
}

static void fill_rect(Gfx *g, uint32_t w0, uint32_t w1) {
  float x1 = ((w0 >> 12) & 0xFFFu) / 4.0f, y1 = (w0 & 0xFFFu) / 4.0f;
  float x0 = ((w1 >> 12) & 0xFFFu) / 4.0f, y0 = (w1 & 0xFFFu) / 4.0f;
  uint32_t cyc = CYCLE_TYPE(g->othermode_h);
  if (g->cimg_addr == g->zimg_addr) { /* clearing the depth buffer */
    g->draws = grow(g->draws, &g->cap_draws, g->ndraws + 1, sizeof(GfxDraw));
    g->draws[g->ndraws++] = (GfxDraw){GD_CLEAR_DEPTH, g->nverts, 0, 0};
    return;
  }
  static const float white[4] = {1, 1, 1, 1};
  if (cyc == CYC_FILL || cyc == CYC_COPY) {
    x1 += 1;
    y1 += 1;
  }
  if (cyc == CYC_FILL) {
    /* Fill color: two RGBA5551 pixels; use the first. */
    uint32_t p = g->fill_color >> 16;
    float saved_prim[4],
        col[4] = {((p >> 11) & 31u) / 31.0f, ((p >> 6) & 31u) / 31.0f,
                  ((p >> 1) & 31u) / 31.0f, 1.0f};
    uint32_t sw0 = g->combine_w0, sw1 = g->combine_w1;
    memcpy(saved_prim, g->prim, sizeof(saved_prim));
    memcpy(g->prim, col, sizeof(col));
    /* combine = (0 - 0) * 0 + PRIM for color and alpha */
    g->combine_w0 =
        (15u << 20) | (31u << 15) | (7u << 12) | (7u << 9) | (15u << 5) | 31u;
    g->combine_w1 = (15u << 28) | (15u << 24) | (7u << 21) | (7u << 18) |
                    (3u << 15) | (7u << 12) | (3u << 9) | (3u << 6) |
                    (7u << 3) | 3u;
    uint32_t saved_l = g->othermode_l;
    g->othermode_l &= ~(RM_FORCE_BL | RM_CVG_X_ALPHA | 3u);
    emit_rect(g, x0, y0, x1, y1, NULL, white, 0);
    g->othermode_l = saved_l;
    g->combine_w0 = sw0;
    g->combine_w1 = sw1;
    memcpy(g->prim, saved_prim, sizeof(saved_prim));
    return;
  }
  emit_rect(g, x0, y0, x1, y1, NULL, white, 0);
}

static void tex_rect(Gfx *g, uint32_t w0, uint32_t w1, int flip) {
  float x1 = ((w0 >> 12) & 0xFFFu) / 4.0f, y1 = (w0 & 0xFFFu) / 4.0f;
  unsigned tile = (w1 >> 24) & 7u;
  float x0 = ((w1 >> 12) & 0xFFFu) / 4.0f, y0 = (w1 & 0xFFFu) / 4.0f;
  float s = (int16_t)(g->rdphalf[0] >> 16) / 32.0f;
  float t = (int16_t)(g->rdphalf[0] & 0xFFFFu) / 32.0f;
  float dsdx = (int16_t)(g->rdphalf[1] >> 16) / 1024.0f;
  float dtdy = (int16_t)(g->rdphalf[1] & 0xFFFFu) / 1024.0f;
  uint32_t cyc = CYCLE_TYPE(g->othermode_h);
  if (cyc == CYC_COPY) {
    dsdx /= 4.0f;
    x1 += 1;
    y1 += 1;
  }
  float ds = (x1 - x0) * dsdx, dt = (y1 - y0) * dtdy;
  float uv[4][2];
  if (!flip) {
    float q[4][2] = {{s, t}, {s + ds, t}, {s + ds, t + dt}, {s, t + dt}};
    memcpy(uv, q, sizeof(q));
  } else {
    float dsf = (y1 - y0) * dsdx, dtf = (x1 - x0) * dtdy;
    float q[4][2] = {{s, t}, {s, t + dtf}, {s + dsf, t + dtf}, {s + dsf, t}};
    memcpy(uv, q, sizeof(q));
  }
  static const float white[4] = {1, 1, 1, 1};
  emit_rect(g, x0, y0, x1, y1, uv, white, tile);
}

/* --- walker ------------------------------------------------------------- */

void gfx_run_task(Gfx *g, const uint8_t *rdram, uint32_t dl) {
  g->rdram = rdram;
  g->nverts = g->ndraws = g->nstates = 0;
  g->dl_hash = HW_HASH_INIT;
  g->commands = g->unknown = 0;
  g->texture_dirty = 1;
  reset_rsp(g);

  uint32_t stack[MAX_DEPTH], depth = 0, pc = dl & RDRAM_MASK;
  while (g->commands < MAX_COMMANDS) {
    uint32_t w0 = rd32(g, pc), w1 = rd32(g, pc + 4);
    pc += 8;
    g->commands++;
    hash(g, (uint64_t)w0 << 32 | w1);
    switch (w0 >> 24) {
    case G_SPNOOP:
    case G_CULLDL:
    case 0xE6:
    case 0xE7:
    case 0xE8:
    case 0xE9: /* syncs */
      break;
    case G_MTX:
      matrix(g, w0, w1);
      break;
    case G_MOVEMEM:
      movemem(g, w0, w1);
      break;
    case G_VTX:
      load_vertices(g, w0, w1);
      break;
    case G_DL:
      if (!((w0 >> 16) & 0xFFu)) { /* push */
        if (depth == MAX_DEPTH)
          return;
        stack[depth++] = pc;
      }
      pc = seg(g, w1);
      break;
    case G_ENDDL:
      if (depth == 0)
        return;
      pc = stack[--depth];
      break;
    case G_TRI1: {
      const GfxRspVertex *v[3] = {&g->vtx[((w1 >> 16) & 0xFFu) / 10u & 15u],
                                  &g->vtx[((w1 >> 8) & 0xFFu) / 10u & 15u],
                                  &g->vtx[(w1 & 0xFFu) / 10u & 15u]};
      emit_triangle(g, v);
      break;
    }
    case G_POPMTX:
      if (g->mv_top > 0) {
        g->mv_top--;
        g->lights_dirty = 1;
        mat_mul(g->mvp, g->mv[g->mv_top], g->proj);
      }
      break;
    case G_MOVEWORD:
      moveword(g, w0, w1);
      break;
    case G_TEXTURE:
      g->tex_scale_s = (uint16_t)(w1 >> 16);
      g->tex_scale_t = (uint16_t)w1;
      g->tex_tile = (w0 >> 8) & 7u;
      g->tex_on = (w0 & 0xFFu) != 0;
      g->texture_dirty = 1;
      break;
    case G_SETGEOMETRYMODE:
      g->geometry_mode |= w1;
      break;
    case G_CLEARGEOMETRYMODE:
      g->geometry_mode &= ~w1;
      break;
    case G_SETOTHERMODE_H:
    case G_SETOTHERMODE_L: {
      uint32_t sft = (w0 >> 8) & 0xFFu, len = w0 & 0xFFu;
      uint32_t mask = (len >= 32 ? ~0u : ((1u << len) - 1u)) << sft;
      uint32_t *m =
          (w0 >> 24) == G_SETOTHERMODE_H ? &g->othermode_h : &g->othermode_l;
      *m = (*m & ~mask) | (w1 & mask);
      g->texture_dirty = 1;
      break;
    }
    case G_RDPSETOTHERMODE:
      g->othermode_h = w0 & 0x00FFFFFFu;
      g->othermode_l = w1;
      break;
    case G_RDPHALF_1:
    case G_RDPHALF_2:
    case G_RDPHALF_CONT:
      break;
    case G_TEXRECT:
    case G_TEXRECTFLIP: {
      /* F3D follows with two half-words: (s, t) and (dsdx, dtdy). */
      g->rdphalf[0] = rd32(g, pc + 4);
      g->rdphalf[1] = rd32(g, pc + 12);
      hash(g, (uint64_t)rd32(g, pc) << 32 | g->rdphalf[0]);
      hash(g, (uint64_t)rd32(g, pc + 8) << 32 | g->rdphalf[1]);
      pc += 16;
      tex_rect(g, w0, w1, (w0 >> 24) == G_TEXRECTFLIP);
      break;
    }
    case G_SETSCISSOR:
      g->scissor[0] = (int16_t)(((w0 >> 12) & 0xFFFu) / 4u);
      g->scissor[1] = (int16_t)((w0 & 0xFFFu) / 4u);
      g->scissor[2] = (int16_t)(((w1 >> 12) & 0xFFFu) / 4u);
      g->scissor[3] = (int16_t)((w1 & 0xFFFu) / 4u);
      break;
    case G_LOADTLUT:
      load_tlut(g, w1);
      break;
    case G_SETTILESIZE:
      set_tile_size(g, w0, w1);
      break;
    case G_LOADBLOCK:
      load(g, w0, w1, 1);
      break;
    case G_LOADTILE:
      load(g, w0, w1, 0);
      break;
    case G_SETTILE:
      set_tile(g, w0, w1);
      break;
    case G_FILLRECT:
      fill_rect(g, w0, w1);
      break;
    case G_SETFILLCOLOR:
      g->fill_color = w1;
      break;
    case G_SETFOGCOLOR:
      color_from_rgba(g->fog, w1);
      break;
    case G_SETBLENDCOLOR:
      color_from_rgba(g->blend, w1);
      break;
    case G_SETPRIMCOLOR:
      color_from_rgba(g->prim, w1);
      g->prim_lod = (w0 & 0xFFu) / 255.0f;
      break;
    case G_SETENVCOLOR:
      color_from_rgba(g->env, w1);
      break;
    case G_SETCOMBINE:
      g->combine_w0 = w0 & 0x00FFFFFFu;
      g->combine_w1 = w1;
      break;
    case G_SETTIMG:
      g->timg_fmt = (w0 >> 21) & 7u;
      g->timg_siz = (w0 >> 19) & 3u;
      g->timg_width = (w0 & 0xFFFu) + 1u;
      g->timg_addr = seg(g, w1);
      break;
    case G_SETZIMG:
      g->zimg_addr = seg(g, w1);
      break;
    case G_SETCIMG:
      g->cimg_addr = seg(g, w1);
      g->cimg_width = (w0 & 0xFFFu) + 1u;
      if (g->cimg_width >= 64 && g->cimg_width <= 1024) {
        g->screen_w = (uint16_t)g->cimg_width;
        g->screen_h = (uint16_t)(g->cimg_width * 3u / 4u);
      }
      break;
    default:
      g->unknown++;
      break;
    }
  }
}
