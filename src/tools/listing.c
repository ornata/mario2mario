/* listing: ROM byte range -> MIPS listing, one word per line:
 *
 *   ADDR  HEXWORD  mnemonic operands
 *
 * Usage: listing <rom.z64> <rom_start> <rom_end> <ram_base>
 * Numbers accept C prefixes (0x...). [rom_start, rom_end) must be
 * word-aligned and inside the ROM; ram_base is the address of rom_start.
 * Words that are not valid instructions print as `.word 0xXXXXXXXX`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/tools/mips.h"
#include "src/tools/z64.h"

static int parse_u32(const char *s, uint32_t *out) {
  char *end;
  unsigned long long v = strtoull(s, &end, 0);
  if (!*s || *end || v > 0xFFFFFFFFull)
    return 0;
  *out = (uint32_t)v;
  return 1;
}

/* Under `bazel run` the cwd is the runfiles tree; resolve relative paths
 * against the directory the user invoked bazel from. */
static const char *resolve_path(const char *path, char *buf, size_t cap) {
  const char *wd = getenv("BUILD_WORKING_DIRECTORY");
  if (path[0] == '/' || !wd || !*wd)
    return path;
  snprintf(buf, cap, "%s/%s", wd, path);
  return buf;
}

int main(int argc, char **argv) {
  uint32_t start, end, base;
  if (argc != 5 || !parse_u32(argv[2], &start) || !parse_u32(argv[3], &end) ||
      !parse_u32(argv[4], &base)) {
    fprintf(stderr, "usage: %s <rom.z64> <rom_start> <rom_end> <ram_base>\n",
            argv[0]);
    return 2;
  }
  char pathbuf[4096];
  const char *path = resolve_path(argv[1], pathbuf, sizeof(pathbuf));

  Z64Rom rom;
  Z64Status st = z64_open(path, &rom);
  if (st != Z64_OK) {
    fprintf(stderr, "%s: %s\n", path, z64_status_str(st));
    return 1;
  }
  if ((start | end) & 3u || start > end || end > rom.size) {
    fprintf(stderr, "bad range [0x%X, 0x%X) for ROM of 0x%zX bytes\n", start,
            end, rom.size);
    z64_close(&rom);
    return 2;
  }

  mips_init();
  char text[MIPS_TEXT_MAX];
  for (uint32_t off = start; off < end; off += 4) {
    uint32_t addr = base + (off - start);
    uint32_t word = z64_be32(rom.data + off);
    mips_format(word, mips_decode(word), addr, text);
    printf("%08X  %08X  %s\n", addr, word, text);
  }
  z64_close(&rom);
  return 0;
}
