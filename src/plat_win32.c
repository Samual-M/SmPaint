/*
 * plat_win32.c - Windows platform layer for Paint (Windows 10 and later; x86, x64 and ARM64).
 * Plain C using only the Win32 API (user32, gdi32, shell32, ole-free). No third-party libraries.
 *
 * Build with MinGW-w64 / llvm-mingw / MSYS2 (see Makefile.win) or any C99 compiler with the Windows SDK.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include "plat.h"
#undef fopen            /* plat.h maps fopen to plat_fopen */

/* ------------------------------------------------------------------ utf-8 helpers */
static wchar_t *u8_to_w(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static int w_to_u8(const wchar_t *w, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    if (n <= 0) { if (cap > 0) out[0] = 0; return -1; }
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
    return 0;
}
FILE *plat_fopen(const char *path, const char *mode)
{
    wchar_t *wp = u8_to_w(path), *wm = u8_to_w(mode);
    FILE *f = NULL;
    if (wp && wm) f = _wfopen(wp, wm);
    free(wp); free(wm);
    return f;
}

/* ------------------------------------------------------------------ state */
#define QCAP 1024
static PEvent q[QCAP];
static int q_head, q_tail;
static HWND hwnd;
static int scale = 1;                  /* integer UI scale: framebuffer pixels -> device pixels */
static int cur_w, cur_h;               /* client size in framebuffer pixels */
static int min_w, min_h;
static uint32_t *shadow; static int sh_w, sh_h;
static HCURSOR cur_cursor;
static int tracking;
static int in_sizemove, in_hook, hook_busy;
static void (*live_hook)(void);
static char drop_path[1024];
static int quit_posted;

void plat_set_live_hook(void (*fn)(void)) { live_hook = fn; }
const char *plat_drop_path(void) { return drop_path; }

static void push(const PEvent *e)
{
    int n = (q_tail + 1) % QCAP;
    if (n == q_head) return;           /* queue full: drop */
    q[q_tail] = *e; q_tail = n;
}
static void push_simple(int type, int x, int y, int button, int key, int mods, uint32_t ch)
{
    PEvent e; memset(&e, 0, sizeof e);
    e.type = type; e.x = x; e.y = y; e.button = button; e.key = key; e.mods = mods; e.ch = ch;
    push(&e);
}

/* ------------------------------------------------------------------ dpi */
typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(HANDLE);
typedef BOOL (WINAPI *AdjustWindowRectExForDpi_t)(LPRECT, DWORD, BOOL, DWORD, UINT);

static int window_dpi(HWND h)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    GetDpiForWindow_t f = u ? (GetDpiForWindow_t)(void *)GetProcAddress(u, "GetDpiForWindow") : NULL;
    if (f && h) { UINT d = f(h); if (d) return (int)d; }
    HDC dc = GetDC(NULL);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d > 0 ? d : 96;
}
static int scale_for_dpi(int dpi) { int s = (dpi * 2 + 96) / 192; return s < 1 ? 1 : s > 4 ? 4 : s; }
static void adjust_rect(RECT *r, DWORD style, DWORD ex, int dpi)
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    AdjustWindowRectExForDpi_t f = u ? (AdjustWindowRectExForDpi_t)(void *)GetProcAddress(u, "AdjustWindowRectExForDpi") : NULL;
    if (f) f(r, style, FALSE, ex, (UINT)dpi); else AdjustWindowRectEx(r, style, FALSE, ex);
}

/* ------------------------------------------------------------------ keys */
static int cur_mods(void)
{
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= MOD_SHIFT;
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (ctrl && alt && (GetKeyState(VK_RMENU) & 0x8000)) { ctrl = 0; alt = 0; }   /* AltGr produces text, not shortcuts */
    if (ctrl) m |= MOD_CTRL;
    if (alt) m |= MOD_ALT;
    return m;
}
static int key_from_vk(WPARAM vk)
{
    switch (vk) {
    case VK_BACK: return KEY_BACKSPACE; case VK_TAB: return KEY_TAB; case VK_RETURN: return KEY_ENTER;
    case VK_ESCAPE: return KEY_ESC; case VK_SPACE: return KEY_SPACE; case VK_DELETE: return KEY_DELETE;
    case VK_LEFT: return KEY_LEFT; case VK_RIGHT: return KEY_RIGHT; case VK_UP: return KEY_UP; case VK_DOWN: return KEY_DOWN;
    case VK_HOME: return KEY_HOME; case VK_END: return KEY_END; case VK_PRIOR: return KEY_PGUP; case VK_NEXT: return KEY_PGDN;
    case VK_INSERT: return KEY_INSERT; case VK_F1: return KEY_F1;
    case VK_ADD: case VK_OEM_PLUS: return KEY_PLUS;
    case VK_SUBTRACT: case VK_OEM_MINUS: return KEY_MINUS;
    }
    if (vk >= 'A' && vk <= 'Z') return (int)vk - 'A' + 'a';
    if (vk >= '0' && vk <= '9') return (int)vk;
    return 0;
}

/* ------------------------------------------------------------------ painting */
static void blit(HDC dc)
{
    if (!shadow || sh_w <= 0 || sh_h <= 0) return;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sh_w; bi.bmiHeader.biHeight = -sh_h;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);                          /* nearest-neighbour: crisp pixel scaling */
    StretchDIBits(dc, 0, 0, sh_w * scale, sh_h * scale, 0, 0, sh_w, sh_h, shadow, &bi, DIB_RGB_COLORS, SRCCOPY);
}
void plat_present(const uint32_t *px, int w, int h)
{
    if (!hwnd) return;
    if (w != sh_w || h != sh_h) {
        free(shadow); shadow = (uint32_t *)malloc((size_t)w * (size_t)h * 4); sh_w = w; sh_h = h;
        if (!shadow) { sh_w = sh_h = 0; return; }
    }
    memcpy(shadow, px, (size_t)w * (size_t)h * 4);
    HDC dc = GetDC(hwnd);
    if (dc) { blit(dc); ReleaseDC(hwnd, dc); }
}

/* run the core's event/paint code from inside a modal OS loop (live resizing) */
static void run_hook(void)
{
    if (!live_hook || hook_busy) return;
    hook_busy = 1; in_hook = 1;
    live_hook();
    in_hook = 0; hook_busy = 0;
}

/* ------------------------------------------------------------------ window procedure */
static unsigned wheel_last_t;
static double pinch_scale = 1.0;
static int wheel_acc;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        blit(dc);
        EndPaint(h, &ps);
        push_simple(EV_REDRAW, 0, 0, 0, 0, 0, 0);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mi = (MINMAXINFO *)lp;
        RECT r = { 0, 0, min_w * scale, min_h * scale };
        adjust_rect(&r, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE), window_dpi(h));
        mi->ptMinTrackSize.x = r.right - r.left; mi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) return 0;
        int w = LOWORD(lp) / scale, hh = HIWORD(lp) / scale;
        if (w < 1) w = 1; if (hh < 1) hh = 1;
        if (w != cur_w || hh != cur_h) {
            cur_w = w; cur_h = hh;
            push_simple(EV_RESIZE, w, hh, 0, 0, 0, 0);
            if (in_sizemove) run_hook();      /* live repaint while the user drags the window edge */
        }
        return 0;
    }
    case WM_ENTERSIZEMOVE: in_sizemove = 1; return 0;
    case WM_EXITSIZEMOVE: in_sizemove = 0; return 0;
    case WM_DPICHANGED: {
        int dpi = HIWORD(wp);
        scale = scale_for_dpi(dpi);
        const RECT *r = (const RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE: push_simple(EV_CLOSE, 0, 0, 0, 0, 0, 0); return 0;
    case WM_DESTROY: if (!quit_posted) { quit_posted = 1; PostQuitMessage(0); } return 0;
    case WM_KILLFOCUS: push_simple(EV_FOCUSOUT, 0, 0, 0, 0, 0, 0); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(cur_cursor ? cur_cursor : LoadCursorW(NULL, IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        int b = m == WM_LBUTTONDOWN ? BTN_LEFT : m == WM_RBUTTONDOWN ? BTN_RIGHT : BTN_MIDDLE;
        SetCapture(h);
        push_simple(EV_MOUSEDOWN, GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale, b, 0, cur_mods(), 0);
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        int b = m == WM_LBUTTONUP ? BTN_LEFT : m == WM_RBUTTONUP ? BTN_RIGHT : BTN_MIDDLE;
        if (!(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
        push_simple(EV_MOUSEUP, GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale, b, 0, cur_mods(), 0);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!tracking) { TRACKMOUSEEVENT t; memset(&t, 0, sizeof t); t.cbSize = sizeof t; t.dwFlags = TME_LEAVE; t.hwndTrack = h; TrackMouseEvent(&t); tracking = 1; }
        int b = (wp & MK_LBUTTON) ? BTN_LEFT : (wp & MK_RBUTTON) ? BTN_RIGHT : 0;
        /* coalesce: replace a trailing move event instead of queueing many */
        int x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
        int last = (q_tail + QCAP - 1) % QCAP;
        if (q_head != q_tail && q[last].type == EV_MOUSEMOVE) { q[last].x = x; q[last].y = y; q[last].button = b; q[last].mods = cur_mods(); }
        else push_simple(EV_MOUSEMOVE, x, y, b, 0, cur_mods(), 0);
        return 0;
    }
    case WM_MOUSELEAVE: tracking = 0; push_simple(EV_LEAVE, 0, 0, 0, 0, 0, 0); return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &p);
        int delta = GET_WHEEL_DELTA_WPARAM(wp), mods = cur_mods();
        unsigned now = (unsigned)GetTickCount();
        if (m == WM_MOUSEHWHEEL) mods |= MOD_SHIFT;
        if ((mods & MOD_CTRL) && m == WM_MOUSEWHEEL && abs(delta) < 120) {
            /* a precision-touchpad pinch arrives as Ctrl + small wheel deltas: turn it into smooth pinch events */
            if (now - wheel_last_t > 350) { pinch_scale = 1.0; push_simple(EV_PINCH, p.x / scale, p.y / scale, 1, 1000, mods, 0); }
            pinch_scale *= 1.0 + delta / 1200.0;
            if (pinch_scale < 0.05) pinch_scale = 0.05; if (pinch_scale > 20.0) pinch_scale = 20.0;
            push_simple(EV_PINCH, p.x / scale, p.y / scale, 2, (int)(pinch_scale * 1000.0), mods, 0);
            wheel_last_t = now;
            return 0;
        }
        wheel_acc += delta;
        while (wheel_acc >= 120) { wheel_acc -= 120; push_simple(EV_WHEEL, p.x / scale, p.y / scale, 1, 0, mods, 0); }
        while (wheel_acc <= -120) { wheel_acc += 120; push_simple(EV_WHEEL, p.x / scale, p.y / scale, -1, 0, mods, 0); }
        wheel_last_t = now;
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        int key = key_from_vk(wp), mods = cur_mods();
        if (m == WM_SYSKEYDOWN) {
            if (wp == VK_F4 || wp == VK_SPACE || wp == VK_TAB || wp == VK_F10) break;   /* let Windows handle Alt+F4, Alt+Space ... */
        }
        if (wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_MENU || wp == VK_LWIN || wp == VK_RWIN) return 0;
        if (!key) {
            /* punctuation etc.: ask Windows for the character this key makes */
            UINT c = MapVirtualKeyW((UINT)wp, MAPVK_VK_TO_CHAR) & 0x7fff;
            if (c >= 32 && c < 127) key = (int)c;
        }
        if (key) push_simple(EV_KEYDOWN, 0, 0, 0, key, mods, 0);
        if (m == WM_SYSKEYDOWN) return 0;
        break;                                  /* continue to TranslateMessage -> WM_CHAR */
    }
    case WM_KEYUP: return 0;
    case WM_SYSKEYUP: if (wp == VK_F10) break; return 0;   /* tapping Alt must not open the (absent) system menu */
    case WM_SYSCHAR: if (wp == ' ') break; return 0;     /* swallow Alt+letter (core handles menus) so Windows doesn't beep */
    case WM_CHAR: {
        static wchar_t hi;
        uint32_t c = (uint32_t)wp;
        if (c >= 0xD800 && c <= 0xDBFF) { hi = (wchar_t)c; return 0; }
        if (c >= 0xDC00 && c <= 0xDFFF) { if (hi) { c = 0x10000u + (((uint32_t)hi - 0xD800u) << 10) + (c - 0xDC00u); hi = 0; } else return 0; }
        if (c < 32 || c == 127) return 0;
        if ((GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_RMENU) & 0x8000)) return 0;
        push_simple(EV_CHAR, 0, 0, 0, 0, 0, c);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp;
        wchar_t wpth[1024];
        if (DragQueryFileW(d, 0, wpth, 1024)) {
            if (w_to_u8(wpth, drop_path, sizeof drop_path) == 0) push_simple(EV_DROPFILE, 0, 0, 0, 0, 0, 0);
        }
        DragFinish(d);
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* ------------------------------------------------------------------ init / events */
int plat_init(int w, int h, int minw, int minh, const char *title)
{
    /* crisp rendering on high-DPI screens: per-monitor-v2 awareness where available (Windows 10 1703+), else system DPI aware */
    HMODULE u = GetModuleHandleW(L"user32.dll");
    SetProcessDpiAwarenessContext_t sp = u ? (SetProcessDpiAwarenessContext_t)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (!sp || !sp((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) {
        typedef BOOL (WINAPI *SetProcessDPIAware_t)(void);
        SetProcessDPIAware_t f = u ? (SetProcessDPIAware_t)(void *)GetProcAddress(u, "SetProcessDPIAware") : NULL;
        if (f) f();
    }
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.style = CS_OWNDC; wc.lpfnWndProc = wndproc; wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.lpszClassName = L"MSPaintCClass";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));          /* from mspaint.rc if linked */
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return -1;
    min_w = minw; min_h = minh;
    scale = scale_for_dpi(window_dpi(NULL));
    DWORD style = WS_OVERLAPPEDWINDOW, ex = 0;
    RECT r = { 0, 0, w * scale, h * scale };
    adjust_rect(&r, style, ex, window_dpi(NULL));
    wchar_t *wt = u8_to_w(title);
    hwnd = CreateWindowExW(ex, L"MSPaintCClass", wt ? wt : L"SmPaint", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    free(wt);
    if (!hwnd) return -1;
    scale = scale_for_dpi(window_dpi(hwnd));
    cur_w = w; cur_h = h;
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    return 0;
}
void plat_shutdown(void)
{
    if (hwnd) { DestroyWindow(hwnd); hwnd = NULL; }
    free(shadow); shadow = NULL;
}
void plat_set_title(const char *t)
{
    wchar_t *w = u8_to_w(t);
    if (w && hwnd) SetWindowTextW(hwnd, w);
    free(w);
}
uint32_t plat_ticks_ms(void) { return (uint32_t)GetTickCount64(); }

static int pump_messages(void)
{
    MSG msg; int got = 0;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { push_simple(EV_CLOSE, 0, 0, 0, 0, 0, 0); return got; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
        got = 1;
    }
    return got;
}
int plat_wait_event(PEvent *ev, int timeout_ms)
{
    DWORD t0 = (DWORD)GetTickCount();
    for (;;) {
        if (q_head != q_tail) { *ev = q[q_head]; q_head = (q_head + 1) % QCAP; return 1; }
        if (in_hook) { memset(ev, 0, sizeof *ev); return 0; }      /* never pump messages re-entrantly from the live hook */
        pump_messages();
        if (q_head != q_tail) continue;
        DWORD wait;
        if (timeout_ms < 0) wait = INFINITE;
        else {
            int el = (int)(GetTickCount() - t0);
            if (el >= timeout_ms) { memset(ev, 0, sizeof *ev); return 0; }
            wait = (DWORD)(timeout_ms - el);
        }
        MsgWaitForMultipleObjectsEx(0, NULL, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

/* ------------------------------------------------------------------ icons & cursors */
static HBITMAP make_argb_bitmap(const uint32_t *argb, int w, int h)
{
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (bm && bits) memcpy(bits, argb, (size_t)w * (size_t)h * 4);
    return bm;
}
static HICON make_icon(const uint32_t *argb, int w, int h, int is_icon, int hx, int hy)
{
    HBITMAP color = make_argb_bitmap(argb, w, h);
    HBITMAP mask = CreateBitmap(w, h, 1, 1, NULL);          /* all zero: colour bitmap's alpha decides */
    ICONINFO ii; memset(&ii, 0, sizeof ii);
    ii.fIcon = is_icon ? TRUE : FALSE; ii.xHotspot = (DWORD)hx; ii.yHotspot = (DWORD)hy;
    ii.hbmColor = color; ii.hbmMask = mask;
    HICON ic = (color && mask) ? CreateIconIndirect(&ii) : NULL;
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return ic;
}
void plat_set_icon(const uint32_t *argb, int w, int h)
{
    HICON ic = make_icon(argb, w, h, 1, 0, 0);
    if (!ic || !hwnd) return;
    SendMessageW(hwnd, WM_SETICON, w >= 32 ? ICON_BIG : ICON_SMALL, (LPARAM)ic);
}
void *plat_make_cursor(const uint32_t *argb32x32, int hx, int hy)
{
    return (void *)make_icon(argb32x32, 32, 32, 0, hx, hy);
}
void plat_set_cursor(void *c)
{
    cur_cursor = (HCURSOR)c;
    if (hwnd) {
        POINT p; RECT r;
        if (GetCursorPos(&p) && GetClientRect(hwnd, &r)) {
            ScreenToClient(hwnd, &p);
            if (PtInRect(&r, p)) SetCursor(cur_cursor ? cur_cursor : LoadCursorW(NULL, IDC_ARROW));
        }
    }
}

/* ------------------------------------------------------------------ clipboard (CF_DIB + registered "PNG") */
#include "imgio.h"
static UINT fmt_png(void) { static UINT f; if (!f) f = RegisterClipboardFormatW(L"PNG"); return f; }
void plat_clip_set_image(const uint32_t *px, int w, int h)
{
    if (!hwnd || !OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t n;
    uint8_t *bmp = bmp_encode(px, w, h, 24, 0, &n);              /* BITMAPINFOHEADER + pixels, i.e. a packed DIB */
    if (bmp) {
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
        if (g) { void *p = GlobalLock(g); if (p) { memcpy(p, bmp, n); GlobalUnlock(g); if (!SetClipboardData(CF_DIB, g)) GlobalFree(g); } else GlobalFree(g); }
        free(bmp);
    }
    uint8_t *png = png_encode(px, w, h, &n);
    if (png) {
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
        if (g) { void *p = GlobalLock(g); if (p) { memcpy(p, png, n); GlobalUnlock(g); if (!SetClipboardData(fmt_png(), g)) GlobalFree(g); } else GlobalFree(g); }
        free(png);
    }
    CloseClipboard();
}
void plat_clip_set_text(const char *utf8)
{
    if (!hwnd || !OpenClipboard(hwnd)) return;
    EmptyClipboard();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    HGLOBAL g = n > 0 ? GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t)) : NULL;
    if (g) {
        wchar_t *p = (wchar_t *)GlobalLock(g);
        if (p) { MultiByteToWideChar(CP_UTF8, 0, utf8, -1, p, n); GlobalUnlock(g); if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g); }
        else GlobalFree(g);
    }
    CloseClipboard();
}
char *plat_clip_get_text(void)
{
    char *out = NULL;
    if (!OpenClipboard(hwnd)) return NULL;
    HANDLE g = GetClipboardData(CF_UNICODETEXT);
    if (g) {
        const wchar_t *p = (const wchar_t *)GlobalLock(g);
        if (p) {
            int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, NULL, 0, NULL, NULL);
            if (n > 0) { out = (char *)malloc((size_t)n); WideCharToMultiByte(CP_UTF8, 0, p, -1, out, n, NULL, NULL); }
            GlobalUnlock(g);
        }
    }
    CloseClipboard();
    return out;
}
uint32_t *plat_clip_get_image(int *w, int *h)
{
    uint32_t *out = NULL;
    if (!OpenClipboard(hwnd)) return NULL;
    HANDLE g = GetClipboardData(fmt_png());
    if (g) {
        size_t n = GlobalSize(g); void *p = GlobalLock(g);
        if (p) { out = png_decode((const uint8_t *)p, n, w, h); GlobalUnlock(g); }
    }
    if (!out) {
        g = GetClipboardData(CF_DIB);
        if (g) {
            size_t n = GlobalSize(g); void *p = GlobalLock(g);
            if (p) { out = bmp_decode((const uint8_t *)p, n, w, h); GlobalUnlock(g); }
        }
    }
    CloseClipboard();
    return out;
}

/* ------------------------------------------------------------------ text rendering (GDI, aliased) */
static const wchar_t *map_family(const char *fam, wchar_t *buf, int cap)
{
    if (!fam || !*fam) return L"Segoe UI";
    if (!strcmp(fam, "Sans") || !strcmp(fam, "Liberation Sans") || !strcmp(fam, "DejaVu Sans")) return L"Arial";
    if (!strcmp(fam, "Serif") || !strcmp(fam, "Liberation Serif") || !strcmp(fam, "DejaVu Serif")) return L"Times New Roman";
    if (!strcmp(fam, "Monospace") || !strcmp(fam, "Liberation Mono") || !strcmp(fam, "DejaVu Sans Mono")) return L"Courier New";
    MultiByteToWideChar(CP_UTF8, 0, fam, -1, buf, cap);
    return buf;
}
uint8_t *plat_render_text(const char *utf8, const char *family, int px, int bold, int italic, int underline, int *ow, int *oh)
{
    wchar_t fam[64]; fam[0] = 0;
    const wchar_t *face = map_family(family, fam, 64);
    wchar_t *ws = u8_to_w(utf8 ? utf8 : "");
    if (!ws) return NULL;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) { free(ws); return NULL; }
    HFONT f = CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, italic ? TRUE : FALSE, underline ? TRUE : FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, face);
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    int lh = tm.tmHeight > 0 ? tm.tmHeight : px;
    /* split into lines */
    size_t len = wcslen(ws); int nl = 1, maxw = 1;
    for (size_t i = 0; i < len; i++) if (ws[i] == L'\n') nl++;
    size_t *ls = (size_t *)malloc(sizeof(size_t) * (size_t)(nl + 1)), *le = (size_t *)malloc(sizeof(size_t) * (size_t)(nl + 1));
    int li = 0; ls[0] = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || ws[i] == L'\n') {
            size_t e = i; if (e > ls[li] && ws[e - 1] == L'\r') e--;
            le[li] = e;
            SIZE sz; sz.cx = 0; sz.cy = 0;
            if (e > ls[li]) GetTextExtentPoint32W(dc, ws + ls[li], (int)(e - ls[li]), &sz);
            if (sz.cx + (italic ? 2 : 0) > maxw) maxw = (int)sz.cx + (italic ? 2 : 0);
            li++; if (i < len) ls[li] = i + 1;
        }
    }
    int W = maxw, H = nl * lh; if (H < 1) H = 1;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    uint8_t *mask = NULL;
    if (bm && bits) {
        HGDIOBJ oldbm = SelectObject(dc, bm);
        memset(bits, 0xff, (size_t)W * (size_t)H * 4);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(0, 0, 0)); SetTextAlign(dc, TA_LEFT | TA_TOP);
        for (int i = 0; i < li; i++) if (le[i] > ls[i]) TextOutW(dc, 0, i * lh, ws + ls[i], (int)(le[i] - ls[i]));
        GdiFlush();
        mask = (uint8_t *)malloc((size_t)W * (size_t)H);
        if (mask) {
            const uint32_t *p = (const uint32_t *)bits;
            for (size_t i = 0; i < (size_t)W * (size_t)H; i++) mask[i] = ((p[i] & 0xff) < 128) ? 255 : 0;
        }
        SelectObject(dc, oldbm);
    }
    if (bm) DeleteObject(bm);
    SelectObject(dc, old); DeleteObject(f); DeleteDC(dc);
    free(ls); free(le); free(ws);
    *ow = W; *oh = H;
    return mask;
}

/* ------------------------------------------------------------------ file system helpers */
int plat_listdir(const char *path, PDirEnt **out, int *count)
{
    int cap = 64, n = 0;
    PDirEnt *a = (PDirEnt *)malloc(sizeof(PDirEnt) * (size_t)cap);
    if (!a) return -1;
    /* "/" (or "") = list the drives */
    if (!path || !*path || !strcmp(path, "/")) {
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; i++) if (mask & (1u << i)) {
            if (n == cap) { cap *= 2; a = (PDirEnt *)realloc(a, sizeof(PDirEnt) * (size_t)cap); }
            snprintf(a[n].name, sizeof a[n].name, "%c:", 'A' + i); a[n].is_dir = 1; a[n].size = 0; n++;
        }
        *out = a; *count = n; return 0;
    }
    char p2[1100]; const char *pp = path;
    if (pp[0] == '/' && pp[1] && pp[2] == ':') pp++;                 /* "/C:" -> "C:" */
    size_t pl = strlen(pp);
    snprintf(p2, sizeof p2, "%s%s*", pp, (pl && (pp[pl - 1] == '/' || pp[pl - 1] == '\\' || pp[pl - 1] == ':')) ? ((pp[pl - 1] == ':') ? "/" : "") : "/");
    wchar_t *wp = u8_to_w(p2);
    if (!wp) { free(a); return -1; }
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(wp, &fd);
    free(wp);
    if (hf == INVALID_HANDLE_VALUE) { free(a); return -1; }
    do {
        if (!wcscmp(fd.cFileName, L".")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) { if (wcscmp(fd.cFileName, L"..")) continue; }
        if (n == cap) { cap *= 2; a = (PDirEnt *)realloc(a, sizeof(PDirEnt) * (size_t)cap); }
        char nm[512];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nm, sizeof nm, NULL, NULL) <= 0) continue;
        snprintf(a[n].name, sizeof a[n].name, "%s", nm);
        a[n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        a[n].size = (long)fd.nFileSizeLow; n++;
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    *out = a; *count = n;
    return 0;
}
const char *plat_home_dir(void)
{
    static char home[1024];
    if (!home[0]) {
        wchar_t w[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, w))) w_to_u8(w, home, sizeof home);
        else snprintf(home, sizeof home, "C:/");
    }
    return home;
}
int plat_getcwd(char *buf, int n)
{
    /* Used as the default folder for Open/Save. When launched from the Start menu the working directory is the
     * program's own folder, which is useless: use Pictures (or the profile folder) instead. */
    wchar_t cwd[MAX_PATH], exe[MAX_PATH];
    int use_cwd = 0;
    if (_wgetcwd(cwd, MAX_PATH) && GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        wchar_t *sl = wcsrchr(exe, L'\\'); if (sl) *sl = 0;
        use_cwd = _wcsicmp(cwd, exe) != 0;
        wchar_t sysd[MAX_PATH];
        if (GetWindowsDirectoryW(sysd, MAX_PATH) && _wcsnicmp(cwd, sysd, wcslen(sysd)) == 0) use_cwd = 0;
    }
    if (use_cwd) return w_to_u8(cwd, buf, n);
    wchar_t pic[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, 0, pic))) return w_to_u8(pic, buf, n);
    snprintf(buf, (size_t)n, "%s", plat_home_dir());
    return 0;
}

/* ------------------------------------------------------------------ fit window to image */
void plat_fit_window(int w, int h)
{
    if (!hwnd) return;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) return;
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    RECT r = { 0, 0, w * scale, h * scale };
    adjust_rect(&r, style, ex, window_dpi(hwnd));
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
    if (ww > aw || wh > ah) { ShowWindow(hwnd, SW_MAXIMIZE); return; }
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    RECT cur; GetWindowRect(hwnd, &cur);
    int x = cur.left, y = cur.top;
    if (x + ww > mi.rcWork.right) x = mi.rcWork.right - ww;
    if (y + wh > mi.rcWork.bottom) y = mi.rcWork.bottom - wh;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    SetWindowPos(hwnd, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ------------------------------------------------------------------ entry point for GUI subsystem builds */
#ifdef PAINT_WIN_GUI
extern int main(int argc, char **argv);
int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;
    int argc = 0; LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = (char *)malloc((size_t)n + 1);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    LocalFree(wargv);
    return main(argc, argv);
}
#endif


/* ------------------------------------------------------------------ native file dialog (Windows common dialog) */
int plat_file_dialog(int save, const char *title, char *path, int pathn, const char *const *names, const char *const *pats, int nfilters, int *fidx)
{
    static wchar_t fbuf[8192], file[32768], init[1024];
    size_t o = 0;
    for (int i = 0; i < nfilters; i++) {
        wchar_t *n = u8_to_w(names[i]), *p = u8_to_w(pats[i]);
        if (!n || !p) { free(n); free(p); return -1; }
        size_t ln = wcslen(n), lp = wcslen(p);
        if (o + ln + lp + 4 >= sizeof fbuf / sizeof fbuf[0]) { free(n); free(p); break; }
        memcpy(fbuf + o, n, (ln + 1) * sizeof(wchar_t)); o += ln + 1;
        memcpy(fbuf + o, p, (lp + 1) * sizeof(wchar_t)); o += lp + 1;
        free(n); free(p);
    }
    fbuf[o] = 0;
    wchar_t *wp = u8_to_w(path); wchar_t *wt = u8_to_w(title);
    file[0] = 0; init[0] = 0;
    if (wp) {
        for (wchar_t *c = wp; *c; c++) if (*c == L'/') *c = L'\\';
        wchar_t *sl = wcsrchr(wp, L'\\');
        if (sl) { size_t d = (size_t)(sl - wp); if (d >= 1024) d = 1023; wmemcpy(init, wp, d); init[d] = 0; wcsncpy(file, sl + 1, 32767); }
        else wcsncpy(file, wp, 32767);
        file[32767] = 0;
    }
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = hwnd; ofn.lpstrFilter = fbuf; ofn.nFilterIndex = (DWORD)((fidx && *fidx >= 0 ? *fidx : 0) + 1);
    ofn.lpstrFile = file; ofn.nMaxFile = 32768; ofn.lpstrInitialDir = init[0] ? init : NULL; ofn.lpstrTitle = wt;
    ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    free(wp); free(wt);
    if (!ok) return CommDlgExtendedError() ? -1 : 0;
    w_to_u8(file, path, pathn);
    for (char *c = path; *c; c++) if (*c == '\\') *c = '/';
    if (fidx) *fidx = (int)ofn.nFilterIndex - 1;
    return 1;
}
