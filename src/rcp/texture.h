/* RDP texel formats -> RGBA8.
 *
 * Textures are decoded straight from RDRAM (not from an emulated TMEM):
 * the display-list HLE records where each TMEM load came from, so the
 * render tile's rows can be read at their source address and stride.
 * Formats: fmt 0 RGBA (16/32 bit), 2 CI (4/8 bit, via TLUT), 3 IA
 * (4/8/16 bit), 4 I (4/8 bit). Unknown combinations decode as I. */
#ifndef M2M_RCP_TEXTURE_H
#define M2M_RCP_TEXTURE_H

#include <stdint.h>

enum { TX_RGBA = 0, TX_YUV = 1, TX_CI = 2, TX_IA = 3, TX_I = 4 };
enum { TX_4B = 0, TX_8B = 1, TX_16B = 2, TX_32B = 3 };

/* Bits per texel of `siz`. */
static inline unsigned tex_bits(unsigned siz) {
  return 4u << siz;
}

/* Decodes a w x h texture; row r starts at addr + r * stride (bytes).
 * `tlut` holds 256 palette entries (host order); `tlut_ia` selects IA16
 * palette entries instead of RGBA16; `palette` is the CI4 bank. */
void tex_decode(uint8_t *out, const uint8_t *rdram, uint32_t rdram_size,
                uint32_t addr, uint32_t stride, uint32_t w, uint32_t h,
                unsigned fmt, unsigned siz, const uint16_t *tlut, int tlut_ia,
                unsigned palette);

#endif
