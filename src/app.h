/* app.h - shared declarations of the portable Paint core */
#ifndef APP_H
#define APP_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gfx.h"
#include "plat.h"
#include "imgio.h"
#include "resdata.h"

#define APP_VERSION "V1.18"

typedef struct { int x, y; } Pt;
typedef struct { int x, y, w, h; } Rc;
static inline int rc_in(const Rc *r, int x, int y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }

enum { T_FREESEL, T_SELECT, T_ERASER, T_FILL, T_PICK, T_MAG, T_PENCIL, T_BRUSH, T_AIR, T_TEXT, T_LINE, T_CURVE,
       T_RECT, T_POLY, T_ELLIPSE, T_RRECT, T_COUNT };
#define T_NONE T_COUNT          /* no tool: click text boxes / shapes to move and edit them */
enum { SH_RECT, SH_ELLIPSE, SH_RRECT };

/* command ids (taken from the original resources) */
enum {
    CMD_NEW = 57600, CMD_OPEN = 57601, CMD_SAVE = 57603, CMD_SAVEAS = 57604, CMD_PAGESETUP = 57605, CMD_PRINT = 57607,
    CMD_PRINTPREVIEW = 57609, CMD_RECENT = 57616, CMD_CLEARSEL = 57632, CMD_COPY = 57634, CMD_CUT = 57635,
    CMD_PASTE = 57637, CMD_SELECTALL = 57642, CMD_UNDO = 57643, CMD_REPEAT = 57644, CMD_ABOUT = 57664, CMD_EXIT = 57665,
    CMD_HELP = 57670, CMD_BG_CENTER = 57675, CMD_BG_TILE = 57677, CMD_STATUSBAR = 59393, CMD_TOOLBOX = 59415,
    CMD_COLORBOX = 59416, CMD_SEND = 37662, CMD_COPYTO = 37663, CMD_PASTEFROM = 37664, CMD_SCANNER = 37666,
    CMD_ZOOM_NORMAL = 37670, CMD_ZOOM_LARGE = 37671, CMD_ZOOM_CUSTOM = 37672, CMD_VIEWBITMAP = 37673,
    CMD_THUMB = 37676, CMD_GRID = 37677, CMD_TEXTBAR = 37678, CMD_FLIPROT = 37680, CMD_STRETCH = 37681,
    CMD_INVERT = 37682, CMD_ATTR = 37683, CMD_CLEARIMG = 37684, CMD_OPAQUE = 6868, CMD_EDITCOLORS = 6869,
    CMD_ZOOMIN = 6811, CMD_ZOOMOUT = 6810, CMD_COPYIMG = 6870, CMD_CROP = 6871
};

/* ---- application state ---- */
typedef struct {
    int active, lifted, pushed, transparent;
    int x, y, w, h;     /* rectangle in image coordinates */
    Img *pix;           /* floating content (w*h) once lifted */
    uint8_t *mask;      /* freeform mask (w*h) or NULL = whole rectangle */
    int free_cnt, free_cap; Pt *free_pts; /* freeform path while dragging */
    int copy_mode;
} Sel;

typedef struct { uint8_t font, size, flags; } TStyle;   /* per-character text style: flags 1 bold, 2 italic, 4 underline */
typedef struct {
    int active;         /* text box exists */
    int x, y, w, h;     /* box in image coords */
    int minw, minh;     /* size the box was drawn with; it grows beyond this to fit the text */
    char *text; TStyle *st; int len, cap, caret, sel;   /* sel = selection anchor, -1 none */
    char font[64]; int size, bold, italic, underline;
    int frozen, from_obj; px_t fg, bg; int trans; int tscale;   /* tscale: text size multiplier (permille) fixed when the box is made */   /* committed boxes keep their own colours; from_obj = re-opened for editing */
} TextBox;
typedef struct { int kind, n, fillstyle, lw, closed; Pt *p; px_t outline, fill; } Shape;   /* finished line/curve/rect/ellipse/polygon: stays editable */
typedef struct { int n; Shape *o; } SList;
typedef struct { int n; TextBox *o; } TList;      /* finished text boxes: stay editable until something else changes the picture */

typedef struct {
    Img *fb; int ww, wh;
    Img *img;
    Img *undo[3]; int nundo; Img *redo[3]; int nredo;
    px_t fg, bg, pal[28], custom[16];
    int tool, prevtool;
    int zp, sx, sy;   /* zp = zoom in percent (10..800) */
    int o_eraser, o_brush, o_air, o_line, o_fill, o_mag, o_trans, o_shapew;
    int show_toolbox, show_colorbox, show_status, show_text, show_grid, show_thumb;
    int grid_w, grid_h, tile_grid;
    char path[1024]; int dirty, bw;
    Sel sel; TextBox tb; TList tl, tundo[3], tredo[3];
    SList sl, sundo[3], sredo[3]; int ssel;   /* finished shapes; ssel = selected one (-1 none) */
    Img *shadow;   /* picture area hidden by shrinking the canvas; comes back if the canvas is enlarged again */
    char status_help[256];
    char recent[4][1024]; int nrecent;
    Img *clip; /* internal clipboard */
    int dpi;
} App;
extern App A;

/* ---- layout rectangles (computed in main.c) ---- */
extern Rc R_menubar, R_toolbox, R_opts, R_colorbox, R_status, R_work, R_view, R_hscroll, R_vscroll, R_textbar;

/* ---- canvas.c ---- */
void cv_line(Img *im, int x0, int y0, int x1, int y1, px_t c, int w);
void cv_dot(Img *im, int x, int y, px_t c, int w);
void cv_brush(Img *im, int x, int y, int shape, int size, px_t c);
void cv_brush_line(Img *im, int x0, int y0, int x1, int y1, int shape, int size, px_t c);
void cv_flood(Img *im, int x, int y, px_t c);
void cv_air(Img *im, int x, int y, int radius, px_t c);
void cv_shape(Img *im, int kind, int x0, int y0, int x1, int y1, int fillstyle, px_t outline, px_t fill, int lw);
void cv_poly(Img *im, const Pt *p, int n, int closed, int fillstyle, px_t outline, px_t fill, int lw);
void cv_curve(Img *im, Pt p0, Pt p1, Pt p2, Pt p3, px_t c, int lw);
void cv_fillpoly_mask(uint8_t *mask, int w, int h, const Pt *p, int n, int ox, int oy);
Img *cv_flip(const Img *s, int horiz);
Img *cv_rotate(const Img *s, int deg);
Img *cv_stretch(const Img *s, int nw, int nh);
Img *cv_skew(const Img *s, int hdeg, int vdeg, px_t bg);
Img *cv_crop_resize(const Img *s, int nw, int nh, px_t bg);
void cv_invert(Img *im);
void cv_to_bw(Img *im);

/* ---- ui.c ---- */
typedef struct Modal {
    void (*paint)(struct Modal *, Img *);
    int (*event)(struct Modal *, const PEvent *);
    void *ud; int done; int result;
} Modal;
int modal_run(Modal *m);
void ui_request_paint(void);
int pump_event(PEvent *ev, int timeout_ms); /* main.c: fb resize + test script handled */
void app_paint_all(Img *fb);                /* main.c */

typedef struct MItem { int type, id; char text[80]; int sub; /* index of submenu or -1 */ } MItem;
int menu_bar_run(int start_top, int *cmd_out);  /* opens menu modally, returns 1 if command chosen */
int popup_menu_run(const ResMenuItem *items, int x, int y, int *cmd_out);
void menu_state(int id, int *enabled, int *checked); /* main.c */
const char *menu_help_text(int id);

#define MB_OK 0
#define MB_YESNOCANCEL 1
#define MB_OKCANCEL 2
#define MB_YESNO 3
#define ID_OK 1
#define ID_CANCEL 2
#define ID_YES 6
#define ID_NO 7
int msgbox(const char *title, const char *text, int type, int icon);

/* data-driven dialogs from the original resources */
typedef struct {
    ResCtl r; int x, y, w, h; int checked, enabled, visible; char text[64];
    int group_start; int hot;
} DCtl;
typedef struct Dlg {
    const ResDlg *res; DCtl c[64]; int n; int focus; int caret; int sel_all;
    int dx, dy, dw, dh; /* window rect incl. frame */
    int cx, cy, cw, ch; /* client rect */
    Modal m; char title[64]; int pressed;
    void (*on_cmd)(struct Dlg *, int id, int param);
    void (*on_paint)(struct Dlg *, Img *, int id, int x, int y, int w, int h);
    void *ud;
} Dlg;
void dlg_init(Dlg *d, const ResDlg *res, const char *title);
int dlg_run(Dlg *d);
DCtl *dlg_find(Dlg *d, int id);
void dlg_set_check(Dlg *d, int id, int v);
int dlg_get_check(Dlg *d, int id);
void dlg_set_text(Dlg *d, int id, const char *t);
int dlg_get_int(Dlg *d, int id, int dflt);
void dlg_set_enabled(Dlg *d, int id, int en);

int file_dialog(int save, const char *title, char *path, int pathn, int filter_mode);
int color_dialog(px_t *color);
void about_dialog(void);
const char *str_res(int id);

/* ---- view transform (ui_main.c) ---- */
int view_ox(void); int view_oy(void);           /* screen position of image pixel (0,0) */
void view_center_on(int ix, int iy);
void view_clamp(void);
void ui_relayout(void);
void update_title(void);

/* ---- tools.c ---- */
void app_init(void);
void undo_push(void);
void do_undo(void);
void do_redo(void);
void sel_commit(void);
void sel_cancel(void);
void tool_select(int t);
void tool_press(int ix, int iy, int btn, int mods);
void tool_move(int ix, int iy, int mods);
void tool_release(int ix, int iy, int mods);
void tool_tick(void);
int tool_key(const PEvent *ev);
int tool_drag_active(void);
int tool_busy(void);              /* a multi-step drawing (curve/polygon/text) in progress */
Img *build_comp(int *owned);      /* image with floating selection/text composited */
void draw_overlays(Img *fb);      /* selection frames, text box, etc. */
int sel_hit(int ix, int iy);      /* 0 none, 1 inside selection, 2.. handles */
int cmd_exec(int id);
/* zoom helpers: image px <-> screen px at A.zp percent */
#define ZOOM_MIN 10
#define ZOOM_MAX 800
static inline int z_i2s(int i) { return (int)((long long)i * A.zp / 100); }
static inline int z_s2i(int s) { long long n = (long long)s * 100; long long d = A.zp; long long q = n / d; if ((n % d) != 0 && n < 0) q--; return (int)q; }
void set_zoom(int z, int cx, int cy);
void zoom_fit_image(void);
void tool_cancel(void);
void tool_dblclick(void);
int app_confirm_discard(void);
int app_quit_request(void);
const char *tool_cursor_for(int ix, int iy, int *cursor_idx);
void app_load_file(const char *path);
void set_canvas_size(int w, int h);
void text_commit(void);
void text_autogrow(void);
extern const int SHAPE_W[6];
int shape_lw(void);
void text_set_font(const char *name);
void text_set_size(int pt);
void text_toggle(int which);
void text_select_all(void);
void app_bake_text(void);
void app_crop_to_selection(void);
Img *app_flat_image(int *owned);
int text_has_sel(void);
int text_edit_cmd(int id);
void tool_status_text(char *out, int n);
extern int g_file_filter;
int list_popup(const char **items, int n, int x, int y, int w, int cur);
int menu_bar_titles(char out[][32], int *xs, int *ws);

/* ---- main.c ---- */
void app_new_image(int w, int h);
void set_status_help(const char *s);
#endif
