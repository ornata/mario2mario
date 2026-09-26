/* chunker: listing -> translation units, as JSON lines on stdout.
 *
 * Usage: chunker [--target N] [listing.txt]   (default N = 200; stdin
 * when no file is given). The input is //tools:listing output for one
 * contiguous range. Each output line is one unit:
 *
 *   {"unit","start","end" (exclusive),"insns",
 *    "entries"          unit start + addresses targeted from other units,
 *    "return_points"    addresses after the delay slot of jal/jalr/b*al,
 *    "external_targets" direct branch/jump targets outside the unit,
 *    "jr_sites"         jr/jalr addresses,
 *    "blocks":[{"start","insns","data_words","targeted",
 *               "exit":{"kind", "at", "target", "in_range", "next",
 *                       "likely", "link", "delay_slot_cti"}}]}
 *
 * exit.kind is one of fallthrough, branch, jump, jal, jr, jalr, eret,
 * end. Structural only: nothing here interprets what the code does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/tools/chunk.h"
#include "src/tools/mips.h"

static char *read_all(FILE *f, size_t *len) {
  size_t cap = 1 << 20, n = 0;
  char *buf = malloc(cap);
  for (;;) {
    n += fread(buf + n, 1, cap - n, f);
    if (n < cap)
      break;
    cap *= 2;
    buf = realloc(buf, cap);
  }
  *len = n;
  return buf;
}

int main(int argc, char **argv) {
  unsigned long target = 200;
  const char *path = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
      char *end;
      target = strtoul(argv[++i], &end, 0);
      if (*end || target == 0 || target > 1000000) {
        fprintf(stderr, "bad --target\n");
        return 2;
      }
    } else if (!path && argv[i][0] != '-') {
      path = argv[i];
    } else {
      fprintf(stderr, "usage: %s [--target N] [listing.txt]\n", argv[0]);
      return 2;
    }
  }

  char pathbuf[4096];
  const char *wd = getenv("BUILD_WORKING_DIRECTORY");
  if (path && path[0] != '/' && wd && *wd) {
    snprintf(pathbuf, sizeof(pathbuf), "%s/%s", wd, path);
    path = pathbuf;
  }
  FILE *in = path ? fopen(path, "rb") : stdin;
  if (!in) {
    perror(path);
    return 1;
  }
  size_t len;
  char *text = read_all(in, &len);
  if (path)
    fclose(in);

  mips_init();
  Listing l;
  char err[256];
  if (!listing_parse(text, len, &l, err, sizeof(err))) {
    fprintf(stderr, "%s: %s\n", path ? path : "<stdin>", err);
    free(text);
    return 1;
  }
  free(text);

  Chunks c;
  chunk_build(&l, (uint32_t)target, &c);
  chunks_write_jsonl(stdout, &l, &c);
  chunks_free(&c);
  listing_free(&l);
  return 0;
}
