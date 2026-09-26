/* COP1: moves, arithmetic, conversions, compares.
 *
 * Arithmetic runs on the host FPU with its rounding mode set from
 * FCR31.RM before every operation (this file is built with a strict FP
 * model so the compiler neither folds nor reorders). IEEE flags come
 * from the host and are mapped to FCR31 cause/flag bits; a cause bit
 * whose enable is set, or Unimplemented (E), raises EXC_FPE without
 * writing the result.
 *
 * VR4300-specific rules modelled here:
 * - NaN results are the MIPS default quiet NaNs 0x7FBFFFFF /
 *   0x7FF7FFFFFFFFFFFF; a NaN operand to arithmetic raises Invalid.
 * - With FCR31.FS a denormal result flushes to signed zero (U|I);
 *   without FS it raises Unimplemented.
 * - Conversion to W/L of NaN, infinity or an out-of-range value raises
 *   Unimplemented (the VR4300 does not produce the IEEE invalid result).
 * - Not modelled: Unimplemented on denormal operands. */
#include <fenv.h>
#include <string.h>
#include <tgmath.h>

#include "src/oracle/cpu.h"

#define RT(w) (((w) >> 16) & 31u)
#define FS(w) (((w) >> 11) & 31u)
#define FT(w) (((w) >> 16) & 31u)
#define FD(w) (((w) >> 6) & 31u)

#define NAN_S 0x7FBFFFFFu
#define NAN_D 0x7FF7FFFFFFFFFFFFull

void fpu_sync_host(const CpuState *c) {
  static const int modes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD,
                               FE_DOWNWARD};
  fesetround(modes[c->fcr31 & FCR31_RM]);
}

static unsigned host_cause(void) {
  int e = fetestexcept(FE_ALL_EXCEPT);
  return (e & FE_INEXACT ? FPX_I : 0) | (e & FE_UNDERFLOW ? FPX_U : 0) |
         (e & FE_OVERFLOW ? FPX_O : 0) | (e & FE_DIVBYZERO ? FPX_Z : 0) |
         (e & FE_INVALID ? FPX_V : 0);
}

/* Sets FCR31.cause; traps or accumulates flags. Returns 1 to commit. */
static int commit(Oracle *o, unsigned cause) {
  CpuState *c = &o->cpu;
  unsigned enables = (c->fcr31 >> FCR31_ENABLE_SH) & 31u;
  c->fcr31 = (c->fcr31 & ~(0x3Fu << FCR31_CAUSE_SH)) | cause << FCR31_CAUSE_SH;
  if ((cause & enables) || (cause & FPX_E)) {
    cpu_exception(o, EXC_FPE, 0);
    return 0;
  }
  c->fcr31 |= (cause & 31u) << FCR31_FLAGS_SH;
  return 1;
}

int fpu_write_fcr31(Oracle *o, uint32_t v) {
  CpuState *c = &o->cpu;
  c->fcr31 = v & FCR31_MASK;
  fpu_sync_host(c);
  unsigned cause = (c->fcr31 >> FCR31_CAUSE_SH) & 0x3Fu;
  unsigned enables = (c->fcr31 >> FCR31_ENABLE_SH) & 31u;
  if ((cause & enables) || (cause & FPX_E)) {
    cpu_exception(o, EXC_FPE, 0);
    return 0;
  }
  return 1;
}

/* Rounds x to an integral value with MIPS rounding mode rm (exact). */
static double round_rm(double x, unsigned rm) {
  switch (rm & 3u) {
  case 1:
    return trunc(x);
  case 2:
    return ceil(x);
  case 3:
    return floor(x);
  default: {
    double f = floor(x), d = x - f;
    if (d > 0.5 || (d == 0.5 && fmod(f, 2.0) != 0.0))
      f += 1.0;
    return f;
  }
  }
}

/* Converts x to a W (bits=32) or L (bits=64) integer. */
static int to_int(double x, unsigned rm, int bits, uint64_t *out,
                  unsigned *cause) {
  if (isnan(x) || isinf(x)) {
    *cause |= FPX_E;
    return 0;
  }
  double r = round_rm(x, rm);
  double lim = bits == 32 ? 2147483648.0 : 9223372036854775808.0;
  if (r >= lim || r < -lim) {
    *cause |= FPX_E;
    return 0;
  }
  if (r != x)
    *cause |= FPX_I;
  *out = bits == 32 ? (uint64_t)(uint32_t)(int32_t)r : (uint64_t)(int64_t)r;
  return 1;
}

static float f32(uint32_t b) {
  float f;
  memcpy(&f, &b, 4);
  return f;
}
static uint32_t b32(float f) {
  uint32_t b;
  memcpy(&b, &f, 4);
  return b;
}
static double f64(uint64_t b) {
  double d;
  memcpy(&d, &b, 8);
  return d;
}
static uint64_t b64(double d) {
  uint64_t b;
  memcpy(&b, &d, 8);
  return b;
}

/* Result fix-ups shared by both formats. */
static uint32_t fix32(float r, unsigned *cause, uint32_t fcr31) {
  if (isnan(r))
    return NAN_S;
  if (fpclassify(r) == FP_SUBNORMAL) {
    if (!(fcr31 & FCR31_FS)) {
      *cause |= FPX_E;
      return 0;
    }
    *cause |= FPX_U | FPX_I;
    return b32(r) & 0x80000000u;
  }
  return b32(r);
}

static uint64_t fix64(double r, unsigned *cause, uint32_t fcr31) {
  if (isnan(r))
    return NAN_D;
  if (fpclassify(r) == FP_SUBNORMAL) {
    if (!(fcr31 & FCR31_FS)) {
      *cause |= FPX_E;
      return 0;
    }
    *cause |= FPX_U | FPX_I;
    return b64(r) & 0x8000000000000000ull;
  }
  return b64(r);
}

/* MIPS legacy NaN encoding: the mantissa MSB set means signalling. */
static int snan32(uint32_t b) {
  return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) &&
         (b & 0x00400000u);
}
static int snan64(uint64_t b) {
  return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
         (b & 0x000FFFFFFFFFFFFFull) && (b & 0x0008000000000000ull);
}

/* One instantiation per format: T is the C type, G/P read and write the
 * raw bits, F/B convert bits<->T, FIX fixes results, SNAN tests
 * operands, NAN is the default NaN, W is the bit width. */
#define DEFINE_FP_OP(NAME, T, G, P, F, B, FIX, SNAN, NANV)                     \
  static int NAME(Oracle *o, uint32_t w) {                                     \
    CpuState *c = &o->cpu;                                                     \
    unsigned funct = w & 63u, cause = 0;                                       \
    uint64_t sbits = G(c, FS(w)), tbits = G(c, FT(w));                         \
    T a = F(sbits), b = F(tbits), r = 0;                                       \
    uint64_t out = 0;                                                          \
    int wide = 0; /* result is a 64-bit (D or L) value */                      \
    fpu_sync_host(c);                                                          \
    feclearexcept(FE_ALL_EXCEPT);                                              \
    if (funct >= 48) { /* c.cond.fmt */                                        \
      int un = isnan(a) || isnan(b);                                           \
      int res = (un && (funct & 1)) || (!un && a == b && (funct & 2)) ||       \
                (!un && a < b && (funct & 4));                                 \
      if (un && ((funct & 8) || SNAN(sbits) || SNAN(tbits)))                   \
        cause |= FPX_V;                                                        \
      if (!commit(o, cause))                                                   \
        return 0;                                                              \
      c->fcr31 = res ? c->fcr31 | FCR31_C : c->fcr31 & ~FCR31_C;               \
      return 1;                                                                \
    }                                                                          \
    int two = funct <= 3, arith = funct <= 7 && funct != 6;                    \
    if (arith && (isnan(a) || (two && isnan(b)))) {                            \
      cause |= FPX_V;                                                          \
      out = NANV;                                                              \
      wide = sizeof(T) == 8;                                                   \
      goto done;                                                               \
    }                                                                          \
    switch (funct) {                                                           \
    case 0:                                                                    \
      r = a + b;                                                               \
      break;                                                                   \
    case 1:                                                                    \
      r = a - b;                                                               \
      break;                                                                   \
    case 2:                                                                    \
      r = a * b;                                                               \
      break;                                                                   \
    case 3:                                                                    \
      r = a / b;                                                               \
      break;                                                                   \
    case 4:                                                                    \
      r = (T)sqrt(a);                                                          \
      break;                                                                   \
    case 5:                                                                    \
      r = (T)fabs(a);                                                          \
      break;                                                                   \
    case 6: /* mov: bit copy, no exceptions */                                 \
      P(c, FD(w), sbits);                                                      \
      return 1;                                                                \
    case 7:                                                                    \
      r = -a;                                                                  \
      break;                                                                   \
    case 8:                                                                    \
    case 9:                                                                    \
    case 10:                                                                   \
    case 11: /* round/trunc/ceil/floor.l */                                    \
    case 12:                                                                   \
    case 13:                                                                   \
    case 14:                                                                   \
    case 15: /* ... .w */                                                      \
    case 36:                                                                   \
    case 37: { /* cvt.w / cvt.l */                                             \
      int bits = funct == 36 || (funct >= 12 && funct <= 15) ? 32 : 64;        \
      unsigned rm = funct >= 36 ? c->fcr31 & FCR31_RM : funct & 3u;            \
      if (isnan(a) && SNAN(sbits))                                             \
        cause |= FPX_V;                                                        \
      if (!to_int((double)a, rm, bits, &out, &cause) && !(cause & FPX_E))      \
        cause |= FPX_E;                                                        \
      wide = bits == 64;                                                       \
      goto done;                                                               \
    }                                                                          \
    case 32: /* cvt.s */                                                       \
      if (sizeof(T) == 4)                                                      \
        goto unimplemented;                                                    \
      if (isnan(a)) {                                                          \
        cause |= FPX_V;                                                        \
        out = NAN_S;                                                           \
        goto done;                                                             \
      }                                                                        \
      out = fix32((float)a, &cause, c->fcr31);                                 \
      cause |= host_cause();                                                   \
      goto done;                                                               \
    case 33: /* cvt.d */                                                       \
      if (sizeof(T) == 8)                                                      \
        goto unimplemented;                                                    \
      wide = 1;                                                                \
      if (isnan(a)) {                                                          \
        cause |= FPX_V;                                                        \
        out = NAN_D;                                                           \
        goto done;                                                             \
      }                                                                        \
      out = fix64((double)a, &cause, c->fcr31);                                \
      cause |= host_cause();                                                   \
      goto done;                                                               \
    default:                                                                   \
      goto unimplemented;                                                      \
    }                                                                          \
    cause |= host_cause();                                                     \
    out = FIX(r, &cause, c->fcr31);                                            \
    wide = sizeof(T) == 8;                                                     \
  done:                                                                        \
    if (!commit(o, cause))                                                     \
      return 0;                                                                \
    if (wide)                                                                  \
      fpr_set64(c, FD(w), out);                                                \
    else                                                                       \
      fpr_set32(c, FD(w), (uint32_t)out);                                      \
    return 1;                                                                  \
  unimplemented:                                                               \
    commit(o, FPX_E);                                                          \
    return 0;                                                                  \
  }

static uint64_t get32_bits(const CpuState *c, unsigned i) {
  return fpr_get32(c, i);
}
static float f32_of(uint64_t b) {
  return f32((uint32_t)b);
}
static void put32_bits(CpuState *c, unsigned i, uint64_t v) {
  fpr_set32(c, i, (uint32_t)v);
}

DEFINE_FP_OP(fp_single, float, get32_bits, put32_bits, f32_of, b32, fix32,
             snan32, NAN_S)
DEFINE_FP_OP(fp_double, double, fpr_get64, fpr_set64, f64, b64, fix64, snan64,
             NAN_D)

/* cvt.s / cvt.d from integer formats W (fmt 20) and L (fmt 21). */
static int fp_from_int(Oracle *o, uint32_t w, int from64) {
  CpuState *c = &o->cpu;
  unsigned funct = w & 63u, cause = 0;
  int64_t v = from64 ? (int64_t)fpr_get64(c, FS(w))
                     : (int64_t)(int32_t)fpr_get32(c, FS(w));
  fpu_sync_host(c);
  feclearexcept(FE_ALL_EXCEPT);
  if (funct == 32) {
    uint32_t out = b32((float)v);
    cause |= host_cause();
    if (!commit(o, cause))
      return 0;
    fpr_set32(c, FD(w), out);
    return 1;
  }
  if (funct == 33) {
    uint64_t out = b64((double)v);
    cause |= host_cause();
    if (!commit(o, cause))
      return 0;
    fpr_set64(c, FD(w), out);
    return 1;
  }
  commit(o, FPX_E);
  return 0;
}

int fpu_exec(Oracle *o, uint32_t w) {
  CpuState *c = &o->cpu;
  switch ((w >> 21) & 31u) {
  case 0: /* mfc1 */
    c->gpr[RT(w)] = sext32(fpr_get32(c, FS(w)));
    return 1;
  case 1: /* dmfc1 */
    c->gpr[RT(w)] = fpr_get64(c, FS(w));
    return 1;
  case 2: /* cfc1 */
    c->gpr[RT(w)] = FS(w) == 31  ? sext32(c->fcr31)
                    : FS(w) == 0 ? sext32(c->fcr0)
                                 : 0;
    return 1;
  case 4: /* mtc1 */
    fpr_set32(c, FS(w), (uint32_t)c->gpr[RT(w)]);
    return 1;
  case 5: /* dmtc1 */
    fpr_set64(c, FS(w), c->gpr[RT(w)]);
    return 1;
  case 6: /* ctc1 */
    if (FS(w) == 31)
      return fpu_write_fcr31(o, (uint32_t)c->gpr[RT(w)]);
    return 1;
  case 16:
    return fp_single(o, w);
  case 17:
    return fp_double(o, w);
  case 20:
    return fp_from_int(o, w, 0);
  case 21:
    return fp_from_int(o, w, 1);
  default:
    commit(o, FPX_E);
    return 0;
  }
}
