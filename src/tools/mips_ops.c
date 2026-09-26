/* The R4300i instruction table: one row per instruction form.
 *
 * `match` holds the fixed bits with every operand field zero; the
 * operand kinds define which bits are variable. Rows sharing a decode
 * slot are tried in table order, so `nop` precedes `sll`. */
#include "src/tools/mips.h"

/* Encoding builders for the fixed part of each instruction class. */
#define OP(p)       ((uint32_t)(p) << 26)
#define SPECIAL(f)  (OP(0) | (uint32_t)(f))
#define REGIMM(rt)  (OP(1) | (uint32_t)(rt) << 16)
#define COP0(rs)    (OP(16) | (uint32_t)(rs) << 21)
#define COP0CO(f)   (OP(16) | 1u << 25 | (uint32_t)(f))
#define COP1(rs)    (OP(17) | (uint32_t)(rs) << 21)
#define BC1(ndtf)   (OP(17) | 8u << 21 | (uint32_t)(ndtf) << 16)
#define FPU(fmt, f) (OP(17) | (uint32_t)(fmt) << 21 | (uint32_t)(f))

/* COP1 fmt field values. */
#define FMT_S 16
#define FMT_D 17
#define FMT_W 20
#define FMT_L 21

#define RS      MOPK_RS
#define RT      MOPK_RT
#define RD      MOPK_RD
#define SA      MOPK_SA
#define SIMM    MOPK_SIMM
#define UIMM    MOPK_UIMM
#define MEM     MOPK_MEM
#define BRANCH  MOPK_BRANCH
#define JUMP    MOPK_JUMP
#define FS      MOPK_FS
#define FT      MOPK_FT
#define FD      MOPK_FD
#define C0REG   MOPK_C0REG
#define FCR     MOPK_FCR
#define CACHEOP MOPK_CACHEOP
#define CODE20  MOPK_CODE20
#define CODE10  MOPK_CODE10

#define B  MFLOW_BRANCH
#define BL (MFLOW_BRANCH | MFLOW_LIKELY)
#define J  MFLOW_JUMP
#define JL (MFLOW_JUMP | MFLOW_LINK)

/* FPU op present for single and double formats. */
#define FP_SD(n, f, a, b, c)                                                   \
  {n ".s", FPU(FMT_S, f), {a, b, c}, 0}, {n ".d", FPU(FMT_D, f), {a, b, c}, 0}
#define FP_CMP(n, f) FP_SD("c." n, f, FS, FT, 0)

const MipsOpSpec mips_ops[] = {
    /* Primary opcodes. */
    {"j", OP(2), {JUMP}, J},
    {"jal", OP(3), {JUMP}, JL},
    {"beq", OP(4), {RS, RT, BRANCH}, B},
    {"bne", OP(5), {RS, RT, BRANCH}, B},
    {"blez", OP(6), {RS, BRANCH}, B},
    {"bgtz", OP(7), {RS, BRANCH}, B},
    {"addi", OP(8), {RT, RS, SIMM}, 0},
    {"addiu", OP(9), {RT, RS, SIMM}, 0},
    {"slti", OP(10), {RT, RS, SIMM}, 0},
    {"sltiu", OP(11), {RT, RS, SIMM}, 0},
    {"andi", OP(12), {RT, RS, UIMM}, 0},
    {"ori", OP(13), {RT, RS, UIMM}, 0},
    {"xori", OP(14), {RT, RS, UIMM}, 0},
    {"lui", OP(15), {RT, UIMM}, 0},
    {"beql", OP(20), {RS, RT, BRANCH}, BL},
    {"bnel", OP(21), {RS, RT, BRANCH}, BL},
    {"blezl", OP(22), {RS, BRANCH}, BL},
    {"bgtzl", OP(23), {RS, BRANCH}, BL},
    {"daddi", OP(24), {RT, RS, SIMM}, 0},
    {"daddiu", OP(25), {RT, RS, SIMM}, 0},
    {"ldl", OP(26), {RT, MEM}, 0},
    {"ldr", OP(27), {RT, MEM}, 0},
    {"lb", OP(32), {RT, MEM}, 0},
    {"lh", OP(33), {RT, MEM}, 0},
    {"lwl", OP(34), {RT, MEM}, 0},
    {"lw", OP(35), {RT, MEM}, 0},
    {"lbu", OP(36), {RT, MEM}, 0},
    {"lhu", OP(37), {RT, MEM}, 0},
    {"lwr", OP(38), {RT, MEM}, 0},
    {"lwu", OP(39), {RT, MEM}, 0},
    {"sb", OP(40), {RT, MEM}, 0},
    {"sh", OP(41), {RT, MEM}, 0},
    {"swl", OP(42), {RT, MEM}, 0},
    {"sw", OP(43), {RT, MEM}, 0},
    {"sdl", OP(44), {RT, MEM}, 0},
    {"sdr", OP(45), {RT, MEM}, 0},
    {"swr", OP(46), {RT, MEM}, 0},
    {"cache", OP(47), {CACHEOP, MEM}, 0},
    {"ll", OP(48), {RT, MEM}, 0},
    {"lwc1", OP(49), {FT, MEM}, 0},
    {"lld", OP(52), {RT, MEM}, 0},
    {"ldc1", OP(53), {FT, MEM}, 0},
    {"ld", OP(55), {RT, MEM}, 0},
    {"sc", OP(56), {RT, MEM}, 0},
    {"swc1", OP(57), {FT, MEM}, 0},
    {"scd", OP(60), {RT, MEM}, 0},
    {"sdc1", OP(61), {FT, MEM}, 0},
    {"sd", OP(63), {RT, MEM}, 0},

    /* SPECIAL (opcode 0), keyed by funct. */
    {"nop", SPECIAL(0), {0}, 0},
    {"sll", SPECIAL(0), {RD, RT, SA}, 0},
    {"srl", SPECIAL(2), {RD, RT, SA}, 0},
    {"sra", SPECIAL(3), {RD, RT, SA}, 0},
    {"sllv", SPECIAL(4), {RD, RT, RS}, 0},
    {"srlv", SPECIAL(6), {RD, RT, RS}, 0},
    {"srav", SPECIAL(7), {RD, RT, RS}, 0},
    {"jr", SPECIAL(8), {RS}, MFLOW_REG},
    {"jalr", SPECIAL(9), {RD, RS}, MFLOW_REG | MFLOW_LINK},
    {"syscall", SPECIAL(12), {CODE20}, 0},
    {"break", SPECIAL(13), {CODE20}, 0},
    {"sync", SPECIAL(15), {0}, 0},
    {"mfhi", SPECIAL(16), {RD}, 0},
    {"mthi", SPECIAL(17), {RS}, 0},
    {"mflo", SPECIAL(18), {RD}, 0},
    {"mtlo", SPECIAL(19), {RS}, 0},
    {"dsllv", SPECIAL(20), {RD, RT, RS}, 0},
    {"dsrlv", SPECIAL(22), {RD, RT, RS}, 0},
    {"dsrav", SPECIAL(23), {RD, RT, RS}, 0},
    {"mult", SPECIAL(24), {RS, RT}, 0},
    {"multu", SPECIAL(25), {RS, RT}, 0},
    {"div", SPECIAL(26), {RS, RT}, 0},
    {"divu", SPECIAL(27), {RS, RT}, 0},
    {"dmult", SPECIAL(28), {RS, RT}, 0},
    {"dmultu", SPECIAL(29), {RS, RT}, 0},
    {"ddiv", SPECIAL(30), {RS, RT}, 0},
    {"ddivu", SPECIAL(31), {RS, RT}, 0},
    {"add", SPECIAL(32), {RD, RS, RT}, 0},
    {"addu", SPECIAL(33), {RD, RS, RT}, 0},
    {"sub", SPECIAL(34), {RD, RS, RT}, 0},
    {"subu", SPECIAL(35), {RD, RS, RT}, 0},
    {"and", SPECIAL(36), {RD, RS, RT}, 0},
    {"or", SPECIAL(37), {RD, RS, RT}, 0},
    {"xor", SPECIAL(38), {RD, RS, RT}, 0},
    {"nor", SPECIAL(39), {RD, RS, RT}, 0},
    {"slt", SPECIAL(42), {RD, RS, RT}, 0},
    {"sltu", SPECIAL(43), {RD, RS, RT}, 0},
    {"dadd", SPECIAL(44), {RD, RS, RT}, 0},
    {"daddu", SPECIAL(45), {RD, RS, RT}, 0},
    {"dsub", SPECIAL(46), {RD, RS, RT}, 0},
    {"dsubu", SPECIAL(47), {RD, RS, RT}, 0},
    {"tge", SPECIAL(48), {RS, RT, CODE10}, 0},
    {"tgeu", SPECIAL(49), {RS, RT, CODE10}, 0},
    {"tlt", SPECIAL(50), {RS, RT, CODE10}, 0},
    {"tltu", SPECIAL(51), {RS, RT, CODE10}, 0},
    {"teq", SPECIAL(52), {RS, RT, CODE10}, 0},
    {"tne", SPECIAL(54), {RS, RT, CODE10}, 0},
    {"dsll", SPECIAL(56), {RD, RT, SA}, 0},
    {"dsrl", SPECIAL(58), {RD, RT, SA}, 0},
    {"dsra", SPECIAL(59), {RD, RT, SA}, 0},
    {"dsll32", SPECIAL(60), {RD, RT, SA}, 0},
    {"dsrl32", SPECIAL(62), {RD, RT, SA}, 0},
    {"dsra32", SPECIAL(63), {RD, RT, SA}, 0},

    /* REGIMM (opcode 1), keyed by rt. */
    {"bltz", REGIMM(0), {RS, BRANCH}, B},
    {"bgez", REGIMM(1), {RS, BRANCH}, B},
    {"bltzl", REGIMM(2), {RS, BRANCH}, BL},
    {"bgezl", REGIMM(3), {RS, BRANCH}, BL},
    {"tgei", REGIMM(8), {RS, SIMM}, 0},
    {"tgeiu", REGIMM(9), {RS, SIMM}, 0},
    {"tlti", REGIMM(10), {RS, SIMM}, 0},
    {"tltiu", REGIMM(11), {RS, SIMM}, 0},
    {"teqi", REGIMM(12), {RS, SIMM}, 0},
    {"tnei", REGIMM(14), {RS, SIMM}, 0},
    {"bltzal", REGIMM(16), {RS, BRANCH}, B | MFLOW_LINK},
    {"bgezal", REGIMM(17), {RS, BRANCH}, B | MFLOW_LINK},
    {"bltzall", REGIMM(18), {RS, BRANCH}, BL | MFLOW_LINK},
    {"bgezall", REGIMM(19), {RS, BRANCH}, BL | MFLOW_LINK},

    /* COP0 (opcode 16): moves keyed by rs, CO operations by funct. */
    {"mfc0", COP0(0), {RT, C0REG}, 0},
    {"dmfc0", COP0(1), {RT, C0REG}, 0},
    {"mtc0", COP0(4), {RT, C0REG}, 0},
    {"dmtc0", COP0(5), {RT, C0REG}, 0},
    {"tlbr", COP0CO(1), {0}, 0},
    {"tlbwi", COP0CO(2), {0}, 0},
    {"tlbwr", COP0CO(6), {0}, 0},
    {"tlbp", COP0CO(8), {0}, 0},
    {"eret", COP0CO(24), {0}, MFLOW_ERET},

    /* COP1 (opcode 17): moves and BC keyed by rs. */
    {"mfc1", COP1(0), {RT, FS}, 0},
    {"dmfc1", COP1(1), {RT, FS}, 0},
    {"cfc1", COP1(2), {RT, FCR}, 0},
    {"mtc1", COP1(4), {RT, FS}, 0},
    {"dmtc1", COP1(5), {RT, FS}, 0},
    {"ctc1", COP1(6), {RT, FCR}, 0},
    {"bc1f", BC1(0), {BRANCH}, B},
    {"bc1t", BC1(1), {BRANCH}, B},
    {"bc1fl", BC1(2), {BRANCH}, BL},
    {"bc1tl", BC1(3), {BRANCH}, BL},

    /* COP1 arithmetic, keyed by fmt then funct. */
    FP_SD("add", 0, FD, FS, FT),
    FP_SD("sub", 1, FD, FS, FT),
    FP_SD("mul", 2, FD, FS, FT),
    FP_SD("div", 3, FD, FS, FT),
    FP_SD("sqrt", 4, FD, FS, 0),
    FP_SD("abs", 5, FD, FS, 0),
    FP_SD("mov", 6, FD, FS, 0),
    FP_SD("neg", 7, FD, FS, 0),
    FP_SD("round.l", 8, FD, FS, 0),
    FP_SD("trunc.l", 9, FD, FS, 0),
    FP_SD("ceil.l", 10, FD, FS, 0),
    FP_SD("floor.l", 11, FD, FS, 0),
    FP_SD("round.w", 12, FD, FS, 0),
    FP_SD("trunc.w", 13, FD, FS, 0),
    FP_SD("ceil.w", 14, FD, FS, 0),
    FP_SD("floor.w", 15, FD, FS, 0),
    {"cvt.s.d", FPU(FMT_D, 32), {FD, FS}, 0},
    {"cvt.s.w", FPU(FMT_W, 32), {FD, FS}, 0},
    {"cvt.s.l", FPU(FMT_L, 32), {FD, FS}, 0},
    {"cvt.d.s", FPU(FMT_S, 33), {FD, FS}, 0},
    {"cvt.d.w", FPU(FMT_W, 33), {FD, FS}, 0},
    {"cvt.d.l", FPU(FMT_L, 33), {FD, FS}, 0},
    FP_SD("cvt.w", 36, FD, FS, 0),
    FP_SD("cvt.l", 37, FD, FS, 0),
    FP_CMP("f", 48),
    FP_CMP("un", 49),
    FP_CMP("eq", 50),
    FP_CMP("ueq", 51),
    FP_CMP("olt", 52),
    FP_CMP("ult", 53),
    FP_CMP("ole", 54),
    FP_CMP("ule", 55),
    FP_CMP("sf", 56),
    FP_CMP("ngle", 57),
    FP_CMP("seq", 58),
    FP_CMP("ngl", 59),
    FP_CMP("lt", 60),
    FP_CMP("nge", 61),
    FP_CMP("le", 62),
    FP_CMP("ngt", 63),
};

const uint16_t mips_op_count = sizeof(mips_ops) / sizeof(mips_ops[0]);

const char *const mips_gpr_names[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
    "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
    "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

const char *const mips_c0_names[32] = {
    "Index",      "Random",     "EntryLo0",   "EntryLo1",   "Context",
    "PageMask",   "Wired",      "Reserved7",  "BadVAddr",   "Count",
    "EntryHi",    "Compare",    "Status",     "Cause",      "EPC",
    "PRId",       "Config",     "LLAddr",     "WatchLo",    "WatchHi",
    "XContext",   "Reserved21", "Reserved22", "Reserved23", "Reserved24",
    "Reserved25", "PErr",       "CacheErr",   "TagLo",      "TagHi",
    "ErrorEPC",   "Reserved31",
};
