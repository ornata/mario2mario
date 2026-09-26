/* Decoder: a small fixed-shape decode tree over mips_ops[].
 *
 * The tree mirrors the ISA's own encoding hierarchy -- primary opcode,
 * then the SPECIAL funct / REGIMM rt / COP0 rs (+CO funct) / COP1 rs
 * (+fmt funct) sub-tables. Each leaf slot holds a short list of candidate
 * rows (usually one); a row matches when (word & mask) == match. Leaf
 * lists are stored CSR-style in two flat arrays. */
#include "src/tools/mips.h"

#include <string.h>

enum {
  N_ROOT,
  N_SPECIAL,
  N_REGIMM,
  N_COP0,
  N_COP0_CO,
  N_COP1,
  N_COP1_FMT,
  N_COUNT
};

/* Selector field of each node: (word >> shift) & ((1 << bits) - 1). */
static const struct {
  uint8_t shift, bits;
} node_sel[N_COUNT] = {
    [N_ROOT] = {26, 6},    [N_SPECIAL] = {0, 6}, [N_REGIMM] = {16, 5},
    [N_COP0] = {21, 5},    [N_COP0_CO] = {0, 6}, [N_COP1] = {21, 5},
    [N_COP1_FMT] = {0, 6},
};

/* Slots [lo, hi] of `parent` descend into `child`. */
static const struct {
  uint8_t parent, lo, hi, child;
} node_edges[] = {
    {N_ROOT, 0, 0, N_SPECIAL},    {N_ROOT, 1, 1, N_REGIMM},
    {N_ROOT, 16, 16, N_COP0},     {N_COP0, 16, 31, N_COP0_CO},
    {N_ROOT, 17, 17, N_COP1},     {N_COP1, 16, 17, N_COP1_FMT},
    {N_COP1, 20, 21, N_COP1_FMT},
};

enum { SLOT_COUNT = 64 + 64 + 32 + 32 + 64 + 32 + 64, MAX_OPS = 512 };

static uint16_t node_base[N_COUNT];
static uint8_t slot_child[SLOT_COUNT]; /* child node + 1, or 0 for a leaf */
static uint16_t leaf_start[SLOT_COUNT + 1];
static uint16_t leaf_ops[MAX_OPS];
static uint32_t op_mask[MAX_OPS];

static const uint32_t opk_bits[MOPK_COUNT] = {
    [MOPK_NONE] = 0,
    [MOPK_RS] = 0x03E00000,
    [MOPK_RT] = 0x001F0000,
    [MOPK_RD] = 0x0000F800,
    [MOPK_SA] = 0x000007C0,
    [MOPK_SIMM] = 0x0000FFFF,
    [MOPK_UIMM] = 0x0000FFFF,
    [MOPK_MEM] = 0x03E0FFFF,
    [MOPK_BRANCH] = 0x0000FFFF,
    [MOPK_JUMP] = 0x03FFFFFF,
    [MOPK_FS] = 0x0000F800,
    [MOPK_FT] = 0x001F0000,
    [MOPK_FD] = 0x000007C0,
    [MOPK_C0REG] = 0x0000F800,
    [MOPK_FCR] = 0x0000F800,
    [MOPK_CACHEOP] = 0x001F0000,
    [MOPK_CODE20] = 0x03FFFFC0,
    [MOPK_CODE10] = 0x0000FFC0,
};

static unsigned walk(uint32_t word) {
  unsigned node = N_ROOT;
  for (;;) {
    unsigned sel =
        (word >> node_sel[node].shift) & ((1u << node_sel[node].bits) - 1u);
    unsigned slot = node_base[node] + sel;
    if (!slot_child[slot])
      return slot;
    node = slot_child[slot] - 1u;
  }
}

uint32_t mips_operand_bits(uint16_t op) {
  const uint8_t *k = mips_ops[op].opk;
  return opk_bits[k[0]] | opk_bits[k[1]] | opk_bits[k[2]];
}

void mips_init(void) {
  unsigned base = 0;
  for (unsigned n = 0; n < N_COUNT; n++) {
    node_base[n] = (uint16_t)base;
    base += 1u << node_sel[n].bits;
  }
  memset(slot_child, 0, sizeof(slot_child));
  for (size_t e = 0; e < sizeof(node_edges) / sizeof(node_edges[0]); e++)
    for (unsigned s = node_edges[e].lo; s <= node_edges[e].hi; s++)
      slot_child[node_base[node_edges[e].parent] + s] =
          (uint8_t)(node_edges[e].child + 1);

  /* Two-pass CSR fill: count rows per leaf, prefix-sum, then place rows
   * in table order so earlier rows win within a slot. */
  uint16_t count[SLOT_COUNT] = {0};
  uint16_t leaf_of[MAX_OPS];
  for (uint16_t i = 0; i < mips_op_count; i++) {
    op_mask[i] = ~mips_operand_bits(i);
    leaf_of[i] = (uint16_t)walk(mips_ops[i].match);
    count[leaf_of[i]]++;
  }
  leaf_start[0] = 0;
  for (unsigned s = 0; s < SLOT_COUNT; s++)
    leaf_start[s + 1] = (uint16_t)(leaf_start[s] + count[s]);
  uint16_t fill[SLOT_COUNT];
  memcpy(fill, leaf_start, sizeof(fill));
  for (uint16_t i = 0; i < mips_op_count; i++)
    leaf_ops[fill[leaf_of[i]]++] = i;

  mips_asm_init();
}

uint16_t mips_decode(uint32_t word) {
  unsigned slot = walk(word);
  for (unsigned i = leaf_start[slot]; i < leaf_start[slot + 1]; i++) {
    uint16_t op = leaf_ops[i];
    if ((word & op_mask[op]) == mips_ops[op].match)
      return op;
  }
  return MIPS_OP_INVALID;
}

uint32_t mips_target(uint32_t word, uint16_t op, uint32_t addr) {
  if (mips_ops[op].flow & MFLOW_JUMP)
    return ((addr + 4u) & 0xF0000000u) | (word & 0x03FFFFFFu) << 2;
  int32_t off = (int32_t)(int16_t)(word & 0xFFFFu) * 4;
  return addr + 4u + (uint32_t)off;
}

/* --- Formatting ------------------------------------------------------- */

typedef struct {
  char *p;
} Out;

static void put_str(Out *o, const char *s) {
  size_t n = strlen(s);
  memcpy(o->p, s, n);
  o->p += n;
}

static void put_hex(Out *o, uint32_t v, int min_digits) {
  static const char digits[] = "0123456789ABCDEF";
  char tmp[8];
  int n = 0;
  do {
    tmp[n++] = digits[v & 15u];
    v >>= 4;
  } while (v);
  while (n < min_digits)
    tmp[n++] = '0';
  *o->p++ = '0';
  *o->p++ = 'x';
  while (n)
    *o->p++ = tmp[--n];
}

static void put_shex(Out *o, uint32_t imm16) {
  int32_t v = (int16_t)imm16;
  if (v < 0) {
    *o->p++ = '-';
    put_hex(o, (uint32_t)-v, 1);
  } else {
    put_hex(o, (uint32_t)v, 1);
  }
}

static void put_dec(Out *o, uint32_t v) {
  if (v >= 10)
    *o->p++ = (char)('0' + v / 10);
  *o->p++ = (char)('0' + v % 10);
}

static void put_gpr(Out *o, uint32_t r) {
  *o->p++ = '$';
  put_str(o, mips_gpr_names[r]);
}

static void put_reg_num(Out *o, const char *prefix, uint32_t r) {
  put_str(o, prefix);
  put_dec(o, r);
}

#define F_RS(w) (((w) >> 21) & 31u)
#define F_RT(w) (((w) >> 16) & 31u)
#define F_RD(w) (((w) >> 11) & 31u)
#define F_SA(w) (((w) >> 6) & 31u)

/* Emits one operand; returns 0 if it was an omitted optional operand. */
static int put_operand(Out *o, uint8_t kind, uint32_t w, uint16_t op,
                       uint32_t addr) {
  switch (kind) {
  case MOPK_RS:
    put_gpr(o, F_RS(w));
    return 1;
  case MOPK_RT:
    put_gpr(o, F_RT(w));
    return 1;
  case MOPK_RD:
    put_gpr(o, F_RD(w));
    return 1;
  case MOPK_SA:
    put_dec(o, F_SA(w));
    return 1;
  case MOPK_SIMM:
    put_shex(o, w & 0xFFFFu);
    return 1;
  case MOPK_UIMM:
    put_hex(o, w & 0xFFFFu, 1);
    return 1;
  case MOPK_MEM:
    put_shex(o, w & 0xFFFFu);
    *o->p++ = '(';
    put_gpr(o, F_RS(w));
    *o->p++ = ')';
    return 1;
  case MOPK_BRANCH:
  case MOPK_JUMP:
    put_hex(o, mips_target(w, op, addr), 8);
    return 1;
  case MOPK_FS:
    put_reg_num(o, "$f", F_RD(w));
    return 1;
  case MOPK_FT:
    put_reg_num(o, "$f", F_RT(w));
    return 1;
  case MOPK_FD:
    put_reg_num(o, "$f", F_SA(w));
    return 1;
  case MOPK_C0REG:
    *o->p++ = '$';
    put_str(o, mips_c0_names[F_RD(w)]);
    return 1;
  case MOPK_FCR:
    put_reg_num(o, "$fcr", F_RD(w));
    return 1;
  case MOPK_CACHEOP:
    put_hex(o, F_RT(w), 1);
    return 1;
  case MOPK_CODE20:
    if (!((w >> 6) & 0xFFFFFu))
      return 0;
    put_hex(o, (w >> 6) & 0xFFFFFu, 1);
    return 1;
  case MOPK_CODE10:
    if (!((w >> 6) & 0x3FFu))
      return 0;
    put_hex(o, (w >> 6) & 0x3FFu, 1);
    return 1;
  default:
    return 0;
  }
}

size_t mips_format(uint32_t word, uint16_t op, uint32_t addr, char *buf) {
  Out o = {buf};
  if (op == MIPS_OP_INVALID) {
    put_str(&o, ".word ");
    put_hex(&o, word, 8);
    *o.p = '\0';
    return (size_t)(o.p - buf);
  }
  const MipsOpSpec *s = &mips_ops[op];
  put_str(&o, s->name);
  for (int i = 0; i < MIPS_MAX_OPERANDS && s->opk[i]; i++) {
    char *mark = o.p;
    put_str(&o, i ? ", " : " ");
    if (!put_operand(&o, s->opk[i], word, op, addr))
      o.p = mark;
  }
  *o.p = '\0';
  return (size_t)(o.p - buf);
}
