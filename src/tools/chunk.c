#include "src/tools/chunk.h"

#include <stdlib.h>
#include <string.h>

#include "src/tools/mips.h"

const char *const exit_kind_names[EXIT_COUNT] = {
    [EXIT_FALLTHROUGH] = "fallthrough",
    [EXIT_BRANCH] = "branch",
    [EXIT_JUMP] = "jump",
    [EXIT_JAL] = "jal",
    [EXIT_JR] = "jr",
    [EXIT_JALR] = "jalr",
    [EXIT_ERET] = "eret",
    [EXIT_END] = "end",
};

/* --- Listing parsing -------------------------------------------------- */

static int hex8(const char *s, uint32_t *out) {
  uint32_t v = 0;
  for (int i = 0; i < 8; i++) {
    char c = s[i];
    uint32_t d;
    if (c >= '0' && c <= '9')
      d = (uint32_t)(c - '0');
    else if (c >= 'A' && c <= 'F')
      d = (uint32_t)(c - 'A' + 10);
    else
      return 0;
    v = v << 4 | d;
  }
  *out = v;
  return 1;
}

int listing_parse(const char *text, size_t len, Listing *out, char *err,
                  size_t err_cap) {
  memset(out, 0, sizeof(*out));
  uint32_t lines = 0;
  for (size_t i = 0; i < len; i++)
    lines += text[i] == '\n';
  lines += len && text[len - 1] != '\n';

  out->word = malloc(sizeof(uint32_t) * (lines ? lines : 1));
  out->op = malloc(sizeof(uint16_t) * (lines ? lines : 1));
  out->targeted = calloc(lines ? lines : 1, 1);

  char want[MIPS_TEXT_MAX];
  const char *p = text, *end = text + len;
  for (uint32_t lineno = 1; p < end; lineno++) {
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    if (!eol)
      eol = end;
    size_t n = (size_t)(eol - p);
    if (n == 0) {
      p = eol + 1;
      continue;
    }
    uint32_t addr, word;
    if (n < 20 || !hex8(p, &addr) || p[8] != ' ' || p[9] != ' ' ||
        !hex8(p + 10, &word) || p[18] != ' ' || p[19] != ' ') {
      snprintf(err, err_cap, "line %u: expected 'ADDR  HEXWORD  text'", lineno);
      listing_free(out);
      return 0;
    }
    if (out->n == 0)
      out->base = addr;
    if (addr != out->base + 4u * out->n) {
      snprintf(err, err_cap, "line %u: address %08X is not contiguous", lineno,
               addr);
      listing_free(out);
      return 0;
    }
    uint16_t op = mips_decode(word);
    size_t wn = mips_format(word, op, addr, want);
    if (wn != n - 20 || memcmp(want, p + 20, wn) != 0) {
      snprintf(err, err_cap, "line %u: text does not match decode of %08X",
               lineno, word);
      listing_free(out);
      return 0;
    }
    out->word[out->n] = word;
    out->op[out->n] = op;
    out->n++;
    p = eol + 1;
  }
  return 1;
}

void listing_free(Listing *l) {
  free(l->word);
  free(l->op);
  free(l->targeted);
  memset(l, 0, sizeof(*l));
}

/* --- Blocks and units ------------------------------------------------- */

static uint8_t flow_of(const Listing *l, uint32_t i) {
  return l->op[i] == MIPS_OP_INVALID ? 0 : mips_ops[l->op[i]].flow;
}

static int is_direct(uint8_t flow) {
  return (flow & (MFLOW_BRANCH | MFLOW_JUMP)) != 0;
}

static uint32_t target_of(const Listing *l, uint32_t i) {
  return mips_target(l->word[i], l->op[i], l->base + 4u * i);
}

/* Index of `addr` in the listing, or UINT32_MAX. */
static uint32_t index_of(const Listing *l, uint32_t addr) {
  uint32_t d = addr - l->base;
  return (d & 3u) || d / 4u >= l->n ? UINT32_MAX : d / 4u;
}

static uint8_t exit_kind(uint8_t flow) {
  if (flow & MFLOW_BRANCH)
    return EXIT_BRANCH;
  if (flow & MFLOW_JUMP)
    return flow & MFLOW_LINK ? EXIT_JAL : EXIT_JUMP;
  if (flow & MFLOW_REG)
    return flow & MFLOW_LINK ? EXIT_JALR : EXIT_JR;
  return EXIT_ERET;
}

static int unconditional(uint8_t exit) {
  return exit == EXIT_JUMP || exit == EXIT_JR || exit == EXIT_ERET;
}

void chunk_build(const Listing *l, uint32_t target, Chunks *out) {
  memset(out, 0, sizeof(*out));
  uint32_t n = l->n;
  uint8_t *leader = calloc(n + 1, 1);

  /* Leaders from structure only; also mark targeted instructions. */
  if (n)
    leader[0] = 1;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t f = flow_of(l, i);
    if (is_direct(f)) {
      uint32_t t = index_of(l, target_of(l, i));
      if (t != UINT32_MAX)
        leader[t] = l->targeted[t] = 1;
    }
    if (f & MFLOW_DELAY)
      leader[i + 2 < n ? i + 2 : n] = 1;
    if (f & MFLOW_ERET)
      leader[i + 1] = 1;
  }

  /* Blocks: scan, closing at a delay slot, eret, or the next leader. */
  out->blocks = malloc(sizeof(Block) * (n ? n : 1));
  for (uint32_t i = 0; i < n;) {
    Block *b = &out->blocks[out->block_count++];
    memset(b, 0, sizeof(*b));
    b->first = i;
    b->exit = EXIT_END;
    for (;;) {
      uint8_t f = flow_of(l, i);
      if (f & (MFLOW_DELAY | MFLOW_ERET)) {
        b->exit_at = i;
        b->flow = f;
        b->exit = exit_kind(f);
        if (is_direct(f)) {
          b->target = target_of(l, i);
          b->target_in_range = index_of(l, b->target) != UINT32_MAX;
        }
        if ((f & MFLOW_DELAY) && i + 1 < n) {
          i++; /* the delay slot belongs to its branch */
          b->delay_slot_cti = (flow_of(l, i) & (MFLOW_DELAY | MFLOW_ERET)) != 0;
        }
        i++;
        break;
      }
      i++;
      if (i >= n)
        break;
      if (leader[i]) {
        b->exit = EXIT_FALLTHROUGH;
        break;
      }
    }
    b->count = i - b->first;
    for (uint32_t k = b->first; k < i; k++)
      b->data_words += l->op[k] == MIPS_OP_INVALID;
  }

  /* Units: whole blocks, greedily. Close once at `target`, or early (at
   * 3/4 target) after an unconditional flow change; never grow past
   * 5/4 target unless a single block is that large. */
  uint32_t soft = target - target / 4, hard = target + target / 4;
  out->units = malloc(sizeof(Unit) * (out->block_count ? out->block_count : 1));
  out->unit_of = malloc(sizeof(uint32_t) * (n ? n : 1));
  Unit *u = NULL;
  for (uint32_t bi = 0; bi < out->block_count; bi++) {
    const Block *b = &out->blocks[bi];
    if (u && u->count + b->count > hard)
      u = NULL;
    if (!u) {
      u = &out->units[out->unit_count++];
      *u = (Unit){bi, 0, b->first, 0};
    }
    u->block_count++;
    u->count += b->count;
    for (uint32_t k = b->first; k < b->first + b->count; k++)
      out->unit_of[k] = out->unit_count - 1;
    if (u->count >= target || (u->count >= soft && unconditional(b->exit)))
      u = NULL;
  }
  free(leader);
}

void chunks_free(Chunks *c) {
  free(c->blocks);
  free(c->units);
  free(c->unit_of);
  memset(c, 0, sizeof(*c));
}

/* --- JSON lines ------------------------------------------------------- */

/* (unit, address) pairs, sorted and de-duplicated, one list per kind. */
typedef struct {
  uint32_t unit, addr;
} Pair;

typedef struct {
  Pair *v;
  uint32_t n, cap, cursor;
} PairList;

static void pair_push(PairList *pl, uint32_t unit, uint32_t addr) {
  if (pl->n == pl->cap) {
    pl->cap = pl->cap ? pl->cap * 2 : 256;
    pl->v = realloc(pl->v, sizeof(Pair) * pl->cap);
  }
  pl->v[pl->n++] = (Pair){unit, addr};
}

static int cmp_pair(const void *a, const void *b) {
  const Pair *x = a, *y = b;
  if (x->unit != y->unit)
    return x->unit < y->unit ? -1 : 1;
  return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static void pair_finish(PairList *pl) {
  qsort(pl->v, pl->n, sizeof(Pair), cmp_pair);
  uint32_t w = 0;
  for (uint32_t r = 0; r < pl->n; r++)
    if (!w || cmp_pair(&pl->v[w - 1], &pl->v[r]) != 0)
      pl->v[w++] = pl->v[r];
  pl->n = w;
}

/* Writes `"key":[...]` for unit `unit`, consuming its pairs. */
static void pair_write(FILE *f, const char *key, PairList *pl, uint32_t unit) {
  fprintf(f, ",\"%s\":[", key);
  for (int first = 1; pl->cursor < pl->n && pl->v[pl->cursor].unit == unit;
       pl->cursor++, first = 0)
    fprintf(f, "%s\"0x%08X\"", first ? "" : ",", pl->v[pl->cursor].addr);
  fputc(']', f);
}

static void write_block(FILE *f, const Listing *l, const Block *b) {
  uint32_t start = l->base + 4u * b->first;
  fprintf(f, "{\"start\":\"0x%08X\",\"insns\":%u,\"data_words\":%u", start,
          b->count, b->data_words);
  fputs(",\"targeted\":[", f);
  for (uint32_t k = b->first, first = 1; k < b->first + b->count; k++)
    if (l->targeted[k]) {
      fprintf(f, "%s\"0x%08X\"", first ? "" : ",", l->base + 4u * k);
      first = 0;
    }
  fprintf(f, "],\"exit\":{\"kind\":\"%s\"", exit_kind_names[b->exit]);
  if (b->exit == EXIT_FALLTHROUGH)
    fprintf(f, ",\"next\":\"0x%08X\"", start + 4u * b->count);
  if (b->exit != EXIT_FALLTHROUGH && b->exit != EXIT_END)
    fprintf(f, ",\"at\":\"0x%08X\"", l->base + 4u * b->exit_at);
  if (is_direct(b->flow))
    fprintf(f, ",\"target\":\"0x%08X\",\"in_range\":%s", b->target,
            b->target_in_range ? "true" : "false");
  if (b->exit == EXIT_BRANCH)
    fprintf(f, ",\"likely\":%s,\"link\":%s",
            b->flow & MFLOW_LIKELY ? "true" : "false",
            b->flow & MFLOW_LINK ? "true" : "false");
  if (b->delay_slot_cti)
    fputs(",\"delay_slot_cti\":true", f);
  fputs("}}", f);
}

void chunks_write_jsonl(FILE *f, const Listing *l, const Chunks *c) {
  /* entries: unit start + addresses targeted from another unit.
   * return_points: addresses a linking instruction returns to.
   * external_targets: direct targets outside the unit.
   * jr_sites: jr/jalr instructions in the unit. */
  PairList entries = {0}, returns = {0}, externals = {0}, jrs = {0};
  for (uint32_t ui = 0; ui < c->unit_count; ui++)
    pair_push(&entries, ui, l->base + 4u * c->units[ui].first);
  for (uint32_t i = 0; i < l->n; i++) {
    uint8_t fl = flow_of(l, i);
    uint32_t ui = c->unit_of[i];
    if (fl & MFLOW_REG)
      pair_push(&jrs, ui, l->base + 4u * i);
    if ((fl & MFLOW_LINK) && i + 2 < l->n)
      pair_push(&returns, c->unit_of[i + 2], l->base + 4u * (i + 2));
    if (!is_direct(fl))
      continue;
    uint32_t t = target_of(l, i), ti = index_of(l, t);
    if (ti == UINT32_MAX || c->unit_of[ti] != ui)
      pair_push(&externals, ui, t);
    if (ti != UINT32_MAX && c->unit_of[ti] != ui)
      pair_push(&entries, c->unit_of[ti], t);
  }
  pair_finish(&entries);
  pair_finish(&returns);
  pair_finish(&externals);
  pair_finish(&jrs);

  for (uint32_t ui = 0; ui < c->unit_count; ui++) {
    const Unit *u = &c->units[ui];
    fprintf(f,
            "{\"unit\":%u,\"start\":\"0x%08X\",\"end\":\"0x%08X\","
            "\"insns\":%u",
            ui, l->base + 4u * u->first, l->base + 4u * (u->first + u->count),
            u->count);
    pair_write(f, "entries", &entries, ui);
    pair_write(f, "return_points", &returns, ui);
    pair_write(f, "external_targets", &externals, ui);
    pair_write(f, "jr_sites", &jrs, ui);
    fputs(",\"blocks\":[", f);
    for (uint32_t bi = 0; bi < u->block_count; bi++) {
      if (bi)
        fputc(',', f);
      write_block(f, l, &c->blocks[u->first_block + bi]);
    }
    fputs("]}\n", f);
  }
  free(entries.v);
  free(returns.v);
  free(externals.v);
  free(jrs.v);
}
