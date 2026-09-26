/* .rec save/load round trip and pad-hook replay/record semantics. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/rcp/input.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

int main(void) {
  char path[4096];
  const char *tmp = getenv("TEST_TMPDIR");
  snprintf(path, sizeof(path), "%s/t.rec", tmp ? tmp : ".");

  Rec r = {0};
  rec_push(&r, (RecFrame){BTN_A | BTN_START, -80, 127});
  rec_push(&r, (RecFrame){BTN_CR, 5, -128});
  CHECK(rec_save(&r, path));

  /* Header + two little-endian records, byte for byte. */
  FILE *f = fopen(path, "rb");
  uint8_t bytes[32];
  size_t n = fread(bytes, 1, sizeof(bytes), f);
  fclose(f);
  static const uint8_t want[24] = {
      'M', '2', 'M', 'R', 'E',  'C',  0,    0,    1,    0,    0,    0,
      2,   0,   0,   0,   0x00, 0x90, 0xB0, 0x7F, 0x01, 0x00, 0x05, 0x80};
  CHECK(n == 24 && memcmp(bytes, want, 24) == 0);

  Rec back;
  CHECK(rec_load(&back, path));
  CHECK(back.n == 2 && back.frames[0].buttons == (BTN_A | BTN_START) &&
        back.frames[0].x == -80 && back.frames[0].y == 127 &&
        back.frames[1].buttons == BTN_CR && back.frames[1].y == -128);

  /* Replay consumes one record per poll, then reads released; record
   * captures exactly what the game saw. */
  HwState *hw = calloc(1, sizeof(HwState));
  Rec rec_out = {0};
  InputState in = {&back, &rec_out, {BTN_B, 1, 1}, 0};
  input_pad_hook(&in, hw, 0);
  CHECK(hw->pad[0].buttons == (BTN_A | BTN_START) && hw->pad[0].stick_x == -80);
  input_pad_hook(&in, hw, 1); /* other ports are ignored */
  input_pad_hook(&in, hw, 0);
  CHECK(hw->pad[0].buttons == BTN_CR);
  input_pad_hook(&in, hw, 0);
  CHECK(hw->pad[0].buttons == 0 && hw->pad[0].stick_x == 0);
  CHECK(in.polls == 3 && rec_out.n == 3);
  in.replay = NULL;
  input_pad_hook(&in, hw, 0);
  CHECK(hw->pad[0].buttons == BTN_B); /* live input without a replay */

  rec_free(&r);
  rec_free(&back);
  rec_free(&rec_out);
  free(hw);
  printf("%d failures\n", failures);
  return failures ? 1 : 0;
}
