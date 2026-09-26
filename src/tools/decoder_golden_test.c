/* Hand-derived decodings from the R4300i encodings (no ROM needed),
 * checked in both directions (decode+format, and assemble back).
 * Each expected string was worked out bit-by-bit from the ISA fields,
 * independently of the table in mips_ops.c. */
#include <stdio.h>
#include <string.h>

#include "src/tools/mips.h"

typedef struct {
  uint32_t addr, word;
  const char *text;
} Golden;

static const Golden goldens[] = {
    /* The first game-code words at the entry point (ROM 0x1000). */
    {0x80246000, 0x3C088034, "lui $t0, 0x8034"},
    {0x80246004, 0x3C090002, "lui $t1, 0x2"},
    {0x80246008, 0x2508A580, "addiu $t0, $t0, -0x5A80"},
    {0x8024600C, 0x3529CEE0, "ori $t1, $t1, 0xCEE0"},
    {0x80246010, 0x2129FFF8, "addi $t1, $t1, -0x8"},
    {0x80246014, 0xAD000000, "sw $zero, 0x0($t0)"},
    {0x80246018, 0xAD000004, "sw $zero, 0x4($t0)"},
    {0x8024601C, 0x1520FFFC, "bne $t1, $zero, 0x80246010"},
    {0x80246020, 0x21080008, "addi $t0, $t0, 0x8"},
    {0x80246024, 0x3C0A8024, "lui $t2, 0x8024"},
    {0x80246028, 0x3C1D8020, "lui $sp, 0x8020"},
    {0x8024602C, 0x254A6DF8, "addiu $t2, $t2, 0x6DF8"},
    {0x80246030, 0x01400008, "jr $t2"},
    {0x80246034, 0x27BD0600, "addiu $sp, $sp, 0x600"},
    /* Integer / SPECIAL. */
    {0x80246000, 0x00000000, "nop"},
    {0x80246000, 0x03E00008, "jr $ra"},
    {0x80246000, 0x0320F809, "jalr $ra, $t9"},
    {0x80246000, 0x00041080, "sll $v0, $a0, 2"},
    {0x80246000, 0x000217C3, "sra $v0, $v0, 31"},
    {0x80246000, 0x0085102A, "slt $v0, $a0, $a1"},
    {0x80246000, 0x00850018, "mult $a0, $a1"},
    {0x80246000, 0x00851018, ".word 0x00851018"}, /* mult with rd != 0 */
    {0x80246000, 0x00001012, "mflo $v0"},
    {0x80246000, 0x0004103C, "dsll32 $v0, $a0, 0"},
    {0x80246000, 0x0000000C, "syscall"},
    {0x80246000, 0x00A0000C, "syscall 0x28000"},
    {0x80246000, 0x0000000D, "break"},
    {0x80246000, 0x0000000F, "sync"},
    {0x80246000, 0x00A60034, "teq $a1, $a2"},
    {0x80246000, 0x00A601F4, "teq $a1, $a2, 0x7"},
    {0x80246000, 0x00000401, ".word 0x00000401"}, /* funct 1 unused */
    /* Immediates, loads/stores. */
    {0x80246000, 0x30A200FF, "andi $v0, $a1, 0xFF"},
    {0x80246000, 0x2C410003, "sltiu $at, $v0, 0x3"},
    {0x80246000, 0x8FBF0014, "lw $ra, 0x14($sp)"},
    {0x80246000, 0xAFBFFFEC, "sw $ra, -0x14($sp)"},
    {0x80246000, 0x88A40003, "lwl $a0, 0x3($a1)"},
    {0x80246000, 0x98A40000, "lwr $a0, 0x0($a1)"},
    {0x80246000, 0xDFBF0018, "ld $ra, 0x18($sp)"},
    {0x80246000, 0xE7A40010, "swc1 $f4, 0x10($sp)"},
    {0x80246000, 0xBD100000, "cache 0x10, 0x0($t0)"},
    /* Jumps and branches (targets are absolute). */
    {0x80246000, 0x0C09180C, "jal 0x80246030"},
    {0x80246000, 0x08091800, "j 0x80246000"},
    {0x80246000, 0x04110003, "bgezal $zero, 0x80246010"},
    {0x80246000, 0x50400002, "beql $v0, $zero, 0x8024600C"},
    /* COP0 / COP1. */
    {0x80246000, 0x40086000, "mfc0 $t0, $Status"},
    {0x80246000, 0x42000018, "eret"},
    {0x80246000, 0x44803000, "mtc1 $zero, $f6"},
    {0x80246000, 0x4459F800, "cfc1 $t9, $fcr31"},
    {0x80246000, 0x45000003, "bc1f 0x80246010"},
    {0x80246000, 0x45030003, "bc1tl 0x80246010"},
    {0x80246000, 0x46062100, "add.s $f4, $f4, $f6"},
    {0x80246000, 0x46802021, "cvt.d.w $f0, $f4"},
    {0x80246000, 0x4600203C, "c.lt.s $f4, $f0"},
    {0x80246000, 0x46003004, "sqrt.s $f0, $f6"},
    {0x80246000, 0x46013004, ".word 0x46013004"}, /* sqrt with ft != 0 */
    {0x80246000, 0x4620010D, "trunc.w.d $f4, $f0"},
    {0x80246000, 0x46200021, ".word 0x46200021"}, /* cvt.d.d invalid */
    /* Not R4300i instructions. */
    {0x80246000, 0x4C000000, ".word 0x4C000000"}, /* COP3 */
    {0x80246000, 0x7C000000, ".word 0x7C000000"}, /* opcode 31 */
};

/* Text the assembler must refuse (range, syntax, unknown names). */
static const char *const rejects[] = {
    "addiu $t0, $t0, 0x8000",
    "addiu $t0, $t0, -0x8001",
    "andi $v0, $a1, -0x1",
    "sll $v0, $a0, 32",
    "lw $ra, 0x14",
    "add.s $f4, $f4, $f32",
    "mfc0 $t0, $Bogus",
    "jr $x9",
    "jr $ra, $ra",
    "j 0x90000000",
    "beq $zero, $zero, 0x80246002",
    "frobnicate $t0",
    "nop ",
    ".word -0x1",
};

int main(void) {
  mips_init();
  int failures = 0;
  char text[MIPS_TEXT_MAX];
  for (size_t i = 0; i < sizeof(goldens) / sizeof(goldens[0]); i++) {
    const Golden *g = &goldens[i];
    mips_format(g->word, mips_decode(g->word), g->addr, text);
    if (strcmp(text, g->text) != 0) {
      fprintf(stderr, "%08X  %08X: got \"%s\", want \"%s\"\n", g->addr, g->word,
              text, g->text);
      failures++;
    }
    uint32_t back = 0;
    if (!mips_assemble(g->text, g->addr, &back) || back != g->word) {
      fprintf(stderr, "assemble \"%s\": got %08X, want %08X\n", g->text, back,
              g->word);
      failures++;
    }
  }
  for (size_t i = 0; i < sizeof(rejects) / sizeof(rejects[0]); i++) {
    uint32_t w;
    if (mips_assemble(rejects[i], 0x80246000, &w)) {
      fprintf(stderr, "accepted \"%s\" as %08X\n", rejects[i], w);
      failures++;
    }
  }
  printf("%zu goldens, %d failures, %u table rows\n",
         sizeof(goldens) / sizeof(goldens[0]), failures, mips_op_count);
  return failures ? 1 : 0;
}
