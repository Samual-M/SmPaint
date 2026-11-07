/* gfx.c - portable software rendering helpers */
#include "gfx.h"
#include "plat.h"
#include "font.h"

Img *img_new(int w, int h, px_t fill)
{
    Img *i = (Img *)malloc(sizeof(Img));
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    i->w = w; i->h = h;
    i->p = (px_t *)malloc((size_t)w * h * sizeof(px_t));
    for (size_t k = 0; k < (size_t)w * h; k++) i->p[k] = fill;
    return i;
}
Img *img_clone(const Img *s)
{
    Img *i = (Img *)malloc(sizeof(Img));
    i->w = s->w; i->h = s->h;
    i->p = (px_t *)malloc((size_t)s->w * s->h * sizeof(px_t));
    memcpy(i->p, s->p, (size_t)s->w * s->h * sizeof(px_t));
    return i;
}
void img_free(Img *i) { if (i) { free(i->p); free(i); } }
void img_fill(Img *i, px_t c) { for (size_t k = 0; k < (size_t)i->w * i->h; k++) i->p[k] = c; }

void gx_rect(Img *d, int x, int y, int w, int h, px_t c)
{
    int x2 = x + w, y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > d->w) x2 = d->w;
    if (y2 > d->h) y2 = d->h;
    for (int j = y; j < y2; j++) {
        px_t *r = d->p + (size_t)j * d->w;
        for (int i = x; i < x2; i++) r[i] = c;
    }
}
void gx_hline(Img *d, int x1, int x2, int y, px_t c) { gx_rect(d, x1, y, x2 - x1 + 1, 1, c); }
void gx_vline(Img *d, int x, int y1, int y2, px_t c) { gx_rect(d, x, y1, 1, y2 - y1 + 1, c); }
void gx_frame(Img *d, int x, int y, int w, int h, px_t c)
{
    gx_rect(d, x, y, w, 1, c); gx_rect(d, x, y + h - 1, w, 1, c);
    gx_rect(d, x, y, 1, h, c); gx_rect(d, x + w - 1, y, 1, h, c);
}
void gx_dotrect(Img *d, int x, int y, int w, int h, px_t a, px_t b, int phase)
{
    for (int i = 0; i < w; i++) {
        img_put(d, x + i, y, ((i + phase) & 1) ? a : b);
        img_put(d, x + i, y + h - 1, ((i + h + phase) & 1) ? a : b);
    }
    for (int j = 1; j < h - 1; j++) {
        img_put(d, x, y + j, ((j + phase) & 1) ? a : b);
        img_put(d, x + w - 1, y + j, ((j + w + phase) & 1) ? a : b);
    }
}
static void edge1(Img *d, int x, int y, int w, int h, px_t tl, px_t br)
{
    gx_rect(d, x, y, w - 1, 1, tl); gx_rect(d, x, y, 1, h - 1, tl);
    gx_rect(d, x + 1, y + h - 1, w - 1, 1, br); gx_rect(d, x + w - 1, y + 1, 1, h - 1, br);
}
void gx_raised1(Img *d, int x, int y, int w, int h) { edge1(d, x, y, w, h, C_HILITE, C_SHADOW); }
void gx_sunken1(Img *d, int x, int y, int w, int h) { edge1(d, x, y, w, h, C_SHADOW, C_HILITE); }
void gx_raised(Img *d, int x, int y, int w, int h)
{
    edge1(d, x, y, w, h, C_HILITE, C_DKSHADOW);
    edge1(d, x + 1, y + 1, w - 2, h - 2, C_LIGHT, C_SHADOW);
}
void gx_sunken(Img *d, int x, int y, int w, int h)
{
    edge1(d, x, y, w, h, C_SHADOW, C_HILITE);
    edge1(d, x + 1, y + 1, w - 2, h - 2, C_DKSHADOW, C_LIGHT);
}
void gx_etched(Img *d, int x, int y, int w, int h)
{
    edge1(d, x, y, w, h, C_SHADOW, C_HILITE);
    edge1(d, x + 1, y + 1, w - 2, h - 2, C_HILITE, C_SHADOW);
}

void gx_argb(Img *d, int x, int y, const uint32_t *src, int sw, int sh, int sx, int sy, int cw, int ch, int tint_on, px_t tint)
{
    (void)sh;
    for (int j = 0; j < ch; j++)
        for (int i = 0; i < cw; i++) {
            uint32_t v = src[(sy + j) * sw + sx + i];
            if ((v >> 24) < 128) continue;
            img_put(d, x + i, y + j, tint_on ? tint : (v & 0xffffff));
        }
}
void gx_argb_recolor(Img *d, int x, int y, const uint32_t *src, int sw, int sh, int sx, int sy, int cw, int ch, px_t from, px_t to)
{
    (void)sh;
    for (int j = 0; j < ch; j++)
        for (int i = 0; i < cw; i++) {
            uint32_t v = src[(sy + j) * sw + sx + i];
            if ((v >> 24) < 128) continue;
            v &= 0xffffff;
            img_put(d, x + i, y + j, v == from ? to : v);
        }
}

/* ---- UI text ---- */
static int decode_utf8(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    int c = *s++;
    if (c >= 0xf0) { c = ((c & 7) << 18) | ((s[0] & 63) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); s += 3; }
    else if (c >= 0xe0) { c = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63); s += 2; }
    else if (c >= 0xc0) { c = ((c & 31) << 6) | (s[0] & 63); s += 1; }
    *ps = (const char *)s;
    return c;
}
static int glyph_index(int c) { return (c >= 32 && c < 256) ? c - 32 : ('?' - 32); }
static int gw(int c, int bold) { return (bold ? font_bold_adv : font_reg_adv)[glyph_index(c)]; }

/* ---- larger, clearer UI text for the menu bar, menus, status bar, text bar and drop-down lists ----
   (dialogs keep the original bitmap font because their layouts are fixed-size) */
static int g_big;
int tx_big(int on) { int p = g_big; g_big = on; return p; }
#define BIG_PX 13
typedef struct { uint64_t key; uint8_t *m; int w, h; } BigGlyphs;
static BigGlyphs big_cache[512];
static void big_plain(const char *s, int mnem, char *out, int outn, int *ul_byte)
{
    int n = 0; *ul_byte = -1;
    while (*s && n < outn - 1) {
        if (mnem && *s == '&') { s++; if (*s == '&') { out[n++] = *s++; continue; } if (*s && *ul_byte < 0) *ul_byte = n; continue; }
        if (*s == '\t') { s++; continue; }
        out[n++] = *s++;
    }
    out[n] = 0;
}
static const BigGlyphs *big_get(const char *plain, int bold)
{
    uint64_t h = 1469598103934665603ull ^ (uint64_t)bold;
    for (const char *p = plain; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ull;
    h |= 1; BigGlyphs *e = &big_cache[h & 511];
    if (e->key == h) return e;
    free(e->m); e->m = NULL; e->w = e->h = 0;
    if (*plain) e->m = plat_render_text(plain, "Arial", BIG_PX, bold, 0, 0, &e->w, &e->h);
    e->key = h; return e;
}
int tx_width(const char *s, int bold, int mnem)
{
    if (g_big) { char b[512]; int u; big_plain(s, mnem, b, sizeof b, &u); if (!*b) return 0; return big_get(b, bold)->w; }
    int w = 0;
    while (*s) {
        int c = decode_utf8(&s);
        if (mnem && c == '&') { if (*s == '&') { s++; c = '&'; } else continue; }
        if (c == '\t') continue;
        w += gw(c, bold);
    }
    return w;
}
static void glyph(Img *d, int x, int y, int c, px_t col, int bold)
{
    const unsigned short *b = (bold ? font_bold_bits : font_reg_bits) + glyph_index(c) * FONT_H_PX;
    for (int j = 0; j < FONT_H_PX; j++) {
        unsigned v = b[j];
        if (!v) continue;
        for (int i = 0; i < 16; i++)
            if (v & (0x8000 >> i)) img_put(d, x + i, y + j, col);
    }
}
/* y = top of the text line box (13px) */
int tx_draw(Img *d, int x, int y, const char *s, px_t col, int bold, int mnem)
{
    if (g_big) {
        char b[512]; int u; big_plain(s, mnem, b, sizeof b, &u);
        if (!*b) return 0;
        const BigGlyphs *e = big_get(b, bold);
        if (!e->m) return 0;
        int oy = y + (13 - e->h) / 2;
        for (int j = 0; j < e->h; j++) for (int i = 0; i < e->w; i++) if (e->m[j * e->w + i]) img_put(d, x + i, oy + j, col);
        if (u >= 0) {
            char pre[512]; memcpy(pre, b, (size_t)u); pre[u] = 0;
            int ux = x + (u ? big_get(pre, bold)->w : 0), cl = 1; while (((unsigned char)b[u + cl] & 0xc0) == 0x80) cl++;
            char ch[8]; memcpy(ch, b + u, (size_t)cl); ch[cl] = 0;
            gx_hline(d, ux, ux + big_get(ch, bold)->w - 1, oy + (e->h * 4 + 2) / 5 + 1, col);
        }
        return e->w;
    }
    int x0 = x;
    while (*s) {
        int c = decode_utf8(&s);
        int ul = 0;
        if (mnem && c == '&') { if (*s == '&') { s++; c = '&'; } else { c = decode_utf8(&s); ul = 1; if (!c) break; } }
        if (c == '\t') continue;
        glyph(d, x, y, c, col, bold);
        if (ul) gx_hline(d, x, x + gw(c, bold) - 1, y + FONT_ASCENT + 1, col);
        x += gw(c, bold);
    }
    return x - x0;
}
int tx_draw_dis(Img *d, int x, int y, const char *s, int mnem)
{
    tx_draw(d, x + 1, y + 1, s, C_HILITE, 0, mnem);
    return tx_draw(d, x, y, s, C_SHADOW, 0, mnem);
}
void tx_draw_clip(Img *d, int x, int y, const char *s, px_t c, int maxw)
{
    char buf[512];
    tx_ellipsis_fit(s, maxw, buf, sizeof buf);
    tx_draw(d, x, y, buf, c, 0, 0);
}
int tx_ellipsis_fit(const char *s, int maxw, char *out, int outn)
{
    if (tx_width(s, 0, 0) <= maxw) { strncpy(out, s, outn - 1); out[outn - 1] = 0; return 0; }
    int n = (int)strlen(s);
    if (n > outn - 4) n = outn - 4;
    while (n > 0) {
        memcpy(out, s, n); strcpy(out + n, "...");
        if (tx_width(out, 0, 0) <= maxw) return 1;
        n--;
    }
    out[0] = 0;
    return 1;
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int str_icmp(const char *a, const char *b)
{
    while (*a && lc((unsigned char)*a) == lc((unsigned char)*b)) { a++; b++; }
    return lc((unsigned char)*a) - lc((unsigned char)*b);
}
int str_nicmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = lc((unsigned char)*a) - lc((unsigned char)*b);
        if (d || !*a) return d;
    }
    return 0;
}
