#include "src/rcp/input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kMagic = "M2MREC\0";

void rec_push(Rec *r, RecFrame f) {
  if (r->n == r->cap) {
    r->cap = r->cap ? r->cap * 2 : 1024;
    r->frames = realloc(r->frames, sizeof(RecFrame) * r->cap);
  }
  r->frames[r->n++] = f;
}

void rec_free(Rec *r) {
  free(r->frames);
  memset(r, 0, sizeof(*r));
}

static void put_le32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

int rec_save(const Rec *r, const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return 0;
  uint8_t hdr[16] = {0};
  memcpy(hdr, kMagic, 8);
  put_le32(hdr + 8, 1);
  put_le32(hdr + 12, r->n);
  fwrite(hdr, 1, 16, f);
  for (uint32_t i = 0; i < r->n; i++) {
    uint8_t rec[4] = {(uint8_t)r->frames[i].buttons,
                      (uint8_t)(r->frames[i].buttons >> 8),
                      (uint8_t)r->frames[i].x, (uint8_t)r->frames[i].y};
    fwrite(rec, 1, 4, f);
  }
  return fclose(f) == 0;
}

int rec_load(Rec *r, const char *path) {
  memset(r, 0, sizeof(*r));
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  uint8_t hdr[16];
  if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, kMagic, 8) != 0 ||
      get_le32(hdr + 8) != 1) {
    fclose(f);
    return 0;
  }
  uint32_t n = get_le32(hdr + 12);
  for (uint32_t i = 0; i < n; i++) {
    uint8_t rec[4];
    if (fread(rec, 1, 4, f) != 4) {
      fclose(f);
      rec_free(r);
      return 0;
    }
    rec_push(r, (RecFrame){(uint16_t)(rec[0] | rec[1] << 8), (int8_t)rec[2],
                           (int8_t)rec[3]});
  }
  fclose(f);
  return 1;
}

void input_pad_hook(void *user, HwState *hw, unsigned port) {
  InputState *in = user;
  if (port != 0)
    return;
  RecFrame f = in->live;
  if (in->replay)
    f = in->replay->pos < in->replay->n ? in->replay->frames[in->replay->pos++]
                                        : (RecFrame){0, 0, 0};
  if (in->record)
    rec_push(in->record, f);
  in->polls++;
  hw->pad[0].buttons = f.buttons;
  hw->pad[0].stick_x = f.x;
  hw->pad[0].stick_y = f.y;
}
