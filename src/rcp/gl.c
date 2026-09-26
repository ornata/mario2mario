#include "src/rcp/gl.h"

#include <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *vs_src =
    "#version 120\n"
    "attribute vec4 a_pos;\n"
    "attribute vec4 a_col;\n"
    "attribute vec2 a_uv;\n"
    "varying vec4 v_col;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_Position = a_pos; v_col = a_col; v_uv = a_uv; }\n";

/* Selector values match the CC_* enum in gfx.h. */
static const char *fs_src =
    "#version 120\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_textured, u_two_cycle, u_alpha_test;\n"
    "uniform vec4 u_prim, u_env;\n"
    "uniform float u_prim_lod, u_alpha_ref;\n"
    "uniform ivec4 u_c0, u_a0, u_c1, u_a1;\n"
    "varying vec4 v_col;\n"
    "varying vec2 v_uv;\n"
    "vec4 T, S;\n"
    "vec3 rgb(int s, vec4 c) {\n"
    "  if (s == 0) return c.rgb;\n"
    "  if (s == 1 || s == 2) return T.rgb;\n"
    "  if (s == 3) return u_prim.rgb;\n"
    "  if (s == 4) return S.rgb;\n"
    "  if (s == 5) return u_env.rgb;\n"
    "  if (s == 6) return vec3(1.0);\n"
    "  if (s == 8) return vec3(c.a);\n"
    "  if (s == 9 || s == 10) return vec3(T.a);\n"
    "  if (s == 11) return vec3(u_prim.a);\n"
    "  if (s == 12) return vec3(S.a);\n"
    "  if (s == 13) return vec3(u_env.a);\n"
    "  if (s == 14) return vec3(u_prim_lod);\n"
    "  if (s == 15) return vec3(0.5);\n"
    "  return vec3(0.0);\n"
    "}\n"
    "float alpha(int s, vec4 c) {\n"
    "  if (s == 0) return c.a;\n"
    "  if (s == 1 || s == 2) return T.a;\n"
    "  if (s == 3) return u_prim.a;\n"
    "  if (s == 4) return S.a;\n"
    "  if (s == 5) return u_env.a;\n"
    "  if (s == 6) return 1.0;\n"
    "  if (s == 14) return u_prim_lod;\n"
    "  return 0.0;\n"
    "}\n"
    "vec4 cycle(ivec4 cs, ivec4 as, vec4 c) {\n"
    "  vec3 o = (rgb(cs.x, c) - rgb(cs.y, c)) * rgb(cs.z, c) + rgb(cs.w, c);\n"
    "  float a = (alpha(as.x, c) - alpha(as.y, c)) * alpha(as.z, c) +\n"
    "            alpha(as.w, c);\n"
    "  return clamp(vec4(o, a), 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "  T = u_textured != 0 ? texture2D(u_tex, v_uv) : vec4(1.0);\n"
    "  S = v_col;\n"
    "  vec4 c = cycle(u_c0, u_a0, vec4(0.0));\n"
    "  if (u_two_cycle != 0) c = cycle(u_c1, u_a1, c);\n"
    "  if (u_alpha_test != 0 && c.a < u_alpha_ref) discard;\n"
    "  gl_FragColor = c;\n"
    "}\n";

static unsigned compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    fprintf(stderr, "shader: %s\n", log);
    return 0;
  }
  return s;
}

int gl_init(GlRenderer *r, int width, int height) {
  memset(r, 0, sizeof(*r));
  r->width = width;
  r->height = height;
  GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
  GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
  if (!vs || !fs)
    return 0;
  r->prog = glCreateProgram();
  glAttachShader(r->prog, vs);
  glAttachShader(r->prog, fs);
  glLinkProgram(r->prog);
  GLint ok = 0;
  glGetProgramiv(r->prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(r->prog, sizeof(log), NULL, log);
    fprintf(stderr, "link: %s\n", log);
    return 0;
  }
  r->loc_pos = glGetAttribLocation(r->prog, "a_pos");
  r->loc_col = glGetAttribLocation(r->prog, "a_col");
  r->loc_uv = glGetAttribLocation(r->prog, "a_uv");
  r->u_tex = glGetUniformLocation(r->prog, "u_tex");
  r->u_textured = glGetUniformLocation(r->prog, "u_textured");
  r->u_prim = glGetUniformLocation(r->prog, "u_prim");
  r->u_env = glGetUniformLocation(r->prog, "u_env");
  r->u_prim_lod = glGetUniformLocation(r->prog, "u_prim_lod");
  r->u_c0 = glGetUniformLocation(r->prog, "u_c0");
  r->u_a0 = glGetUniformLocation(r->prog, "u_a0");
  r->u_c1 = glGetUniformLocation(r->prog, "u_c1");
  r->u_a1 = glGetUniformLocation(r->prog, "u_a1");
  r->u_two_cycle = glGetUniformLocation(r->prog, "u_two_cycle");
  r->u_alpha_test = glGetUniformLocation(r->prog, "u_alpha_test");
  r->u_alpha_ref = glGetUniformLocation(r->prog, "u_alpha_ref");
  /* A white 1x1 texture keeps the sampler complete for untextured draws. */
  static const uint8_t white[4] = {255, 255, 255, 255};
  glGenTextures(1, &r->white);
  glBindTexture(GL_TEXTURE_2D, r->white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               white);
  return 1;
}

static void drop_textures(GlRenderer *r) {
  for (uint32_t i = 0; i < r->tex_cap; i++)
    if (r->tex_ids[i])
      glDeleteTextures(1, &r->tex_ids[i]);
  memset(r->tex_ids, 0, sizeof(unsigned) * r->tex_cap);
}

void gl_free(GlRenderer *r) {
  drop_textures(r);
  free(r->tex_ids);
  if (r->white)
    glDeleteTextures(1, &r->white);
  if (r->prog)
    glDeleteProgram(r->prog);
  memset(r, 0, sizeof(*r));
}

static const GLint wraps[3] = {GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE};

static unsigned texture_name(GlRenderer *r, const Gfx *g, uint32_t i) {
  if (r->generation != g->cache_generation) {
    drop_textures(r);
    r->generation = g->cache_generation;
  }
  if (i >= r->tex_cap) {
    uint32_t cap = r->tex_cap ? r->tex_cap : 256;
    while (cap <= i)
      cap *= 2;
    r->tex_ids = realloc(r->tex_ids, sizeof(unsigned) * cap);
    memset(r->tex_ids + r->tex_cap, 0, sizeof(unsigned) * (cap - r->tex_cap));
    r->tex_cap = cap;
  }
  if (!r->tex_ids[i]) {
    const GfxTexture *t = &g->textures[i];
    glGenTextures(1, &r->tex_ids[i]);
    glBindTexture(GL_TEXTURE_2D, r->tex_ids[i]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->w, t->h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, gfx_texels(g, i));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wraps[t->wrap_s]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wraps[t->wrap_t]);
  }
  return r->tex_ids[i];
}

void gl_render(GlRenderer *r, const Gfx *g) {
  glViewport(0, 0, r->width, r->height);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_TRUE);
  glClearColor(0, 0, 0, 1);
  glClearDepth(1.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDisable(GL_CULL_FACE);
  glEnable(GL_SCISSOR_TEST);
  glUseProgram(r->prog);
  glUniform1i(r->u_tex, 0);
  glActiveTexture(GL_TEXTURE0);

  const GfxVertex *v = g->verts;
  GLsizei stride = sizeof(GfxVertex);
  if (g->nverts) {
    glEnableVertexAttribArray((GLuint)r->loc_pos);
    glEnableVertexAttribArray((GLuint)r->loc_col);
    glEnableVertexAttribArray((GLuint)r->loc_uv);
    glVertexAttribPointer((GLuint)r->loc_pos, 4, GL_FLOAT, GL_FALSE, stride,
                          &v->x);
    glVertexAttribPointer((GLuint)r->loc_col, 4, GL_FLOAT, GL_FALSE, stride,
                          &v->r);
    glVertexAttribPointer((GLuint)r->loc_uv, 2, GL_FLOAT, GL_FALSE, stride,
                          &v->u);
  }
  float sx = (float)r->width / g->screen_w, sy = (float)r->height / g->screen_h;
  for (uint32_t i = 0; i < g->ndraws; i++) {
    const GfxDraw *d = &g->draws[i];
    if (d->kind == GD_CLEAR_DEPTH) {
      glDisable(GL_SCISSOR_TEST);
      glDepthMask(GL_TRUE);
      glClear(GL_DEPTH_BUFFER_BIT);
      glEnable(GL_SCISSOR_TEST);
      continue;
    }
    const GfxState *s = &g->states[d->state];
    int x0 = (int)(s->scissor[0] * sx), x1 = (int)(s->scissor[2] * sx);
    int y0 = (int)(s->scissor[1] * sy), y1 = (int)(s->scissor[3] * sy);
    glScissor(x0, r->height - y1, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
    if (s->flags & (GS_DEPTH_TEST | GS_DEPTH_WRITE)) {
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(s->flags & GS_DEPTH_TEST ? GL_LEQUAL : GL_ALWAYS);
    } else {
      glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(s->flags & GS_DEPTH_WRITE ? GL_TRUE : GL_FALSE);
    if (s->flags & GS_DECAL) {
      glEnable(GL_POLYGON_OFFSET_FILL);
      glPolygonOffset(-1.0f, -2.0f);
    } else {
      glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (s->flags & GS_BLEND) {
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
      glDisable(GL_BLEND);
    }
    int textured = (s->flags & GS_TEXTURED) && s->texture >= 0;
    if (textured) {
      glBindTexture(GL_TEXTURE_2D, texture_name(r, g, (uint32_t)s->texture));
      GLint f = s->flags & GS_FILTER ? GL_LINEAR : GL_NEAREST;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    } else {
      glBindTexture(GL_TEXTURE_2D, r->white);
    }
    glUniform1i(r->u_textured, textured);
    glUniform4fv(r->u_prim, 1, s->prim);
    glUniform4fv(r->u_env, 1, s->env);
    glUniform1f(r->u_prim_lod, s->prim_lod);
    glUniform4i(r->u_c0, s->comb[0], s->comb[1], s->comb[2], s->comb[3]);
    glUniform4i(r->u_a0, s->comb[4], s->comb[5], s->comb[6], s->comb[7]);
    glUniform4i(r->u_c1, s->comb[8], s->comb[9], s->comb[10], s->comb[11]);
    glUniform4i(r->u_a1, s->comb[12], s->comb[13], s->comb[14], s->comb[15]);
    glUniform1i(r->u_two_cycle, (s->flags & GS_TWO_CYCLE) != 0);
    glUniform1i(r->u_alpha_test, (s->flags & GS_ALPHA_TEST) != 0);
    glUniform1f(r->u_alpha_ref, s->alpha_ref);
    glDrawArrays(GL_TRIANGLES, (GLint)d->first, (GLsizei)d->count);
  }
  glDisable(GL_SCISSOR_TEST);
}

void gl_read_rgba(const GlRenderer *r, uint8_t *out) {
  size_t row = (size_t)r->width * 4;
  uint8_t *tmp = malloc(row * (size_t)r->height);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, r->width, r->height, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
  for (int y = 0; y < r->height; y++)
    memcpy(out + (size_t)y * row, tmp + (size_t)(r->height - 1 - y) * row, row);
  free(tmp);
}
