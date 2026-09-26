/* Re-encoder: the inverse of mips_format, driven by the same table.
 *
 * A mnemonic is looked up in a name-sorted index over mips_ops[]; its
 * operand kinds then say which field each parsed operand fills. The
 * word is the row's fixed pattern OR'd with the operand fields, so any
 * text mips_format produced encodes back to exactly the decoded word. */
#include <stdlib.h>
#include <string.h>

#include "src/tools/mips.h"

enum { MAX_OPS = 512 };

static uint16_t by_name[MAX_OPS];     /* op ids sorted by mnemonic */
static uint8_t gpr_by_key[128 * 128]; /* two-char ABI name -> reg + 1 */

static int cmp_name(const void *a, const void *b) {
  return strcmp(mips_ops[*(const uint16_t *)a].name,
                mips_ops[*(const uint16_t *)b].name);
}

void mips_asm_init(void) {
  for (uint16_t i = 0; i < mips_op_count; i++)
    by_name[i] = i;
  qsort(by_name, mips_op_count, sizeof(by_name[0]), cmp_name);

  memset(gpr_by_key, 0, sizeof(gpr_by_key));
  for (unsigned r = 1; r < 32; r++) {
    const char *n = mips_gpr_names[r];
    gpr_by_key[(unsigned)n[0] * 128 + (unsigned)n[1]] = (uint8_t)(r + 1);
  }
}

static int find_op(const char *name, size_t len) {
  size_t lo = 0, hi = mips_op_count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    const char *m = mips_ops[by_name[mid]].name;
    int c = strncmp(m, name, len);
    if (c == 0 && m[len] != '\0')
      c = 1;
    if (c == 0)
      return by_name[mid];
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  return -1;
}

/* --- Operand scanners. Each advances *p on success. ------------------ */

static int is_alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

/* Signed number: [-]0xHEX or [-]DEC. */
static int scan_int(const char **p, int64_t *out) {
  const char *s = *p;
  int neg = 0;
  if (*s == '-') {
    neg = 1;
    s++;
  }
  int base = 10;
  if (s[0] == '0' && s[1] == 'x') {
    base = 16;
    s += 2;
  }
  int64_t v = 0;
  const char *digits = s;
  for (;; s++) {
    int d;
    if (*s >= '0' && *s <= '9')
      d = *s - '0';
    else if (base == 16 && *s >= 'A' && *s <= 'F')
      d = *s - 'A' + 10;
    else if (base == 16 && *s >= 'a' && *s <= 'f')
      d = *s - 'a' + 10;
    else
      break;
    v = v * base + d;
    if (v > 0xFFFFFFFFll)
      return 0;
  }
  if (s == digits)
    return 0;
  *out = neg ? -v : v;
  *p = s;
  return 1;
}

static int scan_range(const char **p, int64_t lo, int64_t hi, uint32_t *out) {
  int64_t v;
  if (!scan_int(p, &v) || v < lo || v > hi)
    return 0;
  *out = (uint32_t)v;
  return 1;
}

static int scan_lit(const char **p, const char *lit) {
  size_t n = strlen(lit);
  if (strncmp(*p, lit, n) != 0)
    return 0;
  *p += n;
  return 1;
}

static int scan_gpr(const char **p, uint32_t *r) {
  const char *s = *p;
  if (*s++ != '$')
    return 0;
  if (scan_lit(&s, "zero")) {
    *r = 0;
  } else {
    if (!is_alnum(s[0]) || !is_alnum(s[1]))
      return 0;
    unsigned v = gpr_by_key[(unsigned)s[0] * 128 + (unsigned)s[1]];
    if (!v)
      return 0;
    *r = v - 1;
    s += 2;
  }
  if (is_alnum(*s))
    return 0;
  *p = s;
  return 1;
}

static int scan_numbered(const char **p, const char *prefix, uint32_t *r) {
  const char *s = *p;
  if (!scan_lit(&s, prefix) || *s == '-' || !scan_range(&s, 0, 31, r))
    return 0;
  *p = s;
  return 1;
}

static int scan_c0(const char **p, uint32_t *r) {
  const char *s = *p;
  if (*s++ != '$')
    return 0;
  size_t n = 0;
  while (is_alnum(s[n]))
    n++;
  for (uint32_t i = 0; i < 32; i++) {
    if (strlen(mips_c0_names[i]) == n && strncmp(mips_c0_names[i], s, n) == 0) {
      *r = i;
      *p = s + n;
      return 1;
    }
  }
  return 0;
}

#define RS_SHIFT 21
#define RT_SHIFT 16
#define RD_SHIFT 11
#define SA_SHIFT 6

/* Parses one operand of `kind` and returns its field bits in *bits. */
static int scan_operand(const char **p, uint8_t kind, uint32_t addr,
                        uint32_t *bits) {
  uint32_t v, r;
  switch (kind) {
  case MOPK_RS:
    return scan_gpr(p, &r) ? (*bits = r << RS_SHIFT, 1) : 0;
  case MOPK_RT:
    return scan_gpr(p, &r) ? (*bits = r << RT_SHIFT, 1) : 0;
  case MOPK_RD:
    return scan_gpr(p, &r) ? (*bits = r << RD_SHIFT, 1) : 0;
  case MOPK_SA:
    return *p[0] != '-' && scan_range(p, 0, 31, &v) ? (*bits = v << SA_SHIFT, 1)
                                                    : 0;
  case MOPK_SIMM:
    return scan_range(p, -0x8000, 0x7FFF, &v) ? (*bits = v & 0xFFFFu, 1) : 0;
  case MOPK_UIMM:
    return scan_range(p, 0, 0xFFFF, &v) ? (*bits = v, 1) : 0;
  case MOPK_MEM:
    if (!scan_range(p, -0x8000, 0x7FFF, &v) || !scan_lit(p, "(") ||
        !scan_gpr(p, &r) || !scan_lit(p, ")"))
      return 0;
    *bits = r << RS_SHIFT | (v & 0xFFFFu);
    return 1;
  case MOPK_BRANCH: {
    if (!scan_range(p, 0, 0xFFFFFFFFll, &v))
      return 0;
    int32_t off = (int32_t)(v - (addr + 4u));
    if (off & 3 || off < -0x20000 || off > 0x1FFFC)
      return 0;
    *bits = (uint32_t)(off / 4) & 0xFFFFu;
    return 1;
  }
  case MOPK_JUMP:
    if (!scan_range(p, 0, 0xFFFFFFFFll, &v) || v & 3u ||
        (v & 0xF0000000u) != ((addr + 4u) & 0xF0000000u))
      return 0;
    *bits = (v >> 2) & 0x03FFFFFFu;
    return 1;
  case MOPK_FS:
    return scan_numbered(p, "$f", &r) ? (*bits = r << RD_SHIFT, 1) : 0;
  case MOPK_FT:
    return scan_numbered(p, "$f", &r) ? (*bits = r << RT_SHIFT, 1) : 0;
  case MOPK_FD:
    return scan_numbered(p, "$f", &r) ? (*bits = r << SA_SHIFT, 1) : 0;
  case MOPK_C0REG:
    return scan_c0(p, &r) ? (*bits = r << RD_SHIFT, 1) : 0;
  case MOPK_FCR:
    return scan_numbered(p, "$fcr", &r) ? (*bits = r << RD_SHIFT, 1) : 0;
  case MOPK_CACHEOP:
    return scan_range(p, 0, 31, &v) ? (*bits = v << RT_SHIFT, 1) : 0;
  case MOPK_CODE20:
    return scan_range(p, 0, 0xFFFFF, &v) ? (*bits = v << SA_SHIFT, 1) : 0;
  case MOPK_CODE10:
    return scan_range(p, 0, 0x3FF, &v) ? (*bits = v << SA_SHIFT, 1) : 0;
  default:
    return 0;
  }
}

static int is_optional(uint8_t kind) {
  return kind == MOPK_CODE20 || kind == MOPK_CODE10;
}

int mips_assemble(const char *text, uint32_t addr, uint32_t *word) {
  const char *p = text;
  size_t n = 0;
  while (p[n] && p[n] != ' ')
    n++;

  if (n == 5 && strncmp(p, ".word", 5) == 0) {
    p += 5;
    uint32_t v;
    if (!scan_lit(&p, " ") || *p == '-' ||
        !scan_range(&p, 0, 0xFFFFFFFFll, &v) || *p)
      return 0;
    *word = v;
    return 1;
  }

  int op = find_op(p, n);
  if (op < 0)
    return 0;
  p += n;

  const MipsOpSpec *s = &mips_ops[op];
  uint32_t w = s->match;
  for (int i = 0; i < MIPS_MAX_OPERANDS && s->opk[i]; i++) {
    if (!*p && is_optional(s->opk[i]))
      break;
    uint32_t bits;
    if (!scan_lit(&p, i ? ", " : " ") ||
        !scan_operand(&p, s->opk[i], addr, &bits))
      return 0;
    w |= bits;
  }
  if (*p)
    return 0;
  *word = w;
  return 1;
}
