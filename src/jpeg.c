/* jpeg.c - self-contained JPEG codec: baseline + progressive decoder, baseline encoder (4:4:4) */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "imgio.h"

typedef uint8_t u8;
typedef uint32_t u32;

static const u8 zz[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

/* ============================ decoder ============================ */
typedef struct { u8 bits[17]; u8 vals[256]; int mincode[18], maxcode[18], valptr[18]; int ok; } HT;
typedef struct {
    int id, h, v, tq, td, ta, pred;
    int bw, bh;              /* blocks per line / lines (padded to MCU) */
    int cw, ch;              /* component dimension in pixels */
    short *coef;
} Comp;
typedef struct {
    const u8 *d; size_t n, pos;
    int bitbuf, bitcnt, marker, eobrun;
    int w, h, ncomp, prog, ri;
    int hmax, vmax, mcux, mcuy;
    unsigned short q[4][64];
    HT dc[4], ac[4];
    Comp c[4];
} JD;

static void ht_build(HT *t)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k; t->mincode[l] = code;
        code += t->bits[l]; k += t->bits[l];
        t->maxcode[l] = t->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    t->maxcode[17] = 0x7fffffff; t->ok = 1;
}
static int get_byte_raw(JD *j) { return j->pos < j->n ? j->d[j->pos++] : -1; }
static int fill_bit(JD *j)
{
    if (!j->bitcnt) {
        if (j->marker) { j->bitbuf = 0; j->bitcnt = 8; }
        else {
            int b = get_byte_raw(j);
            if (b < 0) { j->bitbuf = 0; j->bitcnt = 8; j->marker = 0xd9; }
            else if (b == 0xff) {
                int b2 = get_byte_raw(j);
                while (b2 == 0xff) b2 = get_byte_raw(j);
                if (b2 == 0) { j->bitbuf = 0xff; j->bitcnt = 8; }
                else { j->marker = b2; j->bitbuf = 0; j->bitcnt = 8; }
            } else { j->bitbuf = b; j->bitcnt = 8; }
        }
    }
    j->bitcnt--;
    return (j->bitbuf >> j->bitcnt) & 1;
}
static int get_bits(JD *j, int n) { int v = 0; while (n--) v = (v << 1) | fill_bit(j); return v; }
static int extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }
static int huff_decode(JD *j, const HT *t)
{
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | fill_bit(j);
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l] && code >= t->mincode[l]) return t->vals[t->valptr[l] + code - t->mincode[l]];
    }
    return 0;
}
static void decode_block(JD *j, Comp *c, short *blk, int ss, int se, int ah, int al)
{
    if (!j->prog) {
        int s = huff_decode(j, &j->dc[c->td]);
        int diff = s ? extend(get_bits(j, s), s & 15) : 0;
        c->pred += diff; blk[0] = (short)c->pred;
        for (int k = 1; k < 64; k++) {
            int rs = huff_decode(j, &j->ac[c->ta]), r = rs >> 4; s = rs & 15;
            if (s == 0) { if (r == 15) { k += 15; continue; } break; }
            k += r; if (k > 63) break;
            blk[zz[k]] = (short)extend(get_bits(j, s), s);
        }
        return;
    }
    if (ss == 0) {
        if (ah == 0) {
            int s = huff_decode(j, &j->dc[c->td]);
            int diff = s ? extend(get_bits(j, s), s & 15) : 0;
            c->pred += diff; blk[0] = (short)(c->pred * (1 << al));
        } else if (fill_bit(j)) blk[0] |= (short)(1 << al);
        return;
    }
    if (ah == 0) {
        if (j->eobrun > 0) { j->eobrun--; return; }
        for (int k = ss; k <= se; k++) {
            int rs = huff_decode(j, &j->ac[c->ta]), r = rs >> 4, s = rs & 15;
            if (s) { k += r; if (k > 63) break; blk[zz[k]] = (short)(extend(get_bits(j, s), s) * (1 << al)); }
            else if (r == 15) k += 15;
            else { j->eobrun = 1 << r; if (r) j->eobrun += get_bits(j, r); j->eobrun--; break; }
        }
        return;
    }
    int p1 = 1 << al, m1 = -(1 << al), k = ss;
    if (j->eobrun == 0) {
        for (; k <= se; k++) {
            int rs = huff_decode(j, &j->ac[c->ta]), r = rs >> 4, s = rs & 15;
            if (s) { s = fill_bit(j) ? p1 : m1; }
            else if (r != 15) { j->eobrun = 1 << r; if (r) j->eobrun += get_bits(j, r); break; }
            do {
                short *cf = &blk[zz[k]];
                if (*cf != 0) { if (fill_bit(j) && ((*cf & p1) == 0)) *cf += (short)(*cf >= 0 ? p1 : m1); }
                else { if (--r < 0) break; }
                k++;
            } while (k <= se);
            if (s && k <= 63) blk[zz[k]] = (short)s;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; k++) {
            short *cf = &blk[zz[k]];
            if (*cf != 0 && fill_bit(j) && ((*cf & p1) == 0)) *cf += (short)(*cf >= 0 ? p1 : m1);
        }
        j->eobrun--;
    }
}
static int do_restart(JD *j)
{
    j->bitcnt = 0;
    if (!j->marker) { /* find marker */
        int b;
        while ((b = get_byte_raw(j)) >= 0) { if (b == 0xff) { int b2 = get_byte_raw(j); if (b2 >= 0xd0 && b2 <= 0xd7) { break; } if (b2 != 0 && b2 != 0xff) { j->pos -= 2; break; } } }
    } else if (j->marker >= 0xd0 && j->marker <= 0xd7) j->marker = 0;
    j->marker = 0; j->eobrun = 0;
    for (int i = 0; i < j->ncomp; i++) j->c[i].pred = 0;
    return 0;
}
static int decode_scan(JD *j, int *sc, int nsc, int ss, int se, int ah, int al)
{
    j->bitcnt = 0; j->marker = 0; j->eobrun = 0;
    for (int i = 0; i < j->ncomp; i++) j->c[i].pred = 0;
    int cnt = 0;
    if (nsc == 1) {
        Comp *c = &j->c[sc[0]];
        int bx = (c->cw + 7) / 8, by = (c->ch + 7) / 8;
        for (int y = 0; y < by; y++) for (int x = 0; x < bx; x++) {
            if (j->ri && cnt && cnt % j->ri == 0) do_restart(j);
            decode_block(j, c, c->coef + ((size_t)y * c->bw + x) * 64, ss, se, ah, al);
            cnt++;
        }
    } else {
        for (int my = 0; my < j->mcuy; my++) for (int mx = 0; mx < j->mcux; mx++) {
            if (j->ri && cnt && cnt % j->ri == 0) do_restart(j);
            for (int i = 0; i < nsc; i++) {
                Comp *c = &j->c[sc[i]];
                for (int v = 0; v < c->v; v++) for (int h = 0; h < c->h; h++)
                    decode_block(j, c, c->coef + ((size_t)(my * c->v + v) * c->bw + mx * c->h + h) * 64, ss, se, ah, al);
            }
            cnt++;
        }
    }
    /* skip to next marker */
    j->bitcnt = 0;
    if (j->marker) { j->pos -= 0; }
    return 0;
}
static int next_marker(JD *j)
{
    for (;;) {
        int b = get_byte_raw(j);
        if (b < 0) return -1;
        if (b != 0xff) continue;
        int b2 = get_byte_raw(j);
        while (b2 == 0xff) b2 = get_byte_raw(j);
        if (b2 > 0 && !(b2 >= 0xd0 && b2 <= 0xd7) && b2 != 1) return b2;
        if (b2 < 0) return -1;
    }
}
static float idct_c[8][8]; static int idct_ready;
static void idct_init(void)
{
    for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++) idct_c[x][u] = (float)((u == 0 ? 0.35355339059327373 : 0.5) * cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
    idct_ready = 1;
}
static void idct_block(const short *in, const unsigned short *q /* natural order */, u8 *out, int stride)
{
    float t[64], f[64];
    for (int i = 0; i < 64; i++) f[i] = (float)in[i] * q[i];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { float s = 0; for (int u = 0; u < 8; u++) s += idct_c[x][u] * f[y * 8 + u]; t[y * 8 + x] = s; }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
        float s = 0; for (int v = 0; v < 8; v++) s += idct_c[y][v] * t[v * 8 + x];
        int p = (int)floorf(s + 128.5f); out[y * stride + x] = (u8)(p < 0 ? 0 : p > 255 ? 255 : p);
    }
}
uint32_t *jpg_decode(const uint8_t *d, size_t n, int *ow, int *oh)
{
    if (n < 4 || d[0] != 0xff || d[1] != 0xd8) return NULL;
    if (!idct_ready) idct_init();
    JD *j = (JD *)calloc(1, sizeof(JD));
    j->d = d; j->n = n; j->pos = 2;
    int ok = 0, got_scan = 0;
    unsigned short qz[4][64]; memset(qz, 0, sizeof qz);
    for (;;) {
        int m = next_marker(j);
        if (m < 0 || m == 0xd9) break;
        if (m == 0xd8 || (m >= 0xd0 && m <= 0xd7) || m == 0x01) continue;
        if (j->pos + 2 > n) break;
        size_t len = (d[j->pos] << 8) | d[j->pos + 1];
        size_t seg = j->pos + 2, end = j->pos + len;
        if (end > n) break;
        if (m == 0xdb) {
            while (seg < end) {
                int pq = d[seg] >> 4, tq = d[seg] & 15; seg++;
                if (tq > 3) goto fail;
                for (int i = 0; i < 64; i++) { qz[tq][zz[i]] = pq ? (d[seg] << 8) | d[seg + 1] : d[seg]; seg += pq ? 2 : 1; }
            }
            memcpy(j->q, qz, sizeof qz);
        } else if (m == 0xc4) {
            while (seg + 17 <= end) {
                int tc = d[seg] >> 4, th = d[seg] & 15; seg++;
                HT *t = tc ? &j->ac[th & 3] : &j->dc[th & 3]; int tot = 0;
                memset(t, 0, sizeof *t);
                for (int i = 1; i <= 16; i++) { t->bits[i] = d[seg++]; tot += t->bits[i]; }
                if (tot > 256 || seg + tot > end) goto fail;
                memcpy(t->vals, d + seg, tot); seg += tot; ht_build(t);
            }
        } else if (m == 0xc0 || m == 0xc1 || m == 0xc2) {
            if (d[seg] != 8) goto fail;
            j->prog = (m == 0xc2); j->h = (d[seg + 1] << 8) | d[seg + 2]; j->w = (d[seg + 3] << 8) | d[seg + 4]; j->ncomp = d[seg + 5];
            if (j->w <= 0 || j->h <= 0 || j->ncomp < 1 || j->ncomp > 3) goto fail;
            if ((long long)j->w * j->h > 100000000LL) goto fail;
            j->hmax = j->vmax = 1;
            for (int i = 0; i < j->ncomp; i++) {
                Comp *c = &j->c[i]; c->id = d[seg + 6 + i * 3]; c->h = d[seg + 7 + i * 3] >> 4; c->v = d[seg + 7 + i * 3] & 15; c->tq = d[seg + 8 + i * 3] & 3;
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) goto fail;
                if (c->h > j->hmax) j->hmax = c->h; if (c->v > j->vmax) j->vmax = c->v;
            }
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax); j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->ncomp; i++) {
                Comp *c = &j->c[i];
                c->bw = j->mcux * c->h; c->bh = j->mcuy * c->v;
                c->cw = (j->w * c->h + j->hmax - 1) / j->hmax; c->ch = (j->h * c->v + j->vmax - 1) / j->vmax;
                c->coef = (short *)calloc((size_t)c->bw * c->bh * 64, sizeof(short));
                if (!c->coef) goto fail;
            }
            ok = 1;
        } else if (m == 0xc3 || (m >= 0xc5 && m <= 0xcf && m != 0xc8 && m != 0xcc)) goto fail; /* lossless/arithmetic unsupported */
        else if (m == 0xdd) j->ri = (d[seg] << 8) | d[seg + 1];
        else if (m == 0xda) {
            if (!ok) goto fail;
            int ns = d[seg++]; int sc[4];
            for (int i = 0; i < ns; i++) {
                int cid = d[seg], t = d[seg + 1]; seg += 2; int idx = -1;
                for (int k = 0; k < j->ncomp; k++) if (j->c[k].id == cid) idx = k;
                if (idx < 0) idx = i < j->ncomp ? i : 0;
                j->c[idx].td = (t >> 4) & 3; j->c[idx].ta = t & 3; sc[i] = idx;
            }
            int ss = d[seg], se = d[seg + 1], ah = d[seg + 2] >> 4, al = d[seg + 2] & 15;
            if (!j->prog) { ss = 0; se = 63; ah = al = 0; }
            j->pos = end;
            decode_scan(j, sc, ns, ss, se, ah, al);
            got_scan = 1;
            continue;
        }
        j->pos = end;
    }
    if (!ok || !got_scan) goto fail;
    {
        uint32_t *px = (uint32_t *)malloc((size_t)j->w * j->h * 4);
        u8 *planes[4] = { 0 }; int pstride[4];
        for (int i = 0; i < j->ncomp; i++) {
            Comp *c = &j->c[i]; pstride[i] = c->bw * 8;
            planes[i] = (u8 *)malloc((size_t)c->bw * 8 * c->bh * 8);
            for (int by = 0; by < c->bh; by++) for (int bx = 0; bx < c->bw; bx++)
                idct_block(c->coef + ((size_t)by * c->bw + bx) * 64, j->q[c->tq], planes[i] + (size_t)by * 8 * pstride[i] + bx * 8, pstride[i]);
        }
        for (int y = 0; y < j->h; y++) for (int x = 0; x < j->w; x++) {
            int s[3];
            for (int i = 0; i < j->ncomp; i++) {
                Comp *c = &j->c[i];
                s[i] = planes[i][(size_t)(y * c->v / j->vmax) * pstride[i] + x * c->h / j->hmax];
            }
            int r, g, b;
            if (j->ncomp == 1) r = g = b = s[0];
            else {
                float Y = (float)s[0], cb = (float)s[1] - 128.f, cr = (float)s[2] - 128.f;
                r = (int)floorf(Y + 1.402f * cr + 0.5f); g = (int)floorf(Y - 0.344136f * cb - 0.714136f * cr + 0.5f); b = (int)floorf(Y + 1.772f * cb + 0.5f);
                r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
            }
            px[(size_t)y * j->w + x] = ((u32)r << 16) | (g << 8) | b;
        }
        for (int i = 0; i < j->ncomp; i++) { free(planes[i]); free(j->c[i].coef); }
        *ow = j->w; *oh = j->h; free(j);
        return px;
    }
fail:
    for (int i = 0; i < 4; i++) free(j->c[i].coef);
    free(j);
    return NULL;
}

/* ============================ encoder ============================ */
static const u8 dc_l_bits[17] = { 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const u8 dc_c_bits[17] = { 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const u8 dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const u8 ac_l_bits[17] = { 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const u8 ac_l_vals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa };
static const u8 ac_c_bits[17] = { 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const u8 ac_c_vals[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa };
static const u8 qbase_l[64] = { 16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55, 14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92, 49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99 };
static const u8 qbase_c[64] = { 17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99, 24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99 };
typedef struct { int code[256]; int len[256]; } HC;
static void hc_build(HC *h, const u8 *bits, const u8 *vals)
{
    memset(h, 0, sizeof *h);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) { for (int i = 0; i < bits[l]; i++) { h->code[vals[k]] = code++; h->len[vals[k]] = l; k++; } code <<= 1; }
}
typedef struct { u8 *p; size_t n, cap; u32 acc; int cnt; } JW;
static void jw8(JW *w, int v) { if (w->n == w->cap) { w->cap = w->cap * 2 + 1024; w->p = (u8 *)realloc(w->p, w->cap); } w->p[w->n++] = (u8)v; }
static void jbits(JW *w, int v, int n)
{
    if (!n) return;
    w->acc = (w->acc << n) | (v & ((1 << n) - 1)); w->cnt += n;
    while (w->cnt >= 8) { int b = (w->acc >> (w->cnt - 8)) & 255; jw8(w, b); if (b == 0xff) jw8(w, 0); w->cnt -= 8; }
}
static void jmark(JW *w, int m, const u8 *d, int len) { jw8(w, 0xff); jw8(w, m); if (len >= 0) { jw8(w, (len + 2) >> 8); jw8(w, (len + 2) & 255); for (int i = 0; i < len; i++) jw8(w, d[i]); } }
static int nbits(int v) { int n = 0; v = v < 0 ? -v : v; while (v) { n++; v >>= 1; } return n; }
static void enc_block(JW *w, const float *blk, const int *q, int *pred, const HC *dc, const HC *ac)
{
    int c[64];
    for (int i = 0; i < 64; i++) { float v = blk[zz[i]] / (float)q[zz[i]]; c[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f); }
    int diff = c[0] - *pred; *pred = c[0];
    int s = nbits(diff); jbits(w, dc->code[s], dc->len[s]); if (s) jbits(w, diff < 0 ? diff - 1 : diff, s);
    int run = 0;
    for (int k = 1; k < 64; k++) {
        if (c[k] == 0) { run++; continue; }
        while (run > 15) { jbits(w, ac->code[0xf0], ac->len[0xf0]); run -= 16; }
        s = nbits(c[k]); int rs = (run << 4) | s; jbits(w, ac->code[rs], ac->len[rs]); jbits(w, c[k] < 0 ? c[k] - 1 : c[k], s); run = 0;
    }
    if (run) jbits(w, ac->code[0], ac->len[0]);
}
uint8_t *jpg_encode(const uint32_t *px, int width, int height, int quality, size_t *outn)
{
    if (!idct_ready) idct_init();
    if (quality < 1) quality = 1; if (quality > 100) quality = 100;
    int sc = quality < 50 ? 5000 / quality : 200 - quality * 2;
    int ql[64], qc[64];
    for (int i = 0; i < 64; i++) {
        int a = (qbase_l[i] * sc + 50) / 100, b = (qbase_c[i] * sc + 50) / 100;
        ql[i] = a < 1 ? 1 : a > 255 ? 255 : a; qc[i] = b < 1 ? 1 : b > 255 ? 255 : b;
    }
    HC dcl, dcc, acl, acc; hc_build(&dcl, dc_l_bits, dc_vals); hc_build(&dcc, dc_c_bits, dc_vals); hc_build(&acl, ac_l_bits, ac_l_vals); hc_build(&acc, ac_c_bits, ac_c_vals);
    JW w = { 0 };
    jw8(&w, 0xff); jw8(&w, 0xd8);
    static const u8 jfif[14] = { 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };
    jmark(&w, 0xe0, jfif, 14);
    for (int t = 0; t < 2; t++) { u8 b[65]; b[0] = (u8)t; for (int i = 0; i < 64; i++) b[1 + i] = (u8)(t ? qc : ql)[zz[i]]; jmark(&w, 0xdb, b, 65); }
    u8 sof[15] = { 8, (u8)(height >> 8), (u8)height, (u8)(width >> 8), (u8)width, 3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1 };
    jmark(&w, 0xc0, sof, 15);
    const u8 *bits[4] = { dc_l_bits, ac_l_bits, dc_c_bits, ac_c_bits }; const u8 *vals[4] = { dc_vals, ac_l_vals, dc_vals, ac_c_vals }; int nv[4] = { 12, 162, 12, 162 };
    for (int t = 0; t < 4; t++) {
        u8 b[17 + 162]; b[0] = (u8)(((t & 1) << 4) | (t >> 1)); for (int i = 1; i <= 16; i++) b[i] = bits[t][i]; memcpy(b + 17, vals[t], nv[t]);
        jmark(&w, 0xc4, b, 17 + nv[t]);
    }
    u8 sos[10] = { 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 };
    jmark(&w, 0xda, sos, 10);
    int pred[3] = { 0, 0, 0 };
    for (int by = 0; by < (height + 7) / 8; by++) for (int bx = 0; bx < (width + 7) / 8; bx++) {
        float blk[3][64];
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
            int xx = bx * 8 + x, yy = by * 8 + y; if (xx >= width) xx = width - 1; if (yy >= height) yy = height - 1;
            u32 c = px[(size_t)yy * width + xx]; float r = (float)((c >> 16) & 255), g = (float)((c >> 8) & 255), b = (float)(c & 255);
            blk[0][y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128.f;
            blk[1][y * 8 + x] = -0.168736f * r - 0.331264f * g + 0.5f * b;
            blk[2][y * 8 + x] = 0.5f * r - 0.418688f * g - 0.081312f * b;
        }
        for (int ci = 0; ci < 3; ci++) {
            float f[64], t[64];
            for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) { float s = 0; for (int y = 0; y < 8; y++) s += idct_c[y][v] * blk[ci][y * 8 + u]; t[v * 8 + u] = s; }
            for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) { float s = 0; for (int x = 0; x < 8; x++) s += idct_c[x][u] * t[v * 8 + x]; f[v * 8 + u] = s; }
            enc_block(&w, f, ci ? qc : ql, &pred[ci], ci ? &dcc : &dcl, ci ? &acc : &acl);
        }
    }
    if (w.cnt) jbits(&w, 0x7f, 8 - w.cnt);
    jw8(&w, 0xff); jw8(&w, 0xd9);
    *outn = w.n;
    return w.p;
}
