/* OpenGL 2.1 backend: draws a Gfx frame (see gfx.h) into the current
 * GL context's default framebuffer.
 *
 * One GLSL 1.20 program evaluates the RDP color combiner generically:
 * each cycle computes (A - B) * C + D for RGB and alpha from unified
 * input selectors (uniform ivec4s), with cycle 2 fed by cycle 1 in
 * two-cycle mode. TEXEL1 is approximated by TEXEL0, NOISE by 0.5 and
 * the chroma-key/LOD inputs by 0. Blending is plain alpha blending,
 * coverage is not modelled, fog is not applied. */
#ifndef M2M_RCP_GL_H
#define M2M_RCP_GL_H

#include <stdint.h>

#include "src/rcp/gfx.h"

typedef struct {
  unsigned prog, white;
  int loc_pos, loc_col, loc_uv;
  int u_tex, u_textured, u_prim, u_env, u_prim_lod, u_c0, u_a0, u_c1, u_a1,
      u_two_cycle, u_alpha_test, u_alpha_ref;
  unsigned *tex_ids; /* GL name per gfx texture-cache entry (0: none) */
  uint32_t tex_cap, generation;
  int width, height; /* drawable size in pixels */
} GlRenderer;

/* Requires a current GL 2.1 context. Returns 0 on shader failure. */
int gl_init(GlRenderer *r, int width, int height);
void gl_free(GlRenderer *r);

void gl_render(GlRenderer *r, const Gfx *g);

/* Reads the framebuffer as top-down RGBA8 (width * height * 4 bytes). */
void gl_read_rgba(const GlRenderer *r, uint8_t *out);

#endif
