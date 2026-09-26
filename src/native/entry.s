// Runtime glue between C and translated code (hand-written).
//
// _m2m_enter(ns, code): C -> translated code. _m2m_exit: back to C.
// _m2m_dispatch: w0 = MIPS virtual address -> jump to its code.
// _m2m_lazy: default dispatch entry; asks the runtime to link a unit.
// _m2m_redirect: continue at cpu.pc after a runtime-changed PC (or stop).
// _m2m_t_<name>: save the MIPS register file into NativeState, call the C
// service rt_<name>(ns, x0, x16, w17), restore, return x0 = value and
// x16 = redirect flag.

.include "src/native/m2m.inc"

.text

// Loads / stores the host-resident MIPS registers from / to cpu.gpr[].
.macro SAVE_MIPS
    stp   x1, x2, [x27, #8]
    stp   x3, x4, [x27, #24]
    stp   x5, x6, [x27, #40]
    stp   x7, x8, [x27, #56]
    stp   x9, x10, [x27, #72]
    stp   x11, x12, [x27, #88]
    stp   x13, x14, [x27, #104]
    str   x15, [x27, #120]
    stp   x19, x20, [x27, #152]
    stp   x21, x22, [x27, #168]
    stp   x23, x24, [x27, #184]
    str   x25, [x27, #200]
    str   x29, [x27, #232]
    str   x28, [x27, #NS_ICOUNT]
.endm

.macro LOAD_MIPS
    ldp   x1, x2, [x27, #8]
    ldp   x3, x4, [x27, #24]
    ldp   x5, x6, [x27, #40]
    ldp   x7, x8, [x27, #56]
    ldp   x9, x10, [x27, #72]
    ldp   x11, x12, [x27, #88]
    ldp   x13, x14, [x27, #104]
    ldr   x15, [x27, #120]
    ldp   x19, x20, [x27, #152]
    ldp   x21, x22, [x27, #168]
    ldp   x23, x24, [x27, #184]
    ldr   x25, [x27, #200]
    ldr   x29, [x27, #232]
    ldr   x28, [x27, #NS_ICOUNT]
.endm

    .globl _m2m_enter
    .p2align 2
_m2m_enter:
    stp   x29, x30, [sp, #-160]!
    stp   x19, x20, [sp, #16]
    stp   x21, x22, [sp, #32]
    stp   x23, x24, [sp, #48]
    stp   x25, x26, [sp, #64]
    stp   x27, x28, [sp, #80]
    stp   d8, d9, [sp, #96]
    stp   d10, d11, [sp, #112]
    stp   d12, d13, [sp, #128]
    stp   d14, d15, [sp, #144]
    mov   x27, x0
    ldr   x26, [x27, #1888]
    mov   x16, x1
    LOAD_MIPS
    br    x16

    .globl _m2m_exit
    .p2align 2
_m2m_exit:
    SAVE_MIPS
    ldp   x19, x20, [sp, #16]
    ldp   x21, x22, [sp, #32]
    ldp   x23, x24, [sp, #48]
    ldp   x25, x26, [sp, #64]
    ldp   x27, x28, [sp, #80]
    ldp   d8, d9, [sp, #96]
    ldp   d10, d11, [sp, #112]
    ldp   d12, d13, [sp, #128]
    ldp   d14, d15, [sp, #144]
    ldp   x29, x30, [sp], #160
    ret

    .globl _m2m_redirect
    .p2align 2
_m2m_redirect:
    ldr   w16, [x27, #NS_STOP]
    cbnz  w16, _m2m_exit
    ldr   w0, [x27, #796]        // cpu.pc
    // fall through into dispatch

    .globl _m2m_dispatch
    .p2align 2
_m2m_dispatch:
    lsr   w16, w0, #23
    cmp   w16, #0x100            // KSEG0 RDRAM
    b.ne  1f
    ubfx  x16, x0, #2, #21
    ldr   x17, [x27, #NS_DISPATCH]
    ldr   x17, [x17, x16, lsl #3]
    br    x17
1:  bl    _m2m_t_dispatch
    cbnz  x16, _m2m_exit
    br    x0

    .globl _m2m_lazy
    .p2align 2
_m2m_lazy:                        // w0 = vaddr being dispatched
    bl    _m2m_t_link
    cbnz  x16, _m2m_exit
    br    x0

.macro TRAMP name
    .globl _m2m_t_\name
    .p2align 2
_m2m_t_\name:
    SAVE_MIPS
    sub   sp, sp, #80
    stp   x30, xzr, [sp]
    stp   d0, d1, [sp, #16]
    stp   d2, d3, [sp, #32]
    stp   d4, d5, [sp, #48]
    stp   d6, d7, [sp, #64]
    mov   w3, w17
    mov   x2, x16
    mov   x1, x0
    mov   x0, x27
    bl    _rt_\name
    mov   x16, x1
    ldp   d0, d1, [sp, #16]
    ldp   d2, d3, [sp, #32]
    ldp   d4, d5, [sp, #48]
    ldp   d6, d7, [sp, #64]
    ldr   x30, [sp]
    add   sp, sp, #80
    LOAD_MIPS
    ret
.endm

TRAMP boundary
TRAMP read
TRAMP write8
TRAMP write16
TRAMP write32
TRAMP write64
TRAMP mfc0
TRAMP mtc0
TRAMP tlb
TRAMP eret
TRAMP raise
TRAMP cop1
TRAMP fpu
TRAMP cfc1
TRAMP ctc1
TRAMP dispatch
TRAMP link
TRAMP missing
