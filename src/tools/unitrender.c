/* unitrender: codemap listings -> per-unit worker input blocks.
 *
 *   unitrender RUNS_DIR OUT_DIR
 *
 * Pure text formatting for the translation workers. Reads every
 * <vaddr>_v<k>.lst run written by //tools:codemap, splits runs longer
 * than 250 instructions at chunker block boundaries (~200 instructions),
 * packs consecutive pieces of the same version into units of about 200
 * instructions, and writes OUT_DIR/<id>.in.txt containing:
 *
 *   - the unit's listing (runs separated by "--- gap ---"),
 *   - the file header and footer the worker copies verbatim: the unit's
 *     global symbol, and a table with one (pc, word, entry offset) row per
 *     instruction (-1 for delay slots, recognised structurally from the
 *     decoder's flow flags) plus the descriptor the runtime finds through
 *     the __DATA,__m2m_units section.
 *
 * It also prints one "id instructions runs" line per unit. No invocation
 * logic, no semantics. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/tools/chunk.h"
#include "src/tools/mips.h"

typedef struct {
  uint32_t version, start, n;
  uint32_t *addr, *word;
} Piece;

static int cmp_piece(const void *a, const void *b) {
  const Piece *x = a, *y = b;
  if (x->version != y->version)
    return x->version < y->version ? -1 : 1;
  return x->start < y->start ? -1 : x->start > y->start;
}

static char *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  char *buf = malloc((size_t)n + 1);
  *len = fread(buf, 1, (size_t)n, f);
  fclose(f);
  return buf;
}

static Piece *pieces;
static uint32_t npieces, cap_pieces;

static void add_piece(uint32_t version, const Listing *l, uint32_t first,
                      uint32_t count) {
  if (npieces == cap_pieces) {
    cap_pieces = cap_pieces ? cap_pieces * 2 : 256;
    pieces = realloc(pieces, sizeof(Piece) * cap_pieces);
  }
  Piece *p = &pieces[npieces++];
  p->version = version;
  p->start = l->base + 4u * first;
  p->n = count;
  p->addr = malloc(sizeof(uint32_t) * count);
  p->word = malloc(sizeof(uint32_t) * count);
  for (uint32_t i = 0; i < count; i++) {
    p->addr[i] = l->base + 4u * (first + i);
    p->word[i] = l->word[first + i];
  }
}

static void write_unit(const char *dir, Piece *const *ps, uint32_t np) {
  uint32_t n = 0;
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t k = 0; k < np; k++)
    for (uint32_t i = 0; i < ps[k]->n; i++) {
      h = (h ^ ps[k]->word[i]) * 0x100000001B3ull;
      n++;
    }
  char id[32], path[4096], text[MIPS_TEXT_MAX];
  snprintf(id, sizeof(id), "%08X_%08X", ps[0]->start, (uint32_t)(h ^ h >> 32));
  snprintf(path, sizeof(path), "%s/%s.in.txt", dir, id);
  FILE *f = fopen(path, "w");
  if (!f) {
    perror(path);
    exit(1);
  }
  fprintf(f, "UNIT %s\n", id);
  fprintf(f, "WRITE gen/units/unit_%s.s and gen/units/unit_%s.json\n", id, id);
  fprintf(f, "=== LISTING ===\n");
  for (uint32_t k = 0; k < np; k++) {
    if (k)
      fprintf(f, "--- gap ---\n");
    for (uint32_t i = 0; i < ps[k]->n; i++) {
      uint32_t w = ps[k]->word[i], a = ps[k]->addr[i];
      mips_format(w, mips_decode(w), a, text);
      fprintf(f, "%08X  %08X  %s\n", a, w, text);
    }
  }
  fprintf(f, "=== HEADER (copy verbatim to the top of the .s file) ===\n");
  fprintf(f,
          "// Translation unit %s (%u MIPS instructions).\n"
          ".include \"src/native/m2m.inc\"\n"
          ".text\n"
          ".p2align 2\n"
          ".globl _m2m_unit_%s\n"
          "_m2m_unit_%s:\n",
          id, n, id, id);
  fprintf(f, "=== FOOTER (copy verbatim to the end of the .s file) ===\n");
  fprintf(f, ".section __TEXT,__const\n.p2align 2\n_m2m_tab_%s:\n", id);
  for (uint32_t k = 0; k < np; k++)
    for (uint32_t i = 0; i < ps[k]->n; i++) {
      uint32_t a = ps[k]->addr[i], w = ps[k]->word[i];
      int delay = 0;
      if (i > 0) {
        uint16_t op = mips_decode(ps[k]->word[i - 1]);
        delay = op != MIPS_OP_INVALID && (mips_ops[op].flow & MFLOW_DELAY);
      }
      if (delay)
        fprintf(f, "    .long 0x%08X, 0x%08X, -1\n", a, w);
      else
        fprintf(f, "    .long 0x%08X, 0x%08X, L_%08X - _m2m_unit_%s\n", a, w, a,
                id);
    }
  fprintf(f,
          ".section __DATA,__m2m_units\n"
          ".p2align 3\n"
          "    .quad _m2m_unit_%s\n"
          "    .quad _m2m_tab_%s\n"
          "    .quad %u\n",
          id, id, n);
  fprintf(f, "=== END ===\n");
  fclose(f);
  printf("%s %u %u\n", id, n, np);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s RUNS_DIR OUT_DIR\n", argv[0]);
    return 2;
  }
  mips_init();
  DIR *d = opendir(argv[1]);
  if (!d) {
    perror(argv[1]);
    return 1;
  }
  struct dirent *e;
  char path[4096], err[256];
  while ((e = readdir(d))) {
    const char *v = strstr(e->d_name, "_v");
    if (!v || !strstr(e->d_name, ".lst"))
      continue;
    uint32_t version = (uint32_t)strtoul(v + 2, NULL, 10);
    snprintf(path, sizeof(path), "%s/%s", argv[1], e->d_name);
    size_t len;
    char *text = slurp(path, &len);
    Listing l;
    if (!text || !listing_parse(text, len, &l, err, sizeof(err))) {
      fprintf(stderr, "%s: %s\n", path, text ? err : "unreadable");
      return 1;
    }
    free(text);
    if (l.n <= 250) {
      add_piece(version, &l, 0, l.n);
    } else {
      Chunks c;
      chunk_build(&l, 200, &c);
      for (uint32_t u = 0; u < c.unit_count; u++)
        add_piece(version, &l, c.units[u].first, c.units[u].count);
      chunks_free(&c);
    }
    listing_free(&l);
  }
  closedir(d);
  qsort(pieces, npieces, sizeof(Piece), cmp_piece);

  Piece **cur = malloc(sizeof(Piece *) * (npieces ? npieces : 1));
  uint32_t ncur = 0, count = 0;
  for (uint32_t i = 0; i < npieces; i++) {
    Piece *p = &pieces[i];
    if (ncur &&
        (p->version != cur[0]->version || count + p->n > 250 || count >= 200)) {
      write_unit(argv[2], cur, ncur);
      ncur = count = 0;
    }
    cur[ncur++] = p;
    count += p->n;
  }
  if (ncur)
    write_unit(argv[2], cur, ncur);
  return 0;
}
