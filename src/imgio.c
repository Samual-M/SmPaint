/* imgio.c - BMP / PNG / GIF codecs, self-contained (own inflate/deflate, LZW). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgio.h"
#include "plat.h"

typedef uint32_t u32;
typedef uint8_t u8;

uint8_t *file_read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    u8 *b = (u8 *)malloc(sz ? sz : 1);
    if (fread(b, 1, sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *n = (size_t)sz;
    return b;
}
int file_write_all(const char *path, const uint8_t *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(d, 1, n, f);
    int rc = fclose(f);
    return (w == n && rc == 0) ? 0 : -1;
}

/* ---------------- growable buffer ---------------- */
typedef struct { u8 *p; size_t n, cap; } Buf;
static void bput(Buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = (u8 *)realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void b8(Buf *b, int v) { u8 c = (u8)v; bput(b, &c, 1); }
static void b16le(Buf *b, int v) { b8(b, v); b8(b, v >> 8); }
static void b32le(Buf *b, u32 v) { b16le(b, v & 0xffff); b16le(b, v >> 16); }
static void b32be(Buf *b, u32 v) { b8(b, v >> 24); b8(b, v >> 16); b8(b, v >> 8); b8(b, v); }

/* ---------------- inflate ---------------- */
typedef struct { const u8 *d; size_t n, pos; u32 bitbuf; int bitcnt; int err; } BitR;
static int getbit(BitR *r)
{
    if (!r->bitcnt) {
        if (r->pos >= r->n) { r->err = 1; return 0; }
        r->bitbuf = r->d[r->pos++]; r->bitcnt = 8;
    }
    int b = r->bitbuf & 1; r->bitbuf >>= 1; r->bitcnt--;
    return b;
}
static int getbits(BitR *r, int n) { int v = 0; for (int i = 0; i < n; i++) v |= getbit(r) << i; return v; }
typedef struct { unsigned short count[16], symbol[288]; } Huff;
static void huff_build(Huff *h, const u8 *len, int n)
{
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    unsigned short offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (unsigned short)i;
}
static int huff_dec(BitR *r, const Huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= getbit(r);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
        if (r->err) return -1;
    }
    return -1;
}
static const unsigned short lbase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const unsigned short lext[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const unsigned short dbase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const unsigned short dext[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inflate_codes(BitR *r, Buf *o, const Huff *lh, const Huff *dh)
{
    for (;;) {
        int s = huff_dec(r, lh);
        if (s < 0 || r->err) return -1;
        if (s < 256) b8(o, s);
        else if (s == 256) return 0;
        else {
            s -= 257; if (s >= 29) return -1;
            int len = lbase[s] + getbits(r, lext[s]);
            int ds = huff_dec(r, dh); if (ds < 0 || ds >= 30) return -1;
            size_t dist = dbase[ds] + getbits(r, dext[ds]);
            if (dist > o->n) return -1;
            for (int i = 0; i < len; i++) b8(o, o->p[o->n - dist]);
        }
    }
}
/* raw deflate stream -> buffer */
static int inflate_raw(const u8 *d, size_t n, Buf *o)
{
    BitR r = { d, n, 0, 0, 0, 0 };
    int last;
    do {
        last = getbit(&r);
        int type = getbits(&r, 2);
        if (r.err) return -1;
        if (type == 0) {
            r.bitcnt = 0;
            if (r.pos + 4 > n) return -1;
            unsigned len = d[r.pos] | (d[r.pos + 1] << 8); r.pos += 4;
            if (r.pos + len > n) return -1;
            bput(o, d + r.pos, len); r.pos += len;
        } else if (type == 1 || type == 2) {
            Huff lh, dh; u8 lens[320];
            if (type == 1) {
                int i = 0;
                for (; i < 144; i++) lens[i] = 8;
                for (; i < 256; i++) lens[i] = 9;
                for (; i < 280; i++) lens[i] = 7;
                for (; i < 288; i++) lens[i] = 8;
                huff_build(&lh, lens, 288);
                for (i = 0; i < 30; i++) lens[i] = 5;
                huff_build(&dh, lens, 30);
            } else {
                int nlen = getbits(&r, 5) + 257, ndist = getbits(&r, 5) + 1, ncode = getbits(&r, 4) + 4;
                static const u8 ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                u8 cl[19] = {0};
                for (int i = 0; i < ncode; i++) cl[ord[i]] = (u8)getbits(&r, 3);
                Huff ch; huff_build(&ch, cl, 19);
                int idx = 0;
                while (idx < nlen + ndist) {
                    int s = huff_dec(&r, &ch);
                    if (s < 0) return -1;
                    if (s < 16) lens[idx++] = (u8)s;
                    else {
                        int rep, v = 0;
                        if (s == 16) { if (!idx) return -1; v = lens[idx - 1]; rep = 3 + getbits(&r, 2); }
                        else if (s == 17) rep = 3 + getbits(&r, 3);
                        else rep = 11 + getbits(&r, 7);
                        if (idx + rep > nlen + ndist) return -1;
                        while (rep--) lens[idx++] = (u8)v;
                    }
                }
                huff_build(&lh, lens, nlen);
                huff_build(&dh, lens + nlen, ndist);
            }
            if (inflate_codes(&r, o, &lh, &dh)) return -1;
        } else return -1;
    } while (!last);
    return 0;
}

/* ---------------- deflate (fixed Huffman + LZ77 hash) ---------------- */
typedef struct { Buf *b; u32 acc; int cnt; } BitW;
static void wbits(BitW *w, u32 v, int n)
{
    w->acc |= v << w->cnt; w->cnt += n;
    while (w->cnt >= 8) { b8(w->b, w->acc & 255); w->acc >>= 8; w->cnt -= 8; }
}
static void wrev(BitW *w, u32 code, int n) /* huffman codes are MSB first */
{
    u32 r = 0; for (int i = 0; i < n; i++) { r = (r << 1) | (code & 1); code >>= 1; }
    wbits(w, r, n);
}
static void wlit(BitW *w, int s)
{
    if (s < 144) wrev(w, 0x30 + s, 8);
    else if (s < 256) wrev(w, 0x190 + s - 144, 9);
    else if (s < 280) wrev(w, s - 256, 7);
    else wrev(w, 0xc0 + s - 280, 8);
}
static void deflate_fixed(const u8 *d, size_t n, Buf *o)
{
    BitW w = { o, 0, 0 };
    wbits(&w, 1, 1); wbits(&w, 1, 2);
    enum { HB = 15 };
    int *head = (int *)malloc(sizeof(int) << HB);
    int *prev = (int *)malloc(sizeof(int) * (n ? n : 1));
    for (int i = 0; i < (1 << HB); i++) head[i] = -1;
    size_t i = 0;
    while (i < n) {
        int bl = 0, bd = 0;
        if (i + 3 <= n) {
            u32 h = ((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) & ((1 << HB) - 1);
            int c = head[h], tries = 24;
            while (c >= 0 && tries-- && i - c <= 32768) {
                int l = 0; while (l < 258 && i + l < n && d[c + l] == d[i + l]) l++;
                if (l > bl) { bl = l; bd = (int)(i - c); if (l == 258) break; }
                c = prev[c];
            }
        }
        if (bl >= 3) {
            int s = 0; while (s < 28 && lbase[s + 1] <= bl) s++;
            wlit(&w, 257 + s); wbits(&w, bl - lbase[s], lext[s]);
            int ds = 0; while (ds < 29 && dbase[ds + 1] <= bd) ds++;
            wrev(&w, ds, 5); wbits(&w, bd - dbase[ds], dext[ds]);
            for (int k = 0; k < bl; k++, i++)
                if (i + 3 <= n) {
                    u32 h = ((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) & ((1 << HB) - 1);
                    prev[i] = head[h]; head[h] = (int)i;
                }
        } else {
            wlit(&w, d[i]);
            if (i + 3 <= n) {
                u32 h = ((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) & ((1 << HB) - 1);
                prev[i] = head[h]; head[h] = (int)i;
            }
            i++;
        }
    }
    wlit(&w, 256);
    if (w.cnt) wbits(&w, 0, 8 - w.cnt);
    free(head); free(prev);
}
static u32 crc_table[256]; static int crc_init;
static u32 crc32_upd(u32 c, const u8 *d, size_t n)
{
    if (!crc_init) {
        for (u32 i = 0; i < 256; i++) { u32 x = i; for (int k = 0; k < 8; k++) x = (x & 1) ? 0xedb88320u ^ (x >> 1) : x >> 1; crc_table[i] = x; }
        crc_init = 1;
    }
    c = ~c; for (size_t i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 255] ^ (c >> 8);
    return ~c;
}
static u32 adler32(const u8 *d, size_t n)
{
    u32 a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

/* ---------------- PNG ---------------- */
static void png_chunk(Buf *o, const char *type, const u8 *d, size_t n)
{
    b32be(o, (u32)n);
    size_t start = o->n;
    bput(o, type, 4); if (n) bput(o, d, n);
    b32be(o, crc32_upd(0, o->p + start, n + 4));
}
uint8_t *png_encode(const u32 *px, int w, int h, size_t *outn)
{
    Buf raw = {0};
    for (int y = 0; y < h; y++) {
        b8(&raw, 1); /* Sub filter */
        const u32 *r = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            u32 c = r[x], l = x ? r[x - 1] : 0;
            b8(&raw, ((c >> 16) - (l >> 16)) & 255); b8(&raw, ((c >> 8) - (l >> 8)) & 255); b8(&raw, (c - l) & 255);
        }
    }
    Buf z = {0};
    b8(&z, 0x78); b8(&z, 0x9c);
    deflate_fixed(raw.p, raw.n, &z);
    b32be(&z, adler32(raw.p, raw.n));
    Buf o = {0};
    bput(&o, "\x89PNG\r\n\x1a\n", 8);
    Buf hd = {0};
    b32be(&hd, w); b32be(&hd, h); b8(&hd, 8); b8(&hd, 2); b8(&hd, 0); b8(&hd, 0); b8(&hd, 0);
    png_chunk(&o, "IHDR", hd.p, 13);
    png_chunk(&o, "IDAT", z.p, z.n);
    png_chunk(&o, "IEND", NULL, 0);
    free(raw.p); free(z.p); free(hd.p);
    *outn = o.n;
    return o.p;
}

/* single-page PDF: the image, losslessly Flate-compressed (PNG predictor), page = image size at 96 dpi */
uint8_t *pdf_encode(const u32 *px, int w, int h, size_t *outn)
{
    Buf raw = {0};
    for (int y = 0; y < h; y++) {
        b8(&raw, 1);
        const u32 *r = px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            u32 c = r[x], l = x ? r[x - 1] : 0;
            b8(&raw, ((c >> 16) - (l >> 16)) & 255); b8(&raw, ((c >> 8) - (l >> 8)) & 255); b8(&raw, (c - l) & 255);
        }
    }
    Buf z = {0};
    b8(&z, 0x78); b8(&z, 0x9c);
    deflate_fixed(raw.p, raw.n, &z);
    b32be(&z, adler32(raw.p, raw.n));
    Buf o = {0}; char t[512]; size_t off[6]; int n;
    double pw = w * 0.75, ph = h * 0.75;
    bput(&o, "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n", 15);
    off[1] = o.n; n = snprintf(t, sizeof t, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"); bput(&o, t, n);
    off[2] = o.n; n = snprintf(t, sizeof t, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"); bput(&o, t, n);
    off[3] = o.n; n = snprintf(t, sizeof t, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>\nendobj\n", pw, ph); bput(&o, t, n);
    char cs[128]; int cn = snprintf(cs, sizeof cs, "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q\n", pw, ph);
    off[4] = o.n; n = snprintf(t, sizeof t, "4 0 obj\n<< /Length %d >>\nstream\n", cn); bput(&o, t, n); bput(&o, cs, cn); bput(&o, "endstream\nendobj\n", 17);
    off[5] = o.n;
    n = snprintf(t, sizeof t, "5 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 3 /BitsPerComponent 8 /Columns %d >> /Length %zu >>\nstream\n", w, h, w, z.n);
    bput(&o, t, n); bput(&o, z.p, z.n); bput(&o, "\nendstream\nendobj\n", 18);
    size_t xr = o.n;
    n = snprintf(t, sizeof t, "xref\n0 6\n0000000000 65535 f \n"); bput(&o, t, n);
    for (int i = 1; i <= 5; i++) { n = snprintf(t, sizeof t, "%010zu 00000 n \n", off[i]); bput(&o, t, n); }
    n = snprintf(t, sizeof t, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n", xr); bput(&o, t, n);
    free(raw.p); free(z.p);
    *outn = o.n; return o.p;
}
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
u32 *png_decode(const u8 *d, size_t n, int *ow, int *oh)
{
    if (n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    size_t pos = 8; int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    u8 pal[768]; int npal = 0; u8 trns[256]; int ntrns = 0;
    Buf z = {0}; u32 *res = NULL;
    while (pos + 12 <= n) {
        u32 len = be32(d + pos); const u8 *t = d + pos + 4; const u8 *c = d + pos + 8;
        if (pos + 12 + len > n) break;
        if (!memcmp(t, "IHDR", 4) && len >= 13) { w = be32(c); h = be32(c + 4); depth = c[8]; ctype = c[9]; interlace = c[12]; }
        else if (!memcmp(t, "PLTE", 4)) { npal = len / 3; if (npal > 256) npal = 256; memcpy(pal, c, npal * 3); }
        else if (!memcmp(t, "tRNS", 4)) { ntrns = len > 256 ? 256 : len; memcpy(trns, c, ntrns); }
        else if (!memcmp(t, "IDAT", 4)) bput(&z, c, len);
        else if (!memcmp(t, "IEND", 4)) break;
        pos += 12 + len;
    }
    if (w <= 0 || h <= 0 || w > 30000 || h > 30000 || z.n < 6 || interlace) { free(z.p); return NULL; }
    Buf raw = {0};
    if (inflate_raw(z.p + 2, z.n - 2, &raw)) { free(z.p); free(raw.p); return NULL; }
    free(z.p);
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
    int bpp_bits = ch * depth; int bpp = (bpp_bits + 7) / 8;
    size_t stride = ((size_t)w * bpp_bits + 7) / 8;
    if (raw.n < (stride + 1) * h) { free(raw.p); return NULL; }
    res = (u32 *)malloc((size_t)w * h * 4);
    u8 *prev = (u8 *)calloc(stride, 1), *cur = (u8 *)malloc(stride);
    for (int y = 0; y < h; y++) {
        const u8 *row = raw.p + (stride + 1) * y; int f = row[0];
        memcpy(cur, row + 1, stride);
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0, x = cur[i];
            switch (f) {
            case 1: x += a; break; case 2: x += b; break; case 3: x += (a + b) / 2; break;
            case 4: { int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                      x += (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c; } break;
            }
            cur[i] = (u8)x;
        }
        for (int x = 0; x < w; x++) {
            int r = 0, g = 0, bl = 0, al = 255;
            #define SAMPLE(idx) ( depth == 8 ? cur[(idx)] : depth == 16 ? cur[(idx) * 2] : 0 )
            if (depth < 8) {
                int per = 8 / depth, bi = x / per, sh = 8 - depth - (x % per) * depth;
                int v = (cur[bi] >> sh) & ((1 << depth) - 1);
                if (ctype == 3) { r = pal[v * 3]; g = pal[v * 3 + 1]; bl = pal[v * 3 + 2]; if (v < ntrns) al = trns[v]; }
                else { r = g = bl = v * 255 / ((1 << depth) - 1); }
            } else if (ctype == 3) { int v = cur[x]; r = pal[v * 3]; g = pal[v * 3 + 1]; bl = pal[v * 3 + 2]; if (v < ntrns) al = trns[v]; }
            else if (ctype == 0) { r = g = bl = SAMPLE(x); }
            else if (ctype == 4) { r = g = bl = SAMPLE(x * 2); al = SAMPLE(x * 2 + 1); }
            else if (ctype == 2) { r = SAMPLE(x * 3); g = SAMPLE(x * 3 + 1); bl = SAMPLE(x * 3 + 2); }
            else { r = SAMPLE(x * 4); g = SAMPLE(x * 4 + 1); bl = SAMPLE(x * 4 + 2); al = SAMPLE(x * 4 + 3); }
            if (al < 255) { r = (r * al + 255 * (255 - al)) / 255; g = (g * al + 255 * (255 - al)) / 255; bl = (bl * al + 255 * (255 - al)) / 255; }
            res[(size_t)y * w + x] = ((u32)r << 16) | (g << 8) | bl;
        }
        u8 *t = prev; prev = cur; cur = t;
    }
    free(prev); free(cur); free(raw.p);
    *ow = w; *oh = h;
    return res;
}

/* ---------------- quantizer shared by BMP-8 and GIF ---------------- */
typedef struct { u8 r, g, b; } RGB3;
static int quantize(const u32 *px, size_t n, RGB3 *pal, int maxc, u8 *idx_out)
{
    /* exact palette if few colors */
    int np = 0;
    int ok = 1;
    u32 *tab = (u32 *)malloc(sizeof(u32) * 4096);
    int *tabi = (int *)malloc(sizeof(int) * 4096);
    for (int i = 0; i < 4096; i++) tabi[i] = -1;
    for (size_t i = 0; i < n && ok; i++) {
        u32 c = px[i] & 0xffffff; u32 h = (c * 2654435761u) >> 20; int found = -1;
        for (;;) {
            if (tabi[h] < 0) break;
            if (tab[h] == c) { found = tabi[h]; break; }
            h = (h + 1) & 4095;
        }
        if (found < 0) {
            if (np >= maxc) { ok = 0; break; }
            tab[h] = c; tabi[h] = np; pal[np].r = c >> 16; pal[np].g = c >> 8; pal[np].b = c; np++;
        }
        if (idx_out) idx_out[i] = (u8)(found < 0 ? np - 1 : found);
    }
    if (ok) { free(tab); free(tabi); return np; }
    free(tab); free(tabi);
    /* median cut on a 5-bit histogram */
    static int hist[32768];
    memset(hist, 0, sizeof hist);
    for (size_t i = 0; i < n; i++) { u32 c = px[i]; hist[((c >> 19) & 31) << 10 | ((c >> 11) & 31) << 5 | ((c >> 3) & 31)]++; }
    typedef struct { int lo[3], hi[3]; long cnt; } Box;
    Box boxes[256]; int nb = 1;
    boxes[0] = (Box){ {0, 0, 0}, {31, 31, 31}, (long)n };
    while (nb < maxc) {
        int best = -1; long bc = 1;
        for (int i = 0; i < nb; i++) if (boxes[i].cnt > bc) { bc = boxes[i].cnt; best = i; }
        if (best < 0) break;
        Box *b = &boxes[best]; int axis = 0, span = 0;
        for (int a = 0; a < 3; a++) if (b->hi[a] - b->lo[a] > span) { span = b->hi[a] - b->lo[a]; axis = a; }
        if (span == 0) { b->cnt = 1; continue; }
        long tot = 0, acc = 0; long slice[32] = {0};
        for (int r = b->lo[0]; r <= b->hi[0]; r++) for (int g = b->lo[1]; g <= b->hi[1]; g++) for (int bl = b->lo[2]; bl <= b->hi[2]; bl++) {
            int v[3] = { r, g, bl }; long c = hist[r << 10 | g << 5 | bl]; slice[v[axis]] += c; tot += c;
        }
        int split = b->lo[axis];
        for (int v = b->lo[axis]; v < b->hi[axis]; v++) { acc += slice[v]; split = v; if (acc * 2 >= tot) break; }
        Box nbx = *b; b->hi[axis] = split; nbx.lo[axis] = split + 1;
        for (int k = 0; k < 2; k++) {
            Box *q = k ? &nbx : b; long cnt = 0;
            for (int r = q->lo[0]; r <= q->hi[0]; r++) for (int g = q->lo[1]; g <= q->hi[1]; g++) for (int bl = q->lo[2]; bl <= q->hi[2]; bl++) cnt += hist[r << 10 | g << 5 | bl];
            q->cnt = cnt;
        }
        boxes[nb++] = nbx;
    }
    for (int i = 0; i < nb; i++) {
        long sr = 0, sg = 0, sb = 0, sc = 0; Box *q = &boxes[i];
        for (int r = q->lo[0]; r <= q->hi[0]; r++) for (int g = q->lo[1]; g <= q->hi[1]; g++) for (int bl = q->lo[2]; bl <= q->hi[2]; bl++) {
            long c = hist[r << 10 | g << 5 | bl]; sr += c * r; sg += c * g; sb += c * bl; sc += c;
        }
        if (!sc) { pal[i].r = (q->lo[0] << 3) | 4; pal[i].g = (q->lo[1] << 3) | 4; pal[i].b = (q->lo[2] << 3) | 4; }
        else { pal[i].r = (u8)((sr * 8 + 4 * sc) / sc); pal[i].g = (u8)((sg * 8 + 4 * sc) / sc); pal[i].b = (u8)((sb * 8 + 4 * sc) / sc); }
    }
    if (idx_out) {
        static short cache[32768]; for (int i = 0; i < 32768; i++) cache[i] = -1;
        for (size_t i = 0; i < n; i++) {
            u32 c = px[i]; int key = ((c >> 19) & 31) << 10 | ((c >> 11) & 31) << 5 | ((c >> 3) & 31);
            if (cache[key] < 0) {
                int bi = 0, bd = 1 << 30;
                for (int k = 0; k < nb; k++) {
                    int dr = (int)((c >> 16) & 255) - pal[k].r, dg = (int)((c >> 8) & 255) - pal[k].g, db = (int)(c & 255) - pal[k].b;
                    int dd = dr * dr + dg * dg + db * db; if (dd < bd) { bd = dd; bi = k; }
                }
                cache[key] = (short)bi;
            }
            idx_out[i] = (u8)cache[key];
        }
    }
    return nb;
}

/* ---------------- BMP ---------------- */
static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static int le16(const u8 *p) { return p[0] | (p[1] << 8); }
u32 *bmp_decode(const u8 *d, size_t n, int *ow, int *oh)
{
    size_t off = 0, fh_off = 0;
    if (n >= 14 && d[0] == 'B' && d[1] == 'M') { fh_off = le32(d + 10); off = 14; }
    if (n < off + 12) return NULL;
    const u8 *h = d + off; u32 hs = le32(h);
    int w, ht, bpp, comp = 0; u32 clrused = 0; int topdown = 0;
    if (hs == 12) { w = le16(h + 4); ht = le16(h + 6); bpp = le16(h + 10); }
    else if (hs >= 40) {
        w = (int)le32(h + 4); ht = (int)le32(h + 8); bpp = le16(h + 14); comp = (int)le32(h + 16); clrused = le32(h + 32);
    } else return NULL;
    if (ht < 0) { ht = -ht; topdown = 1; }
    if (w <= 0 || ht <= 0 || w > 30000 || ht > 30000) return NULL;
    size_t pal_off = off + hs; int npal = 0; u8 pal[1024] = {0};
    if (bpp <= 8) {
        npal = clrused ? (int)clrused : (1 << bpp); if (npal > 256) npal = 256;
        int esz = hs == 12 ? 3 : 4;
        if (pal_off + (size_t)npal * esz > n) return NULL;
        for (int i = 0; i < npal; i++) { pal[i * 4] = d[pal_off + i * esz + 2]; pal[i * 4 + 1] = d[pal_off + i * esz + 1]; pal[i * 4 + 2] = d[pal_off + i * esz]; }
        pal_off += (size_t)npal * esz;
    }
    u32 masks[3] = { 0xff0000, 0x00ff00, 0x0000ff };
    if (bpp == 16) { masks[0] = 0x7c00; masks[1] = 0x3e0; masks[2] = 0x1f; }
    if (comp == 3 || comp == 6) {
        if (hs >= 52) { masks[0] = le32(h + 40); masks[1] = le32(h + 44); masks[2] = le32(h + 48); pal_off = off + hs; }
        else { masks[0] = le32(d + pal_off); masks[1] = le32(d + pal_off + 4); masks[2] = le32(d + pal_off + 8); pal_off += 12; }
    }
    size_t data = fh_off ? fh_off : pal_off;
    if (fh_off && fh_off < pal_off) data = pal_off;
    if (data > n) return NULL;
    u32 *px = (u32 *)malloc((size_t)w * ht * 4);
    for (size_t i = 0; i < (size_t)w * ht; i++) px[i] = 0xffffff;
    if (comp == 1 || comp == 2) {
        const u8 *s = d + data, *e = d + n; int x = 0, y = 0;
        while (s + 1 < e && y < ht) {
            int c = *s++, v = *s++;
            if (c) {
                for (int k = 0; k < c; k++) {
                    int ix = comp == 1 ? v : ((k & 1) ? (v & 15) : (v >> 4));
                    if (x < w) { int yy = topdown ? y : ht - 1 - y; px[(size_t)yy * w + x] = ((u32)pal[ix * 4] << 16) | (pal[ix * 4 + 1] << 8) | pal[ix * 4 + 2]; }
                    x++;
                }
            } else if (v == 0) { x = 0; y++; }
            else if (v == 1) break;
            else if (v == 2) { if (s + 1 >= e) break; x += s[0]; y += s[1]; s += 2; }
            else {
                int cnt = v; size_t bytes = comp == 1 ? (size_t)cnt : (size_t)(cnt + 1) / 2;
                if (s + bytes > e) break;
                for (int k = 0; k < cnt; k++) {
                    int ix = comp == 1 ? s[k] : ((k & 1) ? (s[k / 2] & 15) : (s[k / 2] >> 4));
                    if (x < w) { int yy = topdown ? y : ht - 1 - y; px[(size_t)yy * w + x] = ((u32)pal[ix * 4] << 16) | (pal[ix * 4 + 1] << 8) | pal[ix * 4 + 2]; }
                    x++;
                }
                s += bytes + (bytes & 1);
            }
        }
        *ow = w; *oh = ht; return px;
    }
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    if (data + stride * ht > n) { free(px); return NULL; }
    for (int y = 0; y < ht; y++) {
        const u8 *row = d + data + stride * (topdown ? y : ht - 1 - y);
        for (int x = 0; x < w; x++) {
            u32 c = 0;
            if (bpp == 24) c = ((u32)row[x * 3 + 2] << 16) | (row[x * 3 + 1] << 8) | row[x * 3];
            else if (bpp == 32 || bpp == 16) {
                u32 v = bpp == 32 ? le32(row + x * 4) : (u32)le16(row + x * 2);
                int rgb[3];
                for (int k = 0; k < 3; k++) {
                    u32 m = masks[k]; int sh = 0; while (m && !(m & 1)) { m >>= 1; sh++; }
                    int bits = 0; while (m & 1) { m >>= 1; bits++; }
                    int val = bits ? (int)((v & masks[k]) >> sh) : 0;
                    rgb[k] = bits ? (val * 255) / ((1 << bits) - 1) : 0;
                }
                c = ((u32)rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
            } else {
                int ix;
                if (bpp == 8) ix = row[x];
                else if (bpp == 4) ix = (row[x / 2] >> ((x & 1) ? 0 : 4)) & 15;
                else if (bpp == 1) ix = (row[x / 8] >> (7 - (x & 7))) & 1;
                else { free(px); return NULL; }
                if (ix >= npal) ix = 0;
                c = ((u32)pal[ix * 4] << 16) | (pal[ix * 4 + 1] << 8) | pal[ix * 4 + 2];
            }
            px[(size_t)y * w + x] = c;
        }
    }
    *ow = w; *oh = ht;
    return px;
}
static const u8 vga16[16][3] = {{0,0,0},{128,0,0},{0,128,0},{128,128,0},{0,0,128},{128,0,128},{0,128,128},{192,192,192},
                                {128,128,128},{255,0,0},{0,255,0},{255,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,255}};
uint8_t *bmp_encode(const u32 *px, int w, int h, int bpp, int file_header, size_t *outn)
{
    Buf o = {0};
    int npal = bpp == 24 ? 0 : (1 << bpp);
    RGB3 pal[256]; u8 *idx = NULL;
    size_t cnt = (size_t)w * h;
    if (bpp == 8) { idx = (u8 *)malloc(cnt); int np = quantize(px, cnt, pal, 256, idx); for (int i = np; i < 256; i++) pal[i] = (RGB3){0, 0, 0}; }
    else if (bpp == 4) {
        idx = (u8 *)malloc(cnt);
        for (int i = 0; i < 16; i++) pal[i] = (RGB3){ vga16[i][0], vga16[i][1], vga16[i][2] };
        for (size_t i = 0; i < cnt; i++) {
            int bi = 0, bd = 1 << 30; u32 c = px[i];
            for (int k = 0; k < 16; k++) {
                int dr = (int)((c >> 16) & 255) - pal[k].r, dg = (int)((c >> 8) & 255) - pal[k].g, db = (int)(c & 255) - pal[k].b;
                int dd = dr * dr + dg * dg + db * db; if (dd < bd) { bd = dd; bi = k; }
            }
            idx[i] = (u8)bi;
        }
    } else if (bpp == 1) {
        idx = (u8 *)malloc(cnt);
        pal[0] = (RGB3){0, 0, 0}; pal[1] = (RGB3){255, 255, 255};
        for (size_t i = 0; i < cnt; i++) { u32 c = px[i]; idx[i] = (((c >> 16 & 255) * 30 + (c >> 8 & 255) * 59 + (c & 255) * 11) / 100) >= 128; }
    }
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    size_t hdr = (file_header ? 14 : 0) + 40 + (size_t)npal * 4;
    if (file_header) { bput(&o, "BM", 2); b32le(&o, (u32)(hdr + stride * h)); b32le(&o, 0); b32le(&o, (u32)hdr); }
    b32le(&o, 40); b32le(&o, w); b32le(&o, h); b16le(&o, 1); b16le(&o, bpp); b32le(&o, 0); b32le(&o, (u32)(stride * h));
    b32le(&o, 2835); b32le(&o, 2835); b32le(&o, npal); b32le(&o, 0);
    for (int i = 0; i < npal; i++) { b8(&o, pal[i].b); b8(&o, pal[i].g); b8(&o, pal[i].r); b8(&o, 0); }
    u8 *row = (u8 *)calloc(stride, 1);
    for (int y = h - 1; y >= 0; y--) {
        memset(row, 0, stride);
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            if (bpp == 24) { u32 c = px[i]; row[x * 3] = c; row[x * 3 + 1] = c >> 8; row[x * 3 + 2] = c >> 16; }
            else if (bpp == 8) row[x] = idx[i];
            else if (bpp == 4) row[x / 2] |= idx[i] << ((x & 1) ? 0 : 4);
            else row[x / 8] |= idx[i] << (7 - (x & 7));
        }
        bput(&o, row, stride);
    }
    free(row); free(idx);
    *outn = o.n;
    return o.p;
}

/* ---------------- GIF ---------------- */
u32 *gif_decode(const u8 *d, size_t n, int *ow, int *oh)
{
    if (n < 13 || memcmp(d, "GIF8", 4)) return NULL;
    int sw = le16(d + 6), sh = le16(d + 8), flags = d[10];
    size_t p = 13; u8 gpal[768]; int gn = 0;
    if (flags & 0x80) { gn = 2 << (flags & 7); if (p + gn * 3 > n) return NULL; memcpy(gpal, d + p, gn * 3); p += gn * 3; }
    if (sw <= 0 || sh <= 0) return NULL;
    u32 *px = (u32 *)malloc((size_t)sw * sh * 4);
    for (size_t i = 0; i < (size_t)sw * sh; i++) px[i] = 0xffffff;
    int trans = -1;
    while (p < n) {
        int t = d[p++];
        if (t == 0x21) {
            int lab = d[p++];
            if (lab == 0xf9 && p + 5 < n && d[p] >= 4) { if (d[p + 1] & 1) trans = d[p + 4]; }
            while (p < n && d[p]) p += d[p] + 1;
            p++;
        } else if (t == 0x2c) {
            if (p + 9 > n) break;
            int ix = le16(d + p), iy = le16(d + p + 2), iw = le16(d + p + 4), ih = le16(d + p + 6), fl = d[p + 8]; p += 9;
            u8 lpal[768]; const u8 *pal = gpal; int pn = gn;
            if (fl & 0x80) { pn = 2 << (fl & 7); if (p + pn * 3 > n) break; memcpy(lpal, d + p, pn * 3); pal = lpal; p += pn * 3; }
            int interlace = fl & 0x40;
            int minc = d[p++];
            Buf data = {0};
            while (p < n && d[p]) { bput(&data, d + p + 1, d[p]); p += d[p] + 1; }
            p++;
            /* LZW */
            int clear = 1 << minc, eoi = clear + 1, next = clear + 2, size = minc + 1;
            short prefix[4096]; u8 suffix[4096]; u8 stack[4097];
            int old = -1; u8 first = 0; size_t bp = 0; u32 acc = 0; int nb = 0;
            u8 *out = (u8 *)malloc((size_t)iw * ih + 1); size_t on = 0;
            for (int i = 0; i < clear; i++) { prefix[i] = -1; suffix[i] = (u8)i; }
            while (on < (size_t)iw * ih) {
                while (nb < size && bp < data.n) { acc |= (u32)data.p[bp++] << nb; nb += 8; }
                if (nb < size) break;
                int code = acc & ((1 << size) - 1); acc >>= size; nb -= size;
                if (code == clear) { size = minc + 1; next = clear + 2; old = -1; continue; }
                if (code == eoi) break;
                int sp = 0, cur = code;
                if (old < 0) { if (code >= clear) break; out[on++] = (u8)code; old = code; first = (u8)code; continue; }
                if (code >= next) { stack[sp++] = first; cur = old; }
                while (cur >= clear && sp < 4096) { stack[sp++] = suffix[cur]; cur = prefix[cur]; }
                first = (u8)cur; stack[sp++] = first;
                while (sp && on < (size_t)iw * ih) out[on++] = stack[--sp];
                if (next < 4096) { prefix[next] = (short)old; suffix[next] = first; next++; if (next == (1 << size) && size < 12) size++; }
                old = code;
            }
            free(data.p);
            int pass = 0, row = 0; static const int st[4] = {8, 8, 4, 2}, sy[4] = {0, 4, 2, 1};
            if (interlace) row = 0;
            for (int y = 0; y < ih; y++) {
                int yy;
                if (interlace) { yy = row; row += st[pass]; if (row >= ih) { pass++; row = pass < 4 ? sy[pass] : 0; } }
                else yy = y;
                if (pass == 0 && interlace && y == 0) {}
                for (int x = 0; x < iw; x++) {
                    size_t si = (size_t)y * iw + x; if (si >= on) continue;
                    int v = out[si]; if (v == trans || v >= pn) continue;
                    int X = ix + x, Y = iy + yy;
                    if (X < sw && Y < sh) px[(size_t)Y * sw + X] = ((u32)pal[v * 3] << 16) | (pal[v * 3 + 1] << 8) | pal[v * 3 + 2];
                }
            }
            free(out);
            *ow = sw; *oh = sh;
            return px;
        } else if (t == 0x3b) break;
        else break;
    }
    free(px);
    return NULL;
}
uint8_t *gif_encode(const u32 *px, int w, int h, size_t *outn)
{
    size_t cnt = (size_t)w * h;
    RGB3 pal[256]; u8 *idx = (u8 *)malloc(cnt);
    int np = quantize(px, cnt, pal, 256, idx);
    int bits = 1; while ((1 << bits) < np) bits++;
    Buf o = {0};
    bput(&o, "GIF89a", 6); b16le(&o, w); b16le(&o, h); b8(&o, 0x80 | 0x70 | (bits - 1)); b8(&o, 0); b8(&o, 0);
    for (int i = 0; i < (1 << bits); i++) { b8(&o, i < np ? pal[i].r : 0); b8(&o, i < np ? pal[i].g : 0); b8(&o, i < np ? pal[i].b : 0); }
    b8(&o, 0x2c); b16le(&o, 0); b16le(&o, 0); b16le(&o, w); b16le(&o, h); b8(&o, 0);
    int minc = bits < 2 ? 2 : bits; b8(&o, minc);
    /* LZW compress */
    Buf lz = {0}; u32 acc = 0; int nb = 0;
    int clear = 1 << minc, eoi = clear + 1, next = clear + 2, size = minc + 1;
    static int ht_code[8192]; static int ht_pre[8192]; static u8 ht_ch[8192];
    for (int i = 0; i < 8192; i++) ht_code[i] = -1;
    #define EMIT(c) do { acc |= (u32)(c) << nb; nb += size; while (nb >= 8) { b8(&lz, acc & 255); acc >>= 8; nb -= 8; } } while (0)
    EMIT(clear);
    int cur = idx[0];
    for (size_t i = 1; i < cnt; i++) {
        int c = idx[i]; u32 hh = ((cur << 8) ^ c) * 2654435761u >> 19; int found = -1;
        while (ht_code[hh] >= 0) { if (ht_pre[hh] == cur && ht_ch[hh] == c) { found = ht_code[hh]; break; } hh = (hh + 1) & 8191; }
        if (found >= 0) { cur = found; continue; }
        EMIT(cur);
        if (next < 4096) {
            ht_code[hh] = next; ht_pre[hh] = cur; ht_ch[hh] = (u8)c; next++;
            if (next > (1 << size) && size < 12) size++;
        } else {
            EMIT(clear); for (int k = 0; k < 8192; k++) ht_code[k] = -1; next = clear + 2; size = minc + 1;
        }
        cur = c;
    }
    EMIT(cur); EMIT(eoi);
    if (nb) b8(&lz, acc & 255);
    for (size_t i = 0; i < lz.n; i += 255) { size_t k = lz.n - i < 255 ? lz.n - i : 255; b8(&o, (int)k); bput(&o, lz.p + i, k); }
    b8(&o, 0); b8(&o, 0x3b);
    free(lz.p); free(idx);
    *outn = o.n;
    return o.p;
}

u32 *any_decode(const u8 *d, size_t n, int *w, int *h)
{
    if (n > 8 && !memcmp(d, "\x89PNG", 4)) return png_decode(d, n, w, h);
    if (n > 6 && !memcmp(d, "GIF8", 4)) return gif_decode(d, n, w, h);
    if (n > 3 && d[0] == 0xff && d[1] == 0xd8) return jpg_decode(d, n, w, h);
    if (n > 2 && d[0] == 'B' && d[1] == 'M') return bmp_decode(d, n, w, h);
    return bmp_decode(d, n, w, h);
}
