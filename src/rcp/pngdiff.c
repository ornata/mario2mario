/* pngdiff: tolerance compare of two RGBA PNGs (as written by png.c).
 *
 *   pngdiff A.png B.png [--max-mean M] [--max-bad F] [--bad-delta D]
 *
 * Passes when the mean absolute channel difference is <= M (default 2.0)
 * and the fraction of pixels with any channel differing by more than D
 * (default 32) is <= F (default 0.01). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/rcp/png.h"

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: %s A.png B.png [--max-mean M] [--max-bad F] "
            "[--bad-delta D]\n",
            argv[0]);
    return 2;
  }
  double max_mean = 2.0, max_bad = 0.01;
  int bad_delta = 32;
  for (int i = 3; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--max-mean"))
      max_mean = atof(argv[i + 1]);
    else if (!strcmp(argv[i], "--max-bad"))
      max_bad = atof(argv[i + 1]);
    else if (!strcmp(argv[i], "--bad-delta"))
      bad_delta = atoi(argv[i + 1]);
  }
  uint32_t wa, ha, wb, hb;
  uint8_t *a = png_read(argv[1], &wa, &ha), *b = png_read(argv[2], &wb, &hb);
  if (!a || !b || wa != wb || ha != hb) {
    fprintf(stderr, "cannot read or size mismatch\n");
    return 1;
  }
  uint64_t sum = 0, bad = 0, n = (uint64_t)wa * ha;
  for (uint64_t p = 0; p < n; p++) {
    int worst = 0;
    for (int c = 0; c < 3; c++) {
      int d = abs((int)a[4 * p + c] - (int)b[4 * p + c]);
      sum += (uint64_t)d;
      if (d > worst)
        worst = d;
    }
    bad += worst > bad_delta;
  }
  double mean = (double)sum / (double)(n * 3), frac = (double)bad / (double)n;
  printf("mean abs diff %.3f, bad pixels %.4f%%\n", mean, 100.0 * frac);
  free(a);
  free(b);
  return mean <= max_mean && frac <= max_bad ? 0 : 1;
}
