#include "src/tools/z64.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void parse_header(const uint8_t *p, Z64Header *h) {
  h->magic = z64_be32(p + 0x00);
  h->clock_rate = z64_be32(p + 0x04);
  h->entry_pc = z64_be32(p + 0x08);
  h->release = z64_be32(p + 0x0C);
  h->crc1 = z64_be32(p + 0x10);
  h->crc2 = z64_be32(p + 0x14);

  memcpy(h->name, p + 0x20, 20);
  h->name[20] = '\0';
  for (int i = 19; i >= 0 && (h->name[i] == ' ' || h->name[i] == '\0'); i--)
    h->name[i] = '\0';

  h->media = (char)p[0x3B];
  h->game_id[0] = (char)p[0x3C];
  h->game_id[1] = (char)p[0x3D];
  h->game_id[2] = '\0';
  h->region = (char)p[0x3E];
  h->version = p[0x3F];
}

Z64Status z64_open(const char *path, Z64Rom *rom) {
  memset(rom, 0, sizeof(*rom));
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return Z64_ERR_OPEN;

  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return Z64_ERR_OPEN;
  }
  /* Header + IPL3 is the minimum; images are whole 32-bit words. */
  if (st.st_size < Z64_DATA_OFFSET || (st.st_size & 3) != 0) {
    close(fd);
    return Z64_ERR_SIZE;
  }

  void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    return Z64_ERR_MAP;

  const uint8_t *data = map;
  uint32_t magic = z64_be32(data);
  Z64Status status = Z64_OK;
  if (magic == Z64_MAGIC_N64)
    status = Z64_ERR_BYTEORDER_N64;
  else if (magic == Z64_MAGIC_V64)
    status = Z64_ERR_BYTEORDER_V64;
  else if (magic != Z64_MAGIC)
    status = Z64_ERR_MAGIC;
  if (status != Z64_OK) {
    munmap(map, (size_t)st.st_size);
    return status;
  }

  rom->data = data;
  rom->size = (size_t)st.st_size;
  parse_header(data, &rom->header);
  return Z64_OK;
}

void z64_close(Z64Rom *rom) {
  if (rom->data)
    munmap((void *)rom->data, rom->size);
  memset(rom, 0, sizeof(*rom));
}

const char *z64_status_str(Z64Status s) {
  static const char *const names[] = {
      [Z64_OK] = "ok",
      [Z64_ERR_OPEN] = "cannot open file",
      [Z64_ERR_MAP] = "cannot map file",
      [Z64_ERR_SIZE] = "bad size (need >= 0x1000 bytes, multiple of 4)",
      [Z64_ERR_BYTEORDER_N64] = "n64 byte order (little-endian); need z64",
      [Z64_ERR_BYTEORDER_V64] = "v64 byte order (byte-swapped); need z64",
      [Z64_ERR_MAGIC] = "not an N64 ROM image (bad magic)",
  };
  return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}
