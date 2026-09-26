/* Shared by ROM-reading tests: the ROM path comes from $M2M_ROM, which
 * BUILD files set from @rom//:defs.bzl (see bazel/rom.bzl). */
#ifndef M2M_TOOLS_ROM_ENV_H
#define M2M_TOOLS_ROM_ENV_H

#include <stdio.h>
#include <stdlib.h>

#include "src/tools/z64.h"

static inline int rom_env_open(Z64Rom *rom) {
  const char *path = getenv("M2M_ROM");
  if (!path || !*path) {
    fprintf(stderr, "M2M_ROM is not set\n");
    return 0;
  }
  Z64Status s = z64_open(path, rom);
  if (s != Z64_OK) {
    fprintf(stderr, "%s: %s\n", path, z64_status_str(s));
    return 0;
  }
  return 1;
}

#endif
