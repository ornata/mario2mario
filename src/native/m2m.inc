// m2m.inc -- runtime interface for translated units (frozen contract).
//
// These macros are the ONLY way translated code talks to the runtime:
// the N64 memory bus, block boundaries and dispatch, COP0/TLB/exception
// services, and FPU exception-flag folding. They contain no MIPS
// instruction semantics; workers write those inline (PROMPT-translate.md).
//
// Registers: x1..x15 = $1..$15, x19..x25 = $19..$25, x29 = $29,
// x26 = RDRAM host base, x27 = NativeState*, x28 = icount.
// Scratch clobbered by macros: x0, x16, x17, x30 (see each macro).
// d0..d7 are preserved by every macro. Macros use numeric local labels
// 9001..9099; translated code must not use numeric labels.

// ---- NativeState offsets (asserted in runtime.c) ----
.set NS_GPR, 0            // cpu.gpr[n] at NS_GPR + 8*n
.set NS_HI, 256
.set NS_LO, 264
.set NS_FPR, 272          // FR=0: single fN at NS_FPR + 8*(N&~1) + 4*(N&1)
.set NS_FCR31, 532
.set NS_STATUS, 632       // cop0[12], low 32 bits
.set NS_LLBIT, 811
.set NS_ICOUNT, 816
.set NS_BOUNDARY_LEFT, 1848
.set NS_ATTN, 1856
.set NS_DISPATCH, 1864
.set NS_CODEMAP, 1872
.set NS_BRANCH, 1880
.set NS_STOP, 1884
.set NS_TMP, 1896         // 8-byte scratch preserved by all macros

// ======================================================================
// Block boundaries and control transfer
// ======================================================================

// Boundary check before continuing at MIPS address `pc` (an immediate).
// Fast path: count the boundary, compare icount with the attention mark.
// Slow path: the runtime emits checkpoints, runs device events and takes
// interrupts; if it redirects, control continues at the new PC.
// Clobbers x0, x16, x17, x30.
.macro M2M_BOUNDARY pc
    ldr   x16, [x27, #NS_BOUNDARY_LEFT]
    subs  x16, x16, #1
    str   x16, [x27, #NS_BOUNDARY_LEFT]
    ldr   x17, [x27, #NS_ATTN]
    ccmp  x28, x17, #2, ne
    b.lo  9001f
    ldr   w0, =\pc
    bl    _m2m_t_boundary
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9001:
.endm

// As M2M_BOUNDARY with the next PC in w0 (kept in w0).
.macro M2M_BOUNDARY_W0
    ldr   x16, [x27, #NS_BOUNDARY_LEFT]
    subs  x16, x16, #1
    str   x16, [x27, #NS_BOUNDARY_LEFT]
    ldr   x17, [x27, #NS_ATTN]
    ccmp  x28, x17, #2, ne
    b.lo  9002f
    bl    _m2m_t_boundary
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9002:
.endm

// Taken transfer to `label` in this unit (MIPS address `pc`), after the
// delay slot: boundary, then branch.
.macro M2M_GOTO label, pc
    M2M_BOUNDARY \pc
    b     \label
.endm

// Taken transfer to MIPS address `pc` outside this unit: boundary, then
// dispatch through the runtime table.
.macro M2M_GOTO_FAR pc
    M2M_BOUNDARY \pc
    ldr   w0, =\pc
    b     _m2m_dispatch
.endm

// Register-indirect transfer (jr/jalr): target virtual address in w0.
.macro M2M_GOTO_W0
    M2M_BOUNDARY_W0
    b     _m2m_dispatch
.endm

// Not-taken conditional branch (after its delay slot) or nullified
// branch-likely: boundary, then fall through to the code that follows,
// which must be the translation of MIPS address `pc`.
.macro M2M_FALL pc
    M2M_BOUNDARY \pc
.endm

// Leave the unit where MIPS execution simply continues at `pc` (the next
// address is not part of this unit). Not a boundary.
.macro M2M_FALLTHROUGH pc
    ldr   w0, =\pc
    b     _m2m_dispatch
.endm

// ======================================================================
// Memory bus. Input: w0 = virtual address (low 32 bits of base + imm).
// `pc` = address of the MIPS instruction, `ds` = 1 if it is in a delay
// slot, else 0 (for exceptions). Big-endian N64 memory is converted to
// host order: the result is the zero-extended value in x0.
// Clobbers x0, x16, x17, x30.
// ======================================================================

.macro M2M_READ8 pc, ds
    bic   w16, w0, #0x20000000
    and   w17, w16, #0xFF800000
    eor   w17, w17, #0x80000000
    cbnz  w17, 9010f
    and   w16, w16, #0x7FFFFF
    ldrb  w0, [x26, x16]
    b     9011f
9010:
    mov   w16, #1
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_read
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9011:
.endm

.macro M2M_READ16 pc, ds
    bic   w16, w0, #0x20000000
    and   w17, w16, #0xFF800001
    eor   w17, w17, #0x80000000
    cbnz  w17, 9012f
    and   w16, w16, #0x7FFFFF
    ldrh  w0, [x26, x16]
    rev16 w0, w0
    b     9013f
9012:
    mov   w16, #2
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_read
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9013:
.endm

.macro M2M_READ32 pc, ds
    bic   w16, w0, #0x20000000
    and   w17, w16, #0xFF800003
    eor   w17, w17, #0x80000000
    cbnz  w17, 9014f
    and   w16, w16, #0x7FFFFF
    ldr   w0, [x26, x16]
    rev   w0, w0
    b     9015f
9014:
    mov   w16, #4
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_read
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9015:
.endm

.macro M2M_READ64 pc, ds
    bic   w16, w0, #0x20000000
    and   w17, w16, #0xFF800007
    eor   w17, w17, #0x80000000
    cbnz  w17, 9016f
    and   w16, w16, #0x7FFFFF
    ldr   x0, [x26, x16]
    rev   x0, x0
    b     9017f
9016:
    mov   w16, #8
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_read
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9017:
.endm

// Stores: w0 = virtual address, x16 = value (low `size` bytes stored).
// Stores to translated code or outside RDRAM take the runtime path.
.macro M2M_WRITE8 pc, ds
    bic   w17, w0, #0x20000000
    and   w30, w17, #0xFF800000
    eor   w30, w30, #0x80000000
    cbnz  w30, 9020f
    and   w17, w17, #0x7FFFFF
    ldr   x30, [x27, #NS_CODEMAP]
    add   x30, x30, x17, lsr #2
    ldrb  w30, [x30]
    cbnz  w30, 9020f
    strb  w16, [x26, x17]
    b     9021f
9020:
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_write8
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9021:
.endm

.macro M2M_WRITE16 pc, ds
    bic   w17, w0, #0x20000000
    and   w30, w17, #0xFF800001
    eor   w30, w30, #0x80000000
    cbnz  w30, 9022f
    and   w17, w17, #0x7FFFFF
    ldr   x30, [x27, #NS_CODEMAP]
    add   x30, x30, x17, lsr #2
    ldrb  w30, [x30]
    cbnz  w30, 9022f
    rev16 w16, w16
    strh  w16, [x26, x17]
    b     9023f
9022:
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_write16
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9023:
.endm

.macro M2M_WRITE32 pc, ds
    bic   w17, w0, #0x20000000
    and   w30, w17, #0xFF800003
    eor   w30, w30, #0x80000000
    cbnz  w30, 9024f
    and   w17, w17, #0x7FFFFF
    ldr   x30, [x27, #NS_CODEMAP]
    add   x30, x30, x17, lsr #2
    ldrb  w30, [x30]
    cbnz  w30, 9024f
    rev   w16, w16
    str   w16, [x26, x17]
    b     9025f
9024:
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_write32
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9025:
.endm

.macro M2M_WRITE64 pc, ds
    bic   w17, w0, #0x20000000
    and   w30, w17, #0xFF800007
    eor   w30, w30, #0x80000000
    cbnz  w30, 9026f
    and   w17, w17, #0x7FFFFF
    ldr   x30, [x27, #NS_CODEMAP]
    add   x30, x30, x17, lsr #2
    ldrh  w30, [x30]
    cbnz  w30, 9026f
    rev   x16, x16
    str   x16, [x26, x17]
    b     9027f
9026:
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_write64
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9027:
.endm

// ======================================================================
// System services (always runtime calls). Clobber x0, x16, x17, x30.
// ======================================================================

// COP0 register `reg` -> x0 (full 64-bit register value; mfc0 must
// sign-extend the low word itself).
.macro M2M_MFC0 reg, pc, ds
    mov   w16, #\reg
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_mfc0
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
.endm

// x0 -> COP0 register `reg` (pass the sign-extended low word for mtc0,
// the full register for dmtc0).
.macro M2M_MTC0 reg, pc, ds
    mov   w16, #\reg
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_mtc0
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
.endm

// tlbr (1), tlbwi (2), tlbwr (6), tlbp (8).
.macro M2M_TLB op, pc
    mov   w16, #\op
    ldr   w17, =(\pc)
    bl    _m2m_t_tlb
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
.endm

// eret: returns through the runtime (boundary included). Never falls
// through.
.macro M2M_ERET pc
    ldr   w17, =(\pc)
    bl    _m2m_t_eret
    b     _m2m_redirect
.endm

// A path the capture never executed (e.g. the taken side of a
// branch-likely whose delay slot is not in the unit): the runtime logs
// `pc` as untranslated and stops. Never falls through.
.macro M2M_MISSING pc
    ldr   w0, =\pc
    bl    _m2m_t_missing
    b     _m2m_redirect
.endm

// Raise exception `code` for the instruction at pc (syscall 8, break 9,
// reserved 10, overflow 12, trap 13). Never falls through.
.macro M2M_RAISE code, pc, ds
    mov   w16, #\code
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_raise
    b     _m2m_redirect
.endm

// Require Status.CU1 (coprocessor-unusable exception otherwise). For
// cfc1/ctc1, which do not access FPRs.
.macro M2M_CU1 pc, ds
    ldr   w16, [x27, #NS_STATUS]
    tbnz  w16, #29, 9030f
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_cop1
    b     _m2m_redirect
9030:
.endm

// Require CU1 and Status.FR = 0 before touching FPRs (mfc1, mtc1,
// dmfc1, dmtc1, lwc1, swc1, ldc1, sdc1, and via M2M_FPU_BEGIN).
.macro M2M_COP1 pc, ds
    ldr   w16, [x27, #NS_STATUS]
    tbz   w16, #29, 9033f
    tbz   w16, #26, 9031f
9033:
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_cop1
    b     _m2m_redirect
9031:
.endm

// Brackets one FP arithmetic/convert/compare operation: BEGIN checks
// COP1 and clears the host FPSR; END folds the host FPSR into FCR31
// (cause, flags) and raises the floating-point exception if enabled.
// Store the result only after M2M_FPU_END.
.macro M2M_FPU_BEGIN pc, ds
    M2M_COP1 \pc, \ds
    msr   fpsr, xzr
.endm

.macro M2M_FPU_END pc, ds
    mrs   x16, fpsr
    ldr   w17, [x27, #NS_FCR31]
    and   w17, w17, #0x3F000
    orr   x17, x17, x16
    cbz   x17, 9032f
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_fpu
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
9032:
.endm

// cfc1: FCR `fs` -> x0 (sign-extended). ctc1: x0 -> FCR `fs`.
.macro M2M_CFC1 fs, pc, ds
    M2M_CU1 \pc, \ds
    mov   w16, #\fs
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_cfc1
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
.endm

.macro M2M_CTC1 fs, pc, ds
    M2M_CU1 \pc, \ds
    mov   w16, #\fs
    ldr   w17, =(\pc + \ds)
    bl    _m2m_t_ctc1
    cbz   x16, 9099f
    b     _m2m_redirect
9099:
.endm
