/* Bijectivity over the whole ROM: every 32-bit word is decoded, rendered
 * as text, parsed back and re-encoded; the result must equal the
 * original word exactly. Words that are not instructions render as
 * `.word 0xXXXXXXXX` and must round-trip through the same parser.
 *
 * Word i sits at RAM 0x80245000 + 4*i, i.e. ROM 0x1000 maps to the entry
 * point 0x80246000; branch and jump targets are encoded relative to it. */
#include <stdio.h>

#include "src/tools/mips.h"
#include "src/tools/rom_env.h"
#include "src/tools/z64.h"

#define RAM_OF_ROM0 0x80245000u

int main(void) {
  Z64Rom rom;
  if (!rom_env_open(&rom))
    return 1;
  mips_init();

  size_t words = rom.size / 4, decoded = 0, failures = 0;
  char text[MIPS_TEXT_MAX];
  for (size_t i = 0; i < words; i++) {
    uint32_t addr = RAM_OF_ROM0 + (uint32_t)(i * 4);
    uint32_t word = z64_be32(rom.data + i * 4), back = 0;
    uint16_t op = mips_decode(word);
    decoded += op != MIPS_OP_INVALID;
    mips_format(word, op, addr, text);
    if (!mips_assemble(text, addr, &back) || back != word) {
      if (failures++ < 20)
        fprintf(stderr, "%08X  %08X  \"%s\" -> %08X\n", addr, word, text, back);
    }
  }

  printf("%zu words: %zu decoded as instructions, %zu as .word, "
         "%zu round-trip failures\n",
         words, decoded, words - decoded, failures);
  z64_close(&rom);
  return failures || words != 2097152 ? 1 : 0;
}
