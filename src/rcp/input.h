/* Controller input: recorded .rec streams and the SI pad hook.
 *
 * .rec format (all little-endian):
 *   header, 16 bytes: magic "M2MREC\0\0" (8), u32 version = 1,
 *                     u32 record count
 *   records, 4 bytes each: u16 buttons, s8 stick_x, s8 stick_y
 * One record is consumed per controller-read command the PIF executes
 * for port 0 (i.e. per SI poll), so replay is exact regardless of wall
 * clock. Past the end of a recording the pad reads as released.
 *
 * Button bits are the N64 controller's: A 0x8000, B 0x4000, Z 0x2000,
 * START 0x1000, D-pad U/D/L/R 0x0800/0x0400/0x0200/0x0100, L 0x0020,
 * R 0x0010, C U/D/L/R 0x0008/0x0004/0x0002/0x0001. */
#ifndef M2M_RCP_INPUT_H
#define M2M_RCP_INPUT_H

#include <stdint.h>

#include "src/hw/hw.h"

enum {
  BTN_A = 0x8000,
  BTN_B = 0x4000,
  BTN_Z = 0x2000,
  BTN_START = 0x1000,
  BTN_DU = 0x0800,
  BTN_DD = 0x0400,
  BTN_DL = 0x0200,
  BTN_DR = 0x0100,
  BTN_L = 0x0020,
  BTN_R = 0x0010,
  BTN_CU = 0x0008,
  BTN_CD = 0x0004,
  BTN_CL = 0x0002,
  BTN_CR = 0x0001,
};

typedef struct {
  uint16_t buttons;
  int8_t x, y;
} RecFrame;

typedef struct {
  RecFrame *frames;
  uint32_t n, cap, pos;
} Rec;

int rec_load(Rec *r, const char *path);
int rec_save(const Rec *r, const char *path);
void rec_push(Rec *r, RecFrame f);
void rec_free(Rec *r);

/* Pad source for port 0. `live` is refreshed by the caller (keyboard);
 * with `replay` set, records replace it; with `record` set, every value
 * reported to the game is appended. */
typedef struct {
  Rec *replay;
  Rec *record;
  RecFrame live;
  uint64_t polls;
} InputState;

/* HwPadHook implementation; `user` is an InputState. */
void input_pad_hook(void *user, HwState *hw, unsigned port);

#endif
