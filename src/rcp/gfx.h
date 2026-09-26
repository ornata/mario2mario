/* Fast3D (F3D) display-list HLE: RSP geometry + RDP state -> flat draw
 * lists. No graphics API here: the output is consumed by the OpenGL
 * backend (gl.c) and, GL-free, by the oracle for display-list hashes.
 *
 * Per task the interpreter walks the display list from the OSTask data
 * pointer (nested G_DL calls/branches, segment table from G_MOVEWORD),
 * transforms vertices on the CPU into clip space with the N64 viewport
 * folded in (so GL can use one full-window viewport and still clip
 * correctly), lights them, and emits triangles grouped into draws. Each
 * draw references a de-duplicated render-state record and optionally a
 * texture from a persistent decoded-texture cache.
 *
 * The stream hash (dl_hash) folds every executed 64-bit command plus the
 * vertex and matrix words it loads, in execution order, with
 * hw_hash_word(); it is the frame-equivalence key. */
#ifndef M2M_RCP_GFX_H
#define M2M_RCP_GFX_H

#include <stdint.h>

/* Combiner input sources after unifying the RDP's per-slot encodings. */
enum {
  CC_COMBINED,
  CC_TEXEL0,
  CC_TEXEL1,
  CC_PRIM,
  CC_SHADE,
  CC_ENV,
  CC_ONE,
  CC_ZERO,
  CC_COMBINED_A,
  CC_TEXEL0_A,
  CC_TEXEL1_A,
  CC_PRIM_A,
  CC_SHADE_A,
  CC_ENV_A,
  CC_PRIM_LOD,
  CC_NOISE,
  CC_COUNT
};

/* One output vertex: clip-space position (viewport applied), shade
 * color, texture coordinate normalised to the bound texture. */
typedef struct {
  float x, y, z, w;
  float r, g, b, a;
  float u, v;
} GfxVertex;

enum {
  GS_BLEND = 1u << 0, /* alpha blend with the framebuffer */
  GS_DEPTH_TEST = 1u << 1,
  GS_DEPTH_WRITE = 1u << 2,
  GS_DECAL = 1u << 3,      /* coplanar decal: depth offset */
  GS_ALPHA_TEST = 1u << 4, /* discard below alpha_ref */
  GS_TWO_CYCLE = 1u << 5,
  GS_FILTER = 1u << 6, /* bilinear */
  GS_TEXTURED = 1u << 7,
};

typedef struct {
  uint32_t flags;   /* GS_* */
  int32_t texture;  /* index into the texture cache, -1 for none */
  uint8_t comb[16]; /* cycle 0: rgb a,b,c,d, alpha a,b,c,d; cycle 1 */
  float prim[4], env[4];
  float prim_lod, alpha_ref;
  int16_t scissor[4]; /* x0, y0, x1, y1 in N64 pixels (exclusive) */
} GfxState;

typedef enum {
  GD_TRIANGLES,  /* vertices [first, first + count) as a triangle list */
  GD_CLEAR_DEPTH /* fill-rectangle on the depth image */
} GfxDrawKind;

typedef struct {
  uint32_t kind; /* GfxDrawKind */
  uint32_t first, count;
  uint32_t state;
} GfxDraw;

/* Decoded texture cache (persistent across frames). */
typedef struct {
  uint64_t key;
  uint32_t offset; /* into texels, RGBA8 */
  uint16_t w, h;
  uint8_t wrap_s, wrap_t; /* 0 repeat, 1 mirror, 2 clamp */
} GfxTexture;

typedef struct {
  uint8_t fmt, siz;
  uint16_t line; /* 64-bit words per row */
  uint16_t tmem, palette;
  uint8_t cms, cmt, masks, maskt, shifts, shiftt;
  uint16_t uls, ult, lrs, lrt; /* 10.2 fixed point */
} GfxTile;

/* Where a TMEM region's texels came from in RDRAM. */
typedef struct {
  uint32_t addr;   /* physical */
  uint32_t stride; /* bytes per row; 0: use the render tile's line */
  uint8_t valid;
} GfxTmemLoad;

typedef struct {
  float pos[4]; /* clip space (viewport applied) */
  float col[4];
  float u, v; /* texels, before tile offset/shift */
} GfxRspVertex;

typedef struct {
  /* ---- per-frame outputs ---- */
  GfxVertex *verts;
  uint32_t nverts, cap_verts;
  GfxDraw *draws;
  uint32_t ndraws, cap_draws;
  GfxState *states;
  uint32_t nstates, cap_states;
  uint64_t dl_hash;
  uint32_t commands; /* executed commands this task */
  uint32_t unknown;  /* commands this HLE does not implement */
  uint16_t screen_w, screen_h;

  /* ---- persistent texture cache ---- */
  GfxTexture *textures;
  uint32_t ntextures, cap_textures;
  uint32_t *tex_index; /* open-addressed key -> texture + 1 */
  uint32_t tex_index_cap;
  uint8_t *texels;
  uint32_t ntexels, cap_texels;
  uint32_t cache_generation; /* bumps when the cache is flushed */

  /* ---- RSP state ---- */
  const uint8_t *rdram;
  uint32_t segments[16];
  float mv[10][4][4];
  uint32_t mv_top;
  float proj[4][4], mvp[4][4];
  float vp_scale[3], vp_trans[3]; /* N64 pixels */
  GfxRspVertex vtx[16];
  uint8_t light_col[8][3];
  int8_t light_dir[8][3];
  int8_t lookat[2][3];
  float light_model[8][3]; /* light dirs in model space */
  uint8_t lights_dirty;
  uint32_t num_lights;
  uint32_t geometry_mode;
  uint16_t tex_scale_s, tex_scale_t;
  uint8_t tex_tile, tex_on;

  /* ---- RDP state ---- */
  uint32_t othermode_h, othermode_l;
  uint32_t combine_w0, combine_w1;
  GfxTile tiles[8];
  GfxTmemLoad tmem_load[512]; /* by TMEM address in 64-bit words */
  uint16_t tlut[256];
  uint64_t tlut_hash;
  uint32_t timg_addr, timg_width;
  uint8_t timg_fmt, timg_siz;
  uint32_t cimg_addr, zimg_addr, cimg_width;
  uint32_t fill_color;
  float prim[4], env[4], blend[4], fog[4];
  float prim_lod;
  int16_t scissor[4];
  int32_t bound_texture; /* resolved texture for bound_tile */
  uint8_t bound_tile;
  uint8_t texture_dirty;
  uint32_t rdphalf[2];
} Gfx;

void gfx_init(Gfx *g);
void gfx_free(Gfx *g);

/* Runs one graphics task whose display list starts at physical `dl`. */
void gfx_run_task(Gfx *g, const uint8_t *rdram, uint32_t dl);

/* Returns the RGBA8 texels of cached texture `i`. */
static inline const uint8_t *gfx_texels(const Gfx *g, uint32_t i) {
  return g->texels + g->textures[i].offset;
}

#endif
