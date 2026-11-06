/* tools.c - drawing tools, selection, text, undo/redo, clipboard, file operations and commands (portable) */
#include <ctype.h>
#include <math.h>
#include "app.h"

App A;
extern int g_mx, g_my, g_min;   /* mouse position in image coords / over canvas (ui_main.c) */
extern int g_quit;

#define DEF_W 640
#define DEF_H 480
static int fdiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static const int ERASER_SZ[4] = { 4, 6, 8, 10 };
static const int BRUSH_SHAPE[12] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3 };
static const int BRUSH_SIZE[12] = { 7, 4, 1, 8, 5, 2, 8, 5, 2, 8, 5, 2 };
static const int AIR_R[3] = { 4, 8, 12 };
static const int MAG_Z[4] = { 1, 2, 6, 8 };

/* ---------------- drag state ---------------- */
static struct {
    int active, btn, tool, mods;
    Pt start, last;
    int mode;               /* 0 normal, 1 move selection, 2 resize selection, 3 rubber band select */
    int handle;
    Pt off;
    Img *base;              /* undo snapshot used as preview base */
    Img *orig;              /* original selection pixels while resizing */
    int ox, oy, ow, oh;     /* original selection rect */
    int cancelled;
} D;
/* multi-step tools */
static struct { int phase; Pt p[4]; int btn; } CV;       /* curve */
static struct { int n; Pt p[512]; int btn; int active; } PG; /* polygon */

const int SHAPE_W[6] = { 1, 2, 3, 4, 6, 8 };
int shape_lw(void) { return SHAPE_W[A.o_shapew < 0 || A.o_shapew > 5 ? 0 : A.o_shapew]; }
static int tb_hit(int ix, int iy);
const char *tool_cursor_for(int ix, int iy, int *idx)
{
    switch (A.tool) {
    case T_SELECT: case T_FREESEL:
        if (A.sel.active) { int h = sel_hit(ix, iy); if (h == 1) { *idx = 0; return "move"; } if (h >= 2) { static const int hc[8] = { 9, 12, 10, 11, 9, 12, 10, 11 }; *idx = hc[(h - 2) & 7]; return "size"; } }
        *idx = 14; return "cross";
    case T_ERASER: *idx = 3; return "eraser";
    case T_FILL: *idx = 6; return "fill";
    case T_PICK: *idx = 5; return "pick";
    case T_MAG: *idx = 8; return "mag";
    case T_PENCIL: *idx = 7; return "pencil";
    case T_BRUSH: *idx = 2; return "brush";
    case T_AIR: *idx = 13; return "air";
    case T_TEXT: {
        int h = tb_hit(ix, iy);
        if (h == 2) { *idx = 0; return "move"; }
        if (h >= 3) { static const int hc[8] = { 9, 12, 10, 11, 9, 12, 10, 11 }; *idx = hc[(h - 3) & 7]; return "size"; }
        *idx = 15; return "text";
    }
    default: *idx = 4; return "cross";
    }
}

/* ---------------- undo / redo ---------------- */
void undo_push(void)
{
    if (A.nundo == 3) { img_free(A.undo[0]); A.undo[0] = A.undo[1]; A.undo[1] = A.undo[2]; A.nundo = 2; }
    A.undo[A.nundo++] = img_clone(A.img);
    for (int i = 0; i < A.nredo; i++) img_free(A.redo[i]);
    A.nredo = 0; A.dirty = 1;
}
static void undo_drop_last(void) { if (A.nundo) img_free(A.undo[--A.nundo]); }
void sel_cancel(void)
{
    img_free(A.sel.pix); free(A.sel.mask); free(A.sel.free_pts);
    memset(&A.sel, 0, sizeof A.sel);
}
void do_undo(void)
{
    tool_cancel();
    if (A.tb.active) { free(A.tb.text); free(A.tb.st); memset(&A.tb, 0, sizeof A.tb); strcpy(A.tb.font, "Arial"); A.tb.size = 10; A.tb.sel = -1; }
    if (A.sel.active) sel_cancel();
    if (!A.nundo) return;
    if (A.nredo == 3) { img_free(A.redo[0]); A.redo[0] = A.redo[1]; A.redo[1] = A.redo[2]; A.nredo = 2; }
    A.redo[A.nredo++] = A.img;
    A.img = A.undo[--A.nundo];
    A.dirty = 1; view_clamp();
}
void do_redo(void)
{
    tool_cancel();
    if (A.sel.active) sel_cancel();
    if (!A.nredo) return;
    if (A.nundo == 3) { img_free(A.undo[0]); A.undo[0] = A.undo[1]; A.undo[1] = A.undo[2]; A.nundo = 2; }
    A.undo[A.nundo++] = A.img;
    A.img = A.redo[--A.nredo];
    A.dirty = 1; view_clamp();
}

/* ---------------- selection ---------------- */
static void sel_blit(Img *dst, const Sel *s, int transparent)
{
    for (int j = 0; j < s->h; j++)
        for (int i = 0; i < s->w; i++) {
            if (s->mask && !s->mask[j * s->w + i]) continue;
            px_t c = s->pix->p[j * s->w + i];
            if (transparent && c == A.bg) continue;
            img_put(dst, s->x + i, s->y + j, c);
        }
}
static void sel_lift(int copy)
{
    Sel *s = &A.sel;
    if (!s->active || s->lifted) return;
    undo_push(); s->pushed = 1;
    s->pix = img_new(s->w, s->h, A.bg);
    for (int j = 0; j < s->h; j++) for (int i = 0; i < s->w; i++) s->pix->p[j * s->w + i] = img_get(A.img, s->x + i, s->y + j);
    if (!copy)
        for (int j = 0; j < s->h; j++) for (int i = 0; i < s->w; i++)
            if (!s->mask || s->mask[j * s->w + i]) img_put(A.img, s->x + i, s->y + j, A.bg);
    s->lifted = 1;
}
void sel_commit(void)
{
    Sel *s = &A.sel;
    if (!s->active) return;
    if (s->lifted) { sel_blit(A.img, s, A.o_trans); A.dirty = 1; }
    sel_cancel();
}
static void sel_make(int x, int y, int w, int h)
{
    sel_cancel();
    A.sel.active = 1; A.sel.x = x; A.sel.y = y; A.sel.w = w; A.sel.h = h;
}
int sel_hit(int ix, int iy)
{
    Sel *s = &A.sel;
    if (!s->active) return 0;
    int hs = 2 + 300 / A.zp; /* handle tolerance in image px */
    int x0 = s->x, y0 = s->y, x1 = s->x + s->w, y1 = s->y + s->h, xm = (x0 + x1) / 2, ym = (y0 + y1) / 2;
    int hx[8] = { x0, xm, x1, x1, x1, xm, x0, x0 }, hy[8] = { y0, y0, y0, ym, y1, y1, y1, ym };
    if (!s->mask)
        for (int i = 0; i < 8; i++) if (abs(ix - hx[i]) <= hs && abs(iy - hy[i]) <= hs) return 2 + i;
    if (ix >= x0 && iy >= y0 && ix < x1 && iy < y1) {
        if (s->mask && !s->mask[(iy - y0) * s->w + (ix - x0)]) return 1; /* inside bbox still draggable */
        return 1;
    }
    return 0;
}

/* ---------------- text tool ----------------
 * Every character carries its own style (font, size, bold/italic/underline) so part of a text box can be selected
 * and restyled. The box wraps/grows with the text; it can be moved by its border and resized by its handles. */
#define TF_BOLD 1
#define TF_ITALIC 2
#define TF_UNDER 4
static char g_fonts[32][64]; static int g_nfonts;
static int font_id(const char *n)
{
    for (int i = 0; i < g_nfonts; i++) if (!strcmp(g_fonts[i], n)) return i;
    if (g_nfonts < 32) { strncpy(g_fonts[g_nfonts], n, 63); g_fonts[g_nfonts][63] = 0; return g_nfonts++; }
    return 0;
}
static int pt_to_px(int pt) { return (pt * 96 + 36) / 72; }
static int text_px(void) { return pt_to_px(A.tb.size); }
static TStyle cur_style(void)
{
    TStyle s; s.font = (uint8_t)font_id(A.tb.font[0] ? A.tb.font : "Arial");
    s.size = (uint8_t)(A.tb.size > 0 && A.tb.size < 256 ? A.tb.size : 10);
    s.flags = (uint8_t)((A.tb.bold ? TF_BOLD : 0) | (A.tb.italic ? TF_ITALIC : 0) | (A.tb.underline ? TF_UNDER : 0));
    return s;
}
static void set_cur(TStyle s)
{
    strncpy(A.tb.font, g_fonts[s.font], 63); A.tb.font[63] = 0; A.tb.size = s.size;
    A.tb.bold = (s.flags & TF_BOLD) != 0; A.tb.italic = (s.flags & TF_ITALIC) != 0; A.tb.underline = (s.flags & TF_UNDER) != 0;
}
static int styles_equal(const TStyle *a, const TStyle *b) { return a->font == b->font && a->size == b->size && a->flags == b->flags; }

/* measuring text means rendering it; remember the results */
typedef struct { uint64_t key; int w, h; } MCache;
static MCache mcache[1024];
static uint64_t run_key(TStyle s, const char *p, int n)
{
    uint64_t h = 1469598103934665603ull;
    h = (h ^ s.font) * 1099511628211ull; h = (h ^ s.size) * 1099511628211ull; h = (h ^ s.flags) * 1099511628211ull;
    for (int i = 0; i < n; i++) h = (h ^ (unsigned char)p[i]) * 1099511628211ull;
    return h | 1;
}
static void run_dims(TStyle s, const char *p, int n, int *w, int *h)
{
    uint64_t k = run_key(s, p, n); MCache *c = &mcache[k & 1023];
    if (c->key == k) { *w = c->w; *h = c->h; return; }
    char *tmp = (char *)malloc((size_t)n + 1); memcpy(tmp, p, (size_t)n); tmp[n] = 0;
    int ww = 0, hh = 0;
    uint8_t *m = plat_render_text(n ? tmp : " ", g_fonts[s.font], pt_to_px(s.size), (s.flags & TF_BOLD) != 0, (s.flags & TF_ITALIC) != 0, (s.flags & TF_UNDER) != 0, &ww, &hh);
    free(m); free(tmp);
    if (!n) ww = 0;
    c->key = k; c->w = ww; c->h = hh; *w = ww; *h = hh;
}
static int asc_of(int h) { return (h * 4 + 2) / 5; }       /* estimated baseline so runs of different sizes line up */
static int span_w(const char *s, int a, int b)
{
    int w = 0, i = a;
    while (i < b) {
        int j = i + 1; while (j < b && styles_equal(&A.tb.st[j], &A.tb.st[i])) j++;
        int rw, rh; run_dims(A.tb.st[i], s + i, j - i, &rw, &rh); w += rw; i = j;
    }
    return w;
}
static char *text_wrapped(void)
{
    TextBox *t = &A.tb;
    char *o = (char *)malloc((size_t)t->len + 1); memcpy(o, t->text ? t->text : "", (size_t)t->len); o[t->len] = 0;
    int maxw = t->w - 2; if (maxw < 4) return o;
    int ls = 0, lastsp = -1;
    for (int i = 0; i <= t->len; i++) {
        if (i == t->len || o[i] == '\n') { ls = i + 1; lastsp = -1; continue; }
        if (o[i] == ' ') lastsp = i;
        if (span_w(o, ls, i + 1) > maxw && lastsp >= ls) { o[lastsp] = '\n'; ls = lastsp + 1; lastsp = -1; i = ls - 1; }
    }
    return o;
}
typedef struct { int ls, le, w, asc, h; } TLine;
static TLine *tlayout(const char *s, int *nl)
{
    TextBox *t = &A.tb; int len = t->len, n = 1;
    for (int i = 0; i < len; i++) if (s[i] == '\n') n++;
    TLine *L = (TLine *)calloc((size_t)n, sizeof(TLine)); int li = 0, ls = 0;
    for (int i = 0; i <= len; i++) {
        if (i != len && s[i] != '\n') continue;
        TLine *l = &L[li++]; l->ls = ls; l->le = i;
        int asc = 0, desc = 0, w = 0;
        if (i == ls) {
            TStyle st = len ? t->st[ls > 0 ? ls - 1 : 0] : cur_style();
            int rw, rh; run_dims(st, "", 0, &rw, &rh); asc = asc_of(rh); desc = rh - asc;
        } else {
            int a = ls;
            while (a < i) {
                int b = a + 1; while (b < i && styles_equal(&t->st[b], &t->st[a])) b++;
                int rw, rh; run_dims(t->st[a], s + a, b - a, &rw, &rh); w += rw;
                int ra = asc_of(rh); if (ra > asc) asc = ra; if (rh - ra > desc) desc = rh - ra;
                a = b;
            }
        }
        l->w = w; l->asc = asc; l->h = asc + desc; ls = i + 1;
    }
    *nl = n; return L;
}
static void text_blit(Img *dst)
{
    TextBox *t = &A.tb;
    if (!t->active) return;
    if (!A.o_trans) for (int j = 0; j < t->h; j++) for (int i = 0; i < t->w; i++) img_put(dst, t->x + i, t->y + j, A.bg);
    if (!t->len) return;
    char *wr = text_wrapped(); int nl; TLine *L = tlayout(wr, &nl); int y = 0;
    for (int li = 0; li < nl; li++) {
        TLine *l = &L[li]; int x = 0, a = l->ls;
        while (a < l->le) {
            int b = a + 1; while (b < l->le && styles_equal(&t->st[b], &t->st[a])) b++;
            TStyle st = t->st[a];
            char *tmp = (char *)malloc((size_t)(b - a) + 1); memcpy(tmp, wr + a, (size_t)(b - a)); tmp[b - a] = 0;
            int tw, th;
            uint8_t *m = plat_render_text(tmp, g_fonts[st.font], pt_to_px(st.size), (st.flags & TF_BOLD) != 0, (st.flags & TF_ITALIC) != 0, (st.flags & TF_UNDER) != 0, &tw, &th);
            free(tmp);
            if (m) {
                int oy = y + l->asc - asc_of(th);
                for (int j = 0; j < th; j++) for (int i = 0; i < tw; i++)
                    if (m[j * tw + i] && x + i < t->w && oy + j < t->h && oy + j >= 0) img_put(dst, t->x + x + i, t->y + oy + j, A.fg);
                free(m);
            }
            x += tw; a = b;
        }
        y += l->h;
    }
    free(L); free(wr);
}
void text_commit(void)
{
    TextBox *t = &A.tb;
    if (!t->active) return;
    if (t->len) { undo_push(); text_blit(A.img); }
    free(t->text); free(t->st);
    char font[64]; int sz = t->size, b = t->bold, it = t->italic, u = t->underline;
    strcpy(font, t->font);
    memset(t, 0, sizeof *t);
    strcpy(t->font, font); t->size = sz; t->bold = b; t->italic = it; t->underline = u; t->sel = -1;
}
/* grow the box so all text fits (wraps only at the image's right edge) */
void text_autogrow(void)
{
    TextBox *t = &A.tb;
    if (!t->active) return;
    int nw = t->minw, nh = t->minh;
    if (t->len) {
        int nl; TLine *L = tlayout(t->text, &nl); int maxw = 0;
        for (int i = 0; i < nl; i++) if (L[i].w > maxw) maxw = L[i].w;
        free(L);
        int need = maxw + 4, avail = A.img->w - t->x;
        if (need > nw) nw = need;
        if (nw > avail) nw = avail < 30 ? 30 : avail;
        t->w = nw;
        char *wr = text_wrapped(); TLine *L2 = tlayout(wr, &nl); int th = 0;
        for (int i = 0; i < nl; i++) th += L2[i].h;
        free(L2); free(wr);
        if (th + 4 > nh) nh = th + 4;
    }
    t->w = nw; t->h = nh;
}
static int tb_sel_range(int *a, int *b)
{
    TextBox *t = &A.tb;
    if (t->sel < 0 || t->sel == t->caret) return 0;
    *a = t->sel < t->caret ? t->sel : t->caret; *b = t->sel < t->caret ? t->caret : t->sel; return 1;
}
static void tb_sync_style(void)       /* typing continues in the style of the character before the caret */
{
    TextBox *t = &A.tb;
    if (!t->len) return;
    set_cur(t->st[t->caret > 0 ? t->caret - 1 : 0]);
}
static void tb_splice(int a, int b, const char *ins, int n)
{
    TextBox *t = &A.tb;
    int need = t->len - (b - a) + n;
    if (need + 1 > t->cap) { t->cap = (need + 1) * 2; t->text = (char *)realloc(t->text, (size_t)t->cap); t->st = (TStyle *)realloc(t->st, sizeof(TStyle) * (size_t)t->cap); }
    memmove(t->text + a + n, t->text + b, (size_t)(t->len - b));
    memmove(t->st + a + n, t->st + b, sizeof(TStyle) * (size_t)(t->len - b));
    memcpy(t->text + a, ins, (size_t)n);
    TStyle cs = cur_style();
    for (int i = 0; i < n; i++) t->st[a + i] = cs;
    t->len = need; t->text[t->len] = 0; t->caret = a + n; t->sel = -1;
}
static void tb_apply(int what, int v)         /* restyle the selection, or the whole text when nothing is selected */
{
    TextBox *t = &A.tb;
    if (!t->active) return;
    int a = 0, b = t->len;
    if (t->len) {
        if (!tb_sel_range(&a, &b)) { a = 0; b = t->len; }
        for (int i = a; i < b; i++) {
            if (what == 0) t->st[i].font = (uint8_t)v;
            else if (what == 1) t->st[i].size = (uint8_t)v;
            else if (v & 0x100) t->st[i].flags = (uint8_t)(t->st[i].flags | (v & 7));
            else t->st[i].flags = (uint8_t)(t->st[i].flags & ~(v & 7));
        }
    }
    text_autogrow();
}
void text_set_font(const char *name) { strncpy(A.tb.font, name, 63); A.tb.font[63] = 0; tb_apply(0, font_id(name)); }
void text_set_size(int pt) { A.tb.size = pt; tb_apply(1, pt); }
void text_toggle(int which)    /* 0 bold, 1 italic, 2 underline */
{
    int *p = which == 0 ? &A.tb.bold : which == 1 ? &A.tb.italic : &A.tb.underline;
    *p = !*p; tb_apply(2, (1 << which) | (*p ? 0x100 : 0));
}
void text_select_all(void) { if (A.tb.active && A.tb.len) { A.tb.sel = 0; A.tb.caret = A.tb.len; } }

/* where is a point relative to the text box? 0 outside, 1 text area, 2 border (move), 3+i resize handle i */
static int tb_hit(int ix, int iy)
{
    TextBox *t = &A.tb;
    if (!t->active) return 0;
    int bz = A.zp >= 100 ? 3 : 300 / A.zp, hs = bz + 2;
    int x0 = t->x, y0 = t->y, x1 = t->x + t->w, y1 = t->y + t->h, xm = (x0 + x1) / 2, ym = (y0 + y1) / 2;
    int hx[8] = { x0, xm, x1, x1, x1, xm, x0, x0 }, hy[8] = { y0, y0, y0, ym, y1, y1, y1, ym };
    for (int i = 0; i < 8; i++) if (abs(ix - hx[i]) <= hs && abs(iy - hy[i]) <= hs) return 3 + i;
    if (ix < x0 - bz || iy < y0 - bz || ix > x1 + bz || iy > y1 + bz) return 0;
    if (ix < x0 + bz || iy < y0 + bz || ix >= x1 - bz || iy >= y1 - bz) return 2;
    return 1;
}
static int tb_index_at(int ix, int iy)
{
    TextBox *t = &A.tb;
    if (!t->len) return 0;
    char *wr = text_wrapped(); int nl; TLine *L = tlayout(wr, &nl);
    int py = iy - t->y - 1, px = ix - t->x - 1, y = 0, li = 0;
    for (; li < nl - 1; li++) { if (py < y + L[li].h) break; y += L[li].h; }
    TLine *l = &L[li]; int res = l->le, prev = 0;
    for (int i = l->ls; i < l->le; ) {
        int e = i + 1; while (e < l->le && ((unsigned char)wr[e] & 0xc0) == 0x80) e++;
        int w = span_w(wr, l->ls, e);
        if (px < (prev + w) / 2) { res = i; break; }
        prev = w; i = e;
    }
    free(L); free(wr);
    return res;
}

static void text_insert(const char *ins)
{
    TextBox *t = &A.tb; int a, b;
    if (!tb_sel_range(&a, &b)) a = b = t->caret;
    tb_splice(a, b, ins, (int)strlen(ins));
}
static void tb_word_at(int i, int *a, int *b)
{
    TextBox *t = &A.tb; int n = t->len;
    if (i >= n) i = n - 1; if (i < 0) { *a = *b = 0; return; }
    int sp = (t->text[i] == ' ' || t->text[i] == '\n');
    int s0 = i, e0 = i + 1;
    while (s0 > 0 && ((t->text[s0 - 1] == ' ' || t->text[s0 - 1] == '\n') == sp)) s0--;
    while (e0 < n && ((t->text[e0] == ' ' || t->text[e0] == '\n') == sp)) e0++;
    *a = s0; *b = e0;
}

/* ---------------- tool handling ---------------- */
static px_t col_for(int btn) { return btn == BTN_RIGHT ? A.bg : A.fg; }
static void erase_at(int x, int y, int btn)
{
    int s = ERASER_SZ[A.o_eraser], x0 = x - s / 2, y0 = y - s / 2;
    for (int j = 0; j < s; j++) for (int i = 0; i < s; i++) {
        int px = x0 + i, py = y0 + j;
        if (btn == BTN_RIGHT) { if (img_get(A.img, px, py) == A.fg && px >= 0 && py >= 0 && px < A.img->w && py < A.img->h) img_put(A.img, px, py, A.bg); }
        else img_put(A.img, px, py, A.bg);
    }
}
static void erase_line(int x0, int y0, int x1, int y1, int btn)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        erase_at(x0, y0, btn);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void restore_base(void) { if (D.base) memcpy(A.img->p, D.base->p, (size_t)A.img->w * A.img->h * sizeof(px_t)); }
static Pt constrain_line(Pt a, Pt b)
{
    int dx = b.x - a.x, dy = b.y - a.y, adx = abs(dx), ady = abs(dy);
    if (adx > 2 * ady) b.y = a.y; else if (ady > 2 * adx) b.x = a.x;
    else { int m = adx > ady ? adx : ady; b.x = a.x + (dx < 0 ? -m : m); b.y = a.y + (dy < 0 ? -m : m); }
    return b;
}
static void shape_colors(int btn, px_t *o, px_t *f)
{
    *o = btn == BTN_RIGHT ? A.bg : A.fg;
    *f = btn == BTN_RIGHT ? A.fg : A.bg;
    if (A.o_fill == 2) *f = *o;
}
static void draw_drag_shape(int mods)
{
    Pt a = D.start, b = D.last;
    px_t o, f; shape_colors(D.btn, &o, &f);
    int lw = (A.tool == T_LINE) ? A.o_line + 1 : shape_lw();
    restore_base();
    if (D.tool == T_LINE) {
        if (mods & MOD_SHIFT) b = constrain_line(a, b);
        cv_line(A.img, a.x, a.y, b.x, b.y, col_for(D.btn), lw);
    } else {
        if (mods & MOD_SHIFT) { int dx = b.x - a.x, dy = b.y - a.y, m = abs(dx) > abs(dy) ? abs(dx) : abs(dy); b.x = a.x + (dx < 0 ? -m : m); b.y = a.y + (dy < 0 ? -m : m); }
        int k = D.tool == T_RECT ? SH_RECT : D.tool == T_ELLIPSE ? SH_ELLIPSE : SH_RRECT;
        cv_shape(A.img, k, a.x, a.y, b.x, b.y, A.o_fill, o, f, lw);
    }
}
static void curve_draw(int upto)
{
    restore_base();
    px_t c = col_for(CV.btn); int lw = A.o_line + 1;
    if (upto == 1) cv_line(A.img, CV.p[0].x, CV.p[0].y, CV.p[3].x, CV.p[3].y, c, lw);
    else cv_curve(A.img, CV.p[0], CV.p[1], CV.p[2], CV.p[3], c, lw);
}
static void poly_draw(Pt cur, int closed)
{
    restore_base();
    px_t o, f; shape_colors(PG.btn, &o, &f);
    Pt pts[513]; int n = PG.n;
    memcpy(pts, PG.p, sizeof(Pt) * n);
    if (!closed) pts[n++] = cur;
    cv_poly(A.img, pts, n, closed, A.o_fill, o, f, shape_lw());
}
static void poly_finish(int closed)
{
    if (!PG.active) return;
    if (closed && PG.n >= 2) { Pt last = PG.p[PG.n - 1]; poly_draw(last, 1); }
    else if (PG.n >= 1) poly_draw(PG.p[PG.n - 1], 0);
    PG.active = 0; PG.n = 0; D.base = NULL;
}
void tool_cancel(void)
{
    if (CV.phase) { restore_base(); undo_drop_last(); CV.phase = 0; D.base = NULL; }
    if (PG.active) { restore_base(); undo_drop_last(); PG.active = 0; PG.n = 0; D.base = NULL; }
    if (D.active) { if (D.base && D.tool >= T_LINE) { restore_base(); undo_drop_last(); } D.active = 0; }
    D.base = NULL;
}
int tool_busy(void) { return CV.phase || PG.active || A.tb.active; }
void tool_dblclick(void) { if (PG.active) poly_finish(1); }
int tool_drag_active(void) { return D.active; }

void tool_select(int t)
{
    if (t == A.tool) return;
    /* finish pending multi-step tools */
    if (CV.phase) { curve_draw(2); CV.phase = 0; D.base = NULL; }
    if (PG.active) poly_finish(0);
    if (A.tb.active) text_commit();
    if (t != T_SELECT && t != T_FREESEL) sel_commit();
    else if (A.sel.active && A.sel.mask && t == T_SELECT) sel_commit();
    if (t != T_PICK && t != T_MAG) A.prevtool = t;
    if (A.tool != T_PICK && A.tool != T_MAG) A.prevtool = A.tool;
    A.tool = t;
}

static void clamp_pt(int *x, int *y) { if (*x < 0) *x = 0; if (*y < 0) *y = 0; if (*x >= A.img->w) *x = A.img->w - 1; if (*y >= A.img->h) *y = A.img->h - 1; }

void tool_press(int ix, int iy, int btn, int mods)
{
    if (btn != BTN_LEFT && btn != BTN_RIGHT) return;
    if (D.active) { /* second button while dragging: cancel */
        if (D.tool >= T_LINE && D.tool != T_CURVE && D.tool != T_POLY) { restore_base(); undo_drop_last(); D.active = 0; D.base = NULL; }
        else if (D.tool == T_PENCIL || D.tool == T_BRUSH || D.tool == T_ERASER || D.tool == T_AIR) { D.active = 0; }
        return;
    }
    D.btn = btn; D.tool = A.tool; D.mods = mods; D.start = D.last = (Pt){ ix, iy };
    D.mode = 0;
    px_t c = col_for(btn);
    switch (A.tool) {
    case T_PENCIL:
        undo_push(); D.active = 1; cv_dot(A.img, ix, iy, c, 1); break;
    case T_BRUSH:
        undo_push(); D.active = 1; cv_brush(A.img, ix, iy, BRUSH_SHAPE[A.o_brush], BRUSH_SIZE[A.o_brush], c); break;
    case T_AIR:
        undo_push(); D.active = 1; cv_air(A.img, ix, iy, AIR_R[A.o_air], c); break;
    case T_ERASER:
        undo_push(); D.active = 1; erase_at(ix, iy, btn); break;
    case T_FILL:
        undo_push(); cv_flood(A.img, ix, iy, c); break;
    case T_PICK: {
        if (ix >= 0 && iy >= 0 && ix < A.img->w && iy < A.img->h) { px_t p = A.img->p[iy * A.img->w + ix]; if (btn == BTN_LEFT) A.fg = p; else A.bg = p; }
        D.active = 1; break; }
    case T_MAG:
        if (A.zp != 100) set_zoom(100, ix, iy); else set_zoom(MAG_Z[A.o_mag] * 100, ix, iy);
        break;
    case T_LINE: case T_RECT: case T_ELLIPSE: case T_RRECT:
        undo_push(); D.base = A.undo[A.nundo - 1]; D.active = 1; draw_drag_shape(mods); break;
    case T_CURVE:
        if (CV.phase == 0) {
            undo_push(); D.base = A.undo[A.nundo - 1]; CV.btn = btn; CV.p[0] = CV.p[3] = (Pt){ ix, iy }; CV.p[1] = CV.p[0]; CV.p[2] = CV.p[0];
            CV.phase = 1; D.active = 1; curve_draw(1);
        } else { D.active = 1; }
        break;
    case T_POLY:
        if (!PG.active) { undo_push(); D.base = A.undo[A.nundo - 1]; PG.active = 1; PG.n = 0; PG.btn = btn; PG.p[PG.n++] = (Pt){ ix, iy }; D.active = 1; poly_draw((Pt){ ix, iy }, 0); }
        else {
            if (PG.n >= 2) { Pt f = PG.p[0]; if (abs(ix - f.x) <= 3 && abs(iy - f.y) <= 3) { poly_finish(1); break; } }
            if (PG.n < 510) PG.p[PG.n++] = (Pt){ ix, iy };
            D.active = 1; poly_draw((Pt){ ix, iy }, 0);
        }
        break;
    case T_TEXT: {
        TextBox *t = &A.tb;
        if (t->active) {
            int h = tb_hit(ix, iy);
            if (h == 2) { D.active = 1; D.mode = 10; D.off = (Pt){ ix - t->x, iy - t->y }; break; }
            if (h >= 3) { D.active = 1; D.mode = 11; D.handle = h - 3; D.ox = t->x; D.oy = t->y; D.ow = t->w; D.oh = t->h; break; }
            if (h == 1) {
                static unsigned last_t; static int last_x, last_y;
                unsigned now = plat_ticks_ms(); int idx = tb_index_at(ix, iy);
                if (now - last_t < 450 && abs(ix - last_x) < 4 && abs(iy - last_y) < 4 && t->len) {
                    int a, b; tb_word_at(idx, &a, &b); t->sel = a; t->caret = b; last_t = 0; D.active = 1; D.mode = 13; break;
                }
                last_t = now; last_x = ix; last_y = iy;
                t->caret = idx; t->sel = idx; tb_sync_style(); D.active = 1; D.mode = 12; break;
            }
            text_commit();
        }
        if (!t->font[0]) { strcpy(t->font, "Arial"); t->size = 10; }
        D.active = 1; D.mode = 3; break;
    }
    case T_SELECT: case T_FREESEL: {
        int h = sel_hit(ix, iy);
        if (h == 1) {
            D.mode = 1; D.off = (Pt){ ix - A.sel.x, iy - A.sel.y }; D.active = 1;
            if (!A.sel.lifted) { sel_lift(mods & MOD_CTRL); }
            A.sel.copy_mode = (mods & MOD_CTRL) != 0;
        } else if (h >= 2) {
            sel_lift(0);
            D.mode = 2; D.handle = h - 2; D.active = 1; D.ox = A.sel.x; D.oy = A.sel.y; D.ow = A.sel.w; D.oh = A.sel.h;
            D.orig = img_clone(A.sel.pix);
        } else {
            sel_commit();
            D.mode = 3; D.active = 1;
            if (A.tool == T_FREESEL) { A.sel.free_cnt = 0; A.sel.free_cap = 256; A.sel.free_pts = (Pt *)malloc(sizeof(Pt) * 256); A.sel.free_pts[A.sel.free_cnt++] = (Pt){ ix, iy }; A.sel.active = 0; }
        }
        break;
    }
    }
}

void tool_move(int ix, int iy, int mods)
{
    if (!D.active) return;
    Pt prev = D.last;
    D.last = (Pt){ ix, iy };
    D.mods = mods;
    px_t c = col_for(D.btn);
    switch (D.tool) {
    case T_PENCIL: cv_line(A.img, prev.x, prev.y, ix, iy, c, 1); break;
    case T_BRUSH: cv_brush_line(A.img, prev.x, prev.y, ix, iy, BRUSH_SHAPE[A.o_brush], BRUSH_SIZE[A.o_brush], c); break;
    case T_AIR: cv_air(A.img, ix, iy, AIR_R[A.o_air], c); break;
    case T_ERASER: erase_line(prev.x, prev.y, ix, iy, D.btn); break;
    case T_PICK:
        if (ix >= 0 && iy >= 0 && ix < A.img->w && iy < A.img->h) { px_t p = A.img->p[iy * A.img->w + ix]; if (D.btn == BTN_LEFT) A.fg = p; else A.bg = p; }
        break;
    case T_LINE: case T_RECT: case T_ELLIPSE: case T_RRECT: draw_drag_shape(mods); break;
    case T_CURVE:
        if (CV.phase == 1) { CV.p[3] = (Pt){ ix, iy }; if (mods & MOD_SHIFT) CV.p[3] = constrain_line(CV.p[0], CV.p[3]); CV.p[1] = CV.p[0]; CV.p[2] = CV.p[3]; curve_draw(1); }
        else if (CV.phase == 2) { CV.p[1] = CV.p[2] = (Pt){ ix, iy }; curve_draw(2); }
        else if (CV.phase == 3) { CV.p[2] = (Pt){ ix, iy }; curve_draw(2); }
        break;
    case T_POLY: if (PG.active) poly_draw((Pt){ ix, iy }, 0); break;
    case T_TEXT: {
        TextBox *t = &A.tb;
        if (D.mode == 10) { t->x = ix - D.off.x; t->y = iy - D.off.y; }
        else if (D.mode == 11) {
            int x0 = D.ox, y0 = D.oy, x1 = D.ox + D.ow, y1 = D.oy + D.oh, h = D.handle;
            if (h == 0 || h == 6 || h == 7) x0 = ix; if (h == 2 || h == 3 || h == 4) x1 = ix;
            if (h == 0 || h == 1 || h == 2) y0 = iy; if (h == 4 || h == 5 || h == 6) y1 = iy;
            if (x1 < x0 + 8) { if (h == 0 || h == 6 || h == 7) x0 = x1 - 8; else x1 = x0 + 8; }
            if (y1 < y0 + 8) { if (h == 0 || h == 1 || h == 2) y0 = y1 - 8; else y1 = y0 + 8; }
            t->x = x0; t->y = y0; t->w = x1 - x0; t->h = y1 - y0; t->minw = t->w; t->minh = t->h;
            text_autogrow();
        } else if (D.mode == 12) { t->caret = tb_index_at(ix, iy); }
        break;
    }
    case T_SELECT: case T_FREESEL:
        if (D.mode == 1) {
            if (D.mods & MOD_SHIFT) { sel_blit(A.img, &A.sel, A.o_trans); }
            A.sel.x = ix - D.off.x; A.sel.y = iy - D.off.y;
        } else if (D.mode == 2) {
            int x0 = D.ox, y0 = D.oy, x1 = D.ox + D.ow, y1 = D.oy + D.oh, h = D.handle;
            if (h == 0 || h == 6 || h == 7) x0 = ix; if (h == 2 || h == 3 || h == 4) x1 = ix;
            if (h == 0 || h == 1 || h == 2) y0 = iy; if (h == 4 || h == 5 || h == 6) y1 = iy;
            if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1;
            Img *ns = cv_stretch(D.orig, x1 - x0, y1 - y0);
            img_free(A.sel.pix); A.sel.pix = ns; A.sel.x = x0; A.sel.y = y0; A.sel.w = x1 - x0; A.sel.h = y1 - y0;
        } else if (D.mode == 3 && D.tool == T_FREESEL && A.sel.free_pts) {
            if (A.sel.free_cnt == A.sel.free_cap) { A.sel.free_cap *= 2; A.sel.free_pts = (Pt *)realloc(A.sel.free_pts, sizeof(Pt) * A.sel.free_cap); }
            A.sel.free_pts[A.sel.free_cnt++] = (Pt){ ix, iy };
        }
        break;
    }
}

void tool_tick(void)
{
    if (D.active && D.tool == T_AIR) cv_air(A.img, D.last.x, D.last.y, AIR_R[A.o_air], col_for(D.btn));
}

void tool_release(int ix, int iy, int mods)
{
    if (!D.active) return;
    tool_move(ix, iy, mods);
    int tool = D.tool;
    switch (tool) {
    case T_PICK: D.active = 0; A.tool = A.prevtool; break;
    case T_CURVE:
        D.active = 0;
        if (CV.phase == 1) {
            if (CV.p[0].x == CV.p[3].x && CV.p[0].y == CV.p[3].y) { curve_draw(2); CV.phase = 0; D.base = NULL; } /* just a dot */
            else CV.phase = 2;
        } else if (CV.phase == 2) CV.phase = 3;
        else if (CV.phase == 3) { curve_draw(2); CV.phase = 0; D.base = NULL; }
        break;
    case T_POLY: D.active = 0; break;
    case T_LINE: case T_RECT: case T_ELLIPSE: case T_RRECT: D.active = 0; D.base = NULL; break;
    case T_TEXT: {
        D.active = 0;
        if (D.mode >= 10) { if (D.mode == 12 && A.tb.sel == A.tb.caret) A.tb.sel = -1; tb_sync_style(); break; }
        int x0 = D.start.x < ix ? D.start.x : ix, y0 = D.start.y < iy ? D.start.y : iy, x1 = D.start.x < ix ? ix : D.start.x, y1 = D.start.y < iy ? iy : D.start.y;
        TextBox *t = &A.tb; t->active = 1;
        t->x = x0; t->y = y0; t->w = x1 - x0 + 1; t->h = y1 - y0 + 1;
        int minh = text_px() + 6; if (t->h < minh) t->h = minh; if (t->w < 30) t->w = 30;
        t->minw = t->w; t->minh = t->h;
        t->len = 0; t->caret = 0; t->sel = -1; free(t->text); free(t->st); t->text = NULL; t->st = NULL; t->cap = 0;
        break;
    }
    case T_SELECT: case T_FREESEL:
        D.active = 0;
        if (D.mode == 3) {
            if (tool == T_SELECT) {
                int x0 = D.start.x, y0 = D.start.y, x1 = ix, y1 = iy;
                clamp_pt(&x0, &y0); clamp_pt(&x1, &y1);
                if (x1 < x0) { int t = x0; x0 = x1; x1 = t; } if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
                if (D.start.x == ix && D.start.y == iy) break;
                sel_make(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
            } else if (A.sel.free_pts && A.sel.free_cnt >= 3) {
                Pt *p = A.sel.free_pts; int n = A.sel.free_cnt;
                int minx = p[0].x, maxx = p[0].x, miny = p[0].y, maxy = p[0].y;
                for (int i = 1; i < n; i++) { if (p[i].x < minx) minx = p[i].x; if (p[i].x > maxx) maxx = p[i].x; if (p[i].y < miny) miny = p[i].y; if (p[i].y > maxy) maxy = p[i].y; }
                if (minx < 0) minx = 0; if (miny < 0) miny = 0; if (maxx >= A.img->w) maxx = A.img->w - 1; if (maxy >= A.img->h) maxy = A.img->h - 1;
                int w = maxx - minx + 1, h = maxy - miny + 1;
                if (w > 0 && h > 0) {
                    uint8_t *m = (uint8_t *)calloc((size_t)w * h, 1);
                    cv_fillpoly_mask(m, w, h, p, n, minx, miny);
                    free(A.sel.free_pts); A.sel.free_pts = NULL;
                    sel_make(minx, miny, w, h); A.sel.mask = m;
                }
            }
            free(A.sel.free_pts); A.sel.free_pts = NULL; A.sel.free_cnt = 0;
        } else if (D.mode == 2) { img_free(D.orig); D.orig = NULL; }
        break;
    default: D.active = 0; break;
    }
    D.mode = 0;
}

static int tool_key_impl(const PEvent *ev)
{
    TextBox *t = &A.tb;
    if (PG.active && ev->type == EV_KEYDOWN && ev->key == KEY_ESC) { poly_finish(0); return 1; }
    if (PG.active && ev->type == EV_KEYDOWN && ev->key == KEY_ENTER) { poly_finish(1); return 1; }
    if (CV.phase && ev->type == EV_KEYDOWN && ev->key == KEY_ESC) { tool_cancel(); return 1; }
    if (D.active && ev->type == EV_KEYDOWN && ev->key == KEY_ESC && D.tool != T_CURVE && D.tool != T_POLY) { tool_cancel(); return 1; }
    if (!t->active) return 0;
    if (ev->type == EV_CHAR) {
        if (ev->ch < 32 || ev->ch == 127) return 0;
        char b[8]; uint32_t c = ev->ch;
        if (c < 0x80) { b[0] = (char)c; b[1] = 0; }
        else if (c < 0x800) { b[0] = (char)(0xc0 | (c >> 6)); b[1] = (char)(0x80 | (c & 63)); b[2] = 0; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 63)); b[2] = (char)(0x80 | (c & 63)); b[3] = 0; }
        else { b[0] = (char)(0xf0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 63)); b[2] = (char)(0x80 | ((c >> 6) & 63)); b[3] = (char)(0x80 | (c & 63)); b[4] = 0; }
        text_insert(b); return 1;
    }
    if (ev->type != EV_KEYDOWN) return 0;
    if (ev->mods & MOD_CTRL) return 0;
    int shift = (ev->mods & MOD_SHIFT) != 0, ra, rb;
    int old = t->caret;
    #define PREV(p) do { (p)--; while ((p) > 0 && ((unsigned char)t->text[p] & 0xc0) == 0x80) (p)--; } while (0)
    #define NEXT(p) do { (p)++; while ((p) < t->len && ((unsigned char)t->text[p] & 0xc0) == 0x80) (p)++; } while (0)
    switch (ev->key) {
    case KEY_ESC: text_commit(); return 1;
    case KEY_ENTER: text_insert("\n"); return 1;
    case KEY_BACKSPACE:
        if (tb_sel_range(&ra, &rb)) { tb_splice(ra, rb, "", 0); tb_sync_style(); return 1; }
        if (t->caret > 0) { int p = t->caret; PREV(p); tb_splice(p, t->caret, "", 0); tb_sync_style(); }
        return 1;
    case KEY_DELETE:
        if (tb_sel_range(&ra, &rb)) { tb_splice(ra, rb, "", 0); tb_sync_style(); return 1; }
        if (t->caret < t->len) { int e = t->caret; NEXT(e); tb_splice(t->caret, e, "", 0); tb_sync_style(); }
        return 1;
    case KEY_LEFT: case KEY_RIGHT: case KEY_HOME: case KEY_END: case KEY_UP: case KEY_DOWN: {
        int c = t->caret;
        if (!shift && tb_sel_range(&ra, &rb) && (ev->key == KEY_LEFT || ev->key == KEY_RIGHT)) { c = ev->key == KEY_LEFT ? ra : rb; t->sel = -1; t->caret = c; tb_sync_style(); return 1; }
        if (ev->key == KEY_LEFT) { if (c > 0) PREV(c); }
        else if (ev->key == KEY_RIGHT) { if (c < t->len) NEXT(c); }
        else if (ev->key == KEY_HOME) { while (c > 0 && t->text[c - 1] != '\n') c--; }
        else if (ev->key == KEY_END) { while (c < t->len && t->text[c] != '\n') c++; }
        else {
            int ls = c; while (ls > 0 && t->text[ls - 1] != '\n') ls--;
            int col = c - ls;
            if (ev->key == KEY_UP) { if (ls > 0) { int ps = ls - 1; while (ps > 0 && t->text[ps - 1] != '\n') ps--; int pl = ls - 1 - ps; c = ps + (col < pl ? col : pl); } }
            else { int le = c; while (le < t->len && t->text[le] != '\n') le++; if (le < t->len) { int ns = le + 1, ne = ns; while (ne < t->len && t->text[ne] != '\n') ne++; c = ns + (col < ne - ns ? col : ne - ns); } }
        }
        if (shift) { if (t->sel < 0) t->sel = old; } else t->sel = -1;
        t->caret = c; tb_sync_style();
        return 1;
    }
    }
    return 0;
}

int tool_key(const PEvent *ev)
{
    int r = tool_key_impl(ev);
    if (A.tb.active) text_autogrow();
    return r;
}

/* ---------------- composition & overlays ---------------- */
Img *build_comp(int *owned)
{
    int need = (A.sel.active && A.sel.lifted) || A.tb.active;
    if (!need) { *owned = 0; return A.img; }
    Img *c = img_clone(A.img);
    if (A.sel.active && A.sel.lifted) sel_blit(c, &A.sel, A.o_trans);
    if (A.tb.active) text_blit(c);
    *owned = 1;
    return c;
}
static void dotted_screen_rect(Img *fb, int x, int y, int w, int h)
{
    gx_dotrect(fb, x, y, w, h, C_BLACK, C_WHITE, 0);
}
static void handle_box(Img *fb, int cx, int cy) { gx_rect(fb, cx - 1, cy - 1, 3, 3, C_NAVY); }
void draw_overlays(Img *fb)
{
    int ox = view_ox(), oy = view_oy();
    extern Rc R_view;
    Rc *v = &R_view;
    (void)v;
    if (A.sel.active) {
        int x = ox + z_i2s(A.sel.x), y = oy + z_i2s(A.sel.y), w = z_i2s(A.sel.w), h = z_i2s(A.sel.h);
        dotted_screen_rect(fb, x - 1, y - 1, w + 2, h + 2);
        if (!A.sel.mask) {
            int xm = x + w / 2, ym = y + h / 2;
            handle_box(fb, x - 1, y - 1); handle_box(fb, xm, y - 1); handle_box(fb, x + w, y - 1); handle_box(fb, x + w, ym);
            handle_box(fb, x + w, y + h); handle_box(fb, xm, y + h); handle_box(fb, x - 1, y + h); handle_box(fb, x - 1, ym);
        }
    }
    if (A.sel.free_pts && A.sel.free_cnt > 1) {
        for (int i = 0; i + 1 < A.sel.free_cnt; i++) {
            Pt a = A.sel.free_pts[i], b = A.sel.free_pts[i + 1];
            int hz = z_i2s(1) / 2;
            int x0 = ox + z_i2s(a.x) + hz, y0 = oy + z_i2s(a.y) + hz, x1 = ox + z_i2s(b.x) + hz, y1 = oy + z_i2s(b.y) + hz;
            int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy, k = 0;
            for (;;) { img_put(fb, x0, y0, (k++ & 2) ? C_WHITE : C_BLACK); if (x0 == x1 && y0 == y1) break; int e2 = 2 * err; if (e2 >= dy) { err += dy; x0 += sx; } if (e2 <= dx) { err += dx; y0 += sy; } }
        }
    }
    if (D.active && D.tool == T_SELECT && D.mode == 3) {
        int x0 = D.start.x, y0 = D.start.y, x1 = D.last.x, y1 = D.last.y;
        clamp_pt(&x0, &y0); clamp_pt(&x1, &y1);
        if (x1 < x0) { int t = x0; x0 = x1; x1 = t; } if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
        dotted_screen_rect(fb, ox + z_i2s(x0) - 1, oy + z_i2s(y0) - 1, z_i2s(x1 - x0 + 1) + 2, z_i2s(y1 - y0 + 1) + 2);
    }
    if (D.active && D.tool == T_TEXT && D.mode == 3) {
        int x0 = D.start.x < D.last.x ? D.start.x : D.last.x, y0 = D.start.y < D.last.y ? D.start.y : D.last.y;
        int x1 = D.start.x < D.last.x ? D.last.x : D.start.x, y1 = D.start.y < D.last.y ? D.last.y : D.start.y;
        dotted_screen_rect(fb, ox + z_i2s(x0) - 1, oy + z_i2s(y0) - 1, z_i2s(x1 - x0 + 1) + 2, z_i2s(y1 - y0 + 1) + 2);
    }
    if (A.tb.active) {
        TextBox *t = &A.tb;
        int x = ox + z_i2s(t->x), y = oy + z_i2s(t->y), w = z_i2s(t->w), h = z_i2s(t->h);
        dotted_screen_rect(fb, x - 1, y - 1, w + 2, h + 2);
        handle_box(fb, x - 1, y - 1); handle_box(fb, x + w / 2, y - 1); handle_box(fb, x + w, y - 1); handle_box(fb, x + w, y + h / 2);
        handle_box(fb, x + w, y + h); handle_box(fb, x + w / 2, y + h); handle_box(fb, x - 1, y + h); handle_box(fb, x - 1, y + h / 2);
        {
            char *wr = text_wrapped(); int nl; TLine *L = tlayout(wr, &nl);
            int ra, rb, has = tb_sel_range(&ra, &rb), yy = 0;
            for (int li = 0; li < nl; li++) {
                TLine *l = &L[li];
                if (has && rb > l->ls && ra <= l->le) {
                    int sa = ra > l->ls ? ra : l->ls, sb = rb < l->le ? rb : l->le;
                    int x0 = span_w(wr, l->ls, sa), x1 = span_w(wr, l->ls, sb);
                    if (sb == l->le && rb > l->le) x1 += 4;            /* selected newline */
                    if (x1 == x0) x1 = x0 + 3;
                    for (int j = z_i2s(yy); j < z_i2s(yy + l->h); j++) for (int i = z_i2s(1 + x0); i < z_i2s(1 + x1); i++) {
                        int px = x + i, py = y + j; if (px < 0 || py < 0 || px >= fb->w || py >= fb->h) continue;
                        px_t *p = &fb->p[py * fb->w + px]; *p = (*p ^ 0xffffff) & 0xffffff;
                    }
                }
                if (!has && (plat_ticks_ms() / 500) % 2 == 0 && t->caret >= l->ls && t->caret <= l->le && (li == nl - 1 || t->caret < l->le + 1)) {
                    int cw = span_w(wr, l->ls, t->caret);
                    gx_vline(fb, x + z_i2s(1 + cw), y + z_i2s(yy), y + z_i2s(yy + l->h) - 1, C_BLACK);
                }
                yy += l->h;
            }
            free(L); free(wr);
        }
    }
    if (g_min && !D.active) {
        if (A.tool == T_ERASER) {
            int s = ERASER_SZ[A.o_eraser];
            gx_frame(fb, ox + z_i2s(g_mx - s / 2), oy + z_i2s(g_my - s / 2), z_i2s(s), z_i2s(s), C_BLACK);
        } else if (A.tool == T_MAG && A.zp == 100) {
            extern Rc R_view;
            int zz = MAG_Z[A.o_mag]; if (zz < 2) zz = 2;
            int w = R_view.w / zz, h = R_view.h / zz;
            int x = g_mx - w / 2, y = g_my - h / 2;
            if (x < 0) x = 0; if (y < 0) y = 0; if (x + w > A.img->w) x = A.img->w - w; if (y + h > A.img->h) y = A.img->h - h;
            gx_dotrect(fb, ox + x, oy + y, w, h, C_BLACK, C_WHITE, 0);
        }
    }
}

void tool_status_text(char *out, int n)
{
    out[0] = 0;
    if (D.active && (D.tool == T_LINE || D.tool == T_RECT || D.tool == T_ELLIPSE || D.tool == T_RRECT || (D.tool == T_SELECT && D.mode == 3) || D.tool == T_TEXT))
        snprintf(out, n, "%dx%d", abs(D.last.x - D.start.x) + 1, abs(D.last.y - D.start.y) + 1);
    else if (A.sel.active) snprintf(out, n, "%dx%d", A.sel.w, A.sel.h);
}

/* ---------------- view / zoom ---------------- */
void set_zoom(int z, int cx, int cy) /* z in percent */
{
    extern Rc R_view;
    if (z < ZOOM_MIN) z = ZOOM_MIN;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    A.zp = z;
    ui_relayout();
    view_center_on(cx, cy);
    view_clamp();
}

/* ---------------- document ---------------- */
void app_new_image(int w, int h)
{
    tool_cancel();
    sel_cancel();
    if (A.tb.active) { free(A.tb.text); free(A.tb.st); A.tb.text = NULL; A.tb.st = NULL; A.tb.active = 0; A.tb.len = A.tb.cap = A.tb.caret = 0; A.tb.sel = -1; }
    img_free(A.img);
    A.img = img_new(w, h, RGB(255, 255, 255));
    for (int i = 0; i < A.nundo; i++) img_free(A.undo[i]);
    for (int i = 0; i < A.nredo; i++) img_free(A.redo[i]);
    A.nundo = A.nredo = 0; A.path[0] = 0; A.dirty = 0; A.sx = A.sy = 0; A.bw = 0;
    ui_relayout(); update_title();
}
void set_canvas_size(int w, int h)
{
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (w > 10000) w = 10000; if (h > 10000) h = 10000;
    if (w == A.img->w && h == A.img->h) return;
    sel_commit();
    undo_push();
    Img *n = cv_crop_resize(A.img, w, h, A.bg);
    img_free(A.img); A.img = n;
    ui_relayout(); view_clamp();
}
void app_init(void)
{
    static const px_t pal[28] = {
        RGB(0,0,0), RGB(128,128,128), RGB(128,0,0), RGB(128,128,0), RGB(0,128,0), RGB(0,128,128), RGB(0,0,128), RGB(128,0,128),
        RGB(128,128,64), RGB(0,64,64), RGB(0,128,255), RGB(0,64,128), RGB(64,0,255), RGB(128,64,0),
        RGB(255,255,255), RGB(192,192,192), RGB(255,0,0), RGB(255,255,0), RGB(0,255,0), RGB(0,255,255), RGB(0,0,255), RGB(255,0,255),
        RGB(255,255,128), RGB(0,255,128), RGB(128,255,255), RGB(128,128,255), RGB(255,0,128), RGB(255,128,64) };
    /* XP palette order: row 1 = first 14, row 2 = next 14 */
    memcpy(A.pal, pal, sizeof pal);
    for (int i = 0; i < 16; i++) A.custom[i] = C_WHITE;
    A.fg = RGB(0, 0, 0); A.bg = RGB(255, 255, 255);
    A.tool = T_PENCIL; A.prevtool = T_PENCIL;
    A.zp = 85; A.show_toolbox = A.show_colorbox = A.show_status = 1; A.show_text = 1;
    A.o_eraser = 2; A.o_brush = 5; A.o_air = 1; A.o_line = 0; A.o_shapew = 0; A.o_fill = 0; A.o_mag = 1; A.o_trans = 0;
    A.grid_w = A.grid_h = 8; A.dpi = 96;
    strcpy(A.tb.font, "Arial"); A.tb.size = 10; A.tb.sel = -1;
    A.img = img_new(DEF_W, DEF_H, RGB(255, 255, 255));
}
int app_confirm_discard(void)
{
    if (A.tb.active) text_commit();
    if (!A.dirty) return 1;
    char t[1100]; const char *nm = A.path[0] ? strrchr(A.path, '/') ? strrchr(A.path, '/') + 1 : A.path : "untitled";
    snprintf(t, sizeof t, "Save changes to %s?", nm);
    int r = msgbox("SmPaint", t, MB_YESNOCANCEL, 0);
    if (r == ID_CANCEL) return 0;
    if (r == ID_YES) { cmd_exec(CMD_SAVE); if (A.dirty) return 0; }
    return 1;
}
int app_quit_request(void) { return app_confirm_discard(); }

static void add_recent(const char *p)
{
    int at = A.nrecent;
    for (int i = 0; i < A.nrecent; i++) if (!strcmp(A.recent[i], p)) { at = i; break; }
    if (at == A.nrecent && A.nrecent < 4) A.nrecent++;
    else if (at == A.nrecent) at = 3;
    for (int i = at; i > 0; i--) strcpy(A.recent[i], A.recent[i - 1]);
    strncpy(A.recent[0], p, 1023);
}
static Img *load_image_file(const char *path)
{
    size_t n; uint8_t *d = file_read_all(path, &n);
    if (!d) { msgbox("SmPaint", "SmPaint cannot open this file.\nFile not found.", MB_OK, 0); return NULL; }
    int w, h; uint32_t *px = any_decode(d, n, &w, &h);
    free(d);
    if (!px) { msgbox("SmPaint", "SmPaint cannot open this file.\nThis is not a valid bitmap file, or its format is not currently supported.", MB_OK, 0); return NULL; }
    Img *im = (Img *)malloc(sizeof(Img)); im->w = w; im->h = h; im->p = px;
    return im;
}
/* show a just-opened image at its natural size (100%) and grow the window to fit it (maximised if it is larger than the screen) */
void zoom_fit_image(void)
{
    extern Rc R_view, R_vscroll, R_hscroll;
    if (!A.img || !A.fb) return;
    A.zp = 100; A.sx = A.sy = 0;
    ui_relayout();
    int vw = R_view.w + (R_vscroll.w ? R_vscroll.w : 0), vh = R_view.h + (R_hscroll.h ? R_hscroll.h : 0);
    int need_w = A.fb->w + (A.img->w + 11 - vw), need_h = A.fb->h + (A.img->h + 11 - vh);
    plat_fit_window(need_w, need_h);
}
void app_load_file(const char *path)
{
    Img *im = load_image_file(path);
    if (!im) return;
    tool_cancel(); sel_cancel();
    if (A.tb.active) { free(A.tb.text); free(A.tb.st); A.tb.text = NULL; A.tb.st = NULL; A.tb.active = 0; A.tb.len = A.tb.cap = A.tb.caret = 0; A.tb.sel = -1; }
    img_free(A.img); A.img = im;
    for (int i = 0; i < A.nundo; i++) img_free(A.undo[i]);
    for (int i = 0; i < A.nredo; i++) img_free(A.redo[i]);
    A.nundo = A.nredo = 0; strncpy(A.path, path, 1023); A.dirty = 0; A.sx = A.sy = 0; A.bw = 0;
    add_recent(path);
    ui_relayout(); zoom_fit_image(); update_title();
}
static int save_image(const char *path, const Img *im, int filter)
{
    const char *dot = strrchr(path, '.'); char ext[8] = ""; if (dot) { strncpy(ext, dot + 1, 7); for (char *p = ext; *p; p++) *p = (char)tolower((unsigned char)*p); }
    uint8_t *b = NULL; size_t n = 0;
    if (!strcmp(ext, "png")) b = png_encode(im->p, im->w, im->h, &n);
    else if (!strcmp(ext, "pdf")) b = pdf_encode(im->p, im->w, im->h, &n);
    else if (!strcmp(ext, "gif")) b = gif_encode(im->p, im->w, im->h, &n);
    else if (!strcmp(ext, "jpg") || !strcmp(ext, "jpeg") || !strcmp(ext, "jpe") || !strcmp(ext, "jfif")) b = jpg_encode(im->p, im->w, im->h, 92, &n);
    else {
        int bpp = filter == 0 ? 1 : filter == 1 ? 4 : filter == 2 ? 8 : 24;
        if (filter < 0) bpp = A.bw ? 1 : 24;
        b = bmp_encode(im->p, im->w, im->h, bpp, 1, &n);
    }
    if (!b) { msgbox("SmPaint", "SmPaint cannot save this file.", MB_OK, 0); return 0; }
    int rc = file_write_all(path, b, n); free(b);
    if (rc) { msgbox("SmPaint", "Cannot save file.\nAccess denied.", MB_OK, 0); return 0; }
    return 1;
}

/* ---------------- clipboard ---------------- */
static Img *sel_content(void)
{
    Sel *s = &A.sel; if (!s->active) return NULL;
    Img *c = img_new(s->w, s->h, A.bg);
    for (int j = 0; j < s->h; j++) for (int i = 0; i < s->w; i++) {
        if (s->mask && !s->mask[j * s->w + i]) continue;
        c->p[j * s->w + i] = s->lifted ? s->pix->p[j * s->w + i] : img_get(A.img, s->x + i, s->y + j);
    }
    return c;
}
static void do_copy(void)
{
    Img *c = sel_content(); if (!c) return;
    img_free(A.clip); A.clip = c;
    plat_clip_set_image(c->p, c->w, c->h);
}
static void sel_delete(void)
{
    Sel *s = &A.sel; if (!s->active) return;
    if (!s->lifted) {
        undo_push();
        for (int j = 0; j < s->h; j++) for (int i = 0; i < s->w; i++) if (!s->mask || s->mask[j * s->w + i]) img_put(A.img, s->x + i, s->y + j, A.bg);
    }
    sel_cancel();
}
static int vis_ix(void) { extern Rc R_view; int v = z_s2i(R_view.x + 3 - view_ox()); return v < 0 ? 0 : v; }
static int vis_iy(void) { extern Rc R_view; int v = z_s2i(R_view.y + 3 - view_oy()); return v < 0 ? 0 : v; }
static void paste_image(Img *src, int atx, int aty)
{
    if (!src) return;
    if (A.tb.active) text_commit();
    sel_commit();
    if (src->w > A.img->w || src->h > A.img->h) {
        int r = msgbox("SmPaint", str_res(9234), MB_YESNO, 0);
        if (r == ID_YES) {
            int nw = src->w > A.img->w ? src->w : A.img->w, nh = src->h > A.img->h ? src->h : A.img->h;
            set_canvas_size(nw, nh);
        }
    } else undo_push();
    /* push may have been done by set_canvas_size; ensure exactly one entry was added */
    tool_select(T_SELECT);
    sel_make(atx, aty, src->w, src->h);
    A.sel.pix = src; A.sel.lifted = 1; A.sel.pushed = 1;
}
static void do_paste(void)
{
    int w, h; uint32_t *px = plat_clip_get_image(&w, &h);
    Img *im;
    if (px) { im = (Img *)malloc(sizeof(Img)); im->w = w; im->h = h; im->p = px; }
    else if (A.clip) im = img_clone(A.clip);
    else return;
    paste_image(im, vis_ix(), vis_iy());
}

/* ---------------- transforms on image or selection ---------------- */
typedef Img *(*XForm)(const Img *, void *);
static void apply_xform(XForm fn, void *ctx, int is_resize)
{
    if (A.tb.active) text_commit();
    if (A.sel.active) {
        sel_lift(0);
        Img *n = fn(A.sel.pix, ctx);
        img_free(A.sel.pix); A.sel.pix = n; free(A.sel.mask); A.sel.mask = NULL;
        A.sel.w = n->w; A.sel.h = n->h;
    } else {
        undo_push();
        Img *n = fn(A.img, ctx);
        img_free(A.img); A.img = n; (void)is_resize;
        ui_relayout(); view_clamp();
    }
}
static Img *xf_flip(const Img *s, void *c) { return cv_flip(s, *(int *)c); }
static Img *xf_rot(const Img *s, void *c) { return cv_rotate(s, *(int *)c); }
static Img *xf_inv(const Img *s, void *c) { (void)c; Img *n = img_clone(s); cv_invert(n); return n; }
typedef struct { int hp, vp, hd, vd; } StretchP;
static Img *xf_stretch(const Img *s, void *c)
{
    StretchP *p = (StretchP *)c;
    Img *a = cv_stretch(s, s->w * p->hp / 100, s->h * p->vp / 100);
    if (p->hd || p->vd) { Img *b = cv_skew(a, p->hd, p->vd, A.bg); img_free(a); return b; }
    return a;
}

/* ---------------- dialogs ---------------- */
static double px_to_unit(int px, int unit, int dpi) { return unit == 0 ? px / (double)dpi : unit == 1 ? px / (double)dpi * 2.54 : px; }
static int unit_to_px(double v, int unit, int dpi) { double p = unit == 0 ? v * dpi : unit == 1 ? v / 2.54 * dpi : v; int r = (int)floor(p + 0.5); return r < 1 ? 1 : r; }
static void attr_fill(Dlg *d, int unit, int wpx, int hpx)
{
    char b[32];
    if (unit == 2) { snprintf(b, sizeof b, "%d", wpx); dlg_set_text(d, 264, b); snprintf(b, sizeof b, "%d", hpx); dlg_set_text(d, 266, b); }
    else { snprintf(b, sizeof b, "%.2f", px_to_unit(wpx, unit, A.dpi)); dlg_set_text(d, 264, b); snprintf(b, sizeof b, "%.2f", px_to_unit(hpx, unit, A.dpi)); dlg_set_text(d, 266, b); }
}
typedef struct { int unit; } AttrCtx;
static int attr_unit(Dlg *d) { return dlg_get_check(d, 1028) ? 0 : dlg_get_check(d, 1029) ? 1 : 2; }
static void attr_cmd(Dlg *d, int id, int param)
{
    AttrCtx *c = (AttrCtx *)d->ud; (void)param;
    if (id == 1028 || id == 1029 || id == 1030) {
        int nu = attr_unit(d);
        DCtl *w = dlg_find(d, 264), *h = dlg_find(d, 266);
        int wpx = unit_to_px(atof(w->text), c->unit, A.dpi), hpx = unit_to_px(atof(h->text), c->unit, A.dpi);
        c->unit = nu; attr_fill(d, nu, wpx, hpx);
    } else if (id == 280) { attr_fill(d, attr_unit(d), DEF_W, DEF_H); }
}
static void dialog_attributes(void)
{
    Dlg d; dlg_init(&d, &res_dlg_attributes, NULL);
    AttrCtx c = { 2 }; d.ud = &c; d.on_cmd = attr_cmd;
    dlg_set_check(&d, 1030, 1); dlg_set_check(&d, A.bw ? 1079 : 1080, 1);
    attr_fill(&d, 2, A.img->w, A.img->h);
    char b[128];
    FILE *f = A.path[0] ? fopen(A.path, "rb") : NULL; long sz = -1;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
    dlg_set_text(&d, 1071, "File last saved:   Not Available");
    if (sz >= 0) { snprintf(b, sizeof b, "Size on disk:   %ld bytes", sz); dlg_set_text(&d, 1072, b); } else dlg_set_text(&d, 1072, "Size on disk:   Not Available");
    snprintf(b, sizeof b, "Resolution:   %d x %d dots per inch", A.dpi, A.dpi); dlg_set_text(&d, 1073, b);
    if (dlg_run(&d) != ID_OK) return;
    int unit = attr_unit(&d);
    int w = unit_to_px(atof(dlg_find(&d, 264)->text), unit, A.dpi), h = unit_to_px(atof(dlg_find(&d, 266)->text), unit, A.dpi);
    int tobw = dlg_get_check(&d, 1079);
    if (tobw && !A.bw) {
        if (msgbox("SmPaint", str_res(2298), MB_YESNO, 0) == ID_YES) { sel_commit(); undo_push(); cv_to_bw(A.img); A.bw = 1; }
    } else if (!tobw) A.bw = 0;
    set_canvas_size(w, h);
}
static void zoom_cmd(Dlg *d, int id, int param)
{
    (void)param;
    if (id >= 1082 && id <= 1086) {
        static const int zs[5] = { 100, 200, 400, 600, 800 };
        char b[16]; snprintf(b, sizeof b, "%d", zs[id - 1082]); dlg_set_text(d, 1090, b);
    }
}
static void dialog_zoom(void)
{
    /* original dialog plus a "Custom" percentage field */
    static ResCtl ctl[14];
    static ResDlg rd;
    int n = res_dlg_zoom.nctl;
    for (int i = 0; i < n; i++) ctl[i] = res_dlg_zoom.ctl[i];
    for (int i = 0; i < n; i++) if (ctl[i].id == 65535) ctl[i].h = 80;
    ctl[n++] = (ResCtl){ 1091, 130, 0x50020000u, 13, 76, 30, 8, "Custom:" };
    ctl[n++] = (ResCtl){ 1090, 129, 0x50810080u, 46, 74, 32, 12, "" };
    ctl[n++] = (ResCtl){ 1092, 130, 0x50020000u, 80, 76, 10, 8, "%" };
    rd = res_dlg_zoom; rd.h = 106; rd.nctl = n; rd.ctl = ctl;
    Dlg d; dlg_init(&d, &rd, NULL);
    d.on_cmd = zoom_cmd;
    for (int i = 0; i < d.n; i++) if (d.c[i].r.id == 1090) { d.focus = i; d.sel_all = 1; d.caret = 0; }
    char b[16]; snprintf(b, sizeof b, "%d%%", A.zp); dlg_set_text(&d, 1081, b);
    snprintf(b, sizeof b, "%d", A.zp); dlg_set_text(&d, 1090, b);
    int ids[5] = { 1082, 1083, 1084, 1085, 1086 }, zs[5] = { 100, 200, 400, 600, 800 };
    for (int i = 0; i < 5; i++) if (zs[i] == A.zp) dlg_set_check(&d, ids[i], 1);
    if (dlg_run(&d) != ID_OK) return;
    int v = dlg_get_int(&d, 1090, A.zp);
    if (v < ZOOM_MIN) v = ZOOM_MIN;
    if (v > ZOOM_MAX) v = ZOOM_MAX;
    set_zoom(v, A.img->w / 2, A.img->h / 2);
}
static void fliprot_cmd(Dlg *d, int id, int param)
{
    (void)param;
    if (id == 1087 || id == 1088 || id == 1089) {
        int on = dlg_get_check(d, 1089);
        dlg_set_enabled(d, 1090, on); dlg_set_enabled(d, 1091, on); dlg_set_enabled(d, 1092, on);
    }
}
static void dialog_fliprot(void)
{
    Dlg d; dlg_init(&d, &res_dlg_fliprot, NULL);
    d.on_cmd = fliprot_cmd;
    dlg_set_check(&d, 1087, 1); dlg_set_check(&d, 1090, 1);
    fliprot_cmd(&d, 1087, 0);
    if (dlg_run(&d) != ID_OK) return;
    int arg;
    if (dlg_get_check(&d, 1087)) { arg = 1; apply_xform(xf_flip, &arg, 0); }
    else if (dlg_get_check(&d, 1088)) { arg = 0; apply_xform(xf_flip, &arg, 0); }
    else { arg = dlg_get_check(&d, 1090) ? 90 : dlg_get_check(&d, 1091) ? 180 : 270; apply_xform(xf_rot, &arg, 1); }
}
static void dialog_stretch(void)
{
    Dlg d; dlg_init(&d, &res_dlg_stretch, NULL);
    dlg_set_text(&d, 1019, "100"); dlg_set_text(&d, 1020, "100"); dlg_set_text(&d, 1021, "0"); dlg_set_text(&d, 1022, "0");
    if (dlg_run(&d) != ID_OK) return;
    StretchP p = { dlg_get_int(&d, 1019, 100), dlg_get_int(&d, 1020, 100), dlg_get_int(&d, 1021, 0), dlg_get_int(&d, 1022, 0) };
    if (p.hp < 1 || p.hp > 500 || p.vp < 1 || p.vp > 500 || abs(p.hd) > 89 || abs(p.vd) > 89) { msgbox("SmPaint", "The values entered are out of range.", MB_OK, 0); return; }
    if (p.hp == 100 && p.vp == 100 && !p.hd && !p.vd) return;
    apply_xform(xf_stretch, &p, 1);
}

typedef struct { Modal m; } ViewBmp;
static void vb_paint(Modal *m, Img *fb)
{
    (void)m;
    gx_rect(fb, 0, 0, fb->w, fb->h, C_BLACK);
    int ow;
    Img *c = build_comp(&ow);
    for (int y = 0; y < c->h && y < fb->h; y++) for (int x = 0; x < c->w && x < fb->w; x++) fb->p[y * fb->w + x] = c->p[y * c->w + x];
    if (ow) img_free(c);
}
static int vb_event(Modal *m, const PEvent *ev)
{
    if (ev->type == EV_MOUSEDOWN || ev->type == EV_KEYDOWN) { m->done = 1; m->result = ID_OK; }
    return 1;
}
static void app_paint_stub(void) {}

int cmd_exec(int id)
{
    (void)app_paint_stub;
    switch (id) {
    case CMD_NEW: if (app_confirm_discard()) { app_new_image(DEF_W, DEF_H); } break;
    case CMD_OPEN: {
        if (!app_confirm_discard()) break;
        char p[1024] = "";
        if (file_dialog(0, "Open", p, sizeof p, 0)) app_load_file(p);
        break;
    }
    case CMD_SAVE:
        if (A.tb.active) text_commit();
        sel_commit();
        if (!A.path[0]) return cmd_exec(CMD_SAVEAS);
        if (save_image(A.path, A.img, -1)) { A.dirty = 0; update_title(); }
        break;
    case CMD_SAVEAS: {
        if (A.tb.active) text_commit();
        sel_commit();
        char p[1024]; strcpy(p, A.path[0] ? A.path : "untitled.jpg");
        if (!A.path[0]) { char cwd[900]; if (plat_getcwd(cwd, sizeof cwd) == 0) snprintf(p, sizeof p, "%s/untitled.jpg", cwd); }
        if (file_dialog(1, "Save As", p, sizeof p, 4)) {
            if (g_file_filter <= 2) { if (msgbox("SmPaint", str_res(2297), MB_YESNO, 0) != ID_YES) break; }
            const char *dot = strrchr(p, '.');
            if (dot && !str_icmp(dot, ".gif") && msgbox("SmPaint", str_res(2297), MB_YESNO, 0) != ID_YES) break;
            int flt = g_file_filter;
            if (save_image(p, A.img, flt)) { strncpy(A.path, p, 1023); A.dirty = 0; add_recent(p); update_title(); }
        }
        break;
    }
    case CMD_COPYTO: {
        if (!A.sel.active) break;
        char p[1024] = "selection.jpg";
        if (file_dialog(1, "Copy To", p, sizeof p, 4)) { Img *c = sel_content(); save_image(p, c, g_file_filter); img_free(c); }
        break;
    }
    case CMD_PASTEFROM: {
        char p[1024] = "";
        if (file_dialog(0, "Paste From", p, sizeof p, 0)) { Img *im = load_image_file(p); if (im) paste_image(im, vis_ix(), vis_iy()); }
        break;
    }
    case CMD_UNDO: do_undo(); break;
    case CMD_REPEAT: do_redo(); break;
    case CMD_CUT: if (A.sel.active) { do_copy(); sel_delete(); } break;
    case CMD_COPY: if (A.sel.active) do_copy(); break;
    case CMD_PASTE: do_paste(); break;
    case CMD_CLEARSEL: if (A.sel.active) sel_delete(); else if (A.tb.active) { free(A.tb.text); free(A.tb.st); A.tb.text = NULL; A.tb.st = NULL; A.tb.len = A.tb.caret = A.tb.cap = 0; A.tb.active = 0; A.tb.sel = -1; } break;
    case CMD_SELECTALL:
        if (A.tb.active) { text_select_all(); break; }
        tool_select(T_SELECT); sel_commit(); sel_make(0, 0, A.img->w, A.img->h); break;
    case CMD_TOOLBOX: A.show_toolbox = !A.show_toolbox; ui_relayout(); break;
    case CMD_COLORBOX: A.show_colorbox = !A.show_colorbox; ui_relayout(); break;
    case CMD_STATUSBAR: A.show_status = !A.show_status; ui_relayout(); break;
    case CMD_TEXTBAR: A.show_text = !A.show_text; ui_relayout(); break;
    case CMD_ZOOM_NORMAL: set_zoom(100, A.img->w / 2, A.img->h / 2); break;
    case CMD_ZOOM_LARGE: set_zoom(400, A.img->w / 2, A.img->h / 2); break;
    case CMD_ZOOM_CUSTOM: dialog_zoom(); break;
    case CMD_ZOOMIN: case CMD_ZOOMOUT: {
        static const int zs[] = { 10, 12, 25, 50, 66, 80, 100, 150, 200, 300, 400, 600, 800 };
        int n = (int)(sizeof zs / sizeof zs[0]), t = A.zp;
        if (id == CMD_ZOOMIN) { for (int i = 0; i < n; i++) if (zs[i] > A.zp) { t = zs[i]; break; } }
        else { for (int i = n - 1; i >= 0; i--) if (zs[i] < A.zp) { t = zs[i]; break; } }
        set_zoom(t, A.img->w / 2, A.img->h / 2); break; }
    case CMD_GRID: A.show_grid = !A.show_grid; break;
    case CMD_THUMB: A.show_thumb = !A.show_thumb; break;
    case CMD_VIEWBITMAP: { ViewBmp v; memset(&v, 0, sizeof v); v.m.paint = vb_paint; v.m.event = vb_event; v.m.ud = &v; modal_run(&v.m); break; }
    case CMD_FLIPROT: dialog_fliprot(); break;
    case CMD_STRETCH: dialog_stretch(); break;
    case CMD_INVERT: apply_xform(xf_inv, NULL, 0); break;
    case CMD_ATTR: dialog_attributes(); break;
    case CMD_CLEARIMG: if (A.tb.active) text_commit(); sel_commit(); undo_push(); img_fill(A.img, A.bg); break;
    case CMD_OPAQUE: A.o_trans = !A.o_trans; break;
    case CMD_EDITCOLORS: { px_t c = A.fg; if (color_dialog(&c)) A.fg = c; break; }
    case CMD_HELP:
        msgbox("SmPaint Help", "SmPaint Help is not included in this port.\n\nSelect a tool in the toolbox, pick colors in the color box\n(left click = foreground, right click = background),\nthen drag in the picture. Hover over a command or tool\nto see a description in the status bar.", MB_OK, 0);
        break;
    case CMD_ABOUT: about_dialog(); break;
    case CMD_EXIT: if (app_quit_request()) g_quit = 1; break;
    case CMD_PAGESETUP: case CMD_PRINT: case CMD_PRINTPREVIEW: case CMD_SEND: case CMD_SCANNER: case CMD_BG_CENTER: case CMD_BG_TILE:
        msgbox("SmPaint", "This command is not available in the Linux port.", MB_OK, 0); break;
    default:
        if (id == CMD_RECENT) break;
        break;
    }
    return 0;
}
