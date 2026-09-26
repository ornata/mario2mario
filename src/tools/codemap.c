/* codemap: executed-word capture -> contiguous code listings.
 *
 *   codemap EXEC_WORDS.txt OUT_DIR
 *
 * Input is //oracle:run --exec-words output (`vaddr word first_icount`,
 * hex, sorted by vaddr then first_icount). Each address may hold several
 * versions over time (code overwritten by DMA or by the CPU); version k
 * of an address is its k-th distinct word in order of first execution.
 * A run is a maximal sequence of consecutive addresses (step 4) with the
 * same version index; each run is written as OUT_DIR/<vaddr>_v<k>.lst in
 * //tools:listing format, ready for //tools:chunker. Purely mechanical:
 * nothing here looks at what the code does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/tools/mips.h"

typedef struct {
  uint32_t vaddr, word, version;
} Row;

static int cmp_version_addr(const void *a, const void *b) {
  const Row *x = a, *y = b;
  if (x->version != y->version)
    return x->version < y->version ? -1 : 1;
  return x->vaddr < y->vaddr ? -1 : x->vaddr > y->vaddr;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s EXEC_WORDS.txt OUT_DIR\n", argv[0]);
    return 2;
  }
  FILE *f = fopen(argv[1], "r");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  Row *rows = NULL;
  uint32_t n = 0, cap = 0, prev_vaddr = 0, version = 0;
  unsigned vaddr, word;
  unsigned long long first;
  while (fscanf(f, "%x %x %llx", &vaddr, &word, &first) == 3) {
    if (n == cap) {
      cap = cap ? cap * 2 : 4096;
      rows = realloc(rows, sizeof(Row) * cap);
    }
    version = n && vaddr == prev_vaddr ? version + 1 : 0;
    rows[n++] = (Row){vaddr, word, version};
    prev_vaddr = vaddr;
  }
  fclose(f);
  qsort(rows, n, sizeof(Row), cmp_version_addr);

  mips_init();
  char text[MIPS_TEXT_MAX], path[4096];
  uint32_t runs = 0;
  for (uint32_t i = 0; i < n;) {
    uint32_t j = i + 1;
    while (j < n && rows[j].version == rows[i].version &&
           rows[j].vaddr == rows[j - 1].vaddr + 4)
      j++;
    snprintf(path, sizeof(path), "%s/%08X_v%u.lst", argv[2], rows[i].vaddr,
             rows[i].version);
    FILE *out = fopen(path, "w");
    if (!out) {
      perror(path);
      return 1;
    }
    for (uint32_t k = i; k < j; k++) {
      mips_format(rows[k].word, mips_decode(rows[k].word), rows[k].vaddr, text);
      fprintf(out, "%08X  %08X  %s\n", rows[k].vaddr, rows[k].word, text);
    }
    fclose(out);
    runs++;
    i = j;
  }
  printf("%u words in %u runs -> %s\n", n, runs, argv[2]);
  free(rows);
  return 0;
}
