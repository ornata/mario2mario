#include "src/rcp/texture.h"

static void rgba16(uint8_t *o, uint32_t v) {
  uint32_t r = (v >> 11) & 31u, g = (v >> 6) & 31u, b = (v >> 1) & 31u;
  o[0] = (uint8_t)(r << 3 | r >> 2);
  o[1] = (uint8_t)(g << 3 | g >> 2);
  o[2] = (uint8_t)(b << 3 | b >> 2);
  o[3] = v & 1u ? 255 : 0;
}

static void ia(uint8_t *o, uint32_t i, uint32_t a) {
  o[0] = o[1] = o[2] = (uint8_t)i;
  o[3] = (uint8_t)a;
}

void tex_decode(uint8_t *out, const uint8_t *rdram, uint32_t rdram_size,
                uint32_t addr, uint32_t stride, uint32_t w, uint32_t h,
                unsigned fmt, unsigned siz, const uint16_t *tlut, int tlut_ia,
                unsigned palette) {
  uint32_t mask = rdram_size - 1;
  for (uint32_t y = 0; y < h; y++) {
    uint32_t row = addr + y * stride;
    for (uint32_t x = 0; x < w; x++) {
      uint8_t *o = out + 4 * (y * w + x);
      uint32_t v;
      switch (siz) {
      case TX_4B: {
        uint8_t b = rdram[(row + x / 2) & mask];
        v = x & 1 ? b & 15u : b >> 4;
        break;
      }
      case TX_8B:
        v = rdram[(row + x) & mask];
        break;
      case TX_16B:
        v = (uint32_t)rdram[(row + 2 * x) & mask] << 8 |
            rdram[(row + 2 * x + 1) & mask];
        break;
      default:
        v = (uint32_t)rdram[(row + 4 * x) & mask] << 24 |
            (uint32_t)rdram[(row + 4 * x + 1) & mask] << 16 |
            (uint32_t)rdram[(row + 4 * x + 2) & mask] << 8 |
            rdram[(row + 4 * x + 3) & mask];
        break;
      }
      if (fmt == TX_CI) {
        uint32_t e = tlut[siz == TX_4B ? (palette * 16u + v) & 255u : v & 255u];
        if (tlut_ia)
          ia(o, e >> 8, e & 255u);
        else
          rgba16(o, e);
      } else if (fmt == TX_RGBA && siz == TX_16B) {
        rgba16(o, v);
      } else if (fmt == TX_RGBA && siz == TX_32B) {
        o[0] = (uint8_t)(v >> 24);
        o[1] = (uint8_t)(v >> 16);
        o[2] = (uint8_t)(v >> 8);
        o[3] = (uint8_t)v;
      } else if (fmt == TX_IA && siz == TX_4B) {
        uint32_t i = (v >> 1) * 255u / 7u;
        ia(o, i, v & 1u ? 255u : 0u);
      } else if (fmt == TX_IA && siz == TX_8B) {
        ia(o, (v >> 4) * 17u, (v & 15u) * 17u);
      } else if (fmt == TX_IA && siz == TX_16B) {
        ia(o, v >> 8, v & 255u);
      } else if (siz == TX_4B) {
        ia(o, v * 17u, v * 17u);
      } else if (siz == TX_8B) {
        ia(o, v, v);
      } else {
        ia(o, (v >> 8) & 255u, 255u);
      }
    }
  }
}
