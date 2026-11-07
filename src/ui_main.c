/* ui_main.c - main window: layout, painting, input dispatch, main loop (portable) */
#include <ctype.h>
#include <math.h>
#include "app.h"

Rc R_menubar, R_toolbox, R_opts, R_colorbox, R_status, R_work, R_view, R_hscroll, R_vscroll, R_textbar;
int g_mx, g_my, g_min, g_quit;
static int g_sx, g_sy, g_have_mouse;      /* screen mouse */
static void *g_cursors[16];
static int g_cur_known = -2;
static int hot_tool = -1, hot_menu = -1; static uint32_t hot_since; static int tip_x, tip_y;
static int dragging;                      /* mouse captured by canvas tool */
static struct { int active, which, w, h; } RZ;   /* canvas resize drag */
static struct { int drag, vert, start, startpos; } SB;
static int pal_click_idx = -1; static uint32_t pal_click_t;
static int need_paint = 1;
static int g_menu_titles_n; static char g_titles[8][32]; static int g_tx[8], g_tw[8];
static uint32_t last_click_t; static int last_click_x, last_click_y;
extern const char *tool_cursor_for(int ix, int iy, int *idx);

static int fdiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
void ui_request_paint(void) { need_paint = 1; }
void set_status_help(const char *s) { strncpy(A.status_help, s, 255); A.status_help[255] = 0; }

/* ---------------- view ---------------- */
static int content_w(void) { return z_i2s(A.img->w) + 3 + 8; }
static int content_h(void) { return z_i2s(A.img->h) + 3 + 8; }
int view_ox(void) { return R_view.x + 3 - A.sx; }
int view_oy(void) { return R_view.y + 3 - A.sy; }
void view_clamp(void)
{
    int mx = content_w() - R_view.w, my = content_h() - R_view.h;
    if (mx < 0) mx = 0; if (my < 0) my = 0;
    if (A.sx > mx) A.sx = mx; if (A.sy > my) A.sy = my; if (A.sx < 0) A.sx = 0; if (A.sy < 0) A.sy = 0;
}
void view_center_on(int ix, int iy)
{
    A.sx = z_i2s(ix) + 3 - R_view.w / 2; A.sy = z_i2s(iy) + 3 - R_view.h / 2;
    view_clamp();
}
static void zoom_at(int pct, int sx, int sy);
#define COLORBOX_H 267
void ui_relayout(void)
{
    int ww = A.fb->w, wh = A.fb->h, yb = wh;
    R_menubar = (Rc){ 0, 0, ww, 19 };
    if (A.show_status) { R_status = (Rc){ 0, wh - 22, ww, 22 }; yb -= 22; } else R_status = (Rc){ 0, wh, ww, 0 };
    /* left column: toolbox on top, the colour palette (rotated 90 degrees clockwise) underneath */
    int wx = (A.show_toolbox || A.show_colorbox) ? 56 : 0, cb = A.show_colorbox ? COLORBOX_H : 0;
    if (A.show_colorbox) R_colorbox = (Rc){ 0, yb - cb, 56, cb }; else R_colorbox = (Rc){ 0, yb, 0, 0 };
    if (A.show_toolbox) R_toolbox = (Rc){ 0, 19, 56, yb - cb - 19 }; else R_toolbox = (Rc){ 0, 19, 0, 0 };
    int wy = 19;
    if (A.show_text && A.tool == T_TEXT) { R_textbar = (Rc){ wx, 19, ww - wx, 30 }; wy += 30; } else R_textbar = (Rc){ 0, 0, 0, 0 };
    R_work = (Rc){ wx, wy, ww - wx, yb - wy };
    int ix = R_work.x + 2, iy = R_work.y + 2, iw = R_work.w - 4, ih = R_work.h - 4;
    int hs = 0, vs = 0, vw = iw, vh = ih;
    for (int k = 0; k < 3; k++) { vw = iw - (vs ? 17 : 0); vh = ih - (hs ? 17 : 0); vs = content_h() > vh; hs = content_w() > vw; }
    R_view = (Rc){ ix, iy, vw, vh };
    R_hscroll = hs ? (Rc){ ix, iy + vh, vw, 17 } : (Rc){ 0, 0, 0, 0 };
    R_vscroll = vs ? (Rc){ ix + vw, iy, 17, vh } : (Rc){ 0, 0, 0, 0 };
    view_clamp();
    need_paint = 1;
}
void update_title(void)
{
    char t[1100]; const char *nm = A.path[0] ? (strrchr(A.path, '/') ? strrchr(A.path, '/') + 1 : A.path) : "untitled";
    snprintf(t, sizeof t, "%s - SmPaint", nm);
    plat_set_title(t);
}

/* ---------------- menu state ---------------- */
void menu_state(int id, int *en, int *ck)
{
    *en = 1; *ck = 0;
    switch (id) {
    case CMD_UNDO: *en = A.nundo > 0 || A.sel.active; break;
    case CMD_COPY: case CMD_CUT: if (A.tb.active) { *en = text_has_sel(); break; } *en = A.sel.active; break;
    case CMD_REPEAT: *en = A.nredo > 0; break;
    case CMD_CLEARSEL: *en = A.sel.active || A.tb.active; break;
    case CMD_COPYTO: *en = A.sel.active; break;
    case CMD_TOOLBOX: *ck = A.show_toolbox; break;
    case CMD_COLORBOX: *ck = A.show_colorbox; break;
    case CMD_STATUSBAR: *ck = A.show_status; break;
    case CMD_TEXTBAR: *ck = A.show_text; break;
    case CMD_GRID: *ck = A.show_grid; break;
    case CMD_THUMB: *ck = A.show_thumb; break;
    case CMD_OPAQUE: *ck = !A.o_trans; break;
    case CMD_ZOOM_NORMAL: *ck = A.zp == 100; break;
    case CMD_ZOOM_LARGE: *ck = A.zp == 400; break;
    case CMD_RECENT: *en = 0; break;
    }
}

/* ---------------- toolbox geometry ---------------- */
static Rc tool_rect(int i) { return (Rc){ R_toolbox.x + 2 + (i % 2) * 26, R_toolbox.y + 3 + (i / 2) * 26, 26, 26 }; }
static Rc opts_rect(void) { return (Rc){ R_toolbox.x + 7, R_toolbox.y + 3 + 8 * 26 + 4, 42, 66 }; }
static int has_shapew(int tool) { return tool == T_RECT || tool == T_ELLIPSE || tool == T_RRECT || tool == T_POLY; }
static Rc opts2_rect(void) { Rc o = opts_rect(); return (Rc){ o.x, o.y + o.h + 4, o.w, o.h }; }
static Rc opt2_item(int i) { Rc o = opts2_rect(); int ih = (o.h - 4) / 6; return (Rc){ o.x + 2, o.y + 2 + i * ih, o.w - 4, ih }; }
static int opt_count(int tool)
{
    switch (tool) {
    case T_SELECT: case T_FREESEL: case T_TEXT: return 2;
    case T_ERASER: case T_MAG: return 4;
    case T_BRUSH: return 12;
    case T_AIR: return 3;
    case T_LINE: case T_CURVE: return 5;
    case T_RECT: case T_POLY: case T_ELLIPSE: case T_RRECT: return 3;
    }
    return 0;
}
static Rc opt_item(int tool, int i)
{
    Rc o = opts_rect(); int ix = o.x + 2, iy = o.y + 2, iw = o.w - 4, ih = o.h - 4, n = opt_count(tool);
    if (tool == T_BRUSH) return (Rc){ ix + (i % 3) * (iw / 3), iy + (i / 3) * (ih / 4), iw / 3, ih / 4 };
    return (Rc){ ix, iy + i * (ih / n), iw, ih / n };
}
static int *opt_ptr(int tool)
{
    switch (tool) {
    case T_SELECT: case T_FREESEL: case T_TEXT: return &A.o_trans;
    case T_ERASER: return &A.o_eraser; case T_MAG: return &A.o_mag; case T_BRUSH: return &A.o_brush; case T_AIR: return &A.o_air;
    case T_LINE: case T_CURVE: return &A.o_line;
    default: return &A.o_fill;
    }
}
static const int tool_str[T_COUNT] = { 621, 620, 637, 623, 639, 638, 636, 640, 627, 622, 624, 628, 641, 632, 643, 634 };

static void draw_glyph(Img *fb, int idx, int x, int y)
{
    gx_argb(fb, x, y, res_toolglyph, 448, 16, idx * 16, 0, 16, 16, 0, 0);
}
static void draw_dither(Img *fb, int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) img_put(fb, x + i, y + j, ((i + j) & 1) ? C_HILITE : C_FACE);
}
static void paint_options(Img *fb)
{
    Rc o = opts_rect();
    if (o.y + o.h > R_toolbox.y + R_toolbox.h) return;
    gx_sunken1(fb, o.x, o.y, o.w, o.h);
    gx_rect(fb, o.x + 1, o.y + 1, o.w - 2, o.h - 2, C_FACE);
    if (has_shapew(A.tool)) {
        Rc o2 = opts2_rect();
        if (o2.y + o2.h <= R_toolbox.y + R_toolbox.h) {
            gx_sunken1(fb, o2.x, o2.y, o2.w, o2.h); gx_rect(fb, o2.x + 1, o2.y + 1, o2.w - 2, o2.h - 2, C_FACE);
            for (int i = 0; i < 6; i++) {
                Rc r = opt2_item(i); int sel = (i == A.o_shapew), lw = SHAPE_W[i];
                if (sel) gx_rect(fb, r.x, r.y, r.w, r.h, C_NAVY);
                gx_rect(fb, r.x + 4, r.y + (r.h - lw) / 2, r.w - 8, lw, sel ? C_WHITE : C_BLACK);
            }
        }
    }
    int n = opt_count(A.tool); if (!n) return;
    int cur = *opt_ptr(A.tool);
    if (A.tool == T_SELECT || A.tool == T_FREESEL || A.tool == T_TEXT) cur = A.o_trans;
    for (int i = 0; i < n; i++) {
        Rc r = opt_item(A.tool, i); int sel = (i == cur);
        if (sel) gx_rect(fb, r.x, r.y, r.w, r.h, C_NAVY);
        px_t fgc = sel ? C_WHITE : C_BLACK;
        switch (A.tool) {
        case T_SELECT: case T_FREESEL: case T_TEXT: {
            /* icons 36x23: left = opaque, right = transparent */
            int ox = r.x + (r.w - 36) / 2, oy = r.y + (r.h - 23) / 2;
            if (sel) gx_rect(fb, r.x, r.y, r.w, r.h, C_NAVY); else gx_rect(fb, r.x, r.y, r.w, r.h, C_FACE);
            if (oy < r.y) oy = r.y;
            gx_argb(fb, ox, oy, res_seltrans, 72, 23, i == 0 ? 0 : 36, 0, 36, 23 > r.h ? r.h : 23, 0, 0);
            if (sel) gx_frame(fb, r.x, r.y, r.w, r.h, C_BLACK);
            break; }
        case T_ERASER: { int s = ((const int[4]){4,6,8,10})[i]; gx_rect(fb, r.x + (r.w - s) / 2, r.y + (r.h - s) / 2, s, s, fgc); break; }
        case T_MAG: { static const int cx[4] = { 0, 23, 46, 69 }; gx_argb_recolor(fb, r.x + (r.w - 23) / 2, r.y + (r.h - 9) / 2 + 1, res_zoomlabels, 92, 9, cx[i], 0, 23, 9, 0, 0); if (sel) { /* invert */ for (int jj = 0; jj < 9; jj++) for (int ii = 0; ii < 23; ii++) { int px = r.x + (r.w - 23) / 2 + ii, py = r.y + (r.h - 9) / 2 + 1 + jj; if (img_get(fb, px, py) == C_BLACK) img_put(fb, px, py, C_WHITE); } } break; }
        case T_BRUSH: {
            static const int shp[12] = { 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3 }, siz[12] = { 7, 4, 1, 8, 5, 2, 8, 5, 2, 8, 5, 2 };
            if (shp[i] == 0) { for (int j = 0; j < siz[i]; j++) for (int k = 0; k < siz[i]; k++) { double dx = k + 0.5 - siz[i] / 2.0, dy = j + 0.5 - siz[i] / 2.0; if (siz[i] == 1 || dx * dx + dy * dy <= siz[i] * siz[i] / 4.0) img_put(fb, r.x + r.w / 2 - siz[i] / 2 + k, r.y + r.h / 2 - siz[i] / 2 + j, fgc); } }
            else cv_brush(fb, r.x + r.w / 2, r.y + r.h / 2, shp[i], siz[i], fgc);
            break; }
        case T_AIR: { int oy = r.y + (r.h - 18) / 2; for (int j = 0; j < 18; j++) for (int k = 0; k < 24; k++) { uint32_t v = res_airbrush[(j + 3) * 72 + i * 24 + k]; if ((v & 0xffffff) == 0) img_put(fb, r.x + (r.w - 24) / 2 + k, oy + j, fgc); } break; }
        case T_LINE: case T_CURVE: { int lw = i + 1; gx_rect(fb, r.x + 4, r.y + (r.h - lw) / 2, r.w - 8, lw, fgc); break; }
        default: {
            static const int g1[4] = { 16, 22, 20, 18 };   /* rect, poly, ellipse, rrect: outline+fill */
            int kind = A.tool == T_RECT ? 0 : A.tool == T_POLY ? 1 : A.tool == T_ELLIPSE ? 2 : 3;
            int cx = r.x + (r.w - 16) / 2, cy = r.y + (r.h - 16) / 2;
            if (i == 0) {
                /* outline only */
                if (kind == 0) gx_frame(fb, cx + 1, cy + 3, 14, 10, fgc);
                else if (kind == 1) { Pt p[4] = { { cx + 4, cy + 3 }, { cx + 13, cy + 3 }, { cx + 5, cy + 12 }, { cx + 1, cy + 8 } }; cv_poly(fb, p, 4, 1, 0, fgc, 0, 1); }
                else if (kind == 2) cv_shape(fb, SH_ELLIPSE, cx + 1, cy + 3, cx + 14, cy + 12, 0, fgc, 0, 1);
                else cv_shape(fb, SH_RRECT, cx + 1, cy + 3, cx + 14, cy + 12, 0, fgc, 0, 1);
            } else {
                int gi = g1[kind] + (i == 2 ? 1 : 0);
                draw_glyph(fb, gi, cx, cy);
            }
            break; }
        }
    }
}
/* - / + zoom buttons at the right of the menu bar: each click changes the zoom by 5% */
static Rc zoom_btn(int plus)
{
    int x0 = g_menu_titles_n ? g_tx[g_menu_titles_n - 1] + g_tw[g_menu_titles_n - 1] + 24 : 260;
    return (Rc){ x0 + (plus ? 40 : 0), 2, 36, 15 };
}
static void paint_zoom_btns(Img *fb)
{
    for (int p = 0; p < 2; p++) {
        Rc r = zoom_btn(p); gx_rect(fb, r.x, r.y, r.w, r.h, C_FACE); gx_raised(fb, r.x, r.y, r.w, r.h);
        gx_rect(fb, r.x + r.w / 2 - 4, r.y + r.h / 2, 9, 1, C_BLACK);
        if (p) gx_rect(fb, r.x + r.w / 2, r.y + r.h / 2 - 4, 1, 9, C_BLACK);
    }
}
static void paint_toolbox(Img *fb)
{
    if (!R_toolbox.w) return;
    gx_rect(fb, R_toolbox.x, R_toolbox.y, R_toolbox.w, R_toolbox.h, C_FACE);
    for (int i = 0; i < T_COUNT; i++) {
        Rc r = tool_rect(i); int sel = (i == A.tool);
        gx_rect(fb, r.x, r.y, r.w, r.h, C_FACE);
        if (sel) { draw_dither(fb, r.x + 2, r.y + 2, r.w - 4, r.h - 4); gx_sunken(fb, r.x, r.y, r.w, r.h); }
        else gx_raised(fb, r.x, r.y, r.w, r.h);
        draw_glyph(fb, i, r.x + 5 + sel, r.y + 5 + sel);
    }
    paint_options(fb);
}

/* ---------------- color box ---------------- */
static Rc ind_rect(void) { return (Rc){ R_colorbox.x + 12, R_colorbox.y + 5, 31, 31 }; }
static Rc pal_rect(int i) { return (Rc){ R_colorbox.x + 12 + (1 - i / 14) * 16, R_colorbox.y + 42 + (i % 14) * 16, 16, 16 }; }
static void paint_colorbox(Img *fb)
{
    if (!R_colorbox.h) return;
    gx_rect(fb, R_colorbox.x, R_colorbox.y, R_colorbox.w, R_colorbox.h, C_FACE);
    gx_hline(fb, R_colorbox.x, R_colorbox.x + R_colorbox.w - 1, R_colorbox.y, C_HILITE);
    Rc ind = ind_rect();
    gx_sunken(fb, ind.x, ind.y, ind.w, ind.h); gx_rect(fb, ind.x + 2, ind.y + 2, ind.w - 4, ind.h - 4, C_FACE);
    gx_rect(fb, ind.x + 11, ind.y + 11, 16, 16, A.bg); gx_sunken1(fb, ind.x + 11, ind.y + 11, 16, 16);
    gx_rect(fb, ind.x + 4, ind.y + 4, 16, 16, A.fg); gx_sunken1(fb, ind.x + 4, ind.y + 4, 16, 16);
    for (int i = 0; i < 28; i++) {
        Rc r = pal_rect(i);
        gx_rect(fb, r.x, r.y, r.w, r.h, A.pal[i]);
        gx_sunken(fb, r.x, r.y, r.w, r.h);
        gx_rect(fb, r.x + 2, r.y + 2, r.w - 4, r.h - 4, A.pal[i]);
    }
}

/* ---------------- status bar ---------------- */
static void draw_icon1(Img *fb, int x, int y, const uint32_t *ic)
{
    for (int j = 0; j < 15; j++) for (int i = 0; i < 15; i++) if ((ic[j * 15 + i] & 0xffffff) == 0) img_put(fb, x + i, y + j, C_BLACK);
}
static void paint_status_impl(Img *fb);
static void paint_status(Img *fb) { int pb = tx_big(1); paint_status_impl(fb); tx_big(pb); }
static void paint_status_impl(Img *fb)
{
    if (!R_status.h) return;
    gx_rect(fb, R_status.x, R_status.y, R_status.w, R_status.h, C_FACE);
    gx_hline(fb, 0, fb->w - 1, R_status.y, C_HILITE);
    int y = R_status.y + 3, h = 18, w3 = 110, w2 = 110, x3 = R_status.w - w3 - 2, x2 = x3 - w2 - 2;
    gx_sunken1(fb, 2, y, x2 - 4, h); gx_sunken1(fb, x2, y, w2, h); gx_sunken1(fb, x3, y, w3, h);
    const char *help = A.status_help[0] ? A.status_help : str_res(57345);
    tx_draw_clip(fb, 6, y + 3, help, C_BLACK, x2 - 14);
    draw_icon1(fb, x2 + 2, y + 1, res_icon_pos);
    if (g_min || (g_have_mouse && rc_in(&R_view, g_sx, g_sy))) { char b[32]; snprintf(b, sizeof b, "%d,%d", g_mx, g_my); tx_draw(fb, x2 + 20, y + 3, b, C_BLACK, 0, 0); }
    draw_icon1(fb, x3 + 2, y + 1, res_icon_size);
    char sz[48]; tool_status_text(sz, sizeof sz);
    if (RZ.active) snprintf(sz, sizeof sz, "%dx%d", RZ.w, RZ.h);
    if (!sz[0]) snprintf(sz, sizeof sz, "%dx%d", A.img->w, A.img->h);
    tx_draw(fb, x3 + 20, y + 3, sz, C_BLACK, 0, 0);
}

/* ---------------- text bar ---------------- */
static const char *font_list[] = { "Arial", "Times New Roman", "Courier New", "Sans", "Serif", "Monospace", "Liberation Sans", "Liberation Serif", "Liberation Mono", "DejaVu Sans", "DejaVu Serif", "DejaVu Sans Mono", "Comic Sans MS", "Verdana", "Tahoma", "Georgia" };
static const int size_list[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };
static Rc tb_name_r(void) { return (Rc){ R_textbar.x + 6, R_textbar.y + 4, 160, 22 }; }
static Rc tb_size_r(void) { return (Rc){ R_textbar.x + 172, R_textbar.y + 4, 48, 22 }; }
static Rc tb_btn_r(int i) { return (Rc){ R_textbar.x + 228 + i * 24, R_textbar.y + 4, 23, 22 }; }
static void paint_textbar_impl(Img *fb);
static void paint_textbar(Img *fb) { int pb = tx_big(1); paint_textbar_impl(fb); tx_big(pb); }
static void paint_textbar_impl(Img *fb)
{
    if (!R_textbar.h) return;
    gx_rect(fb, R_textbar.x, R_textbar.y, R_textbar.w, R_textbar.h, C_FACE);
    gx_hline(fb, R_textbar.x, R_textbar.x + R_textbar.w - 1, R_textbar.y + R_textbar.h - 1, C_SHADOW);
    Rc n = tb_name_r(), s = tb_size_r();
    for (int k = 0; k < 2; k++) {
        Rc r = k ? s : n;
        gx_rect(fb, r.x, r.y, r.w, r.h, C_WHITE); gx_sunken(fb, r.x, r.y, r.w, r.h);
        gx_rect(fb, r.x + r.w - 18, r.y + 2, 16, r.h - 4, C_FACE); gx_raised(fb, r.x + r.w - 18, r.y + 2, 16, r.h - 4);
        for (int q = 0; q < 4; q++) gx_hline(fb, r.x + r.w - 14 + q, r.x + r.w - 6 - q, r.y + 8 + q, C_BLACK);
    }
    tx_draw_clip(fb, n.x + 4, n.y + 4, A.tb.font, C_BLACK, n.w - 24);
    char b[16]; snprintf(b, sizeof b, "%d", A.tb.size); tx_draw(fb, s.x + 4, s.y + 4, b, C_BLACK, 0, 0);
    int st[3] = { A.tb.bold, A.tb.italic, A.tb.underline };
    for (int i = 0; i < 3; i++) {
        Rc r = tb_btn_r(i);
        gx_rect(fb, r.x, r.y, r.w, r.h, C_FACE);
        if (st[i]) { draw_dither(fb, r.x + 2, r.y + 2, r.w - 4, r.h - 4); gx_sunken(fb, r.x, r.y, r.w, r.h); } else gx_raised(fb, r.x, r.y, r.w, r.h);
        gx_argb(fb, r.x + 4 + st[i], r.y + 3 + st[i], res_textglyph, 186, 15, i * 16, 0, 16, 15, 0, 0);
    }
}

/* ---------------- scrollbars ---------------- */
static void sb_geom(int vert, Rc *r1, Rc *r2, Rc *track, Rc *thumb, int *total, int *page, int *pos)
{
    Rc r = vert ? R_vscroll : R_hscroll;
    *total = vert ? content_h() : content_w(); *page = vert ? R_view.h : R_view.w; *pos = vert ? A.sy : A.sx;
    if (vert) { *r1 = (Rc){ r.x, r.y, 17, 17 }; *r2 = (Rc){ r.x, r.y + r.h - 17, 17, 17 }; *track = (Rc){ r.x, r.y + 17, 17, r.h - 34 }; }
    else { *r1 = (Rc){ r.x, r.y, 17, 17 }; *r2 = (Rc){ r.x + r.w - 17, r.y, 17, 17 }; *track = (Rc){ r.x + 17, r.y, r.w - 34, 17 }; }
    int len = vert ? track->h : track->w; if (len < 1) len = 1;
    int tl = (int)((long long)len * *page / *total); if (tl < 14) tl = 14; if (tl > len) tl = len;
    int range = *total - *page; int tp = range > 0 ? (int)((long long)(len - tl) * *pos / range) : 0;
    *thumb = vert ? (Rc){ track->x, track->y + tp, 17, tl } : (Rc){ track->x + tp, track->y, tl, 17 };
}
static void sb_arrow(Img *fb, Rc r, int dir)
{
    gx_rect(fb, r.x, r.y, r.w, r.h, C_FACE); gx_raised(fb, r.x, r.y, r.w, r.h);
    int cx = r.x + 8, cy = r.y + 8;
    for (int k = 0; k < 4; k++) {
        if (dir == 0) gx_hline(fb, cx - 3 + k, cx + 3 - k, cy - 2 + k, C_BLACK);
        else if (dir == 1) gx_hline(fb, cx - 3 + k, cx + 3 - k, cy + 2 - k, C_BLACK);
        else if (dir == 2) gx_vline(fb, cx - 2 + k, cy - 3 + k, cy + 3 - k, C_BLACK);
        else gx_vline(fb, cx + 2 - k, cy - 3 + k, cy + 3 - k, C_BLACK);
    }
}
static void paint_scrollbar(Img *fb, int vert)
{
    Rc r = vert ? R_vscroll : R_hscroll; if (!r.w) return;
    Rc a, b, tr, th; int tot, pg, pos; sb_geom(vert, &a, &b, &tr, &th, &tot, &pg, &pos);
    for (int j = 0; j < tr.h; j++) for (int i = 0; i < tr.w; i++) img_put(fb, tr.x + i, tr.y + j, ((i + j) & 1) ? C_HILITE : C_LIGHT);
    sb_arrow(fb, a, vert ? 0 : 2); sb_arrow(fb, b, vert ? 1 : 3);
    gx_rect(fb, th.x, th.y, th.w, th.h, C_FACE); gx_raised(fb, th.x, th.y, th.w, th.h);
}
static int sb_mouse(int vert, const PEvent *ev)
{
    Rc r = vert ? R_vscroll : R_hscroll; if (!r.w || !rc_in(&r, ev->x, ev->y)) return 0;
    Rc a, b, tr, th; int tot, pg, pos; sb_geom(vert, &a, &b, &tr, &th, &tot, &pg, &pos);
    int step = 16;
    int *p = vert ? &A.sy : &A.sx;
    if (rc_in(&a, ev->x, ev->y)) *p -= step;
    else if (rc_in(&b, ev->x, ev->y)) *p += step;
    else if (rc_in(&th, ev->x, ev->y)) { SB.drag = 1; SB.vert = vert; SB.start = vert ? ev->y : ev->x; SB.startpos = *p; }
    else { int before = vert ? ev->y < th.y : ev->x < th.x; *p += before ? -(pg - 16) : (pg - 16); }
    view_clamp();
    return 1;
}

/* ---------------- canvas painting ---------------- */
static void paint_canvas(Img *fb)
{
    gx_rect(fb, R_work.x, R_work.y, R_work.w, R_work.h, C_WORKSPACE);
    int ow; Img *c = build_comp(&ow);
    int ox = view_ox(), oy = view_oy(), zw = z_i2s(c->w), zh = z_i2s(c->h);
    int x0 = R_view.x, y0 = R_view.y, x1 = R_view.x + R_view.w, y1 = R_view.y + R_view.h;
    int sx0 = ox > x0 ? ox : x0, sx1 = ox + zw < x1 ? ox + zw : x1;
    int sy0 = oy > y0 ? oy : y0, sy1 = oy + zh < y1 ? oy + zh : y1;
    if (sx1 > sx0) {
        static int *xmap; static int xmap_cap;
        if (sx1 - sx0 > xmap_cap) { xmap_cap = sx1 - sx0; xmap = (int *)realloc(xmap, sizeof(int) * xmap_cap); }
        for (int x = sx0; x < sx1; x++) { int ix = z_s2i(x - ox); xmap[x - sx0] = ix >= c->w ? c->w - 1 : ix; }
        for (int y = sy0; y < sy1; y++) {
            int iy = z_s2i(y - oy); if (iy >= c->h) iy = c->h - 1;
            const px_t *row = c->p + (size_t)iy * c->w;
            px_t *dst = fb->p + (size_t)y * fb->w;
            if (A.zp == 100) { for (int x = sx0; x < sx1; x++) dst[x] = row[x - ox]; }
            else if (A.zp > 100) { for (int x = sx0; x < sx1; x++) dst[x] = row[xmap[x - sx0]]; }
            else { /* shrinking: box-filter average of the source area covered by this screen pixel */
                int iy1 = z_s2i(y - oy + 1); if (iy1 <= iy) iy1 = iy + 1; if (iy1 > c->h) iy1 = c->h;
                for (int x = sx0; x < sx1; x++) {
                    int ix = xmap[x - sx0], ix1 = z_s2i(x - ox + 1); if (ix1 <= ix) ix1 = ix + 1; if (ix1 > c->w) ix1 = c->w;
                    unsigned r = 0, g = 0, b = 0, n = 0;
                    for (int yy = iy; yy < iy1; yy++) { const px_t *rr = c->p + (size_t)yy * c->w; for (int xx = ix; xx < ix1; xx++) { px_t p = rr[xx]; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++; } }
                    dst[x] = n ? ((r / n) << 16) | ((g / n) << 8) | (b / n) : row[ix];
                }
            }
        }
    }
    if (A.show_grid && A.zp >= 400) {
        for (int ix = 0; ix <= c->w; ix++) { int x = ox + z_i2s(ix); if (x >= sx0 && x < sx1) for (int y = sy0; y < sy1; y += 2) img_put(fb, x, y, C_SHADOW); }
        for (int iy = 0; iy <= c->h; iy++) { int y = oy + z_i2s(iy); if (y >= sy0 && y < sy1) for (int x = sx0; x < sx1; x += 2) img_put(fb, x, y, C_SHADOW); }
    }
    if (ow) img_free(c);
    /* resize handles */
    int iw = z_i2s(A.img->w), ih = z_i2s(A.img->h);
    int hx[3] = { ox + iw, ox + iw / 2 - 1, ox + iw }, hy[3] = { oy + ih / 2 - 1, oy + ih, oy + ih };
    for (int i = 0; i < 3; i++)
        if (hx[i] >= x0 && hy[i] >= y0 && hx[i] + 3 <= x1 && hy[i] + 3 <= y1) gx_rect(fb, hx[i], hy[i], 3, 3, C_NAVY);
    /* keep overlays inside the view */
    Img clipview = *fb; (void)clipview;
    draw_overlays(fb);
    if (RZ.active) {
        int w = z_i2s(RZ.w), h = z_i2s(RZ.h);
        gx_dotrect(fb, ox, oy, w, h, C_BLACK, C_WHITE, 0);
    }
    /* mask areas outside the view that overlays may have touched */
    gx_rect(fb, R_work.x, R_work.y, R_work.w, 2, C_WORKSPACE);
    /* thumbnail */
    if (A.show_thumb && A.zp > 100) {
        int tw = 120, th = A.img->h * tw / A.img->w; if (th > 120) { th = 120; tw = A.img->w * th / A.img->h; }
        int bx = R_view.x + R_view.w - tw - 14, by = R_view.y + 10;
        gx_rect(fb, bx - 3, by - 3, tw + 6, th + 6, C_FACE); gx_raised(fb, bx - 3, by - 3, tw + 6, th + 6);
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) img_put(fb, bx + x, by + y, A.img->p[(y * A.img->h / th) * A.img->w + x * A.img->w / tw]);
    }
    gx_sunken(fb, R_work.x, R_work.y, R_work.w, R_work.h);
}

/* ---------------- whole window ---------------- */
void app_paint_all(Img *fb)
{
    gx_rect(fb, 0, 0, fb->w, fb->h, C_FACE);
    paint_canvas(fb);
    paint_textbar(fb);
    paint_scrollbar(fb, 1); paint_scrollbar(fb, 0);
    if (R_hscroll.w && R_vscroll.w) gx_rect(fb, R_vscroll.x, R_hscroll.y, 17, 17, C_FACE);
    paint_toolbox(fb);
    paint_colorbox(fb);
    paint_status(fb);
    /* menu bar */
    gx_rect(fb, 0, 0, fb->w, 19, C_FACE);
    gx_hline(fb, 0, fb->w - 1, 18, C_SHADOW); gx_hline(fb, 0, fb->w - 1, 17, C_FACE);
    int pb_ = tx_big(1);
    for (int i = 0; i < g_menu_titles_n; i++) {
        if (i == hot_menu) gx_raised1(fb, g_tx[i], 1, g_tw[i], 17);
        tx_draw(fb, g_tx[i] + 8, 3, g_titles[i], C_BLACK, 0, 1);
    }
    tx_big(pb_);
    paint_zoom_btns(fb);
    /* tooltip */
    if (hot_tool >= 0 && plat_ticks_ms() - hot_since > 600) {
        const char *s = str_res(tool_str[hot_tool]); const char *nl = strchr(s, '\n');
        if (nl) {
            int pbt = tx_big(1); nl++; int w = tx_width(nl, 0, 0) + 8, x = tip_x + 12, y = tip_y + 18;
            if (x + w > fb->w) x = fb->w - w;
            gx_rect(fb, x, y, w, 17, RGB(255, 255, 225)); gx_frame(fb, x, y, w, 17, C_BLACK);
            tx_draw(fb, x + 4, y + 2, nl, C_BLACK, 0, 0);
            tx_big(pbt);
        }
    }
}

/* ---------------- input ---------------- */
static void apply_cursor(int idx)
{
    if (idx == g_cur_known) return;
    g_cur_known = idx;
    if (idx < 0) { plat_set_cursor(NULL); return; }
    if (!g_cursors[idx]) {
        const uint32_t *src = res_cursors[idx].argb; int hx = res_cursors[idx].hx, hy = res_cursors[idx].hy;
        uint32_t tmp[32 * 32], out[32 * 32];
        if (idx == 14 || idx == 15) {
            /* these two are "invert" cursors in the original (pure white here): draw them black with a white outline so they show on any colour */
            memcpy(tmp, src, sizeof tmp);
            if (idx == 15) { /* the stored I-beam is lying on its side: rotate 90 degrees */
                for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) tmp[x * 32 + (31 - y)] = src[y * 32 + x];
                int nhx = 31 - hy, nhy = hx; hx = nhx; hy = nhy;
            }
            for (int i = 0; i < 32 * 32; i++) out[i] = 0;
            for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
                if ((tmp[y * 32 + x] >> 24) == 0) continue;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < 32 && yy < 32 && !(tmp[yy * 32 + xx] >> 24)) out[yy * 32 + xx] = 0xffffffffu;
                }
            }
            for (int i = 0; i < 32 * 32; i++) if (tmp[i] >> 24) out[i] = 0xff000000u;
            src = out;
        }
        g_cursors[idx] = plat_make_cursor(src, hx, hy);
    }
    plat_set_cursor(g_cursors[idx]);
}
static int resize_handle_at(int sx, int sy)
{
    int ox = view_ox(), oy = view_oy(), iw = z_i2s(A.img->w), ih = z_i2s(A.img->h);
    int hx[3] = { ox + iw, ox + iw / 2 - 1, ox + iw }, hy[3] = { oy + ih / 2 - 1, oy + ih, oy + ih };
    for (int i = 0; i < 3; i++) if (sx >= hx[i] - 2 && sx < hx[i] + 5 && sy >= hy[i] - 2 && sy < hy[i] + 5) return i;
    return -1;
}
static int accel_cmd(const PEvent *ev)
{
    int k = ev->key, ctrl = ev->mods & MOD_CTRL, shift = ev->mods & MOD_SHIFT, alt = ev->mods & MOD_ALT;
    if (ctrl) {
        switch (k) {
        case 'n': return shift ? CMD_CLEARIMG : CMD_NEW; case 'o': return CMD_OPEN; case 's': return CMD_SAVE; case 'p': return CMD_PRINT;
        case 'z': return CMD_UNDO; case 'y': return CMD_REPEAT; case 'x': return CMD_CUT; case 'c': return shift ? CMD_COPYIMG : CMD_COPY; case 'v': return CMD_PASTE;
        case 'a': return CMD_SELECTALL; case 't': return CMD_TOOLBOX; case 'l': return CMD_COLORBOX; case 'g': return CMD_GRID;
        case 'f': return CMD_VIEWBITMAP; case 'r': return CMD_FLIPROT; case 'w': return CMD_STRETCH; case 'i': return CMD_INVERT; case 'e': return CMD_ATTR;
        case KEY_PGUP: return CMD_ZOOM_NORMAL; case KEY_PGDN: return CMD_ZOOM_LARGE; case KEY_PLUS: return CMD_ZOOMIN; case KEY_MINUS: return CMD_ZOOMOUT;
        case KEY_INSERT: return CMD_COPY;
        }
    }
    if (k == KEY_DELETE) return shift ? CMD_CUT : CMD_CLEARSEL;
    if (k == KEY_INSERT && shift) return CMD_PASTE;
    if (alt && k == KEY_BACKSPACE) return CMD_UNDO;
    if (k == KEY_F1) return CMD_HELP;
    return 0;
}
static void open_menu(int top)
{
    int cmd;
    if (menu_bar_run(top, &cmd)) cmd_exec(cmd);
}
static void handle_mouse_down(const PEvent *ev)
{
    int x = ev->x, y = ev->y;
    if (rc_in(&R_menubar, x, y)) {
        for (int p = 0; p < 2; p++) {
            Rc r = zoom_btn(p);
            if (rc_in(&r, x, y)) {
                if (ev->button == BTN_LEFT) {
                    int np = p ? (A.zp / 5 + 1) * 5 : ((A.zp - 1) / 5) * 5;
                    zoom_at(np, R_view.x + R_view.w / 2, R_view.y + R_view.h / 2);
                }
                return;
            }
        }
        for (int i = 0; i < g_menu_titles_n; i++) if (x >= g_tx[i] && x < g_tx[i] + g_tw[i]) { open_menu(i); return; }
        return;
    }
    if (R_toolbox.w && rc_in(&R_toolbox, x, y)) {
        for (int i = 0; i < T_COUNT; i++) { Rc r = tool_rect(i); if (rc_in(&r, x, y)) { if (ev->button == BTN_LEFT) { tool_select(i); ui_relayout(); } return; } }
        if (has_shapew(A.tool)) for (int i = 0; i < 6; i++) { Rc r = opt2_item(i); if (rc_in(&r, x, y) && ev->button == BTN_LEFT) { A.o_shapew = i; return; } }
        int n = opt_count(A.tool);
        for (int i = 0; i < n; i++) { Rc r = opt_item(A.tool, i); if (rc_in(&r, x, y) && ev->button == BTN_LEFT) { *opt_ptr(A.tool) = i; return; } }
        return;
    }
    if (R_colorbox.h && rc_in(&R_colorbox, x, y)) {
        for (int i = 0; i < 28; i++) {
            Rc r = pal_rect(i);
            if (rc_in(&r, x, y)) {
                uint32_t now = plat_ticks_ms();
                if (ev->button == BTN_LEFT && pal_click_idx == i && now - pal_click_t < 400) { px_t c = A.pal[i]; if (color_dialog(&c)) { A.pal[i] = c; A.fg = c; } pal_click_idx = -1; return; }
                pal_click_idx = i; pal_click_t = now;
                if (ev->button == BTN_LEFT) A.fg = A.pal[i]; else if (ev->button == BTN_RIGHT) A.bg = A.pal[i];
                return;
            }
        }
        Rc ind = ind_rect();
        if (rc_in(&ind, x, y) && ev->button == BTN_LEFT) { px_t t = A.fg; A.fg = A.bg; A.bg = t; }
        return;
    }
    if (R_textbar.h && rc_in(&R_textbar, x, y)) {
        Rc n = tb_name_r(), s = tb_size_r();
        if (rc_in(&n, x, y)) { int r = list_popup(font_list, 16, n.x, n.y + n.h, n.w, 0); if (r >= 0) text_set_font(font_list[r]); }
        else if (rc_in(&s, x, y)) {
            const char *names[16]; static char buf[16][8];
            for (int i = 0; i < 16; i++) { snprintf(buf[i], 8, "%d", size_list[i]); names[i] = buf[i]; }
            int r = list_popup(names, 16, s.x, s.y + s.h, s.w, 2); if (r >= 0) text_set_size(size_list[r]);
        } else for (int i = 0; i < 3; i++) { Rc r = tb_btn_r(i); if (rc_in(&r, x, y)) text_toggle(i); }
        text_autogrow();
        return;
    }
    if (sb_mouse(1, ev) || sb_mouse(0, ev)) return;
    if (rc_in(&R_view, x, y) || (RZ.active)) {
        if (ev->button == BTN_LEFT) {
            int h = resize_handle_at(x, y);
            if (h >= 0) { RZ.active = 1; RZ.which = h; RZ.w = A.img->w; RZ.h = A.img->h; return; }
        }
        int ix = z_s2i(x - view_ox()), iy = z_s2i(y - view_oy());
        if (ev->button == BTN_RIGHT && (A.tool == T_SELECT || A.tool == T_FREESEL) && A.sel.active && sel_hit(ix, iy) == 1) {
            int cmd; if (popup_menu_run(res_menu_sel, x, y, &cmd)) cmd_exec(cmd); return;
        }
        if (ev->button == BTN_RIGHT && A.tb.active && A.tool == T_TEXT && ix >= A.tb.x && iy >= A.tb.y && ix < A.tb.x + A.tb.w && iy < A.tb.y + A.tb.h) {
            int cmd; if (popup_menu_run(res_menu_text, x, y, &cmd)) cmd_exec(cmd); return;
        }
        uint32_t now = plat_ticks_ms();
        if (A.tool == T_POLY && now - last_click_t < 400 && abs(x - last_click_x) < 5 && abs(y - last_click_y) < 5) { tool_dblclick(); last_click_t = 0; return; }
        last_click_t = now; last_click_x = x; last_click_y = y;
        tool_press(ix, iy, ev->button, ev->mods);
        dragging = tool_drag_active();
        ui_relayout();
    }
}
static void update_hover(int x, int y)
{
    g_sx = x; g_sy = y; g_have_mouse = 1;
    int ix = z_s2i(x - view_ox()), iy = z_s2i(y - view_oy());
    g_mx = ix; g_my = iy;
    g_min = rc_in(&R_view, x, y) && ix >= 0 && iy >= 0;
    int ht = -1, hm = -1;
    for (int i = 0; i < g_menu_titles_n; i++) if (y < 19 && x >= g_tx[i] && x < g_tx[i] + g_tw[i]) hm = i;
    if (R_toolbox.w) for (int i = 0; i < T_COUNT; i++) { Rc r = tool_rect(i); if (rc_in(&r, x, y)) ht = i; }
    if (ht != hot_tool) { hot_tool = ht; hot_since = plat_ticks_ms(); tip_x = x; tip_y = y; if (ht >= 0) { const char *s = str_res(tool_str[ht]); char b[256]; strncpy(b, s, 255); b[255] = 0; char *nl = strchr(b, '\n'); if (nl) *nl = 0; set_status_help(b); } else set_status_help(""); }
    hot_menu = hm;
}
static void update_cursor(int x, int y)
{
    if (RZ.active) { apply_cursor(RZ.which == 0 ? 11 : RZ.which == 1 ? 12 : 9); return; }
    if (rc_in(&R_view, x, y) || dragging) {
        int h = resize_handle_at(x, y);
        if (h >= 0) { apply_cursor(h == 0 ? 11 : h == 1 ? 12 : 9); return; }
        int ix = z_s2i(x - view_ox()), iy = z_s2i(y - view_oy()), idx = 4;
        tool_cursor_for(ix, iy, &idx);
        apply_cursor(idx);
    } else apply_cursor(-1);
}
/* zoom to pct keeping the image point under screen (sx,sy) fixed */
static void zoom_at(int pct, int sx, int sy)
{
    if (pct < ZOOM_MIN) pct = ZOOM_MIN; if (pct > ZOOM_MAX) pct = ZOOM_MAX;
    if (pct == A.zp) return;
    double fx = (sx - view_ox()) * 100.0 / A.zp, fy = (sy - view_oy()) * 100.0 / A.zp;
    A.zp = pct;
    ui_relayout();
    A.sx = (int)(R_view.x + 3 + fx * pct / 100.0 - sx + 0.5); A.sy = (int)(R_view.y + 3 + fy * pct / 100.0 - sy + 0.5);
    view_clamp();
}
static int pinch_base = 100;
static void handle_event(const PEvent *ev)
{
    switch (ev->type) {
    case EV_DROPFILE: if (app_confirm_discard()) { app_load_file(plat_drop_path()); ui_relayout(); } break;
    case EV_MOUSEDOWN: handle_mouse_down(ev); break;
    case EV_MOUSEMOVE: {
        update_hover(ev->x, ev->y);
        if (SB.drag) {
            Rc a, b, tr, th; int tot, pg, pos; sb_geom(SB.vert, &a, &b, &tr, &th, &tot, &pg, &pos);
            int len = SB.vert ? tr.h : tr.w, tl = SB.vert ? th.h : th.w, d = (SB.vert ? ev->y : ev->x) - SB.start;
            if (len - tl > 0) { int np = SB.startpos + (int)((long long)d * (tot - pg) / (len - tl)); if (SB.vert) A.sy = np; else A.sx = np; view_clamp(); }
        } else if (RZ.active) {
            int ix = z_s2i(ev->x - view_ox()), iy = z_s2i(ev->y - view_oy());
            if (RZ.which != 1) RZ.w = ix < 1 ? 1 : ix; if (RZ.which != 0) RZ.h = iy < 1 ? 1 : iy;
        } else if (dragging) {
            tool_move(g_mx, g_my, ev->mods);
        }
        update_cursor(ev->x, ev->y);
        break;
    }
    case EV_MOUSEUP:
        if (SB.drag) SB.drag = 0;
        if (RZ.active) { RZ.active = 0; set_canvas_size(RZ.w, RZ.h); }
        if (dragging) { dragging = 0; tool_release(z_s2i(ev->x - view_ox()), z_s2i(ev->y - view_oy()), ev->mods); ui_relayout(); }
        update_cursor(ev->x, ev->y);
        break;
    case EV_PINCH:
        if (ev->button == 1) pinch_base = A.zp;
        else if (ev->button == 2) zoom_at((int)((long long)pinch_base * ev->key / 1000), ev->x, ev->y);
        break;
    case EV_WHEEL:
        if (ev->mods & MOD_CTRL) { zoom_at(A.zp + (A.zp * ev->button * 10 + (ev->button > 0 ? 99 : -99)) / 100, ev->x, ev->y); }
        else if (ev->mods & MOD_SHIFT) { A.sx -= ev->button * 48; view_clamp(); }
        else { A.sy -= ev->button * 48; view_clamp(); }
        break;
    case EV_CHAR: tool_key(ev); break;
    case EV_KEYDOWN: {
        if (ev->mods & MOD_ALT) {
            for (int i = 0; i < g_menu_titles_n; i++) {
                const char *a = strchr(g_titles[i], '&');
                if (a && tolower((unsigned char)a[1]) == ev->key) { open_menu(i); return; }
            }
        }
        if (ev->key == KEY_F1) { cmd_exec(CMD_HELP); return; }
        if ((ev->mods & MOD_CTRL) && (ev->key == '+' || ev->key == '=' || ev->key == KEY_PLUS || ev->key == '-' || ev->key == '_' || ev->key == KEY_MINUS)) {
            int up = (ev->key == '+' || ev->key == '=' || ev->key == KEY_PLUS);        /* Ctrl + / Ctrl - : next multiple of 5% */
            int np = up ? (A.zp / 5 + 1) * 5 : ((A.zp - 1) / 5) * 5;
            zoom_at(np, R_view.x + R_view.w / 2, R_view.y + R_view.h / 2);
            return;
        }
        if (!(ev->mods & (MOD_CTRL | MOD_ALT)) && tool_key(ev)) return;
        int cmd = accel_cmd(ev);
        if (cmd) { cmd_exec(cmd); ui_relayout(); }
        break;
    }
    case EV_LEAVE: g_min = 0; g_have_mouse = 0; hot_tool = -1; hot_menu = -1; break;
    case EV_CLOSE: cmd_exec(CMD_EXIT); break;
    default: break;
    }
}

/* ---------------- event pump (with optional scripted test input) ---------------- */
#ifdef PAINT_TEST
static FILE *g_script; static uint32_t g_wait_until;
static int key_by_name(const char *n, int *mods)
{
    *mods = 0; char b[64]; strncpy(b, n, 63); b[63] = 0; char *p = b;
    for (;;) { if (!strncmp(p, "ctrl+", 5)) { *mods |= MOD_CTRL; p += 5; } else if (!strncmp(p, "shift+", 6)) { *mods |= MOD_SHIFT; p += 6; } else if (!strncmp(p, "alt+", 4)) { *mods |= MOD_ALT; p += 4; } else break; }
    if (!strcmp(p, "enter")) return KEY_ENTER; if (!strcmp(p, "esc")) return KEY_ESC; if (!strcmp(p, "backspace")) return KEY_BACKSPACE;
    if (!strcmp(p, "delete")) return KEY_DELETE; if (!strcmp(p, "left")) return KEY_LEFT; if (!strcmp(p, "right")) return KEY_RIGHT;
    if (!strcmp(p, "up")) return KEY_UP; if (!strcmp(p, "down")) return KEY_DOWN; if (!strcmp(p, "tab")) return KEY_TAB;
    if (!strcmp(p, "pgup")) return KEY_PGUP; if (!strcmp(p, "pgdn")) return KEY_PGDN;
    return (unsigned char)p[0];
}
static unsigned char g_cq[1024]; static int g_cq_n, g_cq_i;
static int next_test_event(PEvent *ev)
{
    if (g_cq_i < g_cq_n) { memset(ev, 0, sizeof *ev); ev->type = EV_CHAR; ev->ch = g_cq[g_cq_i++]; if (g_cq_i == g_cq_n) g_cq_i = g_cq_n = 0; return 1; }
    static int tried; if (!g_script) { if (tried) return 0; tried = 1; const char *s = getenv("PAINT_SCRIPT"); if (s) g_script = fopen(s, "r"); if (!g_script) return 0; }
    if (g_wait_until && plat_ticks_ms() < g_wait_until) return 0;
    g_wait_until = 0;
    char line[512];
    if (!fgets(line, sizeof line, g_script)) { fclose(g_script); g_script = NULL; memset(ev, 0, sizeof *ev); ev->type = EV_CLOSE; return 0; }
    memset(ev, 0, sizeof *ev);
    char cmd[32], arg[400]; int a = 0, b = 0, c = 0;
    if (sscanf(line, "%31s", cmd) != 1) return 0;
    if (!strcmp(cmd, "#")) return 0;
    if (!strcmp(cmd, "m") || !strcmp(cmd, "d") || !strcmp(cmd, "u")) {
        sscanf(line, "%*s %d %d %d", &a, &b, &c);
        ev->type = cmd[0] == 'm' ? EV_MOUSEMOVE : cmd[0] == 'd' ? EV_MOUSEDOWN : EV_MOUSEUP; ev->x = a; ev->y = b;
        ev->button = c ? c : BTN_LEFT; if (cmd[0] == 'm') ev->button = c;
        return 1;
    }
    if (!strcmp(cmd, "k") || !strcmp(cmd, "kk")) { sscanf(line, "%*s %399s", arg); ev->type = EV_KEYDOWN; ev->key = key_by_name(arg, &ev->mods); return 1; }
    if (!strcmp(cmd, "t")) { /* type text: queued as EV_CHAR events */
        const char *p = strchr(line, ' '); if (!p) return 0; p++;
        size_t n = strlen(p); while (n && p[n - 1] == '\n') n--;
        for (size_t i = 0; i < n && g_cq_n < 1000; i++) g_cq[g_cq_n++] = (unsigned char)p[i];
        return 0;
    }
    if (!strcmp(cmd, "wait")) { sscanf(line, "%*s %d", &a); g_wait_until = plat_ticks_ms() + a; return 0; }
    if (!strcmp(cmd, "shot")) {
        sscanf(line, "%*s %399s", arg);
        size_t n; uint8_t *b2 = png_encode(A.fb->p, A.fb->w, A.fb->h, &n); file_write_all(arg, b2, n); free(b2);
        return 0;
    }
    if (!strcmp(cmd, "cmd")) { sscanf(line, "%*s %d", &a); cmd_exec(a); return 0; }
    if (!strcmp(cmd, "canvas")) { int w, h; sscanf(line, "%*s %d %d", &w, &h); set_canvas_size(w, h); ui_relayout(); return 0; }
    if (!strcmp(cmd, "zoom")) { sscanf(line, "%*s %d", &a); set_zoom(a, 0, 0); return 0; }
    if (!strcmp(cmd, "tool")) { sscanf(line, "%*s %d", &a); tool_select(a); ui_relayout(); return 0; }
    if (!strcmp(cmd, "fg")) { unsigned v; sscanf(line, "%*s %x", &v); A.fg = v; return 0; }
    if (!strcmp(cmd, "bg")) { unsigned v; sscanf(line, "%*s %x", &v); A.bg = v; return 0; }
    if (!strcmp(cmd, "opt")) { sscanf(line, "%*s %d %d", &a, &b); /* set option for current tool */ if (a == 0) A.o_eraser = b; if (a == 1) A.o_brush = b; if (a == 2) A.o_air = b; if (a == 3) A.o_line = b; if (a == 4) A.o_fill = b; if (a == 5) A.o_mag = b; if (a == 6) A.o_trans = b; if (a == 7) A.o_shapew = b; return 0; }
    if (!strcmp(cmd, "quit")) { g_quit = 1; return 0; }
    return 0;
}
#endif
int pump_event(PEvent *ev, int timeout_ms)
{
#ifdef PAINT_TEST
    if (next_test_event(ev)) return 1;
    if (g_script && timeout_ms > 15) timeout_ms = 15;
    if (g_quit) { memset(ev, 0, sizeof *ev); ev->type = EV_CLOSE; return 1; }
    if (ev->type == EV_CLOSE) return 1;
#endif
    int r = plat_wait_event(ev, timeout_ms);
    if (ev->type == EV_RESIZE) {
        img_free(A.fb); A.fb = img_new(ev->x, ev->y, 0);
        ui_relayout();
    }
    if (ev->type == EV_MOUSEMOVE || ev->type == EV_MOUSEDOWN) { g_sx = ev->x; g_sy = ev->y; g_have_mouse = 1; }
    return r;
}

static void live_hook(void)
{
    PEvent ev;
    for (int i = 0; i < 64; i++) { if (!pump_event(&ev, 0) || ev.type == EV_NONE) break; handle_event(&ev); }
    app_paint_all(A.fb);
    plat_present(A.fb->p, A.fb->w, A.fb->h);
}

int main(int argc, char **argv)
{
    memset(&A, 0, sizeof A);
    if (plat_init(1024, 768, 800, 660, "untitled - SmPaint")) return 1;
    A.fb = img_new(1024, 768, 0);
    plat_set_live_hook(live_hook);
    app_init();
    g_menu_titles_n = menu_bar_titles(g_titles, g_tx, g_tw);
    plat_set_icon(res_appicon48, 48, 48); plat_set_icon(res_appicon32, 32, 32); plat_set_icon(res_appicon16, 16, 16);
    ui_relayout();
    update_title();
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') { app_load_file(argv[i]); break; }
    set_status_help("");
    uint32_t last_tick = plat_ticks_ms();
    while (!g_quit) {
        app_paint_all(A.fb);
        plat_present(A.fb->p, A.fb->w, A.fb->h);
        PEvent ev;
        int busy = tool_drag_active() && 1;
        pump_event(&ev, busy ? 30 : 250);
        if (ev.type != EV_NONE) handle_event(&ev);
        uint32_t now = plat_ticks_ms();
        if (now - last_tick >= 30) { last_tick = now; tool_tick(); }
    }
    plat_shutdown();
    return 0;
}
