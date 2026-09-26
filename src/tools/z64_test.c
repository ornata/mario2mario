/* Header and layout checks against the real SM64 (USA) image. */
#include <stdio.h>
#include <string.h>

#include "src/tools/rom_env.h"
#include "src/tools/z64.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                              \
    }                                                                          \
  } while (0)

int main(void) {
  Z64Rom rom;
  if (!rom_env_open(&rom))
    return 1;

  const Z64Header *h = &rom.header;
  CHECK(rom.size == 8u * 1024 * 1024);
  CHECK(h->magic == Z64_MAGIC);
  CHECK(h->entry_pc == 0x80246000u);
  CHECK(strcmp(h->name, "SUPER MARIO 64") == 0);
  CHECK(h->media == 'N');
  CHECK(strcmp(h->game_id, "SM") == 0);
  CHECK(h->region == 'E');
  CHECK(h->version == 0);
  CHECK(h->crc1 == 0x635A2BFFu);
  CHECK(h->crc2 == 0x8B022326u);

  /* IPL3 sits directly after the header and ends where game data begins. */
  CHECK(z64_ipl3(&rom) == rom.data + 0x40);
  CHECK(z64_cart_data(&rom) == rom.data + 0x1000);
  CHECK(z64_cart_data_size(&rom) == rom.size - 0x1000);
  /* First game-code word at the entry point: lui $t0, 0x8034. */
  CHECK(z64_be32(z64_cart_data(&rom)) == 0x3C088034u);

  printf("%s region=%c entry=0x%08X crc=%08X/%08X size=%zu\n", h->name,
         h->region, h->entry_pc, h->crc1, h->crc2, rom.size);
  z64_close(&rom);
  return failures ? 1 : 0;
}
