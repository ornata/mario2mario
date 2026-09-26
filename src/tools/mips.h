/* MIPS R4300i instruction decoder.
 *
 * Table-driven from the ISA encodings: every instruction form is one row
 * of mips_ops[] (mnemonic, fixed-bit pattern, operand kinds, flow flags).
 * The mask of fixed bits is derived from the operand kinds: every bit not
 * owned by an operand field must equal the pattern exactly. A word that
 * matches no row is data and is rendered as `.word 0xXXXXXXXX` -- the
 * decoder never guesses, which is what makes decode/encode bijective. */
#ifndef M2M_TOOLS_MIPS_H
#define M2M_TOOLS_MIPS_H

#include <stddef.h>
#include <stdint.h>

/* Operand kinds. Each owns a fixed bit field of the instruction word. */
typedef enum {
  MOPK_NONE = 0,
  MOPK_RS,      /* GPR, bits 25..21 */
  MOPK_RT,      /* GPR, bits 20..16 */
  MOPK_RD,      /* GPR, bits 15..11 */
  MOPK_SA,      /* shift amount, bits 10..6, decimal */
  MOPK_SIMM,    /* signed 16-bit immediate, bits 15..0 */
  MOPK_UIMM,    /* zero-extended 16-bit immediate, bits 15..0 */
  MOPK_MEM,     /* signed 16-bit offset (15..0) + base GPR (25..21) */
  MOPK_BRANCH,  /* PC-relative target, bits 15..0 (<<2, from pc+4) */
  MOPK_JUMP,    /* region target, bits 25..0 (<<2, in (pc+4)'s 256MB) */
  MOPK_FS,      /* FPR, bits 15..11 */
  MOPK_FT,      /* FPR, bits 20..16 */
  MOPK_FD,      /* FPR, bits 10..6 */
  MOPK_C0REG,   /* COP0 register, bits 15..11 */
  MOPK_FCR,     /* FPU control register, bits 15..11 */
  MOPK_CACHEOP, /* cache operation, bits 20..16 */
  MOPK_CODE20,  /* syscall/break code, bits 25..6, omitted when zero */
  MOPK_CODE10,  /* trap code, bits 15..6, omitted when zero */
  MOPK_COUNT
} MipsOperandKind;

/* Control-flow flags (structural only). */
enum {
  MFLOW_BRANCH = 1u << 0, /* conditional PC-relative branch */
  MFLOW_LIKELY = 1u << 1, /* branch-likely: delay slot nullified if not taken */
  MFLOW_LINK = 1u << 2,   /* writes a return address */
  MFLOW_JUMP = 1u << 3,   /* absolute region jump (j/jal) */
  MFLOW_REG = 1u << 4,    /* register-indirect jump (jr/jalr) */
  MFLOW_ERET = 1u << 5,   /* exception return, no delay slot */
};
#define MFLOW_DELAY (MFLOW_BRANCH | MFLOW_JUMP | MFLOW_REG)

enum { MIPS_MAX_OPERANDS = 3 };

typedef struct {
  const char *name;
  uint32_t match;                 /* fixed bits (operand fields zero) */
  uint8_t opk[MIPS_MAX_OPERANDS]; /* MipsOperandKind, MOPK_NONE-padded */
  uint8_t flow;                   /* MFLOW_* */
} MipsOpSpec;

extern const MipsOpSpec mips_ops[];
extern const uint16_t mips_op_count;

#define MIPS_OP_INVALID 0xFFFFu

/* Longest rendered text (mnemonic + operands) including NUL. */
enum { MIPS_TEXT_MAX = 64 };

/* Builds the derived lookup tables. Call once before anything else. */
void mips_init(void);

/* Returns the mips_ops[] index for `word`, or MIPS_OP_INVALID. */
uint16_t mips_decode(uint32_t word);

/* Field mask owned by operands of `op` (the complement of fixed bits). */
uint32_t mips_operand_bits(uint16_t op);

/* Branch/jump destination of a MFLOW_BRANCH or MFLOW_JUMP instruction at
 * `addr`. Undefined for other ops. */
uint32_t mips_target(uint32_t word, uint16_t op, uint32_t addr);

/* Renders `word` at `addr` (op from mips_decode) as "mnemonic operands",
 * or ".word 0xXXXXXXXX" for MIPS_OP_INVALID. Returns the text length.
 * `buf` must hold MIPS_TEXT_MAX bytes. */
size_t mips_format(uint32_t word, uint16_t op, uint32_t addr, char *buf);

/* Register-name tables shared by formatter and assembler. */
extern const char *const mips_gpr_names[32];
extern const char *const mips_c0_names[32];

#endif
