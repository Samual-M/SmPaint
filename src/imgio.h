/* imgio.h - portable image file codecs (no external libraries). Pixels are 0x00RRGGBB. */
#ifndef IMGIO_H
#define IMGIO_H
#include <stddef.h>
#include <stdint.h>

/* each decoder returns a malloc'd w*h pixel array, or NULL if the data is not valid/supported */
uint32_t *png_decode(const uint8_t *d, size_t n, int *w, int *h);
uint32_t *bmp_decode(const uint8_t *d, size_t n, int *w, int *h); /* with or without BITMAPFILEHEADER */
uint32_t *gif_decode(const uint8_t *d, size_t n, int *w, int *h);
uint32_t *jpg_decode(const uint8_t *d, size_t n, int *w, int *h);
uint32_t *any_decode(const uint8_t *d, size_t n, int *w, int *h); /* sniffs the format */

/* encoders return malloc'd bytes, size in *n */
uint8_t *png_encode(const uint32_t *px, int w, int h, size_t *n);
uint8_t *pdf_encode(const uint32_t *px, int w, int h, size_t *n);
uint8_t *bmp_encode(const uint32_t *px, int w, int h, int bpp /*1,4,8,24*/, int file_header, size_t *n);
uint8_t *gif_encode(const uint32_t *px, int w, int h, size_t *n);
uint8_t *jpg_encode(const uint32_t *px, int w, int h, int quality, size_t *n);

uint8_t *file_read_all(const char *path, size_t *n);
int file_write_all(const char *path, const uint8_t *d, size_t n);
#endif
