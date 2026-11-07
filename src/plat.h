/*
 * plat.h - the ONLY interface between the portable Paint core and the operating system.
 * To port Paint to another OS, implement these functions in a new plat_<os>.c file.
 * The core draws everything (menus, dialogs, canvas) into one 0x00RRGGBB framebuffer.
 */
#ifndef PLAT_H
#define PLAT_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

enum { EV_NONE = 0, EV_RESIZE, EV_REDRAW, EV_MOUSEDOWN, EV_MOUSEUP, EV_MOUSEMOVE, EV_WHEEL, EV_KEYDOWN, EV_CHAR,
       EV_CLOSE, EV_LEAVE, EV_FOCUSOUT, EV_DROPFILE,
       EV_PINCH /* touchpad pinch: button=1 begin/2 update/3 end; key = cumulative scale*1000; x,y = pointer */ };
enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4 };
enum { BTN_LEFT = 1, BTN_MIDDLE = 2, BTN_RIGHT = 3 };
/* special key codes (letters/digits are their lowercase ASCII value, with MOD_CTRL etc. in mods) */
enum { KEY_BACKSPACE = 8, KEY_TAB = 9, KEY_ENTER = 13, KEY_ESC = 27, KEY_SPACE = 32,
       KEY_DELETE = 127, KEY_LEFT = 256, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
       KEY_INSERT, KEY_F1, KEY_PLUS, KEY_MINUS };

typedef struct PEvent {
    int type;
    int x, y;       /* mouse position (client px) / new size for EV_RESIZE (x=w,y=h) */
    int button;     /* BTN_* for mouse events; wheel: +1 up / -1 down */
    int key;        /* EV_KEYDOWN */
    int mods;       /* MOD_* */
    uint32_t ch;    /* EV_CHAR: unicode code point */
} PEvent;

/* EV_DROPFILE: a file was dropped on the window; fetch its path with plat_drop_path() */
const char *plat_drop_path(void);
/* The platform layer may call this hook (if set) from inside an OS modal loop (e.g. live window resizing on
 * Windows/macOS) so the core can process pending events and repaint while the loop is running. */
void plat_set_live_hook(void (*fn)(void));
/* resize the window's client area to w x h, or maximise it if that would not fit the screen */
void plat_fit_window(int w, int h);
int plat_init(int w, int h, int minw, int minh, const char *title);
void plat_shutdown(void);
/* Wait for an event up to timeout_ms (-1 = forever). Returns 0 on timeout (ev->type = EV_NONE). */
int plat_wait_event(PEvent *ev, int timeout_ms);
void plat_present(const uint32_t *px, int w, int h); /* blit whole framebuffer */
void plat_set_title(const char *utf8);
void plat_set_icon(const uint32_t *argb, int w, int h);
void *plat_make_cursor(const uint32_t *argb32x32, int hx, int hy);
void plat_set_cursor(void *cursor); /* NULL = default arrow */
uint32_t plat_ticks_ms(void);

/* clipboard (images). pixels are 0x00RRGGBB, row-major */
void plat_clip_set_image(const uint32_t *px, int w, int h);
uint32_t *plat_clip_get_image(int *w, int *h); /* malloc'd or NULL */
void plat_clip_set_text(const char *utf8);
char *plat_clip_get_text(void);                 /* malloc'd UTF-8 or NULL */
/* Native file dialog (the desktop's own: KDE/GNOME dialog, Windows common dialog, macOS panel).
   names/pats: filter display names and ';'-separated patterns like "*.jpg;*.jpeg". On entry *fidx is the preselected filter,
   on return the chosen one (or -1 if unknown). path = initial path in / out chosen path (UTF-8, '/').
   Returns 1 = chosen, 0 = cancelled, -1 = no native dialog available (caller falls back to the built-in one). */
int plat_file_dialog(int save, const char *title, char *path, int pathn, const char *const *names, const char *const *pats, int nfilters, int *fidx);

/* system font rendering for the Text tool. Returns malloc'd w*h alpha mask (0 or 255), or NULL. */
uint8_t *plat_render_text(const char *utf8, const char *family, int px_size, int bold, int italic, int underline,
                          int *w, int *h);

/* file system helpers */
typedef struct { char name[256]; int is_dir; long size; } PDirEnt;
int plat_listdir(const char *path, PDirEnt **out, int *count); /* malloc'd array; 0 on success */
const char *plat_home_dir(void);
int plat_getcwd(char *buf, int n);
#define PATH_SEP '/'   /* all paths exchanged with the core are UTF-8 and use '/' on every OS */

#ifdef _WIN32
/* fopen() that accepts UTF-8 paths; implemented in plat_win32.c */
FILE *plat_fopen(const char *path, const char *mode);
#define fopen plat_fopen
#endif

#endif
