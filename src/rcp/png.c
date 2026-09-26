#include "src/rcp/png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
  if (!crc_table[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t v = i;
      for (int k = 0; k < 8; k++)
        v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      crc_table[i] = v;
    }
  for (size_t i = 0; i < n; i++)
    c = crc_table[(c ^ p[i]) & 255u] ^ (c >> 8);
  return c;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
  uint8_t hdr[8];
  put32(hdr, n);
  memcpy(hdr + 4, type, 4);
  fwrite(hdr, 1, 8, f);
  fwrite(data, 1, n, f);
  uint32_t c = crc32_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
  c = crc32_update(c, data, n) ^ 0xFFFFFFFFu;
  uint8_t tail[4];
  put32(tail, c);
  fwrite(tail, 1, 4, f);
}

int png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return 0;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13];
  put32(ihdr, w);
  put32(ihdr + 4, h);
  ihdr[8] = 8;  /* bit depth */
  ihdr[9] = 6;  /* RGBA */
  ihdr[10] = 0; /* deflate */
  ihdr[11] = 0; /* filter method */
  ihdr[12] = 0; /* no interlace */
  chunk(f, "IHDR", ihdr, 13);

  size_t raw_n = (size_t)h * (1 + (size_t)w * 4);
  uint8_t *raw = malloc(raw_n);
  for (uint32_t y = 0; y < h; y++) {
    raw[y * (1 + (size_t)w * 4)] = 0;
    memcpy(raw + y * (1 + (size_t)w * 4) + 1, rgba + (size_t)y * w * 4,
           (size_t)w * 4);
  }
  size_t blocks = (raw_n + 65534) / 65535;
  size_t z_n = 2 + blocks * 5 + raw_n + 4;
  uint8_t *z = malloc(z_n), *p = z;
  *p++ = 0x78;
  *p++ = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t off = 0; off < raw_n;) {
    size_t n = raw_n - off > 65535 ? 65535 : raw_n - off;
    *p++ = off + n == raw_n ? 1 : 0;
    *p++ = (uint8_t)n;
    *p++ = (uint8_t)(n >> 8);
    *p++ = (uint8_t)~n;
    *p++ = (uint8_t)(~n >> 8);
    memcpy(p, raw + off, n);
    for (size_t i = 0; i < n; i++) {
      a = (a + raw[off + i]) % 65521u;
      b = (b + a) % 65521u;
    }
    p += n;
    off += n;
  }
  put32(p, b << 16 | a);
  p += 4;
  chunk(f, "IDAT", z, (uint32_t)(p - z));
  chunk(f, "IEND", NULL, 0);
  free(raw);
  free(z);
  return fclose(f) == 0;
}

uint8_t *png_read(const char *path, uint32_t *w, uint32_t *h) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  rewind(f);
  uint8_t *file = malloc((size_t)n);
  if (fread(file, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(file);
    return NULL;
  }
  fclose(f);
  uint8_t *z = NULL, *out = NULL;
  size_t zn = 0;
  for (long off = 8; off + 12 <= n;) {
    uint32_t len = get32(file + off);
    const uint8_t *type = file + off + 4;
    if (!memcmp(type, "IHDR", 4)) {
      *w = get32(file + off + 8);
      *h = get32(file + off + 12);
    } else if (!memcmp(type, "IDAT", 4)) {
      z = realloc(z, zn + len);
      memcpy(z + zn, file + off + 8, len);
      zn += len;
    }
    off += 12 + (long)len;
  }
  size_t raw_n = (size_t)*h * (1 + (size_t)*w * 4);
  uint8_t *raw = malloc(raw_n);
  size_t got = 0;
  for (size_t i = 2; i + 5 <= zn && got < raw_n;) {
    int final = z[i] & 1;
    if ((z[i] >> 1) & 3) /* not a stored block */
      break;
    size_t len = z[i + 1] | (size_t)z[i + 2] << 8;
    if (i + 5 + len > zn || got + len > raw_n)
      break;
    memcpy(raw + got, z + i + 5, len);
    got += len;
    i += 5 + len;
    if (final)
      break;
  }
  if (got == raw_n) {
    out = malloc((size_t)*w * *h * 4);
    for (uint32_t y = 0; y < *h; y++)
      memcpy(out + (size_t)y * *w * 4, raw + y * (1 + (size_t)*w * 4) + 1,
             (size_t)*w * 4);
  }
  free(file);
  free(z);
  free(raw);
  return out;
}
