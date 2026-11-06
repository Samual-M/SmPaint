/* canvas.c - pixel drawing primitives and image transforms (portable, no OS dependencies) */
#include <math.h>
#include "app.h"

static const uint32_t *round_masks[8];
static int masks_ready;
static int mask_on(int size, int i, int j)
{
    if (!masks_ready) {
        round_masks[0] = res_brushmask1; round_masks[1] = res_brushmask2; round_masks[2] = res_brushmask3; round_masks[3] = res_brushmask4;
        round_masks[4] = res_brushmask5; round_masks[5] = res_brushmask6; round_masks[6] = res_brushmask7; round_masks[7] = res_brushmask8;
        masks_ready = 1;
    }
    if (size <= 1) return 1;
    if (size > 8) {
        double r = size / 2.0, dx = i + 0.5 - r, dy = j + 0.5 - r;
        return dx * dx + dy * dy <= r * r;
    }
    return (round_masks[size - 1][j * size + i] & 0xffffff) == 0xffffff;
}
void cv_dot(Img *im, int x, int y, px_t c, int w)
{
    if (w <= 1) { img_put(im, x, y, c); return; }
    int x0 = x - w / 2, y0 = y - w / 2;
    for (int j = 0; j < w; j++)
        for (int i = 0; i < w; i++)
            if (mask_on(w, i, j)) img_put(im, x0 + i, y0 + j, c);
}
typedef void (*StampFn)(Img *, int, int, px_t, int, int);
static void bres(Img *im, int x0, int y0, int x1, int y1, StampFn f, px_t c, int a, int b)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        f(im, x0, y0, c, a, b);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void st_dot(Img *im, int x, int y, px_t c, int w, int unused) { (void)unused; cv_dot(im, x, y, c, w); }
void cv_line(Img *im, int x0, int y0, int x1, int y1, px_t c, int w) { bres(im, x0, y0, x1, y1, st_dot, c, w, 0); }

void cv_brush(Img *im, int x, int y, int shape, int size, px_t c)
{
    if (shape == 0) { cv_dot(im, x, y, c, size); return; }
    int o = size / 2;
    for (int i = 0; i < size; i++) {
        if (shape == 1) for (int j = 0; j < size; j++) img_put(im, x - o + i, y - o + j, c);
        else if (shape == 2) img_put(im, x - o + i, y + (size - 1 - o) - i, c);
        else img_put(im, x - o + i, y - o + i, c);
    }
}
static void st_brush(Img *im, int x, int y, px_t c, int shape, int size) { cv_brush(im, x, y, shape, size, c); }
void cv_brush_line(Img *im, int x0, int y0, int x1, int y1, int shape, int size, px_t c) { bres(im, x0, y0, x1, y1, st_brush, c, shape, size); }

void cv_flood(Img *im, int sx, int sy, px_t c)
{
    if (sx < 0 || sy < 0 || sx >= im->w || sy >= im->h) return;
    px_t t = im->p[sy * im->w + sx];
    if (t == c) return;
    int cap = 1024, n = 0;
    Pt *st = (Pt *)malloc(cap * sizeof(Pt));
    st[n++] = (Pt){ sx, sy };
    while (n) {
        Pt p = st[--n];
        int x = p.x, y = p.y;
        if (im->p[y * im->w + x] != t) continue;
        int l = x, r = x;
        while (l > 0 && im->p[y * im->w + l - 1] == t) l--;
        while (r < im->w - 1 && im->p[y * im->w + r + 1] == t) r++;
        for (int i = l; i <= r; i++) im->p[y * im->w + i] = c;
        for (int d = -1; d <= 1; d += 2) {
            int yy = y + d;
            if (yy < 0 || yy >= im->h) continue;
            int in = 0;
            for (int i = l; i <= r; i++) {
                int m = im->p[yy * im->w + i] == t;
                if (m && !in) { if (n == cap) { cap *= 2; st = (Pt *)realloc(st, cap * sizeof(Pt)); } st[n++] = (Pt){ i, yy }; }
                in = m;
            }
        }
    }
    free(st);
}
void cv_air(Img *im, int x, int y, int r, px_t c)
{
    int cnt = r * r / 3 + 2;
    for (int i = 0; i < cnt; i++) {
        int dx, dy;
        do { dx = rand() % (2 * r + 1) - r; dy = rand() % (2 * r + 1) - r; } while (dx * dx + dy * dy > r * r);
        img_put(im, x + dx, y + dy, c);
    }
}

/* ---------- shapes via per-pixel inside tests ---------- */
typedef struct { int kind, x0, y0, x1, y1; } ShapeG;
static int shape_inside(const ShapeG *g, int px, int py, int k)
{
    int w = g->x1 - g->x0 + 1, h = g->y1 - g->y0 + 1;
    if (g->kind == SH_RECT) return px >= g->x0 + k && px <= g->x1 - k && py >= g->y0 + k && py <= g->y1 - k;
    if (g->kind == SH_ELLIPSE) {
        double a = w / 2.0 - k, b = h / 2.0 - k;
        if (a <= 0 || b <= 0) return 0;
        double dx = (px + 0.5 - (g->x0 + w / 2.0)) / a, dy = (py + 0.5 - (g->y0 + h / 2.0)) / b;
        return dx * dx + dy * dy <= 1.0;
    }
    /* rounded rect */
    if (!(px >= g->x0 + k && px <= g->x1 - k && py >= g->y0 + k && py <= g->y1 - k)) return 0;
    double R = 8; if (R > w / 2.0) R = w / 2.0; if (R > h / 2.0) R = h / 2.0;
    double r = R - k; if (r <= 0) return 1;
    double cx = px + 0.5, cy = py + 0.5, lx = g->x0 + R, rx = g->x1 + 1 - R, ty = g->y0 + R, by = g->y1 + 1 - R;
    double dx = cx < lx ? lx - cx : cx > rx ? cx - rx : 0, dy = cy < ty ? ty - cy : cy > by ? cy - by : 0;
    return dx * dx + dy * dy <= r * r;
}
void cv_shape(Img *im, int kind, int x0, int y0, int x1, int y1, int fs, px_t outline, px_t fill, int lw)
{
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    ShapeG g = { kind, x0, y0, x1, y1 };
    int cx0 = x0 < 0 ? 0 : x0, cy0 = y0 < 0 ? 0 : y0, cx1 = x1 >= im->w ? im->w - 1 : x1, cy1 = y1 >= im->h ? im->h - 1 : y1;
    for (int y = cy0; y <= cy1; y++)
        for (int x = cx0; x <= cx1; x++) {
            if (!shape_inside(&g, x, y, 0)) continue;
            int ring = (fs != 2) && !shape_inside(&g, x, y, lw);
            if (ring) im->p[y * im->w + x] = outline;
            else if (fs != 0) im->p[y * im->w + x] = fill;
        }
}
void cv_fillpoly_mask(uint8_t *mask, int w, int h, const Pt *p, int n, int ox, int oy)
{
    int *xs = (int *)malloc(sizeof(int) * (n + 2));
    for (int y = 0; y < h; y++) {
        int k = 0; double yc = y + oy + 0.5;
        for (int i = 0; i < n; i++) {
            Pt a = p[i], b = p[(i + 1) % n];
            if ((a.y <= yc && b.y > yc) || (b.y <= yc && a.y > yc)) {
                double t = (yc - a.y) / (double)(b.y - a.y);
                xs[k++] = (int)floor(a.x + t * (b.x - a.x) + 0.5);
            }
        }
        for (int i = 1; i < k; i++) { int v = xs[i], j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
        for (int i = 0; i + 1 < k; i += 2)
            for (int x = xs[i]; x < xs[i + 1]; x++) { int mx = x - ox; if (mx >= 0 && mx < w) mask[y * w + mx] = 255; }
    }
    free(xs);
}
void cv_poly(Img *im, const Pt *p, int n, int closed, int fs, px_t outline, px_t fill, int lw)
{
    if (n >= 3 && fs != 0 && closed) {
        int minx = p[0].x, maxx = p[0].x, miny = p[0].y, maxy = p[0].y;
        for (int i = 1; i < n; i++) { if (p[i].x < minx) minx = p[i].x; if (p[i].x > maxx) maxx = p[i].x; if (p[i].y < miny) miny = p[i].y; if (p[i].y > maxy) maxy = p[i].y; }
        int w = maxx - minx + 1, h = maxy - miny + 1;
        uint8_t *m = (uint8_t *)calloc((size_t)w * h, 1);
        cv_fillpoly_mask(m, w, h, p, n, minx, miny);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (m[y * w + x]) img_put(im, minx + x, miny + y, fill);
        free(m);
    }
    if (fs == 2 && closed) return;
    for (int i = 0; i + 1 < n; i++) cv_line(im, p[i].x, p[i].y, p[i + 1].x, p[i + 1].y, outline, lw);
    if (closed && n >= 3) cv_line(im, p[n - 1].x, p[n - 1].y, p[0].x, p[0].y, outline, lw);
    if (n == 1) cv_dot(im, p[0].x, p[0].y, outline, lw);
}
void cv_curve(Img *im, Pt p0, Pt p1, Pt p2, Pt p3, px_t c, int lw)
{
    double len = hypot(p1.x - p0.x, p1.y - p0.y) + hypot(p2.x - p1.x, p2.y - p1.y) + hypot(p3.x - p2.x, p3.y - p2.y);
    int steps = (int)len + 2; if (steps > 4000) steps = 4000;
    int px = p0.x, py = p0.y;
    for (int i = 1; i <= steps; i++) {
        double t = (double)i / steps, u = 1 - t;
        double x = u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x;
        double y = u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y;
        int ix = (int)floor(x + 0.5), iy = (int)floor(y + 0.5);
        cv_line(im, px, py, ix, iy, c, lw);
        px = ix; py = iy;
    }
}

/* ---------- transforms ---------- */
Img *cv_flip(const Img *s, int horiz)
{
    Img *d = img_new(s->w, s->h, 0);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            d->p[y * s->w + x] = horiz ? s->p[y * s->w + (s->w - 1 - x)] : s->p[(s->h - 1 - y) * s->w + x];
    return d;
}
Img *cv_rotate(const Img *s, int deg)
{
    if (deg == 180) { Img *a = cv_flip(s, 1); Img *b = cv_flip(a, 0); img_free(a); return b; }
    Img *d = img_new(s->h, s->w, 0);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            if (deg == 90) d->p[x * d->w + (s->h - 1 - y)] = s->p[y * s->w + x];
            else d->p[(s->w - 1 - x) * d->w + y] = s->p[y * s->w + x];
        }
    return d;
}
Img *cv_stretch(const Img *s, int nw, int nh)
{
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    Img *d = img_new(nw, nh, 0);
    for (int y = 0; y < nh; y++) {
        int sy = (int)((long long)y * s->h / nh);
        for (int x = 0; x < nw; x++) d->p[y * nw + x] = s->p[sy * s->w + (int)((long long)x * s->w / nw)];
    }
    return d;
}
Img *cv_skew(const Img *s, int hdeg, int vdeg, px_t bg)
{
    double th = tan(hdeg * 3.14159265358979 / 180.0), tv = tan(vdeg * 3.14159265358979 / 180.0);
    int extra = (int)floor(fabs(th) * s->h + 0.5);
    int nw = s->w + extra;
    Img *a = img_new(nw, s->h, bg);
    for (int y = 0; y < s->h; y++) {
        /* positive angle skews the top to the right (like Paint) */
        int off = (int)floor((th >= 0 ? (s->h - 1 - y) : y) * fabs(th) + 0.5);
        for (int x = 0; x < s->w; x++) img_put(a, x + off, y, s->p[y * s->w + x]);
    }
    int extra_v = (int)floor(fabs(tv) * nw + 0.5);
    Img *b = img_new(nw, s->h + extra_v, bg);
    for (int x = 0; x < nw; x++) {
        int off = (int)floor((tv >= 0 ? (nw - 1 - x) : x) * fabs(tv) + 0.5);
        for (int y = 0; y < s->h; y++) img_put(b, x, y + off, a->p[y * nw + x]);
    }
    img_free(a);
    return b;
}
Img *cv_crop_resize(const Img *s, int nw, int nh, px_t bg)
{
    Img *d = img_new(nw, nh, bg);
    for (int y = 0; y < nh && y < s->h; y++)
        for (int x = 0; x < nw && x < s->w; x++) d->p[y * nw + x] = s->p[y * s->w + x];
    return d;
}
void cv_invert(Img *im) { for (size_t i = 0; i < (size_t)im->w * im->h; i++) im->p[i] ^= 0xffffff; }
void cv_to_bw(Img *im)
{
    for (size_t i = 0; i < (size_t)im->w * im->h; i++) {
        px_t c = im->p[i]; int l = (PR(c) * 30 + PG(c) * 59 + PB(c) * 11) / 100;
        im->p[i] = l >= 128 ? 0xffffff : 0;
    }
}
