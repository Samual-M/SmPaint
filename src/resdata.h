/* Resource data extracted from the original mspaint.exe (see resdata.c, generated). */
#ifndef RESDATA_H
#define RESDATA_H
#include <stdint.h>

typedef struct { int hx, hy; const uint32_t *argb; } ResCursor; /* 32x32 ARGB */
typedef struct { int id; const char *text; } ResString;
typedef struct { int type, id, depth; const char *text; } ResMenuItem; /* type 1 popup,2 item,3 sep,0 end */
typedef struct { int id, cls; uint32_t style; short x, y, w, h; const char *text; } ResCtl;
typedef struct { const char *title; int w, h, nctl; const ResCtl *ctl; } ResDlg;

/* 28 glyphs of 16x16 (strip 448x16) */
extern const uint32_t res_toolglyph[448 * 16];
extern const uint32_t res_textglyph[186 * 15];
extern const uint32_t res_zoomlabels[92 * 9];
extern const uint32_t res_airbrush[72 * 24];
extern const uint32_t res_seltrans[72 * 23];
extern const uint32_t res_icon_size[15 * 15], res_icon_pos[15 * 15];
extern const uint32_t res_brushmask1[1], res_brushmask2[4], res_brushmask3[9], res_brushmask4[16],
    res_brushmask5[25], res_brushmask6[36], res_brushmask7[49], res_brushmask8[64];
extern const uint32_t res_stretchicon0[1024], res_stretchicon1[1024], res_stretchicon2[1024], res_stretchicon3[1024];
extern const uint32_t res_appicon48[48 * 48], res_appicon32[32 * 32], res_appicon16[16 * 16];
extern const ResCursor res_cursors[16]; /* index = original cursor id - 20 */
extern const ResString res_strings[];
extern const int res_nstrings;
extern const ResMenuItem res_menu_main[], res_menu_text[], res_menu_sel[];
extern const ResDlg res_dlg_attributes, res_dlg_zoom, res_dlg_fliprot, res_dlg_stretch, res_dlg_grid, res_dlg_pagesetup;
#endif
