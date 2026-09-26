/* Minimal PNG I/O for RGBA8 images: zlib "stored" (uncompressed) deflate
 * blocks, filter 0. png_read accepts only files of that shape (what
 * png_write produces), which is all the golden tests need. */
#ifndef M2M_RCP_PNG_H
#define M2M_RCP_PNG_H

#include <stdint.h>

int png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
/* Returns malloc'd RGBA8 pixels or NULL. */
uint8_t *png_read(const char *path, uint32_t *w, uint32_t *h);

#endif
