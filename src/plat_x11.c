/*
 * plat_x11.c - Linux/X11 platform layer for Paint.
 * Implements plat.h using Xlib (window, events, clipboard, cursors) and cairo (system font rendering
 * for the Text tool only).  Everything else is in the portable core.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <stdlib.h>
#include <dirent.h>
#include <locale.h>
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "plat.h"
#include "imgio.h"

static Display *dpy;
static Window win;
static GC gc;
static int scr;
static Visual *vis;
static int depth;
static Atom a_wmdel, a_clip, a_targets, a_png, a_bmp, a_prop, a_neticon, a_utf8, a_target_str;
static XIM xim;
static XIC xic;
static int cur_w, cur_h;
static PEvent pending;
static int has_pending;
static uint8_t *clip_png; static size_t clip_png_n;
static uint8_t *clip_bmp; static size_t clip_bmp_n;

static int x_err(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
/* ---------- XInput 2.4 pinch gestures (hand-declared so no libXi headers are required) ---------- */
typedef struct { int deviceid; int mask_len; unsigned char *mask; } XIEventMask_;
extern int XIQueryVersion(Display *, int *, int *);
extern int XISelectEvents(Display *, Window, XIEventMask_ *, int);
typedef struct { int base, latched, locked, effective; } XIState_;
typedef struct {
    int type; unsigned long serial; Bool send_event; Display *display; int extension; int evtype; Time time;
    int deviceid, sourceid, detail; Window root, event, child;
    double root_x, root_y, event_x, event_y;
    int flags; XIState_ mods; XIState_ group;
    double delta_x, delta_y, delta_unaccel_x, delta_unaccel_y, scale, delta_angle;
} XIPinch_;
static int xi_opcode = -1;
static void xi_init(void)
{
    int ev, er;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &ev, &er)) { xi_opcode = -1; return; }
    int maj = 2, min = 4;
    if (XIQueryVersion(dpy, &maj, &min) != Success || maj < 2 || (maj == 2 && min < 4)) { xi_opcode = -1; return; }
    unsigned char mask[4] = { 0, 0, 0, 0 };
    for (int t = 27; t <= 29; t++) mask[t >> 3] |= (unsigned char)(1 << (t & 7));
    XIEventMask_ m = { 1 /* XIAllMasterDevices */, 4, mask };
    XISelectEvents(dpy, win, &m, 1);
}
static int pinch_event(XEvent *e, PEvent *out)
{
    if (xi_opcode < 0 || e->type != GenericEvent || e->xcookie.extension != xi_opcode) return 0;
    int ok = 0;
    if (XGetEventData(dpy, &e->xcookie)) {
        int t = e->xcookie.evtype;
        if (t >= 27 && t <= 29) {
            const XIPinch_ *p = (const XIPinch_ *)e->xcookie.data;
            double sc = p->scale;
            if (t == 27) sc = 1.0;
            if (t == 29 || (sc > 0.05 && sc < 20.0)) {
                out->type = EV_PINCH; out->button = t - 26; out->key = (int)(sc * 1000.0);
                out->x = (int)p->event_x; out->y = (int)p->event_y; ok = 1;
            }
        }
        XFreeEventData(dpy, &e->xcookie);
    }
    return ok;
}

void plat_fit_window(int w, int h)
{
    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
    if (sw > sh * 22 / 10) sw /= 2;               /* side-by-side monitors: assume one of them */
    int aw = sw - 24, ah = sh - 90;                /* leave room for window decorations / panels */
    Atom st = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom mh = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False), mv = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    int big = (w > aw || h > ah);
    XEvent e; memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage; e.xclient.window = win; e.xclient.message_type = st; e.xclient.format = 32;
    e.xclient.data.l[0] = big ? 1 : 0; e.xclient.data.l[1] = (long)mh; e.xclient.data.l[2] = (long)mv; e.xclient.data.l[3] = 1;
    XSendEvent(dpy, RootWindow(dpy, scr), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    if (w > aw) w = aw;
    if (h > ah) h = ah;
    if (w < 640) w = 640;
    if (h < 480) h = 480;
    XResizeWindow(dpy, win, (unsigned)w, (unsigned)h);
    XFlush(dpy);
}

/* ---------- XDND (file drop) ---------- */
static Atom a_dnd_aware, a_dnd_enter, a_dnd_pos, a_dnd_status, a_dnd_drop, a_dnd_finished, a_dnd_sel, a_dnd_copy, a_urilist, a_dnd_prop;
static Window dnd_src; static int dnd_ok; static Atom dnd_text_plain;
static char drop_path[1024];
void plat_set_live_hook(void (*fn)(void)) { (void)fn; } /* X11 delivers resize events through the normal loop */
const char *plat_drop_path(void) { return drop_path; }
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int parse_uri_list(const char *t, size_t n)
{
    /* first file:// entry, percent-decoded */
    size_t i = 0;
    while (i < n) {
        size_t e = i; while (e < n && t[e] != '\r' && t[e] != '\n') e++;
        if (e - i > 7 && !strncmp(t + i, "file://", 7)) {
            size_t k = i + 7; while (k < e && t[k] != '/') k++; /* skip host */
            size_t o = 0;
            for (; k < e && o < sizeof drop_path - 1; k++) {
                if (t[k] == '%' && k + 2 < e + 0 && hexv(t[k + 1]) >= 0 && hexv(t[k + 2]) >= 0) { drop_path[o++] = (char)(hexv(t[k + 1]) * 16 + hexv(t[k + 2])); k += 2; }
                else drop_path[o++] = t[k];
            }
            drop_path[o] = 0; return o > 0;
        }
        i = e; while (i < n && (t[i] == '\r' || t[i] == '\n')) i++;
    }
    return 0;
}
static void dnd_send(Atom type, long d1, long d2, long d3)
{
    XEvent r; memset(&r, 0, sizeof r);
    r.xclient.type = ClientMessage; r.xclient.window = dnd_src; r.xclient.message_type = type; r.xclient.format = 32;
    r.xclient.data.l[0] = (long)win; r.xclient.data.l[1] = d1; r.xclient.data.l[2] = d2; r.xclient.data.l[3] = d3;
    XSendEvent(dpy, dnd_src, False, NoEventMask, &r); XFlush(dpy);
}
static void dnd_init(void)
{
    a_dnd_aware = XInternAtom(dpy, "XdndAware", False); a_dnd_enter = XInternAtom(dpy, "XdndEnter", False);
    a_dnd_pos = XInternAtom(dpy, "XdndPosition", False); a_dnd_status = XInternAtom(dpy, "XdndStatus", False);
    a_dnd_drop = XInternAtom(dpy, "XdndDrop", False); a_dnd_finished = XInternAtom(dpy, "XdndFinished", False);
    a_dnd_sel = XInternAtom(dpy, "XdndSelection", False); a_dnd_copy = XInternAtom(dpy, "XdndActionCopy", False);
    a_urilist = XInternAtom(dpy, "text/uri-list", False); a_dnd_prop = XInternAtom(dpy, "PAINT_DND", False);
    long ver = 5; XChangeProperty(dpy, win, a_dnd_aware, XA_ATOM, 32, PropModeReplace, (unsigned char *)&ver, 1);
}
int plat_init(int w, int h, int minw, int minh, const char *title)
{
    XSetErrorHandler(x_err);
    setlocale(LC_ALL, "");
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "SmPaint: cannot open X display\n"); return -1; }
    scr = DefaultScreen(dpy);
    vis = DefaultVisual(dpy, scr);
    depth = DefaultDepth(dpy, scr);
    XSetWindowAttributes sa;
    sa.background_pixel = BlackPixel(dpy, scr);
    sa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                    PointerMotionMask | LeaveWindowMask | FocusChangeMask | PropertyChangeMask;
    win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0, w, h, 0, depth, InputOutput, vis, CWBackPixel | CWEventMask, &sa);
    XSizeHints *sh = XAllocSizeHints();
    sh->flags = PMinSize | PSize;
    sh->min_width = minw; sh->min_height = minh; sh->width = w; sh->height = h;
    XSetWMNormalHints(dpy, win, sh);
    XFree(sh);
    { XWMHints *wh = XAllocWMHints(); wh->flags = InputHint; wh->input = True; XSetWMHints(dpy, win, wh); XFree(wh); }
    XClassHint ch = { (char *)"mspaint", (char *)"Paint" };
    XSetClassHint(dpy, win, &ch);
    a_wmdel = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wmdel, 1);
    a_clip = XInternAtom(dpy, "CLIPBOARD", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    a_png = XInternAtom(dpy, "image/png", False);
    a_bmp = XInternAtom(dpy, "image/bmp", False);
    a_prop = XInternAtom(dpy, "PAINT_SEL", False);
    a_neticon = XInternAtom(dpy, "_NET_WM_ICON", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_target_str = a_utf8;
    plat_set_title(title);
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetLocaleModifiers("");
    xim = XOpenIM(dpy, NULL, NULL, NULL);
    if (xim) xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win, XNFocusWindow, win, NULL);
    dnd_init();
    xi_init();
    XMapWindow(dpy, win);
    cur_w = w; cur_h = h;
    return 0;
}
void plat_shutdown(void)
{
    if (xic) XDestroyIC(xic);
    if (xim) XCloseIM(xim);
    if (dpy) XCloseDisplay(dpy);
}
void plat_set_title(const char *t)
{
    XStoreName(dpy, win, t);
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_NAME", False), a_utf8, 8, PropModeReplace, (const unsigned char *)t, (int)strlen(t));
}
void plat_set_icon(const uint32_t *argb, int w, int h)
{
    long *d = (long *)malloc(sizeof(long) * (2 + (size_t)w * h));
    d[0] = w; d[1] = h;
    for (int i = 0; i < w * h; i++) d[2 + i] = argb[i];
    XChangeProperty(dpy, win, a_neticon, XA_CARDINAL, 32, PropModeAppend, (unsigned char *)d, 2 + w * h);
    free(d);
}
uint32_t plat_ticks_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void plat_present(const uint32_t *px, int w, int h)
{
    XImage *im = XCreateImage(dpy, vis, depth, ZPixmap, 0, (char *)px, w, h, 32, w * 4);
    XPutImage(dpy, win, gc, im, 0, 0, 0, 0, w, h);
    im->data = NULL;
    XDestroyImage(im);
    XFlush(dpy);
}

void *plat_make_cursor(const uint32_t *argb, int hx, int hy)
{
    unsigned char src[128], msk[128];
    memset(src, 0, sizeof src); memset(msk, 0, sizeof msk);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            uint32_t v = argb[y * 32 + x];
            if ((v >> 24) < 128) continue;
            int lum = ((v >> 16 & 255) + (v >> 8 & 255) + (v & 255)) / 3;
            msk[y * 4 + x / 8] |= 1 << (x % 8);
            if (lum < 128) src[y * 4 + x / 8] |= 1 << (x % 8); /* black pixel */
        }
    Pixmap ps = XCreateBitmapFromData(dpy, win, (char *)src, 32, 32);
    Pixmap pm = XCreateBitmapFromData(dpy, win, (char *)msk, 32, 32);
    XColor fg = { 0, 0, 0, 0, 0, 0 }, bg = { 0, 65535, 65535, 65535, 0, 0 };
    /* source bit=1 shows fg: use black for set bits, white for unset-but-masked */
    Cursor c = XCreatePixmapCursor(dpy, ps, pm, &fg, &bg, hx, hy);
    XFreePixmap(dpy, ps); XFreePixmap(dpy, pm);
    return (void *)(uintptr_t)c;
}
void plat_set_cursor(void *c)
{
    if (c) XDefineCursor(dpy, win, (Cursor)(uintptr_t)c);
    else XUndefineCursor(dpy, win);
}

/* ---------- clipboard ---------- */
void plat_clip_set_image(const uint32_t *px, int w, int h)
{
    free(clip_png); free(clip_bmp);
    clip_png = png_encode(px, w, h, &clip_png_n);
    clip_bmp = bmp_encode(px, w, h, 24, 1, &clip_bmp_n); /* BMP file with header */
    XSetSelectionOwner(dpy, a_clip, win, CurrentTime);
}
static void handle_selreq(XSelectionRequestEvent *r)
{
    XSelectionEvent n;
    memset(&n, 0, sizeof n);
    n.type = SelectionNotify; n.display = r->display; n.requestor = r->requestor;
    n.selection = r->selection; n.target = r->target; n.time = r->time; n.property = None;
    Atom prop = r->property ? r->property : r->target;
    if (r->target == a_targets) {
        Atom t[3] = { a_targets, a_png, a_bmp };
        XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 3);
        n.property = prop;
    } else if (r->target == a_png && clip_png) {
        XChangeProperty(dpy, r->requestor, prop, a_png, 8, PropModeReplace, clip_png, (int)clip_png_n);
        n.property = prop;
    } else if (r->target == a_bmp && clip_bmp) {
        XChangeProperty(dpy, r->requestor, prop, a_bmp, 8, PropModeReplace, clip_bmp, (int)clip_bmp_n);
        n.property = prop;
    }
    XSendEvent(dpy, r->requestor, False, 0, (XEvent *)&n);
}
uint32_t *plat_clip_get_image(int *w, int *h)
{
    Window owner = XGetSelectionOwner(dpy, a_clip);
    if (owner == None) return NULL;
    if (owner == win) { /* our own data */
        if (!clip_png) return NULL;
        return png_decode(clip_png, clip_png_n, w, h);
    }
    Atom want[2] = { a_png, a_bmp };
    for (int t = 0; t < 2; t++) {
        XDeleteProperty(dpy, win, a_prop);
        XConvertSelection(dpy, a_clip, want[t], a_prop, win, CurrentTime);
        XFlush(dpy);
        uint32_t t0 = plat_ticks_ms();
        while (plat_ticks_ms() - t0 < 800) {
            while (XPending(dpy)) {
                XEvent e; XNextEvent(dpy, &e);
                if (e.type == SelectionRequest) handle_selreq(&e.xselectionrequest);
                if (e.type == SelectionNotify) {
                    if (e.xselection.property == None) goto next;
                    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
                    XGetWindowProperty(dpy, win, a_prop, 0, 0x7fffffff, True, AnyPropertyType, &type, &fmt, &n, &after, &data);
                    uint32_t *res = NULL;
                    if (data && n) res = (want[t] == a_png) ? png_decode(data, n, w, h) : bmp_decode(data, n, w, h);
                    if (data) XFree(data);
                    if (res) return res;
                    goto next;
                }
            }
            usleep(5000);
        }
    next:;
    }
    return NULL;
}

/* ---------- events ---------- */
static int mods_from(unsigned st)
{
    return ((st & ShiftMask) ? MOD_SHIFT : 0) | ((st & ControlMask) ? MOD_CTRL : 0) | ((st & Mod1Mask) ? MOD_ALT : 0);
}
static int key_from_sym(KeySym ks)
{
    switch (ks) {
    case XK_BackSpace: return KEY_BACKSPACE; case XK_Tab: case XK_ISO_Left_Tab: return KEY_TAB;
    case XK_Return: case XK_KP_Enter: return KEY_ENTER; case XK_Escape: return KEY_ESC;
    case XK_Delete: case XK_KP_Delete: return KEY_DELETE;
    case XK_Left: case XK_KP_Left: return KEY_LEFT; case XK_Right: case XK_KP_Right: return KEY_RIGHT;
    case XK_Up: case XK_KP_Up: return KEY_UP; case XK_Down: case XK_KP_Down: return KEY_DOWN;
    case XK_Home: case XK_KP_Home: return KEY_HOME; case XK_End: case XK_KP_End: return KEY_END;
    case XK_Prior: case XK_KP_Prior: return KEY_PGUP; case XK_Next: case XK_KP_Next: return KEY_PGDN;
    case XK_Insert: return KEY_INSERT; case XK_F1: return KEY_F1;
    case XK_KP_Add: return KEY_PLUS; case XK_KP_Subtract: return KEY_MINUS;
    case XK_space: return KEY_SPACE;
    }
    if (ks >= 'A' && ks <= 'Z') return (int)ks - 'A' + 'a';
    if (ks >= 0x20 && ks < 0x7f) return (int)ks;
    return 0;
}
static uint32_t utf8_first(const char *s, int n)
{
    const unsigned char *u = (const unsigned char *)s;
    if (n <= 0) return 0;
    if (u[0] < 0x80) return u[0];
    if (u[0] >= 0xf0 && n >= 4) return ((u[0] & 7) << 18) | ((u[1] & 63) << 12) | ((u[2] & 63) << 6) | (u[3] & 63);
    if (u[0] >= 0xe0 && n >= 3) return ((u[0] & 15) << 12) | ((u[1] & 63) << 6) | (u[2] & 63);
    if (u[0] >= 0xc0 && n >= 2) return ((u[0] & 31) << 6) | (u[1] & 63);
    return 0;
}

static int translate(XEvent *e, PEvent *out)
{
    memset(out, 0, sizeof *out);
    switch (e->type) {
    case Expose:
        if (e->xexpose.count) return 0;
        out->type = EV_REDRAW; return 1;
    case ConfigureNotify:
        if (e->xconfigure.width != cur_w || e->xconfigure.height != cur_h) {
            cur_w = e->xconfigure.width; cur_h = e->xconfigure.height;
            out->type = EV_RESIZE; out->x = cur_w; out->y = cur_h; return 1;
        }
        return 0;
    case ButtonPress: case ButtonRelease: {
        int b = e->xbutton.button;
        out->x = e->xbutton.x; out->y = e->xbutton.y; out->mods = mods_from(e->xbutton.state);
        if (b == 4 || b == 5) {
            if (e->type == ButtonRelease) return 0;
            out->type = EV_WHEEL; out->button = (b == 4) ? 1 : -1; return 1;
        }
        if (b > 3) return 0;
        out->type = (e->type == ButtonPress) ? EV_MOUSEDOWN : EV_MOUSEUP;
        out->button = b == 1 ? BTN_LEFT : b == 2 ? BTN_MIDDLE : BTN_RIGHT;
        return 1;
    }
    case MotionNotify:
        out->type = EV_MOUSEMOVE; out->x = e->xmotion.x; out->y = e->xmotion.y; out->mods = mods_from(e->xmotion.state);
        out->button = (e->xmotion.state & Button1Mask) ? BTN_LEFT : (e->xmotion.state & Button3Mask) ? BTN_RIGHT : 0;
        return 1;
    case GenericEvent: return pinch_event(e, out);
    case LeaveNotify: out->type = EV_LEAVE; return 1;
    case FocusOut: out->type = EV_FOCUSOUT; return 1;
    case ClientMessage: {
        Atom mt = e->xclient.message_type;
        if (mt == a_dnd_enter) {
            dnd_src = (Window)e->xclient.data.l[0]; dnd_ok = 0;
            if (e->xclient.data.l[1] & 1) { /* more than 3 types: read XdndTypeList */
                Atom rt; int rf; unsigned long nit, ba; unsigned char *pd = NULL;
                if (XGetWindowProperty(dpy, dnd_src, XInternAtom(dpy, "XdndTypeList", False), 0, 1024, False, XA_ATOM, &rt, &rf, &nit, &ba, &pd) == Success && pd) {
                    for (unsigned long i = 0; i < nit; i++) if (((Atom *)pd)[i] == a_urilist) dnd_ok = 1;
                    XFree(pd);
                }
            } else for (int i = 2; i < 5; i++) if ((Atom)e->xclient.data.l[i] == a_urilist) dnd_ok = 1;
            return 0;
        }
        if (mt == a_dnd_pos) {
            dnd_src = (Window)e->xclient.data.l[0];
            dnd_send(a_dnd_status, dnd_ok ? 1 : 0, 0, dnd_ok ? (long)a_dnd_copy : 0);
            return 0;
        }
        if (mt == a_dnd_drop) {
            dnd_src = (Window)e->xclient.data.l[0];
            if (dnd_ok) XConvertSelection(dpy, a_dnd_sel, a_urilist, a_dnd_prop, win, (Time)e->xclient.data.l[2]);
            else dnd_send(a_dnd_finished, 0, 0, 0);
            return 0;
        }
        if ((Atom)e->xclient.data.l[0] == a_wmdel) { out->type = EV_CLOSE; return 1; }
        return 0;
    }
    case SelectionNotify:
        if (e->xselection.property == a_dnd_prop) {
            Atom rt; int rf; unsigned long nit, ba; unsigned char *pd = NULL; int got = 0;
            if (XGetWindowProperty(dpy, win, a_dnd_prop, 0, 65536, True, AnyPropertyType, &rt, &rf, &nit, &ba, &pd) == Success && pd) {
                got = parse_uri_list((const char *)pd, nit); XFree(pd);
            }
            dnd_send(a_dnd_finished, got ? 1 : 0, got ? (long)a_dnd_copy : 0, 0);
            if (got) { out->type = EV_DROPFILE; return 1; }
        }
        return 0;
    case SelectionRequest: handle_selreq(&e->xselectionrequest); return 0;
    case KeyPress: {
        char buf[32]; KeySym ks = 0; int n;
        int mods = mods_from(e->xkey.state);
        if (xic) { Status st; n = Xutf8LookupString(xic, &e->xkey, buf, sizeof buf - 1, &ks, &st); if (st == XLookupNone) n = 0; }
        else n = XLookupString(&e->xkey, buf, sizeof buf - 1, &ks, NULL);
        KeySym base = XLookupKeysym(&e->xkey, 0);
        int key = key_from_sym(ks ? ks : base);
        if ((mods & (MOD_CTRL | MOD_ALT)) || !key || key < 32 || key >= 256) {
            if (!(key = key_from_sym(base)) ) key = key_from_sym(ks);
        }
        out->type = EV_KEYDOWN; out->key = key; out->mods = mods;
        if (!(mods & (MOD_CTRL | MOD_ALT)) && n > 0 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] != 127) {
            pending.type = EV_CHAR; pending.ch = utf8_first(buf, n); has_pending = 1;
        }
        return 1;
    }
    }
    return 0;
}

int plat_wait_event(PEvent *ev, int timeout_ms)
{
    uint32_t t0 = plat_ticks_ms();
    for (;;) {
        if (has_pending) { *ev = pending; has_pending = 0; return 1; }
        while (XPending(dpy)) {
            XEvent e; XNextEvent(dpy, &e);
            if (XFilterEvent(&e, None)) continue;
            /* coalesce consecutive motion events */
            while (e.type == MotionNotify && XPending(dpy)) {
                XEvent n; XPeekEvent(dpy, &n);
                if (n.type != MotionNotify) break;
                XNextEvent(dpy, &e);
            }
            if (translate(&e, ev)) return 1;
        }
        int wait;
        if (timeout_ms < 0) wait = -1;
        else {
            int el = (int)(plat_ticks_ms() - t0);
            wait = timeout_ms - el;
            if (wait <= 0) { memset(ev, 0, sizeof *ev); return 0; }
        }
        fd_set fds; FD_ZERO(&fds); FD_SET(ConnectionNumber(dpy), &fds);
        struct timeval tv, *ptv = NULL;
        if (wait >= 0) { tv.tv_sec = wait / 1000; tv.tv_usec = (wait % 1000) * 1000; ptv = &tv; }
        select(ConnectionNumber(dpy) + 1, &fds, NULL, NULL, ptv);
    }
}

/* ---------- text rendering (cairo, aliased) ---------- */
uint8_t *plat_render_text(const char *s, const char *family, int px, int bold, int italic, int underline, int *w, int *h)
{
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_A8, 8, 8);
    cairo_t *cr = cairo_create(tmp);
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_NONE);
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_FULL);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_ON);
    cairo_set_font_options(cr, fo);
    cairo_select_font_face(cr, family && *family ? family : "Sans", italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, px);
    cairo_font_extents_t fe; cairo_font_extents(cr, &fe);
    int lineh = (int)ceil(fe.ascent + fe.descent);
    /* split lines */
    int nl = 1, maxw = 1;
    for (const char *p = s; *p; p++) if (*p == '\n') nl++;
    char **lines = (char **)calloc(nl, sizeof(char *));
    int *lw = (int *)calloc(nl, sizeof(int));
    const char *p = s; int li = 0;
    for (;;) {
        const char *q = strchr(p, '\n');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        lines[li] = (char *)malloc(n + 1); memcpy(lines[li], p, n); lines[li][n] = 0;
        if (n && lines[li][n - 1] == '\r') lines[li][n - 1] = 0;
        cairo_text_extents_t te; cairo_text_extents(cr, lines[li], &te);
        lw[li] = (int)ceil(te.x_advance > te.width + te.x_bearing ? te.x_advance : te.width + te.x_bearing);
        if (lw[li] > maxw) maxw = lw[li];
        li++;
        if (!q) break;
        p = q + 1;
    }
    cairo_destroy(cr); cairo_surface_destroy(tmp);
    int W = maxw + 1, H = lineh * nl;
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
    cr = cairo_create(sf);
    cairo_set_font_options(cr, fo);
    cairo_select_font_face(cr, family && *family ? family : "Sans", italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, px);
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    for (int i = 0; i < nl; i++) {
        double y = i * lineh + fe.ascent;
        cairo_move_to(cr, 0, y);
        cairo_show_text(cr, lines[i]);
        if (underline && lw[i]) cairo_rectangle(cr, 0, floor(y) + 1, lw[i], 1), cairo_fill(cr);
    }
    cairo_surface_flush(sf);
    uint8_t *out = (uint8_t *)malloc((size_t)W * H);
    unsigned char *d = cairo_image_surface_get_data(sf); int st = cairo_image_surface_get_stride(sf);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) out[y * W + x] = d[y * st + x] >= 128 ? 255 : 0;
    for (int i = 0; i < nl; i++) free(lines[i]);
    free(lines); free(lw);
    cairo_font_options_destroy(fo);
    cairo_destroy(cr); cairo_surface_destroy(sf);
    *w = W; *h = H;
    return out;
}

/* ---------- file system ---------- */
int plat_listdir(const char *path, PDirEnt **out, int *count)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    int cap = 64, n = 0;
    PDirEnt *a = (PDirEnt *)malloc(cap * sizeof(PDirEnt));
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".")) continue;
        if (n == cap) { cap *= 2; a = (PDirEnt *)realloc(a, cap * sizeof(PDirEnt)); }
        char full[4096]; snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        snprintf(a[n].name, sizeof a[n].name, "%s", de->d_name);
        a[n].is_dir = S_ISDIR(st.st_mode); a[n].size = (long)st.st_size;
        n++;
    }
    closedir(d);
    *out = a; *count = n;
    return 0;
}
const char *plat_home_dir(void)
{
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/";
}
int plat_getcwd(char *buf, int n) { return getcwd(buf, n) ? 0 : -1; }
