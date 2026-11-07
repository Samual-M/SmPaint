/* gfx.h - portable software rendering helpers (framebuffer / canvas images, UI font, 3D edges) */
#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t px_t; /* 0x00RRGGBB */
#define RGB(r, g, b) ((px_t)(((r) << 16) | ((g) << 8) | (b)))
#define PR(c) (((c) >> 16) & 255)
#define PG(c) (((c) >> 8) & 255)
#define PB(c) ((c) & 255)

typedef struct { int w, h; px_t *p; } Img;

/* classic Windows UI colors */
#define C_FACE RGB(192, 192, 192)
#define C_HILITE RGB(255, 255, 255)
#define C_LIGHT RGB(223, 223, 223)
#define C_SHADOW RGB(128, 128, 128)
#define C_DKSHADOW RGB(64, 64, 64)
#define C_BLACK RGB(0, 0, 0)
#define C_WHITE RGB(255, 255, 255)
#define C_NAVY RGB(0, 0, 128)
#define C_WORKSPACE RGB(128, 128, 128)

Img *img_new(int w, int h, px_t fill);
Img *img_clone(const Img *s);
void img_free(Img *i);
void img_fill(Img *i, px_t c);
static inline px_t img_get(const Img *i, int x, int y) { return (x < 0 || y < 0 || x >= i->w || y >= i->h) ? 0 : i->p[y * i->w + x]; }
static inline void img_put(Img *i, int x, int y, px_t c) { if (x >= 0 && y >= 0 && x < i->w && y < i->h) i->p[y * i->w + x] = c; }

void gx_rect(Img *d, int x, int y, int w, int h, px_t c);
void gx_frame(Img *d, int x, int y, int w, int h, px_t c);
void gx_hline(Img *d, int x1, int x2, int y, px_t c);
void gx_vline(Img *d, int x, int y1, int y2, px_t c);
void gx_dotrect(Img *d, int x, int y, int w, int h, px_t a, px_t b, int phase); /* alternating dotted */
void gx_raised(Img *d, int x, int y, int w, int h);        /* classic raised button edge (2px) */
void gx_sunken(Img *d, int x, int y, int w, int h);        /* classic sunken edge (2px) */
void gx_sunken1(Img *d, int x, int y, int w, int h);       /* 1px sunken (status panes) */
void gx_raised1(Img *d, int x, int y, int w, int h);       /* 1px raised */
void gx_etched(Img *d, int x, int y, int w, int h);        /* group box frame */
void gx_argb(Img *d, int x, int y, const uint32_t *src, int sw, int sh, int sx, int sy, int cw, int ch, int tint_on, px_t tint);
void gx_argb_recolor(Img *d, int x, int y, const uint32_t *src, int sw, int sh, int sx, int sy, int cw, int ch, px_t from, px_t to);

/* UI text (embedded bitmap font). '&' marks mnemonic if mnem != 0. Returns advance width. */
int tx_width(const char *s, int bold, int mnem);
int tx_big(int on);   /* switch to the larger UI text; returns the previous state */
int tx_draw(Img *d, int x, int y, const char *s, px_t c, int bold, int mnem);
int tx_draw_dis(Img *d, int x, int y, const char *s, int mnem); /* disabled look (embossed) */
void tx_draw_clip(Img *d, int x, int y, const char *s, px_t c, int maxw); /* clipped to maxw px */
int tx_ellipsis_fit(const char *s, int maxw, char *out, int outn);

/* portable case-insensitive compares (ASCII) */
int str_icmp(const char *a, const char *b);
int str_nicmp(const char *a, const char *b, size_t n);

#endif
