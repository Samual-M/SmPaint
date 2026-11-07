/* ui.c - portable UI toolkit: modal loop, menus, message boxes, data-driven dialogs, file & color dialogs */
#include <ctype.h>
#include <math.h>
#include "app.h"

#define TXT_H 13

const char *str_res(int id)
{
    for (int i = 0; i < res_nstrings; i++) if (res_strings[i].id == id) return res_strings[i].text;
    return "";
}
const char *menu_help_text(int id)
{
    static char buf[256];
    const char *s = str_res(id);
    strncpy(buf, s, 255); buf[255] = 0;
    char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
    return buf;
}

/* ================= modal loop ================= */
int modal_run(Modal *m)
{
    m->done = 0;
    while (!m->done) {
        app_paint_all(A.fb);
        if (m->paint) m->paint(m, A.fb);
        plat_present(A.fb->p, A.fb->w, A.fb->h);
        PEvent ev;
        pump_event(&ev, 400);
        if (ev.type == EV_CLOSE) { m->result = ID_CANCEL; m->done = 1; break; }
        if (ev.type == EV_NONE || ev.type == EV_REDRAW || ev.type == EV_RESIZE) { if (m->event) { PEvent t = ev; m->event(m, &t); } continue; }
        if (m->event) m->event(m, &ev);
    }
    return m->result;
}

/* ================= drawing helpers ================= */
static void draw_titlebar(Img *fb, int x, int y, int w, const char *title)
{
    for (int i = 0; i < w; i++) {
        int r = 0 + (16 * i) / (w > 1 ? w - 1 : 1), g = 0 + (132 * i) / (w > 1 ? w - 1 : 1), b = 128 + (80 * i) / (w > 1 ? w - 1 : 1);
        gx_rect(fb, x + i, y, 1, 18, RGB(r, g, b));
    }
    tx_draw(fb, x + 4, y + 3, title, C_WHITE, 1, 0);
    /* close button */
    int bx = x + w - 18, by = y + 2;
    gx_rect(fb, bx, by, 16, 14, C_FACE); gx_raised(fb, bx, by, 16, 14);
    for (int k = 0; k < 8; k++) { img_put(fb, bx + 4 + k, by + 3 + k, C_BLACK); img_put(fb, bx + 5 + k, by + 3 + k, C_BLACK); img_put(fb, bx + 11 - k, by + 3 + k, C_BLACK); img_put(fb, bx + 12 - k, by + 3 + k, C_BLACK); }
}
static void draw_window_frame(Img *fb, int x, int y, int w, int h, const char *title)
{
    gx_rect(fb, x, y, w, h, C_FACE);
    gx_raised(fb, x, y, w, h);
    draw_titlebar(fb, x + 3, y + 3, w - 6, title);
}
static void draw_check(Img *fb, int x, int y, px_t c)
{
    static const char *pat[7] = { "......#", ".....##", "#...##.", "##.##..", ".###...", "..#....", "......." };
    (void)pat;
    for (int i = 0; i < 3; i++) { img_put(fb, x + i, y + 2 + i, c); img_put(fb, x + i, y + 3 + i, c); }
    for (int i = 0; i < 4; i++) { img_put(fb, x + 3 + i, y + 4 - i, c); img_put(fb, x + 3 + i, y + 5 - i, c); }
}
static void draw_button(Img *fb, int x, int y, int w, int h, const char *label, int pressed, int def, int focus, int enabled)
{
    gx_rect(fb, x, y, w, h, C_FACE);
    if (def) { gx_frame(fb, x, y, w, h, C_BLACK); x++; y++; w -= 2; h -= 2; }
    if (pressed) { gx_frame(fb, x, y, w, h, C_SHADOW); }
    else gx_raised(fb, x, y, w, h);
    int tw = tx_width(label, 0, 1), tx = x + (w - tw) / 2 + pressed, ty = y + (h - TXT_H) / 2 + pressed;
    if (enabled) tx_draw(fb, tx, ty, label, C_BLACK, 0, 1); else tx_draw_dis(fb, tx, ty, label, 1);
    if (focus) gx_dotrect(fb, x + 4, y + 4, w - 8, h - 8, C_BLACK, C_FACE, 0);
}

/* ================= edit field helper ================= */
static void ed_insert(char *buf, int cap, int *caret, const char *s)
{
    int n = (int)strlen(buf), k = (int)strlen(s);
    if (n + k >= cap) return;
    memmove(buf + *caret + k, buf + *caret, n - *caret + 1);
    memcpy(buf + *caret, s, k); *caret += k;
}
static void utf8_enc(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; o[1] = 0; }
    else if (c < 0x800) { o[0] = (char)(0xc0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); o[2] = 0; }
    else if (c < 0x10000) { o[0] = (char)(0xe0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); o[3] = 0; }
    else { o[0] = (char)(0xf0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63)); o[4] = 0; }
}
/* returns 1 if the buffer changed. *sel != 0 means whole text selected */
static int ed_event(char *buf, int cap, int *caret, int *sel, const PEvent *ev, int numeric)
{
    int n = (int)strlen(buf);
    if (ev->type == EV_CHAR) {
        if (ev->ch < 32 || ev->ch == 127) return 0;
        if (numeric && !((ev->ch >= '0' && ev->ch <= '9') || ev->ch == '.' || ev->ch == '-')) return 0;
        char t[8]; utf8_enc(ev->ch, t);
        if (*sel) { buf[0] = 0; *caret = 0; *sel = 0; }
        ed_insert(buf, cap, caret, t);
        return 1;
    }
    if (ev->type != EV_KEYDOWN) return 0;
    if ((ev->mods & MOD_CTRL) && ev->key == 'a') { *sel = 1; *caret = n; return 0; }
    switch (ev->key) {
    case KEY_BACKSPACE:
        if (*sel) { buf[0] = 0; *caret = 0; *sel = 0; return 1; }
        if (*caret > 0) { int p = *caret - 1; while (p > 0 && ((unsigned char)buf[p] & 0xc0) == 0x80) p--; memmove(buf + p, buf + *caret, n - *caret + 1); *caret = p; return 1; }
        return 0;
    case KEY_DELETE:
        if (*sel) { buf[0] = 0; *caret = 0; *sel = 0; return 1; }
        if (*caret < n) { int e = *caret + 1; while (e < n && ((unsigned char)buf[e] & 0xc0) == 0x80) e++; memmove(buf + *caret, buf + e, n - e + 1); return 1; }
        return 0;
    case KEY_LEFT: *sel = 0; if (*caret > 0) { (*caret)--; while (*caret > 0 && ((unsigned char)buf[*caret] & 0xc0) == 0x80) (*caret)--; } return 0;
    case KEY_RIGHT: *sel = 0; if (*caret < n) { (*caret)++; while (*caret < n && ((unsigned char)buf[*caret] & 0xc0) == 0x80) (*caret)++; } return 0;
    case KEY_HOME: *sel = 0; *caret = 0; return 0;
    case KEY_END: *sel = 0; *caret = n; return 0;
    }
    return 0;
}
static void ed_draw(Img *fb, int x, int y, int w, int h, const char *buf, int caret, int sel, int focus)
{
    gx_rect(fb, x, y, w, h, C_WHITE);
    gx_sunken(fb, x, y, w, h);
    int tx = x + 3, ty = y + (h - TXT_H) / 2;
    int tw = tx_width(buf, 0, 0);
    if (sel && *buf) { gx_rect(fb, tx, ty, tw + 1, TXT_H, C_NAVY); tx_draw(fb, tx, ty, buf, C_WHITE, 0, 0); }
    else tx_draw(fb, tx, ty, buf, C_BLACK, 0, 0);
    if (focus && !sel) {
        char tmp[512]; int c = caret; if (c > (int)sizeof tmp - 1) c = (int)sizeof tmp - 1;
        memcpy(tmp, buf, c); tmp[c] = 0;
        int cx = tx + tx_width(tmp, 0, 0);
        if ((plat_ticks_ms() / 500) % 2 == 0) gx_vline(fb, cx, ty, ty + TXT_H - 1, C_BLACK);
    }
}
static int ed_caret_at(const char *buf, int x)
{
    int n = (int)strlen(buf), best = 0;
    for (int i = 0; i <= n; i++) {
        if (i < n && ((unsigned char)buf[i] & 0xc0) == 0x80) continue;
        char tmp[512]; memcpy(tmp, buf, i); tmp[i] = 0;
        if (tx_width(tmp, 0, 0) <= x) best = i;
    }
    return best;
}

/* ================= menus ================= */
typedef struct { char title[32]; int n; MItem it[28]; } MenuDef;
static MenuDef tops[8]; static int ntops;
static MenuDef subs[4]; static int nsubs;
static int menus_built;

static void build_menu_from(const ResMenuItem *res, MenuDef *tp, int *ntp, MenuDef *sb, int *nsb)
{
    *ntp = 0; *nsb = 0;
    MenuDef *cur = NULL;
    for (const ResMenuItem *r = res; r->type; r++) {
        if (r->depth == 0 && r->type == 1) { cur = &tp[(*ntp)++]; memset(cur, 0, sizeof *cur); strncpy(cur->title, r->text, 31); }
        else if (r->depth == 1 && cur) {
            MItem *m = &cur->it[cur->n++]; memset(m, 0, sizeof *m); m->type = r->type; m->id = r->id; strncpy(m->text, r->text, 79); m->sub = -1;
            if (r->type == 1) { m->sub = *nsb; MenuDef *s = &sb[(*nsb)++]; memset(s, 0, sizeof *s); strncpy(s->title, r->text, 31); }
        } else if (r->depth == 2 && *nsb > 0) {
            MenuDef *s = &sb[*nsb - 1];
            MItem *m = &s->it[s->n++]; memset(m, 0, sizeof *m); m->type = r->type; m->id = r->id; strncpy(m->text, r->text, 79); m->sub = -1;
        }
    }
}

typedef struct {
    Modal m; int bar_mode; int top; int open; /* popup open */
    int hot0, sub, hot1; int cmd; int has_cmd;
    MenuDef *tp; int ntp; MenuDef *sb; int nsb;
    int px, py; /* for context menus */
    int bar_x[10], bar_w[10];
    Rc r0, r1; int moved;
    char recent_label[4][80];
} MenuRun;

static int item_h(const MItem *m) { return m->type == 3 ? 9 : 18; }
static void menu_size_impl(const MenuDef *md, int *w, int *h);
static void menu_size(const MenuDef *md, int *w, int *h) { int pb = tx_big(1); menu_size_impl(md, w, h); tx_big(pb); }
static void menu_size_impl(const MenuDef *md, int *w, int *h)
{
    int mw = 60, hh = 4;
    for (int i = 0; i < md->n; i++) {
        const MItem *m = &md->it[i];
        hh += item_h(m);
        if (m->type == 3) continue;
        char lab[80]; strcpy(lab, m->text);
        char *tab = strchr(lab, '\t'); int aw = 0;
        if (tab) { aw = tx_width(tab + 1, 0, 0) + 20; *tab = 0; }
        int ww = tx_width(lab, 0, 1) + aw + 22 + 22;
        if (ww > mw) mw = ww;
    }
    *w = mw; *h = hh;
}
static int menu_enabled(const MItem *m)
{
    if (m->type == 3) return 0;
    if (m->type == 1) return 1;
    int en = 1, ck = 0; menu_state(m->id, &en, &ck);
    return en;
}
static void draw_popup_impl(Img *fb, const MenuDef *md, int x, int y, int w, int h, int hot);
static void draw_popup(Img *fb, const MenuDef *md, int x, int y, int w, int h, int hot) { int pb = tx_big(1); draw_popup_impl(fb, md, x, y, w, h, hot); tx_big(pb); }
static void draw_popup_impl(Img *fb, const MenuDef *md, int x, int y, int w, int h, int hot)
{
    gx_rect(fb, x, y, w, h, C_FACE); gx_raised(fb, x, y, w, h);
    int iy = y + 2;
    for (int i = 0; i < md->n; i++) {
        const MItem *m = &md->it[i];
        int ih = item_h(m);
        if (m->type == 3) { gx_hline(fb, x + 3, x + w - 4, iy + 3, C_SHADOW); gx_hline(fb, x + 3, x + w - 4, iy + 4, C_HILITE); iy += ih; continue; }
        int en = menu_enabled(m), checked = 0, e2;
        if (m->type == 2) menu_state(m->id, &e2, &checked);
        int sel = (i == hot);
        if (sel) gx_rect(fb, x + 3, iy, w - 6, ih, C_NAVY);
        char lab[80]; strcpy(lab, m->text);
        char *tab = strchr(lab, '\t'); char acc[40] = ""; if (tab) { strcpy(acc, tab + 1); *tab = 0; }
        px_t col = sel ? C_WHITE : C_BLACK;
        if (checked) draw_check(fb, x + 8, iy + 5, en ? (sel ? C_WHITE : C_BLACK) : C_SHADOW);
        if (en) { tx_draw(fb, x + 22, iy + 3, lab, col, 0, 1); if (*acc) tx_draw(fb, x + w - 14 - tx_width(acc, 0, 0), iy + 3, acc, col, 0, 0); }
        else if (sel) { tx_draw(fb, x + 22, iy + 3, lab, C_SHADOW, 0, 1); if (*acc) tx_draw(fb, x + w - 14 - tx_width(acc, 0, 0), iy + 3, acc, C_SHADOW, 0, 0); }
        else { tx_draw_dis(fb, x + 22, iy + 3, lab, 1); if (*acc) tx_draw_dis(fb, x + w - 14 - tx_width(acc, 0, 0), iy + 3, acc, 0); }
        if (m->type == 1) {
            int ax = x + w - 12, ay = iy + 5; px_t ac = en ? col : C_SHADOW;
            for (int k = 0; k < 4; k++) gx_vline(fb, ax + k, ay + k, ay + 7 - k, ac);
        }
        iy += ih;
    }
}
static int item_at(const MenuDef *md, int x, int y, const Rc *r)
{
    if (!rc_in(r, x, y)) return -1;
    int iy = r->y + 2;
    for (int i = 0; i < md->n; i++) {
        int ih = item_h(&md->it[i]);
        if (y >= iy && y < iy + ih) return i;
        iy += ih;
    }
    return -1;
}
static void menurun_layout(MenuRun *mr)
{
    MenuDef *md = mr->bar_mode ? &mr->tp[mr->top] : &mr->tp[0];
    int w, h; menu_size(md, &w, &h);
    int x = mr->bar_mode ? mr->bar_x[mr->top] : mr->px, y = mr->bar_mode ? 19 : mr->py;
    if (x + w > A.fb->w) x = A.fb->w - w;
    if (y + h > A.fb->h) y = A.fb->h - h; if (y < 0) y = 0;
    mr->r0 = (Rc){ x, y, w, h };
    if (mr->sub >= 0) {
        MenuDef *sd = &mr->sb[mr->sub]; int sw, sh; menu_size(sd, &sw, &sh);
        int iy = y + 2;
        for (int i = 0; i < md->n; i++) { if (md->it[i].type == 1 && md->it[i].sub == mr->sub) break; iy += item_h(&md->it[i]); }
        int sx = x + w - 3; if (sx + sw > A.fb->w) sx = x - sw + 3;
        mr->r1 = (Rc){ sx, iy - 2, sw, sh };
    }
}
static void menurun_paint_impl(Modal *m, Img *fb);
static void menurun_paint(Modal *m, Img *fb) { int pb = tx_big(1); menurun_paint_impl(m, fb); tx_big(pb); }
static void menurun_paint_impl(Modal *m, Img *fb)
{
    MenuRun *mr = (MenuRun *)m->ud;
    if (mr->bar_mode) {
        for (int i = 0; i < mr->ntp; i++) {
            int x = mr->bar_x[i], w = mr->bar_w[i];
            if (i == mr->top) { gx_rect(fb, x, 1, w, 17, C_FACE); gx_sunken1(fb, x, 1, w, 17); tx_draw(fb, x + 8 + 1, 3 + 1, mr->tp[i].title, C_BLACK, 0, 1); }
        }
    }
    menurun_layout(mr);
    MenuDef *md = mr->bar_mode ? &mr->tp[mr->top] : &mr->tp[0];
    draw_popup(fb, md, mr->r0.x, mr->r0.y, mr->r0.w, mr->r0.h, mr->hot0);
    if (mr->sub >= 0) draw_popup(fb, &mr->sb[mr->sub], mr->r1.x, mr->r1.y, mr->r1.w, mr->r1.h, mr->hot1);
}
static int next_item(const MenuDef *md, int from, int dir)
{
    int n = md->n;
    if (!n) return -1;
    int i = from;
    for (int k = 0; k < n; k++) {
        i = (i + dir + n) % n;
        if (md->it[i].type != 3) return i;
    }
    return -1;
}
static void menurun_set_hot(MenuRun *mr, int h0, int h1)
{
    MenuDef *md = mr->bar_mode ? &mr->tp[mr->top] : &mr->tp[0];
    if (h0 != mr->hot0) {
        mr->hot0 = h0; mr->hot1 = -1; mr->sub = -1;
        if (h0 >= 0 && md->it[h0].type == 1) mr->sub = md->it[h0].sub;
    }
    if (h1 != mr->hot1) mr->hot1 = h1;
    if (mr->sub >= 0 && mr->hot1 >= 0) {
        set_status_help(menu_help_text(mr->sb[mr->sub].it[mr->hot1].id));
    } else if (mr->hot0 >= 0 && md->it[mr->hot0].type == 2) set_status_help(menu_help_text(md->it[mr->hot0].id));
    else if (mr->hot0 >= 0 && md->it[mr->hot0].type == 1) set_status_help(str_res(37688 + 0));
    else set_status_help("");
}
static void menurun_choose(MenuRun *mr, const MItem *it)
{
    if (!menu_enabled(it) || it->type == 1) return;
    mr->cmd = it->id; mr->has_cmd = 1; mr->m.done = 1; mr->m.result = ID_OK;
}
static int menurun_event(Modal *m, const PEvent *ev)
{
    MenuRun *mr = (MenuRun *)m->ud;
    menurun_layout(mr);
    MenuDef *md = mr->bar_mode ? &mr->tp[mr->top] : &mr->tp[0];
    switch (ev->type) {
    case EV_MOUSEMOVE: case EV_MOUSEDOWN: case EV_MOUSEUP: {
        int x = ev->x, y = ev->y;
        int h1 = (mr->sub >= 0) ? item_at(&mr->sb[mr->sub], x, y, &mr->r1) : -1;
        int h0 = item_at(md, x, y, &mr->r0);
        if (mr->sub >= 0 && h1 >= 0) { menurun_set_hot(mr, mr->hot0, h1); h0 = mr->hot0; }
        else if (h0 >= 0) menurun_set_hot(mr, h0, -1);
        else if (mr->sub >= 0 && rc_in(&mr->r1, x, y)) {}
        else if (!(mr->sub >= 0 && mr->hot1 >= 0 && ev->type == EV_MOUSEMOVE)) menurun_set_hot(mr, -1, -1);
        if (mr->bar_mode && y >= 0 && y < 19) {
            for (int i = 0; i < mr->ntp; i++)
                if (x >= mr->bar_x[i] && x < mr->bar_x[i] + mr->bar_w[i]) {
                    if (ev->type == EV_MOUSEDOWN && i == mr->top) { m->done = 1; m->result = ID_CANCEL; return 1; }
                    if (i != mr->top && ev->type != EV_MOUSEUP) { mr->top = i; mr->hot0 = -1; mr->hot1 = -1; mr->sub = -1; }
                }
        }
        if (ev->type == EV_MOUSEDOWN) {
            if (!(rc_in(&mr->r0, x, y) || (mr->sub >= 0 && rc_in(&mr->r1, x, y)) || (mr->bar_mode && y < 19))) { m->done = 1; m->result = ID_CANCEL; return 1; }
            if (mr->sub >= 0 && h1 >= 0) menurun_choose(mr, &mr->sb[mr->sub].it[h1]);
            else if (h0 >= 0 && md->it[h0].type == 2) menurun_choose(mr, &md->it[h0]);
            mr->moved = 1;
        }
        if (ev->type == EV_MOUSEUP) {
            if (mr->sub >= 0 && h1 >= 0) menurun_choose(mr, &mr->sb[mr->sub].it[h1]);
            else if (h0 >= 0 && md->it[h0].type == 2) menurun_choose(mr, &md->it[h0]);
            else if (!mr->bar_mode && !rc_in(&mr->r0, x, y)) { m->done = 1; m->result = ID_CANCEL; }
        }
        return 1;
    }
    case EV_KEYDOWN:
        switch (ev->key) {
        case KEY_ESC:
            if (mr->sub >= 0) { mr->sub = -1; mr->hot1 = -1; }
            else { m->done = 1; m->result = ID_CANCEL; }
            return 1;
        case KEY_DOWN:
            if (mr->sub >= 0) menurun_set_hot(mr, mr->hot0, next_item(&mr->sb[mr->sub], mr->hot1 < 0 ? -1 : mr->hot1, 1));
            else menurun_set_hot(mr, next_item(md, mr->hot0, 1), -1);
            return 1;
        case KEY_UP:
            if (mr->sub >= 0) menurun_set_hot(mr, mr->hot0, next_item(&mr->sb[mr->sub], mr->hot1 < 0 ? 0 : mr->hot1, -1));
            else menurun_set_hot(mr, next_item(md, mr->hot0 < 0 ? 0 : mr->hot0, -1), -1);
            return 1;
        case KEY_RIGHT:
            if (mr->hot0 >= 0 && md->it[mr->hot0].type == 1 && mr->sub >= 0) { menurun_set_hot(mr, mr->hot0, next_item(&mr->sb[mr->sub], -1, 1)); return 1; }
            if (mr->bar_mode) { mr->top = (mr->top + 1) % mr->ntp; mr->hot0 = -1; mr->sub = -1; mr->hot1 = -1; }
            return 1;
        case KEY_LEFT:
            if (mr->sub >= 0 && mr->hot1 >= 0) { mr->hot1 = -1; return 1; }
            if (mr->bar_mode) { mr->top = (mr->top + mr->ntp - 1) % mr->ntp; mr->hot0 = -1; mr->sub = -1; mr->hot1 = -1; }
            return 1;
        case KEY_ENTER: case KEY_SPACE:
            if (mr->sub >= 0 && mr->hot1 >= 0) menurun_choose(mr, &mr->sb[mr->sub].it[mr->hot1]);
            else if (mr->hot0 >= 0) {
                if (md->it[mr->hot0].type == 1) menurun_set_hot(mr, mr->hot0, next_item(&mr->sb[mr->sub], -1, 1));
                else menurun_choose(mr, &md->it[mr->hot0]);
            }
            return 1;
        default: {
            int k = ev->key;
            if (k >= 'a' && k <= 'z') {
                const MenuDef *cm = (mr->sub >= 0 && mr->hot1 >= 0) ? &mr->sb[mr->sub] : md;
                if (mr->sub >= 0) cm = &mr->sb[mr->sub];
                for (int i = 0; i < cm->n; i++) {
                    const char *a = strchr(cm->it[i].text, '&');
                    if (a && tolower((unsigned char)a[1]) == k) {
                        if (cm->it[i].type == 1) { menurun_set_hot(mr, i, next_item(&mr->sb[cm->it[i].sub], -1, 1)); }
                        else if (cm == md) { menurun_set_hot(mr, i, -1); menurun_choose(mr, &cm->it[i]); }
                        else menurun_choose(mr, &cm->it[i]);
                        break;
                    }
                }
            }
            return 1;
        }
        }
    }
    return 0;
}
static void menu_bar_layout_impl(MenuRun *mr);
static void menu_bar_layout(MenuRun *mr) { int pb = tx_big(1); menu_bar_layout_impl(mr); tx_big(pb); }
static void menu_bar_layout_impl(MenuRun *mr)
{
    int x = 2;
    for (int i = 0; i < mr->ntp; i++) { mr->bar_x[i] = x; mr->bar_w[i] = tx_width(mr->tp[i].title, 0, 1) + 16; x += mr->bar_w[i]; }
}
static void ensure_menus(void)
{
    if (menus_built) return;
    build_menu_from(res_menu_main, tops, &ntops, subs, &nsubs);
    /* File > "Copy Image to Clipboard" right under Save As: whole picture to the clipboard, no file needed */
    for (int t = 0; t < ntops; t++)
        for (int i = 0; i < tops[t].n && tops[t].n < 27; i++)
            if (tops[t].it[i].id == CMD_SAVEAS) {
                MItem *m = &tops[t].it[i + 1];
                memmove(m + 1, m, sizeof(MItem) * (size_t)(tops[t].n - i - 1));
                memset(m, 0, sizeof *m); m->type = tops[t].it[i].type; m->id = CMD_COPYIMG; m->sub = -1;
                strcpy(m->text, "Copy Image to Clipboard\tCtrl+Shift+C");
                tops[t].n++; t = ntops; break;
            }
    menus_built = 1;
}
int menu_bar_titles(char out[][32], int *xs, int *ws)
{
    ensure_menus();
    MenuRun tmp; memset(&tmp, 0, sizeof tmp); tmp.tp = tops; tmp.ntp = ntops;
    menu_bar_layout(&tmp);
    for (int i = 0; i < ntops; i++) { strcpy(out[i], tops[i].title); xs[i] = tmp.bar_x[i]; ws[i] = tmp.bar_w[i]; }
    return ntops;
}
int menu_bar_run(int start_top, int *cmd_out)
{
    ensure_menus();
    MenuRun mr; memset(&mr, 0, sizeof mr);
    mr.bar_mode = 1; mr.top = start_top; mr.tp = tops; mr.ntp = ntops; mr.sb = subs; mr.nsb = nsubs;
    mr.hot0 = mr.hot1 = mr.sub = -1;
    menu_bar_layout(&mr);
    mr.m.paint = menurun_paint; mr.m.event = menurun_event; mr.m.ud = &mr;
    modal_run(&mr.m);
    set_status_help("");
    if (mr.has_cmd) { *cmd_out = mr.cmd; return 1; }
    return 0;
}
int popup_menu_run(const ResMenuItem *items, int x, int y, int *cmd_out)
{
    MenuDef tp[2], sb[2]; int ntp, nsb;
    build_menu_from(items, tp, &ntp, sb, &nsb);
    MenuRun mr; memset(&mr, 0, sizeof mr);
    mr.bar_mode = 0; mr.tp = tp; mr.ntp = ntp; mr.sb = sb; mr.nsb = nsb; mr.px = x; mr.py = y;
    mr.hot0 = mr.hot1 = mr.sub = -1;
    mr.m.paint = menurun_paint; mr.m.event = menurun_event; mr.m.ud = &mr;
    modal_run(&mr.m);
    set_status_help("");
    if (mr.has_cmd) { *cmd_out = mr.cmd; return 1; }
    return 0;
}

/* generic simple list popup (used for combo-like choices): returns index or -1 */
typedef struct { Modal m; const char **items; int n, hot; Rc r; int sel; } ListPop;
static void lp_paint_impl(Modal *m, Img *fb);
static void lp_paint(Modal *m, Img *fb) { int pb = tx_big(1); lp_paint_impl(m, fb); tx_big(pb); }
static void lp_paint_impl(Modal *m, Img *fb)
{
    ListPop *lp = (ListPop *)m->ud;
    gx_rect(fb, lp->r.x, lp->r.y, lp->r.w, lp->r.h, C_WHITE); gx_frame(fb, lp->r.x, lp->r.y, lp->r.w, lp->r.h, C_BLACK);
    for (int i = 0; i < lp->n; i++) {
        int y = lp->r.y + 1 + i * 15;
        if (i == lp->hot) gx_rect(fb, lp->r.x + 1, y, lp->r.w - 2, 15, C_NAVY);
        tx_draw(fb, lp->r.x + 4, y + 1, lp->items[i], i == lp->hot ? C_WHITE : C_BLACK, 0, 0);
    }
}
static int lp_event(Modal *m, const PEvent *ev)
{
    ListPop *lp = (ListPop *)m->ud;
    if (ev->type == EV_MOUSEMOVE || ev->type == EV_MOUSEDOWN || ev->type == EV_MOUSEUP) {
        int in = rc_in(&lp->r, ev->x, ev->y);
        if (in) lp->hot = (ev->y - lp->r.y - 1) / 15;
        if (lp->hot >= lp->n) lp->hot = lp->n - 1;
        if (ev->type == EV_MOUSEDOWN && !in) { m->done = 1; lp->sel = -1; m->result = ID_CANCEL; }
        if ((ev->type == EV_MOUSEDOWN || ev->type == EV_MOUSEUP) && in) { lp->sel = lp->hot; m->done = 1; m->result = ID_OK; }
    } else if (ev->type == EV_KEYDOWN) {
        if (ev->key == KEY_ESC) { m->done = 1; lp->sel = -1; m->result = ID_CANCEL; }
        else if (ev->key == KEY_DOWN && lp->hot < lp->n - 1) lp->hot++;
        else if (ev->key == KEY_UP && lp->hot > 0) lp->hot--;
        else if (ev->key == KEY_ENTER) { lp->sel = lp->hot; m->done = 1; m->result = ID_OK; }
    }
    return 1;
}
int list_popup(const char **items, int n, int x, int y, int w, int cur)
{
    ListPop lp; memset(&lp, 0, sizeof lp);
    lp.items = items; lp.n = n; lp.hot = cur;
    int h = n * 15 + 2; if (y + h > A.fb->h) y = A.fb->h - h;
    lp.r = (Rc){ x, y, w, h };
    lp.m.paint = lp_paint; lp.m.event = lp_event; lp.m.ud = &lp; lp.sel = -1;
    modal_run(&lp.m);
    return lp.m.result == ID_OK ? lp.sel : -1;
}

/* ================= message box ================= */
typedef struct { Modal m; const char *title, *text; int type; int x, y, w, h; int nb; int ids[3]; const char *lab[3]; int def, press, icon; char lines[8][160]; int nl; } MsgB;
static void mb_layout(MsgB *b)
{
    int tw = 0; b->nl = 0;
    char buf[512]; strncpy(buf, b->text, 511); buf[511] = 0;
    for (char *p = strtok(buf, "\n"); p && b->nl < 8; p = strtok(NULL, "\n")) {
        strncpy(b->lines[b->nl], p, 159); b->lines[b->nl][159] = 0;
        int w = tx_width(p, 0, 0); if (w > tw) tw = w;
        b->nl++;
    }
    if (tw > 340) tw = 340;
    b->w = 3 + 16 + 32 + 14 + tw + 22 + 3; if (b->w < 3 * 70 + 40) b->w = 3 * 72 + 30;
    if (b->type == MB_OK && b->w < 160) b->w = 160;
    int th = b->nl * 15; if (th < 32) th = 32;
    b->h = 3 + 18 + 16 + th + 16 + 23 + 14 + 3;
    b->x = (A.fb->w - b->w) / 2; b->y = (A.fb->h - b->h) / 2;
    if (b->type == MB_YESNOCANCEL) { b->nb = 3; b->ids[0] = ID_YES; b->ids[1] = ID_NO; b->ids[2] = ID_CANCEL; b->lab[0] = "&Yes"; b->lab[1] = "&No"; b->lab[2] = "Cancel"; }
    else if (b->type == MB_YESNO) { b->nb = 2; b->ids[0] = ID_YES; b->ids[1] = ID_NO; b->lab[0] = "&Yes"; b->lab[1] = "&No"; }
    else if (b->type == MB_OKCANCEL) { b->nb = 2; b->ids[0] = ID_OK; b->ids[1] = ID_CANCEL; b->lab[0] = "OK"; b->lab[1] = "Cancel"; }
    else { b->nb = 1; b->ids[0] = ID_OK; b->lab[0] = "OK"; }
}
static Rc mb_btn(MsgB *b, int i)
{
    int bw = 75, gap = 6, tot = b->nb * bw + (b->nb - 1) * gap;
    return (Rc){ b->x + (b->w - tot) / 2 + i * (bw + gap), b->y + b->h - 3 - 14 - 23, bw, 23 };
}
static void mb_paint(Modal *m, Img *fb)
{
    MsgB *b = (MsgB *)m->ud; mb_layout(b);
    draw_window_frame(fb, b->x, b->y, b->w, b->h, b->title);
    int iy = b->y + 3 + 18 + 16;
    for (int j = 0; j < 32; j++) for (int i = 0; i < 32; i++) {
        uint32_t v = res_appicon32[j * 32 + i]; if ((v >> 24) < 128) continue; img_put(fb, b->x + 3 + 16 + i, iy + j, v & 0xffffff);
    }
    for (int i = 0; i < b->nl; i++) tx_draw(fb, b->x + 3 + 16 + 32 + 14, iy + 8 + i * 15, b->lines[i], C_BLACK, 0, 0);
    for (int i = 0; i < b->nb; i++) { Rc r = mb_btn(b, i); draw_button(fb, r.x, r.y, r.w, r.h, b->lab[i], b->press == i + 1, i == b->def, i == b->def, 1); }
}
static int mb_event(Modal *m, const PEvent *ev)
{
    MsgB *b = (MsgB *)m->ud; mb_layout(b);
    if (ev->type == EV_MOUSEDOWN && ev->button == BTN_LEFT) {
        b->press = 0;
        for (int i = 0; i < b->nb; i++) { Rc r = mb_btn(b, i); if (rc_in(&r, ev->x, ev->y)) b->press = i + 1; }
        Rc cl = { b->x + b->w - 21, b->y + 5, 16, 14 };
        if (rc_in(&cl, ev->x, ev->y)) { m->done = 1; m->result = b->type == MB_OK ? ID_OK : ID_CANCEL; if (b->type == MB_YESNO) m->result = ID_NO; }
    } else if (ev->type == EV_MOUSEUP && ev->button == BTN_LEFT) {
        for (int i = 0; i < b->nb; i++) { Rc r = mb_btn(b, i); if (rc_in(&r, ev->x, ev->y) && b->press == i + 1) { m->done = 1; m->result = b->ids[i]; } }
        b->press = 0;
    } else if (ev->type == EV_KEYDOWN) {
        if (ev->key == KEY_ESC) { m->done = 1; m->result = (b->type == MB_OK) ? ID_OK : (b->type == MB_YESNO ? ID_NO : ID_CANCEL); }
        else if (ev->key == KEY_ENTER || ev->key == KEY_SPACE) { m->done = 1; m->result = b->ids[b->def]; }
        else if (ev->key == KEY_TAB || ev->key == KEY_LEFT || ev->key == KEY_RIGHT) b->def = (b->def + 1) % b->nb;
        else if (ev->key >= 'a' && ev->key <= 'z') {
            for (int i = 0; i < b->nb; i++) { const char *a = strchr(b->lab[i], '&'); if (a && tolower((unsigned char)a[1]) == ev->key) { m->done = 1; m->result = b->ids[i]; } }
        }
    }
    return 1;
}
int msgbox(const char *title, const char *text, int type, int icon)
{
    MsgB b; memset(&b, 0, sizeof b);
    b.title = title; b.text = text; b.type = type; b.icon = icon;
    b.m.paint = mb_paint; b.m.event = mb_event; b.m.ud = &b;
    return modal_run(&b.m);
}
void about_dialog(void)
{
    char t[300];
    snprintf(t, sizeof t, "SmPaint %s\nNative Linux port of Microsoft Paint (Windows XP).\nAll toolbox art, cursors, menus, dialogs and\nstrings come from the original mspaint.exe.", APP_VERSION);
    msgbox("About SmPaint", t, MB_OK, 0);
}

/* ================= data-driven dialogs (from the original resources) ================= */
#define DLU_X(v) ((v) * 3 / 2)
#define DLU_Y(v) ((v) * 13 / 8)
DCtl *dlg_find(Dlg *d, int id) { for (int i = 0; i < d->n; i++) if (d->c[i].r.id == id && d->c[i].visible) return &d->c[i]; return NULL; }
void dlg_set_check(Dlg *d, int id, int v) { DCtl *c = dlg_find(d, id); if (c) c->checked = v; }
int dlg_get_check(Dlg *d, int id) { DCtl *c = dlg_find(d, id); return c ? c->checked : 0; }
void dlg_set_text(Dlg *d, int id, const char *t) { DCtl *c = dlg_find(d, id); if (c) { strncpy(c->text, t, 63); c->text[63] = 0; } }
int dlg_get_int(Dlg *d, int id, int dflt) { DCtl *c = dlg_find(d, id); return (c && *c->text) ? atoi(c->text) : dflt; }
void dlg_set_enabled(Dlg *d, int id, int en) { DCtl *c = dlg_find(d, id); if (c) c->enabled = en; }
static int ckind(const DCtl *c) { return c->r.cls == 128 ? (int)(c->r.style & 0xf) : -1; }

void dlg_init(Dlg *d, const ResDlg *res, const char *title)
{
    memset(d, 0, sizeof *d);
    d->res = res; strncpy(d->title, title ? title : res->title, 63);
    d->cw = DLU_X(res->w); d->ch = DLU_Y(res->h);
    d->dw = d->cw + 6; d->dh = d->ch + 6 + 18;
    d->focus = -1;
    int grp_start = 0;
    for (int i = 0; i < res->nctl && d->n < 64; i++) {
        DCtl *c = &d->c[d->n];
        c->r = res->ctl[i];
        c->x = DLU_X(c->r.x); c->y = DLU_Y(c->r.y); c->w = DLU_X(c->r.w); c->h = DLU_Y(c->r.h);
        if (c->r.y + c->r.h > res->h + 2) continue;                /* hidden by the original program */
        c->visible = 1; c->enabled = 1;
        strncpy(c->text, c->r.text, 63);
        if (c->r.style & 0x20000) grp_start = d->n;
        c->group_start = grp_start;
        d->n++;
    }
    for (int i = 0; i < d->n; i++) if (d->c[i].r.cls == 129) { d->c[i].text[0] = 0; }
    for (int i = 0; i < d->n; i++) { int k = ckind(&d->c[i]); if (k == 1) { d->focus = i; break; } }
    if (d->focus < 0) for (int i = 0; i < d->n; i++) if (d->c[i].r.style & 0x10000) { d->focus = i; break; }
    d->sel_all = 1;
}
static void dlg_place(Dlg *d)
{
    d->dx = (A.fb->w - d->dw) / 2; d->dy = (A.fb->h - d->dh) / 2;
    d->cx = d->dx + 3; d->cy = d->dy + 3 + 18;
}
static void radio_draw(Img *fb, int x, int y, int checked, int enabled)
{
    for (int j = 0; j < 12; j++) for (int i = 0; i < 12; i++) {
        double dx = i - 5.5, dy = j - 5.5, r = sqrt(dx * dx + dy * dy);
        if (r <= 6.0) {
            px_t c = enabled ? C_WHITE : C_FACE;
            if (r > 4.6) c = (dx + dy < 0) ? C_SHADOW : C_HILITE;
            if (r > 5.3 && dx + dy < 0) c = C_SHADOW;
            img_put(fb, x + i, y + j, c);
        }
    }
    for (int j = 0; j < 12; j++) for (int i = 0; i < 12; i++) {
        double dx = i - 5.5, dy = j - 5.5, r = sqrt(dx * dx + dy * dy);
        if (r > 4.2 && r <= 5.6 && dx + dy < 0) img_put(fb, x + i, y + j, C_SHADOW);
        if (r > 4.2 && r <= 5.6 && dx + dy < -2.2) img_put(fb, x + i, y + j, C_DKSHADOW);
    }
    if (checked) for (int j = 0; j < 12; j++) for (int i = 0; i < 12; i++) {
        double dx = i - 5.5, dy = j - 5.5; if (dx * dx + dy * dy <= 2.9) img_put(fb, x + i, y + j, enabled ? C_BLACK : C_SHADOW);
    }
}
static void dlg_paint(Modal *m, Img *fb)
{
    Dlg *d = (Dlg *)m->ud; dlg_place(d);
    draw_window_frame(fb, d->dx, d->dy, d->dw, d->dh, d->title);
    for (int i = 0; i < d->n; i++) {
        DCtl *c = &d->c[i]; if (!c->visible) continue;
        int x = d->cx + c->x, y = d->cy + c->y, w = c->w, h = c->h;
        int focus = (i == d->focus);
        if (c->r.cls == 128) {
            int k = ckind(c);
            if (k == 7) { /* group box */
                int tw = tx_width(c->text, 0, 1);
                gx_etched(fb, x, y + 6, w, h - 6);
                gx_rect(fb, x + 6, y, tw + 4, 13, C_FACE);
                tx_draw(fb, x + 8, y, c->text, C_BLACK, 0, 1);
            } else if (k == 0 || k == 1) {
                draw_button(fb, x, y, w, h, c->text, d->pressed == i + 1 && c->hot, k == 1 || (focus && k == 0), focus, c->enabled);
            } else if (k == 2 || k == 3) {
                gx_rect(fb, x, y + (h - 13) / 2, 13, 13, C_WHITE); gx_sunken(fb, x, y + (h - 13) / 2, 13, 13);
                if (c->checked) draw_check(fb, x + 3, y + (h - 13) / 2 + 3, C_BLACK);
                int tw = tx_width(c->text, 0, 1), ty = y + (h - TXT_H) / 2;
                tx_draw(fb, x + 19, ty, c->text, C_BLACK, 0, 1);
                if (focus) gx_dotrect(fb, x + 17, ty - 1, tw + 4, TXT_H + 1, C_BLACK, C_FACE, 0);
            } else if (k == 4 || k == 9) {
                radio_draw(fb, x, y + (h - 12) / 2, c->checked, c->enabled);
                int tw = tx_width(c->text, 0, 1), ty = y + (h - TXT_H) / 2;
                tx_draw(fb, x + 18, ty, c->text, C_BLACK, 0, 1);
                if (focus) gx_dotrect(fb, x + 16, ty - 1, tw + 4, TXT_H + 1, C_BLACK, C_FACE, 0);
            }
        } else if (c->r.cls == 130) {
            int st = c->r.style & 0x1f;
            if (st == 3 && !strncmp(c->r.text, "ord", 3)) {
                int n = atoi(c->r.text + 3) - 114; const uint32_t *ic = n == 0 ? res_stretchicon0 : n == 1 ? res_stretchicon1 : n == 2 ? res_stretchicon2 : res_stretchicon3;
                gx_argb(fb, x, y, ic, 32, 32, 0, 0, 32, 32, 0, 0);
            } else if (d->on_paint && (c->r.style & 0x1f) == 0xb) d->on_paint(d, fb, c->r.id, x, y, w, h);
            else {
                int tw = tx_width(c->text, 0, 1), tx = x;
                if (st == 1) tx = x + (w - tw) / 2; else if (st == 2) tx = x + w - tw;
                if (c->r.id == 1081 || c->r.id == 1071 || c->r.id == 1072 || c->r.id == 1073) {}
                tx_draw(fb, tx, y + (h - TXT_H) / 2, c->text, C_BLACK, 0, 1);
            }
        } else if (c->r.cls == 129) {
            ed_draw(fb, x, y, w, h < 18 ? 18 : h, c->text, d->caret, d->sel_all && focus, focus);
        }
    }
    if (d->on_paint) d->on_paint(d, fb, -1, d->cx, d->cy, d->cw, d->ch);
}
static int is_tabstop(const DCtl *c) { return c->visible && c->enabled && (c->r.style & 0x10000) && c->r.cls != 130; }
static void dlg_focus_move(Dlg *d, int dir)
{
    int i = d->focus;
    for (int k = 0; k < d->n; k++) {
        i = (i + dir + d->n) % d->n;
        if (is_tabstop(&d->c[i])) { d->focus = i; d->sel_all = 1; d->caret = (int)strlen(d->c[i].text); return; }
    }
}
static void dlg_activate(Dlg *d, int i)
{
    DCtl *c = &d->c[i]; int k = ckind(c);
    if (k == 3) { c->checked = !c->checked; if (d->on_cmd) d->on_cmd(d, c->r.id, c->checked); }
    else if (k == 2) { c->checked = !c->checked; if (d->on_cmd) d->on_cmd(d, c->r.id, c->checked); }
    else if (k == 9 || k == 4) {
        if (k == 9 || k == 4) {
            int g = c->group_start;
            for (int j = 0; j < d->n; j++) { if (d->c[j].group_start == g && (ckind(&d->c[j]) == 9 || ckind(&d->c[j]) == 4)) d->c[j].checked = 0; }
            c->checked = 1;
            if (d->on_cmd) d->on_cmd(d, c->r.id, 1);
        }
    } else if (k == 0 || k == 1) {
        int id = c->r.id;
        if (d->on_cmd) d->on_cmd(d, id, 0);
        if (id == 1) { d->m.done = 1; d->m.result = ID_OK; }
        else if (id == 2) { d->m.done = 1; d->m.result = ID_CANCEL; }
    }
}
static int dlg_event(Modal *m, const PEvent *ev)
{
    Dlg *d = (Dlg *)m->ud; dlg_place(d);
    if (ev->type == EV_MOUSEDOWN && ev->button == BTN_LEFT) {
        Rc cl = { d->dx + d->dw - 21, d->dy + 5, 16, 14 };
        if (rc_in(&cl, ev->x, ev->y)) { m->done = 1; m->result = ID_CANCEL; return 1; }
        for (int i = 0; i < d->n; i++) {
            DCtl *c = &d->c[i]; if (!c->visible || !c->enabled) continue;
            Rc r = { d->cx + c->x, d->cy + c->y, c->w, c->h };
            if (c->r.cls == 128 && ckind(c) == 7) continue;
            if (c->r.cls == 130) continue;
            if (!rc_in(&r, ev->x, ev->y)) continue;
            d->focus = i; d->sel_all = 0;
            int k = ckind(c);
            if (c->r.cls == 129) { c->hot = 1; d->caret = ed_caret_at(c->text, ev->x - r.x - 3); return 1; }
            if (k == 0 || k == 1) { d->pressed = i + 1; c->hot = 1; }
            else dlg_activate(d, i);
            return 1;
        }
    } else if (ev->type == EV_MOUSEMOVE) {
        if (d->pressed) { DCtl *c = &d->c[d->pressed - 1]; Rc r = { d->cx + c->x, d->cy + c->y, c->w, c->h }; c->hot = rc_in(&r, ev->x, ev->y); }
    } else if (ev->type == EV_MOUSEUP && ev->button == BTN_LEFT) {
        if (d->pressed) {
            DCtl *c = &d->c[d->pressed - 1]; int idx = d->pressed - 1; d->pressed = 0;
            if (c->hot) { c->hot = 0; dlg_activate(d, idx); }
        }
    } else if (ev->type == EV_KEYDOWN || ev->type == EV_CHAR) {
        DCtl *f = d->focus >= 0 ? &d->c[d->focus] : NULL;
        if (ev->type == EV_KEYDOWN) {
            if (ev->key == KEY_ESC) { m->done = 1; m->result = ID_CANCEL; return 1; }
            if (ev->key == KEY_ENTER) {
                if (f && (ckind(f) == 0 || ckind(f) == 1) && f->r.id != 1) { dlg_activate(d, d->focus); return 1; }
                for (int i = 0; i < d->n; i++) if (d->c[i].visible && ckind(&d->c[i]) == 1) { dlg_activate(d, i); break; }
                return 1;
            }
            if (ev->key == KEY_TAB) { dlg_focus_move(d, (ev->mods & MOD_SHIFT) ? -1 : 1); return 1; }
            if (f && ev->key == KEY_SPACE && f->r.cls == 128) { dlg_activate(d, d->focus); return 1; }
            if (f && (ckind(f) == 9 || ckind(f) == 4) && (ev->key == KEY_UP || ev->key == KEY_DOWN || ev->key == KEY_LEFT || ev->key == KEY_RIGHT)) {
                int dir = (ev->key == KEY_UP || ev->key == KEY_LEFT) ? -1 : 1, i = d->focus;
                for (int k = 0; k < d->n; k++) {
                    i = (i + dir + d->n) % d->n;
                    if ((ckind(&d->c[i]) == 9 || ckind(&d->c[i]) == 4) && d->c[i].group_start == f->group_start && d->c[i].enabled) { d->focus = i; dlg_activate(d, i); break; }
                }
                return 1;
            }
            if (ev->mods & MOD_ALT || (ev->key >= 'a' && ev->key <= 'z' && (!f || f->r.cls != 129) )) {
                for (int i = 0; i < d->n; i++) {
                    const char *a = strchr(d->c[i].r.text, '&');
                    if (a && tolower((unsigned char)a[1]) == ev->key && d->c[i].visible && d->c[i].r.cls != 130) {
                        if (d->c[i].r.cls == 130) continue;
                        d->focus = i; dlg_activate(d, i); return 1;
                    }
                    if (a && tolower((unsigned char)a[1]) == ev->key && d->c[i].r.cls == 130 && d->c[i].visible) {
                        for (int j = i + 1; j < d->n; j++) if (d->c[j].r.cls == 129 && d->c[j].visible) { d->focus = j; d->sel_all = 1; d->caret = (int)strlen(d->c[j].text); break; }
                        return 1;
                    }
                }
            }
        }
        if (f && f->r.cls == 129) {
            if (ed_event(f->text, 63, &d->caret, &d->sel_all, ev, 1) && d->on_cmd) d->on_cmd(d, f->r.id, -1);
        }
    }
    return 1;
}
int dlg_run(Dlg *d)
{
    d->m.paint = dlg_paint; d->m.event = dlg_event; d->m.ud = d; d->m.result = ID_CANCEL;
    if (d->focus >= 0 && d->c[d->focus].r.cls == 129) d->caret = (int)strlen(d->c[d->focus].text);
    return modal_run(&d->m);
}

/* ================= file dialog ================= */
typedef struct { const char *name; const char *exts; } Filter;
static const Filter open_filters[] = {
    { "All Picture Files", "bmp;dib;png;gif;jpg;jpeg;jpe;jfif" }, { "Bitmap Files (*.bmp;*.dib)", "bmp;dib" },
    { "GIF (*.gif)", "gif" }, { "JPEG (*.jpg;*.jpeg;*.jpe;*.jfif)", "jpg;jpeg;jpe;jfif" }, { "PNG (*.png)", "png" }, { "All Files", "*" } };
static const Filter save_filters[] = {
    { "Monochrome Bitmap (*.bmp;*.dib)", "bmp;dib" }, { "16 Color Bitmap (*.bmp;*.dib)", "bmp;dib" }, { "256 Color Bitmap (*.bmp;*.dib)", "bmp;dib" },
    { "24-bit Bitmap (*.bmp;*.dib)", "bmp;dib" }, { "JPEG (*.jpg;*.jpeg;*.jpe;*.jfif)", "jpg" }, { "GIF (*.gif)", "gif" }, { "PNG (*.png)", "png" }, { "PDF Document (*.pdf)", "pdf" } };
int g_file_filter; /* chosen filter index (for save) */

typedef struct {
    Modal m; int save; char title[32]; char dir[1024]; PDirEnt *ents; int nent; int *view; int nview;
    char name[512]; int caret, sel; int filter; int filter0; int nfilters; const Filter *filters;
    int top, hot; int press; char result[1024]; int focus; /* 0 list,1 name */
    uint32_t last_click; int last_idx; int dragsb;
    int x, y, w, h;
} FileD;
static int ext_match(const char *name, const char *exts)
{
    if (!strcmp(exts, "*")) return 1;
    const char *dot = strrchr(name, '.'); if (!dot) return 0;
    dot++;
    const char *p = exts;
    while (*p) {
        const char *e = strchr(p, ';'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if (strlen(dot) == n && !str_nicmp(dot, p, n)) return 1;
        if (!e) break; p = e + 1;
    }
    return 0;
}
static int cmp_ent(const void *a, const void *b)
{
    const PDirEnt *x = (const PDirEnt *)a, *y = (const PDirEnt *)b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return str_icmp(x->name, y->name);
}
static void fd_scan(FileD *f)
{
    free(f->ents); free(f->view); f->ents = NULL; f->view = NULL; f->nent = 0; f->nview = 0;
    if (plat_listdir(f->dir, &f->ents, &f->nent)) { f->nent = 0; f->ents = NULL; }
    qsort(f->ents, f->nent, sizeof(PDirEnt), cmp_ent);
    f->view = (int *)malloc(sizeof(int) * (f->nent + 1));
    for (int i = 0; i < f->nent; i++) {
        if (f->ents[i].name[0] == '.' && strcmp(f->ents[i].name, "..")) continue;
        if (!f->ents[i].is_dir && !ext_match(f->ents[i].name, f->filters[f->filter].exts)) continue;
        f->view[f->nview++] = i;
    }
    f->top = 0; f->hot = -1;
}
static void fd_geom(FileD *f) { f->w = 520; f->h = 340; f->x = (A.fb->w - f->w) / 2; f->y = (A.fb->h - f->h) / 2; }
#define FD_LIST_Y 52
static int fd_rows(FileD *f) { return (f->h - 3 - 18 - FD_LIST_Y - 84 + 18) / 16; }
static void fd_paint(Modal *m, Img *fb)
{
    FileD *f = (FileD *)m->ud; fd_geom(f);
    draw_window_frame(fb, f->x, f->y, f->w, f->h, f->title);
    int cx = f->x + 3, cy = f->y + 21;
    tx_draw(fb, cx + 8, cy + 11, f->save ? "Save &in:" : "Look &in:", C_BLACK, 0, 1);
    char fit[512]; tx_ellipsis_fit(f->dir, f->w - 8 - 70 - 120, fit, sizeof fit);
    gx_rect(fb, cx + 70, cy + 6, f->w - 6 - 70 - 100, 22, C_WHITE); gx_sunken(fb, cx + 70, cy + 6, f->w - 6 - 70 - 100, 22);
    tx_draw(fb, cx + 75, cy + 11, fit, C_BLACK, 0, 0);
    draw_button(fb, cx + f->w - 6 - 90, cy + 6, 82, 22, "&Up one level", 0, 0, 0, 1);
    int ly = cy + FD_LIST_Y - 18, lh = f->h - 21 - 3 - 90 - (FD_LIST_Y - 18) + 0;
    int lw = f->w - 6 - 16;
    gx_rect(fb, cx + 8, ly, lw, lh, C_WHITE); gx_sunken(fb, cx + 8, ly, lw, lh);
    int rows = (lh - 4) / 16;
    for (int r = 0; r < rows; r++) {
        int idx = f->top + r; if (idx >= f->nview) break;
        PDirEnt *e = &f->ents[f->view[idx]];
        int y = ly + 2 + r * 16, sel = (idx == f->hot);
        char nm[300]; tx_ellipsis_fit(e->name, lw - 120, nm, sizeof nm);
        int tw = tx_width(nm, 0, 0);
        if (sel) gx_rect(fb, cx + 8 + 22, y, tw + 4, 16, C_NAVY);
        /* icon */
        if (e->is_dir) { gx_rect(fb, cx + 12, y + 4, 12, 9, RGB(255, 255, 0)); gx_frame(fb, cx + 12, y + 4, 12, 9, C_BLACK); gx_rect(fb, cx + 12, y + 3, 5, 2, RGB(255, 255, 0)); }
        else { gx_rect(fb, cx + 14, y + 2, 9, 12, C_WHITE); gx_frame(fb, cx + 14, y + 2, 9, 12, C_BLACK); }
        tx_draw(fb, cx + 8 + 24, y + 2, nm, sel ? C_WHITE : C_BLACK, 0, 0);
        if (!e->is_dir) { char sz[32]; snprintf(sz, sizeof sz, "%ld KB", (e->size + 1023) / 1024); tx_draw(fb, cx + 8 + lw - 70, y + 2, sz, C_BLACK, 0, 0); }
    }
    /* scrollbar */
    int sbx = cx + 8 + lw - 17, sbh = lh - 4;
    if (f->nview > rows) {
        gx_rect(fb, sbx, ly + 2, 15, sbh, C_LIGHT);
        int th = sbh * rows / f->nview; if (th < 12) th = 12;
        int ty = ly + 2 + (sbh - th) * f->top / (f->nview - rows);
        gx_rect(fb, sbx, ty, 15, th, C_FACE); gx_raised(fb, sbx, ty, 15, th);
    }
    int by = ly + lh + 8;
    tx_draw(fb, cx + 8, by + 4, "File &name:", C_BLACK, 0, 1);
    ed_draw(fb, cx + 100, by, f->w - 6 - 100 - 100, 22, f->name, f->caret, f->sel && f->focus == 1, f->focus == 1);
    draw_button(fb, cx + f->w - 6 - 90, by, 82, 23, f->save ? "&Save" : "&Open", f->press == 1, 1, 0, 1);
    tx_draw(fb, cx + 8, by + 32, "Files of &type:", C_BLACK, 0, 1);
    gx_rect(fb, cx + 100, by + 28, f->w - 6 - 100 - 100, 22, C_WHITE); gx_sunken(fb, cx + 100, by + 28, f->w - 6 - 100 - 100, 22);
    tx_draw_clip(fb, cx + 105, by + 32, f->filters[f->filter].name, C_BLACK, f->w - 6 - 100 - 100 - 28);
    int ax = cx + f->w - 6 - 100 - 100 + 100 - 17; gx_rect(fb, ax, by + 30, 15, 18, C_FACE); gx_raised(fb, ax, by + 30, 15, 18);
    for (int k = 0; k < 4; k++) gx_hline(fb, ax + 4 + k, ax + 10 - k, by + 36 + k, C_BLACK);
    draw_button(fb, cx + f->w - 6 - 90, by + 28, 82, 23, "Cancel", f->press == 2, 0, 0, 1);
}
static void fd_go(FileD *f, const char *path)
{
    strncpy(f->dir, path, 1023); f->dir[1023] = 0;
    fd_scan(f);
}
static void fd_up(FileD *f)
{
    char *s = strrchr(f->dir, PATH_SEP);
    if (s && s != f->dir) *s = 0; else strcpy(f->dir, "/");
    fd_scan(f);
}
static void fd_accept(FileD *f)
{
    if (!f->name[0]) return;
    if (f->name[0] == '/' ) strncpy(f->result, f->name, 1023);
    else snprintf(f->result, sizeof f->result, "%s%s%s", f->dir, strcmp(f->dir, "/") ? "/" : "", f->name);
    /* directory typed? */
    PDirEnt *e = NULL; int n = 0;
    if (!plat_listdir(f->result, &e, &n)) { free(e); fd_go(f, f->result); f->name[0] = 0; f->caret = 0; return; }
    if (f->save && f->filter != f->filter0) {   /* user picked another type: switch the extension to match */
        char *d = strrchr(f->result, '.'), *sl = strrchr(f->result, '/');
        if (d && (!sl || d > sl)) {
            int ok = 0; const char *ex = f->filters[f->filter].exts, *q = d + 1;
            while (*ex) { size_t k = strcspn(ex, ";"); if (strlen(q) == k && !str_nicmp(q, ex, k)) ok = 1; ex += k; if (*ex == ';') ex++; }
            if (!ok) *d = 0;
        }
    }
    if (f->save && !(strrchr(f->result, '.') > strrchr(f->result, '/'))) {
        int fi = f->filter; const char *ext = f->filters[fi].exts; char e1[8]; int k = 0; while (ext[k] && ext[k] != ';' && k < 7) { e1[k] = ext[k]; k++; } e1[k] = 0;
        strncat(f->result, ".", 1023 - strlen(f->result)); strncat(f->result, e1, 1023 - strlen(f->result));
    }
    f->m.done = 1; f->m.result = ID_OK;
}
static int fd_event(Modal *m, const PEvent *ev)
{
    FileD *f = (FileD *)m->ud; fd_geom(f);
    int cx = f->x + 3, cy = f->y + 21;
    int ly = cy + FD_LIST_Y - 18, lh = f->h - 21 - 3 - 90 - (FD_LIST_Y - 18), lw = f->w - 6 - 16;
    int rows = (lh - 4) / 16, by = ly + lh + 8;
    Rc up = { cx + f->w - 6 - 90, cy + 6, 82, 22 }, list = { cx + 8, ly, lw, lh }, namer = { cx + 100, by, f->w - 6 - 200, 22 };
    Rc okb = { cx + f->w - 6 - 90, by, 82, 23 }, cab = { cx + f->w - 6 - 90, by + 28, 82, 23 }, typer = { cx + 100, by + 28, f->w - 6 - 200 + 0, 22 };
    if (ev->type == EV_WHEEL) { f->top -= ev->button * 3; if (f->top > f->nview - rows) f->top = f->nview - rows; if (f->top < 0) f->top = 0; return 1; }
    if (ev->type == EV_MOUSEDOWN && ev->button == BTN_LEFT) {
        Rc cl = { f->x + f->w - 21, f->y + 5, 16, 14 };
        if (rc_in(&cl, ev->x, ev->y)) { m->done = 1; m->result = ID_CANCEL; }
        else if (rc_in(&up, ev->x, ev->y)) fd_up(f);
        else if (rc_in(&okb, ev->x, ev->y)) f->press = 1;
        else if (rc_in(&cab, ev->x, ev->y)) f->press = 2;
        else if (rc_in(&namer, ev->x, ev->y)) { f->focus = 1; f->sel = 0; f->caret = ed_caret_at(f->name, ev->x - namer.x - 3); }
        else if (rc_in(&typer, ev->x, ev->y)) {
            const char *names[8]; for (int i = 0; i < f->nfilters; i++) names[i] = f->filters[i].name;
            int r = list_popup(names, f->nfilters, typer.x, typer.y + 22, typer.w, f->filter);
            if (r >= 0) { f->filter = r; fd_scan(f); }
        } else if (rc_in(&list, ev->x, ev->y)) {
            f->focus = 0;
            int sbx = cx + 8 + lw - 17;
            if (f->nview > rows && ev->x >= sbx) { f->dragsb = 1; int sbh = lh - 4; f->top = (ev->y - ly - 2) * (f->nview - rows) / sbh; if (f->top < 0) f->top = 0; if (f->top > f->nview - rows) f->top = f->nview - rows; return 1; }
            int idx = f->top + (ev->y - ly - 2) / 16;
            if (idx >= 0 && idx < f->nview) {
                uint32_t now = plat_ticks_ms();
                PDirEnt *e = &f->ents[f->view[idx]];
                int dbl = (idx == f->last_idx && now - f->last_click < 500);
                f->hot = idx; f->last_idx = idx; f->last_click = now;
                if (dbl) {
                    if (e->is_dir) {
                        if (!strcmp(e->name, "..")) fd_up(f);
                        else { char p[1024]; snprintf(p, sizeof p, "%s%s%s", f->dir, strcmp(f->dir, "/") ? "/" : "", e->name); fd_go(f, p); }
                    } else { strncpy(f->name, e->name, 511); fd_accept(f); }
                } else if (!e->is_dir) { strncpy(f->name, e->name, 511); f->caret = (int)strlen(f->name); f->sel = 1; }
            }
        }
    } else if (ev->type == EV_MOUSEMOVE && f->dragsb) {
        int sbh = lh - 4; f->top = (ev->y - ly - 2) * (f->nview - rows) / (sbh ? sbh : 1);
        if (f->top < 0) f->top = 0; if (f->top > f->nview - rows) f->top = f->nview - rows; if (f->top < 0) f->top = 0;
    } else if (ev->type == EV_MOUSEUP) {
        f->dragsb = 0;
        if (f->press == 1 && rc_in(&okb, ev->x, ev->y)) fd_accept(f);
        if (f->press == 2 && rc_in(&cab, ev->x, ev->y)) { m->done = 1; m->result = ID_CANCEL; }
        f->press = 0;
    } else if (ev->type == EV_KEYDOWN || ev->type == EV_CHAR) {
        if (ev->type == EV_KEYDOWN) {
            if (ev->key == KEY_ESC) { m->done = 1; m->result = ID_CANCEL; return 1; }
            if (ev->key == KEY_ENTER) {
                if (f->name[0]) fd_accept(f);
                else if (f->hot >= 0) { PDirEnt *e = &f->ents[f->view[f->hot]]; if (e->is_dir) { char p[1024]; if (!strcmp(e->name, "..")) fd_up(f); else { snprintf(p, sizeof p, "%s%s%s", f->dir, strcmp(f->dir, "/") ? "/" : "", e->name); fd_go(f, p); } } }
                return 1;
            }
            if (ev->key == KEY_TAB) { f->focus = !f->focus; return 1; }
            if (f->focus == 0) {
                if (ev->key == KEY_DOWN && f->hot < f->nview - 1) f->hot++;
                if (ev->key == KEY_UP && f->hot > 0) f->hot--;
                if (f->hot >= 0) { if (f->hot < f->top) f->top = f->hot; if (f->hot >= f->top + rows) f->top = f->hot - rows + 1; }
                if (f->hot >= 0 && !f->ents[f->view[f->hot]].is_dir && (ev->key == KEY_UP || ev->key == KEY_DOWN)) { strncpy(f->name, f->ents[f->view[f->hot]].name, 511); f->caret = (int)strlen(f->name); f->sel = 1; }
                if (ev->key == KEY_BACKSPACE) fd_up(f);
                return 1;
            }
        }
        if (f->focus == 1) ed_event(f->name, 500, &f->caret, &f->sel, ev, 0);
    }
    return 1;
}
static char lastdir[1024];
static int native_file_dialog(int save, const char *title, char *path, int pathn, int filter_mode)
{
    /* the desktop's own dialog; save offers the useful types (bit-depth BMP variants collapse into 24-bit BMP) */
    static const int smap[5] = { 4, 6, 7, 3, 5 };                 /* JPEG, PNG, PDF, BMP, GIF -> save_filters index */
    const char *names[8], *pats[8]; static char pbuf[8][96]; int map[8], n = 0;
    const Filter *src = save ? save_filters : open_filters; int cnt = save ? 5 : 6;
    for (int i = 0; i < cnt; i++) {
        int si = save ? smap[i] : i; map[n] = si;
        names[n] = save ? (si == 3 ? "Bitmap (*.bmp;*.dib)" : src[si].name) : src[si].name;
        const char *e = src[si].exts; char *o = pbuf[n]; *o = 0;
        if (!strcmp(e, "*")) strcpy(o, "*");
        else { while (*e) { size_t k = strcspn(e, ";"); o += snprintf(o, 96 - (size_t)(o - pbuf[n]), "%s*.%.*s", o == pbuf[n] ? "" : ";", (int)k, e); e += k; if (*e == ';') e++; } }
        pats[n] = pbuf[n]; n++;
    }
    char p[1100]; const char *name = "";
    if (path && *path) { snprintf(p, sizeof p, "%s", path); }
    else { snprintf(p, sizeof p, "%s/", lastdir[0] ? lastdir : plat_home_dir()); }
    (void)name;
    if (!strchr(p, PATH_SEP) ) { char t[1100]; snprintf(t, sizeof t, "%s/%s", lastdir[0] ? lastdir : plat_home_dir(), p); snprintf(p, sizeof p, "%s", t); }
    int fi = 0; for (int i = 0; i < n; i++) if (map[i] == filter_mode) fi = i;
    int r = plat_file_dialog(save, title, p, (int)sizeof p, names, pats, n, &fi);
    if (r <= 0) return r;
    char *sl = strrchr(p, PATH_SEP); if (sl) { size_t d = (size_t)(sl - p); if (d > 1023) d = 1023; memcpy(lastdir, p, d ? d : 1); lastdir[d ? d : 1] = 0; }
    int ext_given = strrchr(p, '.') && (!sl || strrchr(p, '.') > sl);
    int flt = filter_mode;
    if (save) {
        if (ext_given) {
            const char *d = strrchr(p, '.') + 1;
            if (!str_icmp(d, "jpg") || !str_icmp(d, "jpeg") || !str_icmp(d, "jpe") || !str_icmp(d, "jfif")) flt = 4;
            else if (!str_icmp(d, "png")) flt = 6; else if (!str_icmp(d, "gif")) flt = 5; else if (!str_icmp(d, "pdf")) flt = 7;
            else if (!str_icmp(d, "bmp") || !str_icmp(d, "dib")) flt = 3;
        } else {
            if (fi >= 0 && fi < n) flt = map[fi];
            size_t l = strlen(p); const char *e = flt == 6 ? ".png" : flt == 5 ? ".gif" : flt == 7 ? ".pdf" : flt == 3 ? ".bmp" : ".jpg";
            if (l + 5 < sizeof p) strcat(p, e);
        }
        g_file_filter = flt;
    }
    strncpy(path, p, (size_t)pathn - 1); path[pathn - 1] = 0;
    return 1;
}
int file_dialog(int save, const char *title, char *path, int pathn, int filter_mode)
{
    { int nr = native_file_dialog(save, title, path, pathn, filter_mode); if (nr >= 0) return nr; }
    FileD *f = (FileD *)calloc(1, sizeof(FileD));
    f->save = save; strncpy(f->title, title, 31);
    f->filters = save ? save_filters : open_filters; f->nfilters = save ? 8 : 6;
    f->filter = save ? (filter_mode >= 0 && filter_mode < 8 ? filter_mode : 4) : 0;
    f->filter0 = f->filter; f->focus = 1; f->sel = 1;
    if (path && *path) {
        char *s = strrchr(path, PATH_SEP);
        if (s) { size_t n = (size_t)(s - path); if (!n) n = 1; memcpy(f->dir, path, n); f->dir[n] = 0; strncpy(f->name, s + 1, 511); }
        else strncpy(f->name, path, 511);
    }
    if (!f->dir[0]) { if (lastdir[0]) strcpy(f->dir, lastdir); else { if (plat_getcwd(f->dir, 1023)) strcpy(f->dir, plat_home_dir()); } }
    f->caret = (int)strlen(f->name);
    fd_scan(f);
    f->m.paint = fd_paint; f->m.event = fd_event; f->m.ud = f;
    int r = modal_run(&f->m);
    int ok = 0;
    if (r == ID_OK) { strncpy(path, f->result, pathn - 1); path[pathn - 1] = 0; g_file_filter = f->filter; ok = 1; strncpy(lastdir, f->dir, 1023); }
    free(f->ents); free(f->view); free(f);
    return ok;
}

/* ================= Edit Colors (Windows "Color" common dialog) ================= */
static const px_t basic48[48] = {
    0xFF8080, 0xFFFF80, 0x80FF80, 0x00FF80, 0x80FFFF, 0x0080FF, 0xFF80C0, 0xFF80FF,
    0xFF0000, 0xFFFF00, 0x80FF00, 0x00FF40, 0x00FFFF, 0x0080C0, 0x8080C0, 0xFF00FF,
    0x804040, 0xFF8040, 0x00FF00, 0x008080, 0x004080, 0x8080FF, 0x800040, 0xFF0080,
    0x800000, 0xFF8000, 0x008000, 0x008040, 0x0000FF, 0x0000A0, 0x800080, 0x8000FF,
    0x400000, 0x804000, 0x004000, 0x004040, 0x000080, 0x000040, 0x400040, 0x400080,
    0x000000, 0x808000, 0x808040, 0x808080, 0x408080, 0xC0C0C0, 0x400040, 0xFFFFFF };
typedef struct {
    Modal m; px_t color; int expanded; int h, s, l; /* 0..239 / 0..240 */
    int sel_basic, sel_custom; int drag; int focus; char fld[6][8]; int caret, sel_all;
    int x, y, w, h_;
} ColD;
static void rgb2hsl(px_t c, int *h, int *s, int *l)
{
    double r = PR(c) / 255.0, g = PG(c) / 255.0, b = PB(c) / 255.0, mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b));
    double L = (mx + mn) / 2, S = 0, H = 0;
    if (mx != mn) {
        double d = mx - mn; S = L > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
        if (mx == r) H = (g - b) / d + (g < b ? 6 : 0); else if (mx == g) H = (b - r) / d + 2; else H = (r - g) / d + 4;
        H /= 6;
    }
    *h = (int)(H * 240 + 0.5) % 240; *s = (int)(S * 240 + 0.5); *l = (int)(L * 240 + 0.5);
}
static double hue2rgb(double p, double q, double t)
{
    if (t < 0) t += 1; if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t; if (t < 0.5) return q; if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6; return p;
}
static px_t hsl2rgb(int h, int s, int l)
{
    double H = h / 240.0, S = s / 240.0, L = l / 240.0, r, g, b;
    if (S == 0) r = g = b = L;
    else { double q = L < 0.5 ? L * (1 + S) : L + S - L * S, p = 2 * L - q; r = hue2rgb(p, q, H + 1.0 / 3); g = hue2rgb(p, q, H); b = hue2rgb(p, q, H - 1.0 / 3); }
    return RGB((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
}
static void cd_sync(ColD *c)
{
    snprintf(c->fld[0], 8, "%d", c->h); snprintf(c->fld[1], 8, "%d", c->s); snprintf(c->fld[2], 8, "%d", c->l);
    snprintf(c->fld[3], 8, "%d", PR(c->color)); snprintf(c->fld[4], 8, "%d", PG(c->color)); snprintf(c->fld[5], 8, "%d", PB(c->color));
}
static void cd_set_rgb(ColD *c, px_t col) { c->color = col; rgb2hsl(col, &c->h, &c->s, &c->l); cd_sync(c); }
static void cd_geom(ColD *c) { c->w = c->expanded ? 480 : 270; c->h_ = 330; c->x = (A.fb->w - c->w) / 2; c->y = (A.fb->h - c->h_) / 2; }
static void cd_paint(Modal *m, Img *fb)
{
    ColD *c = (ColD *)m->ud; cd_geom(c);
    draw_window_frame(fb, c->x, c->y, c->w, c->h_, "Color");
    int ox = c->x + 3, oy = c->y + 21;
    tx_draw(fb, ox + 6, oy + 6, "&Basic colors:", C_BLACK, 0, 1);
    for (int i = 0; i < 48; i++) {
        int gx = ox + 6 + (i % 8) * 30, gy = oy + 22 + (i / 8) * 24;
        gx_rect(fb, gx, gy, 28, 22, basic48[i]); gx_sunken(fb, gx, gy, 28, 22);
        if (i == c->sel_basic) gx_frame(fb, gx - 1, gy - 1, 30, 24, C_BLACK);
    }
    tx_draw(fb, ox + 6, oy + 172, "&Custom colors:", C_BLACK, 0, 1);
    for (int i = 0; i < 16; i++) {
        int gx = ox + 6 + (i % 8) * 30, gy = oy + 188 + (i / 8) * 24;
        gx_rect(fb, gx, gy, 28, 22, A.custom[i]); gx_sunken(fb, gx, gy, 28, 22);
        if (i == c->sel_custom) gx_frame(fb, gx - 1, gy - 1, 30, 24, C_BLACK);
    }
    draw_button(fb, ox + 6, oy + 240, 244, 23, "&Define Custom Colors >>", 0, 0, 0, !c->expanded);
    draw_button(fb, ox + 6, oy + 272, 75, 23, "OK", 0, 1, 0, 1);
    draw_button(fb, ox + 90, oy + 272, 75, 23, "Cancel", 0, 0, 0, 1);
    if (!c->expanded) return;
    int sx = ox + 266, sy = oy + 6;
    gx_sunken(fb, sx - 2, sy - 2, 184, 184);
    for (int y = 0; y < 180; y++) for (int x = 0; x < 180; x++) img_put(fb, sx + x, sy + y, hsl2rgb(x * 240 / 180, 240 - y * 240 / 180, c->l));
    int hx = sx + c->h * 180 / 240, hy = sy + (240 - c->s) * 180 / 240;
    gx_hline(fb, hx - 6, hx - 2, hy, C_BLACK); gx_hline(fb, hx + 2, hx + 6, hy, C_BLACK); gx_vline(fb, hx, hy - 6, hy - 2, C_BLACK); gx_vline(fb, hx, hy + 2, hy + 6, C_BLACK);
    int lx = sx + 194;
    gx_sunken(fb, lx - 2, sy - 2, 14, 184);
    for (int y = 0; y < 180; y++) gx_rect(fb, lx, sy + y, 10, 1, hsl2rgb(c->h, c->s, 240 - y * 240 / 180));
    int ly = sy + (240 - c->l) * 180 / 240;
    for (int k = 0; k < 5; k++) gx_vline(fb, lx + 12 + k, ly - k, ly + k, C_BLACK);
    gx_rect(fb, sx, sy + 196, 60, 40, c->color); gx_sunken(fb, sx, sy + 196, 60, 40);
    tx_draw(fb, sx + 66, sy + 200, "Color", C_BLACK, 0, 0); tx_draw(fb, sx + 66, sy + 216, "Solid", C_BLACK, 0, 0);
    static const char *lab[6] = { "Hu&e:", "&Sat:", "&Lum:", "&Red:", "&Green:", "Bl&ue:" };
    for (int i = 0; i < 6; i++) {
        int fx = sx + 110 + (i / 3) * 92, fy = sy + 196 + (i % 3) * 24;
        tx_draw(fb, fx, fy + 4, lab[i], C_BLACK, 0, 1);
        ed_draw(fb, fx + 36, fy, 36, 20, c->fld[i], c->caret, c->sel_all && c->focus == i, c->focus == i);
    }
    draw_button(fb, ox + 90 + 85, oy + 272, 150, 23, "&Add to Custom Colors", 0, 0, 0, 1);
}
static int cd_event(Modal *m, const PEvent *ev)
{
    ColD *c = (ColD *)m->ud; cd_geom(c);
    int ox = c->x + 3, oy = c->y + 21, sx = ox + 266, sy = oy + 6;
    if (ev->type == EV_MOUSEDOWN || (ev->type == EV_MOUSEMOVE && c->drag)) {
        int x = ev->x, y = ev->y;
        if (ev->type == EV_MOUSEDOWN) {
            Rc cl = { c->x + c->w - 21, c->y + 5, 16, 14 }; if (rc_in(&cl, x, y)) { m->done = 1; m->result = ID_CANCEL; return 1; }
            for (int i = 0; i < 48; i++) { Rc r = { ox + 6 + (i % 8) * 30, oy + 22 + (i / 8) * 24, 28, 22 }; if (rc_in(&r, x, y)) { c->sel_basic = i; c->sel_custom = -1; cd_set_rgb(c, basic48[i]); } }
            for (int i = 0; i < 16; i++) { Rc r = { ox + 6 + (i % 8) * 30, oy + 188 + (i / 8) * 24, 28, 22 }; if (rc_in(&r, x, y)) { c->sel_custom = i; c->sel_basic = -1; cd_set_rgb(c, A.custom[i]); } }
            Rc def = { ox + 6, oy + 240, 244, 23 }, ok = { ox + 6, oy + 272, 75, 23 }, cn = { ox + 90, oy + 272, 75, 23 }, add = { ox + 175, oy + 272, 150, 23 };
            if (rc_in(&def, x, y) && !c->expanded) { c->expanded = 1; cd_set_rgb(c, c->color); }
            if (rc_in(&ok, x, y)) { m->done = 1; m->result = ID_OK; }
            if (rc_in(&cn, x, y)) { m->done = 1; m->result = ID_CANCEL; }
            if (c->expanded && rc_in(&add, x, y)) { int i = c->sel_custom >= 0 ? c->sel_custom : 0; A.custom[i] = c->color; c->sel_custom = i; }
            if (c->expanded) {
                Rc sp = { sx, sy, 180, 180 }, lm = { sx + 194, sy, 10, 180 };
                if (rc_in(&sp, x, y)) c->drag = 1; else if (rc_in(&lm, x, y)) c->drag = 2;
                for (int i = 0; i < 6; i++) { Rc r = { sx + 110 + (i / 3) * 92 + 36, sy + 196 + (i % 3) * 24, 36, 20 }; if (rc_in(&r, x, y)) { c->focus = i; c->sel_all = 1; c->caret = (int)strlen(c->fld[i]); } }
            }
        }
        if (c->drag == 1) { int h = (x - sx) * 240 / 180, s = 240 - (y - sy) * 240 / 180; c->h = h < 0 ? 0 : h > 239 ? 239 : h; c->s = s < 0 ? 0 : s > 240 ? 240 : s; c->color = hsl2rgb(c->h, c->s, c->l); cd_sync(c); }
        if (c->drag == 2) { int l = 240 - (y - sy) * 240 / 180; c->l = l < 0 ? 0 : l > 240 ? 240 : l; c->color = hsl2rgb(c->h, c->s, c->l); cd_sync(c); }
    } else if (ev->type == EV_MOUSEUP) c->drag = 0;
    else if (ev->type == EV_KEYDOWN || ev->type == EV_CHAR) {
        if (ev->type == EV_KEYDOWN && ev->key == KEY_ESC) { m->done = 1; m->result = ID_CANCEL; return 1; }
        if (ev->type == EV_KEYDOWN && ev->key == KEY_ENTER) { m->done = 1; m->result = ID_OK; return 1; }
        if (c->expanded && ed_event(c->fld[c->focus], 7, &c->caret, &c->sel_all, ev, 1)) {
            int v[6]; for (int i = 0; i < 6; i++) v[i] = atoi(c->fld[i]);
            if (c->focus < 3) { c->h = v[0] % 240; c->s = v[1] > 240 ? 240 : v[1]; c->l = v[2] > 240 ? 240 : v[2]; c->color = hsl2rgb(c->h, c->s, c->l); }
            else { for (int i = 3; i < 6; i++) if (v[i] > 255) v[i] = 255; c->color = RGB(v[3], v[4], v[5]); rgb2hsl(c->color, &c->h, &c->s, &c->l); }
        }
    }
    return 1;
}
int color_dialog(px_t *color)
{
    ColD *c = (ColD *)calloc(1, sizeof(ColD));
    c->color = *color; c->sel_basic = c->sel_custom = -1;
    for (int i = 0; i < 48; i++) if (basic48[i] == *color) c->sel_basic = i;
    rgb2hsl(*color, &c->h, &c->s, &c->l); cd_sync(c);
    c->m.paint = cd_paint; c->m.event = cd_event; c->m.ud = c;
    int r = modal_run(&c->m);
    if (r == ID_OK) *color = c->color;
    free(c);
    return r == ID_OK;
}
