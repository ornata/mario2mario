/* z64 ROM image access: header fields, IPL3, cartridge data.
 *
 * Only the native big-endian z64 byte order is accepted; n64/v64 images
 * are rejected rather than converted. The whole file is memory-mapped
 * read-only and all accessors are plain offsets into that mapping. */
#ifndef M2M_TOOLS_Z64_H
#define M2M_TOOLS_Z64_H

#include <stddef.h>
#include <stdint.h>

enum {
  Z64_HEADER_SIZE = 0x40,
  Z64_IPL3_OFFSET = 0x40,
  Z64_IPL3_SIZE = 0x1000 - 0x40,
  Z64_DATA_OFFSET = 0x1000,
};

/* Header magics as the first big-endian word of each byte order. */
#define Z64_MAGIC     0x80371240u /* z64: native big-endian */
#define Z64_MAGIC_N64 0x40123780u /* n64: 32-bit little-endian */
#define Z64_MAGIC_V64 0x37804012u /* v64: 16-bit byte-swapped */

typedef enum {
  Z64_OK = 0,
  Z64_ERR_OPEN,
  Z64_ERR_MAP,
  Z64_ERR_SIZE,
  Z64_ERR_BYTEORDER_N64,
  Z64_ERR_BYTEORDER_V64,
  Z64_ERR_MAGIC,
} Z64Status;

/* Decoded header (ROM 0x00..0x3F). All multi-byte fields are big-endian
 * in the image and host-order here. */
typedef struct {
  uint32_t magic;      /* 0x00: PI BSD domain 1 config + magic */
  uint32_t clock_rate; /* 0x04 */
  uint32_t entry_pc;   /* 0x08: boot address the IPL3 jumps to */
  uint32_t release;    /* 0x0C: libultra release */
  uint32_t crc1;       /* 0x10 */
  uint32_t crc2;       /* 0x14 */
  char name[21];       /* 0x20..0x33, NUL-terminated, trailing spaces cut */
  char media;          /* 0x3B: 'N' cartridge, ... */
  char game_id[3];     /* 0x3C..0x3D + NUL: two-character game id */
  char region;         /* 0x3E: 'E' USA, 'J' Japan, 'P' Europe, ... */
  uint8_t version;     /* 0x3F */
} Z64Header;

typedef struct {
  const uint8_t *data; /* whole image, read-only mapping */
  size_t size;         /* bytes */
  Z64Header header;
} Z64Rom;

Z64Status z64_open(const char *path, Z64Rom *rom);
void z64_close(Z64Rom *rom);
const char *z64_status_str(Z64Status s);

static inline uint32_t z64_be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         (uint32_t)p[3];
}

static inline const uint8_t *z64_ipl3(const Z64Rom *rom) {
  return rom->data + Z64_IPL3_OFFSET;
}

static inline const uint8_t *z64_cart_data(const Z64Rom *rom) {
  return rom->data + Z64_DATA_OFFSET;
}

static inline size_t z64_cart_data_size(const Z64Rom *rom) {
  return rom->size - Z64_DATA_OFFSET;
}

#endif
