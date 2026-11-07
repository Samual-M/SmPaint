/*
 * plat_cocoa.m - macOS platform layer for Paint (macOS 11 Big Sur and later; Apple Silicon and Intel).
 *
 * AppKit can only be used from Objective-C, so this one file is .m; the whole portable core stays plain C.
 * Build with Makefile.mac (universal arm64 + x86_64 binary). Uses only system frameworks: Cocoa, CoreText.
 *
 * The framebuffer is drawn at 1 framebuffer pixel = 1 point, and the window server scales it up with
 * nearest-neighbour on Retina displays, so the classic UI is crisp (pixel-doubled) rather than blurry.
 */
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "plat.h"
#include "imgio.h"

/* ------------------------------------------------------------------ event queue */
#define QCAP 1024
static PEvent q[QCAP];
static int q_head, q_tail;
static void push_ev(PEvent e)
{
    int n = (q_tail + 1) % QCAP;
    if (n == q_head) return;
    q[q_tail] = e; q_tail = n;
}
static void push_simple(int type, int x, int y, int button, int key, int mods, uint32_t ch)
{
    PEvent e; memset(&e, 0, sizeof e);
    e.type = type; e.x = x; e.y = y; e.button = button; e.key = key; e.mods = mods; e.ch = ch;
    push_ev(e);
}

static char drop_path[1024];
static void (*live_hook)(void);
static int in_hook, hook_busy;
void plat_set_live_hook(void (*fn)(void)) { live_hook = fn; }
const char *plat_drop_path(void) { return drop_path; }
static void queue_open_path(NSString *p)
{
    if (!p) return;
    const char *u = [p fileSystemRepresentation];
    if (!u) return;
    strncpy(drop_path, u, sizeof drop_path - 1); drop_path[sizeof drop_path - 1] = 0;
    push_simple(EV_DROPFILE, 0, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ globals */
@class PaintView;
static NSWindow *g_window;
static PaintView *g_view;
static NSCursor *g_cursor;           /* NULL = arrow */
static uint32_t *g_shadow; static int g_sh_w, g_sh_h;
static int g_cur_w, g_cur_h;
static NSImage *g_app_icon; static int g_app_icon_size;

static int mods_from(NSEventModifierFlags f)
{
    int m = 0;
    if (f & NSEventModifierFlagShift) m |= MOD_SHIFT;
    if (f & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) m |= MOD_CTRL;   /* Cmd (and Ctrl) act as the shortcut key */
    if (f & NSEventModifierFlagOption) m |= MOD_ALT;
    return m;
}

static int key_from_event(NSEvent *e)
{
    switch ([e keyCode]) {
    case 51: return KEY_BACKSPACE; case 117: return KEY_DELETE; case 36: case 76: return KEY_ENTER; case 48: return KEY_TAB;
    case 53: return KEY_ESC; case 49: return KEY_SPACE;
    case 123: return KEY_LEFT; case 124: return KEY_RIGHT; case 125: return KEY_DOWN; case 126: return KEY_UP;
    case 115: return KEY_HOME; case 119: return KEY_END; case 116: return KEY_PGUP; case 121: return KEY_PGDN;
    case 114: return KEY_INSERT; case 122: return KEY_F1;
    case 69: return KEY_PLUS; case 78: return KEY_MINUS;
    }
    NSString *s = [e charactersIgnoringModifiers];
    if ([s length] == 0) return 0;
    unichar c = [s characterAtIndex:0];
    if (c >= 'A' && c <= 'Z') c = (unichar)(c - 'A' + 'a');
    if (c == '=' || c == '+') return KEY_PLUS;
    if (c == '-' || c == '_') return KEY_MINUS;
    if (c >= 32 && c < 127) return (int)c;
    return 0;
}

/* ------------------------------------------------------------------ the view */
@interface PaintView : NSView <NSTextInputClient, NSDraggingDestination>
{
    NSTrackingArea *_area;
    double _wheelAccY, _wheelAccX;
    double _pinch;
    BOOL _ctrlClick;
}
@end

@implementation PaintView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)wantsDefaultClipping { return NO; }

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_area) { [self removeTrackingArea:_area]; _area = nil; }
    _area = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                           owner:self userInfo:nil];
    [self addTrackingArea:_area];
}
- (void)resetCursorRects { [self addCursorRect:[self bounds] cursor:(g_cursor ? g_cursor : [NSCursor arrowCursor])]; }

- (void)setFrameSize:(NSSize)s
{
    [super setFrameSize:s];
    int w = (int)floor(s.width), h = (int)floor(s.height);
    if (w < 1 || h < 1) return;
    if (w != g_cur_w || h != g_cur_h) {
        g_cur_w = w; g_cur_h = h;
        push_simple(EV_RESIZE, w, h, 0, 0, 0, 0);
        if ([self inLiveResize] && live_hook && !hook_busy) {      /* repaint while the user drags the window edge */
            hook_busy = 1; in_hook = 1; live_hook(); in_hook = 0; hook_busy = 0;
        }
    }
}

- (void)drawRect:(NSRect)dirty
{
    if (!g_shadow || g_sh_w <= 0 || g_sh_h <= 0) return;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, g_shadow, (size_t)g_sh_w * (size_t)g_sh_h * 4, NULL);
    CGImageRef img = CGImageCreate((size_t)g_sh_w, (size_t)g_sh_h, 8, 32, (size_t)g_sh_w * 4, cs,
                                   kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, dp, NULL, false, kCGRenderingIntentDefault);
    if (img) {
        CGContextSaveGState(ctx);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);          /* crisp pixel scaling on Retina */
        NSRect b = [self bounds];
        CGContextTranslateCTM(ctx, 0, b.size.height);                         /* view is flipped: flip the image back */
        CGContextScaleCTM(ctx, 1, -1);
        CGContextDrawImage(ctx, CGRectMake(0, 0, g_sh_w, g_sh_h), img);
        CGContextRestoreGState(ctx);
        CGImageRelease(img);
    }
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
}

/* ---- mouse ---- */
- (void)mouseEv:(NSEvent *)e type:(int)type button:(int)b
{
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    push_simple(type, (int)floor(p.x), (int)floor(p.y), b, 0, mods_from([e modifierFlags]), 0);
}
- (void)mouseDown:(NSEvent *)e
{
    /* Ctrl+click is a right click on macOS */
    if ([e modifierFlags] & NSEventModifierFlagControl) { [self mouseEv:e type:EV_MOUSEDOWN button:BTN_RIGHT]; _ctrlClick = YES; return; }
    _ctrlClick = NO;
    [self mouseEv:e type:EV_MOUSEDOWN button:BTN_LEFT];
}
- (void)mouseUp:(NSEvent *)e { [self mouseEv:e type:EV_MOUSEUP button:_ctrlClick ? BTN_RIGHT : BTN_LEFT]; _ctrlClick = NO; }
- (void)rightMouseDown:(NSEvent *)e { [self mouseEv:e type:EV_MOUSEDOWN button:BTN_RIGHT]; }
- (void)rightMouseUp:(NSEvent *)e { [self mouseEv:e type:EV_MOUSEUP button:BTN_RIGHT]; }
- (void)otherMouseDown:(NSEvent *)e { [self mouseEv:e type:EV_MOUSEDOWN button:BTN_MIDDLE]; }
- (void)otherMouseUp:(NSEvent *)e { [self mouseEv:e type:EV_MOUSEUP button:BTN_MIDDLE]; }
- (void)moveEv:(NSEvent *)e button:(int)b
{
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    int x = (int)floor(p.x), y = (int)floor(p.y);
    int last = (q_tail + QCAP - 1) % QCAP;
    if (q_head != q_tail && q[last].type == EV_MOUSEMOVE) { q[last].x = x; q[last].y = y; q[last].button = b; q[last].mods = mods_from([e modifierFlags]); }
    else push_simple(EV_MOUSEMOVE, x, y, b, 0, mods_from([e modifierFlags]), 0);
}
- (void)mouseMoved:(NSEvent *)e { [self moveEv:e button:0]; }
- (void)mouseDragged:(NSEvent *)e { [self moveEv:e button:_ctrlClick ? BTN_RIGHT : BTN_LEFT]; }
- (void)rightMouseDragged:(NSEvent *)e { [self moveEv:e button:BTN_RIGHT]; }
- (void)otherMouseDragged:(NSEvent *)e { [self moveEv:e button:BTN_MIDDLE]; }
- (void)mouseExited:(NSEvent *)e { push_simple(EV_LEAVE, 0, 0, 0, 0, 0, 0); }

- (void)scrollWheel:(NSEvent *)e
{
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    int x = (int)floor(p.x), y = (int)floor(p.y), mods = mods_from([e modifierFlags]);
    double dy = [e scrollingDeltaY], dx = [e scrollingDeltaX];
    double step = [e hasPreciseScrollingDeltas] ? 10.0 : 1.0;      /* trackpads report points; mouse wheels report lines */
    _wheelAccY += dy; _wheelAccX += dx;
    while (_wheelAccY >= step) { _wheelAccY -= step; push_simple(EV_WHEEL, x, y, 1, 0, mods, 0); }
    while (_wheelAccY <= -step) { _wheelAccY += step; push_simple(EV_WHEEL, x, y, -1, 0, mods, 0); }
    while (_wheelAccX >= step) { _wheelAccX -= step; push_simple(EV_WHEEL, x, y, 1, 0, mods | MOD_SHIFT, 0); }
    while (_wheelAccX <= -step) { _wheelAccX += step; push_simple(EV_WHEEL, x, y, -1, 0, mods | MOD_SHIFT, 0); }
}
- (void)magnifyWithEvent:(NSEvent *)e
{
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    int x = (int)floor(p.x), y = (int)floor(p.y);
    NSEventPhase ph = [e phase];
    if (ph & NSEventPhaseBegan) { _pinch = 1.0; push_simple(EV_PINCH, x, y, 1, 1000, 0, 0); }
    _pinch *= 1.0 + [e magnification];
    if (_pinch < 0.05) _pinch = 0.05; if (_pinch > 20.0) _pinch = 20.0;
    push_simple(EV_PINCH, x, y, 2, (int)(_pinch * 1000.0), 0, 0);
    if (ph & (NSEventPhaseEnded | NSEventPhaseCancelled)) push_simple(EV_PINCH, x, y, 3, (int)(_pinch * 1000.0), 0, 0);
}

/* ---- keyboard ---- */
- (BOOL)performKeyEquivalent:(NSEvent *)e
{
    /* Cmd+key never reaches keyDown: — take it here, except the few the system menu owns (Cmd+Q quit, Cmd+H hide, Cmd+M minimise) */
    if (!([e modifierFlags] & NSEventModifierFlagCommand)) return NO;
    NSString *s = [e charactersIgnoringModifiers];
    if ([s length] == 1) {
        unichar c = [s characterAtIndex:0];
        if (c == 'q' || c == 'h' || c == 'm' || c == ',') return NO;
    }
    int key = key_from_event(e);
    if (!key) return NO;
    push_simple(EV_KEYDOWN, 0, 0, 0, key, mods_from([e modifierFlags]), 0);
    return YES;
}
- (void)keyDown:(NSEvent *)e
{
    int mods = mods_from([e modifierFlags]);
    int key = key_from_event(e);
    if (key) push_simple(EV_KEYDOWN, 0, 0, 0, key, mods, 0);
    /* text input (dead keys, IME, Option+letter characters) goes through the text input system */
    if (!([e modifierFlags] & (NSEventModifierFlagCommand | NSEventModifierFlagControl)))
        [self interpretKeyEvents:@[ e ]];
}
- (void)flagsChanged:(NSEvent *)e { }

/* NSTextInputClient (only insertText matters) */
- (void)insertText:(id)str replacementRange:(NSRange)r
{
    NSString *s = [str isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString *)str string] : (NSString *)str;
    NSUInteger n = [s length];
    for (NSUInteger i = 0; i < n; ) {
        unichar c = [s characterAtIndex:i]; uint32_t cp = c; i++;
        if (c >= 0xD800 && c <= 0xDBFF && i < n) { unichar lo = [s characterAtIndex:i]; if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000u + (((uint32_t)c - 0xD800u) << 10) + (lo - 0xDC00u); i++; } }
        if (cp < 32 || cp == 127 || (cp >= 0xF700 && cp <= 0xF8FF)) continue;     /* control + macOS function-key private use */
        push_simple(EV_CHAR, 0, 0, 0, 0, 0, cp);
    }
}
- (void)doCommandBySelector:(SEL)sel { }
- (void)setMarkedText:(id)s selectedRange:(NSRange)sr replacementRange:(NSRange)rr { }
- (void)unmarkText { }
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRange)markedRange { return NSMakeRange(NSNotFound, 0); }
- (BOOL)hasMarkedText { return NO; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)ar { return nil; }
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)ar { return NSZeroRect; }
- (NSUInteger)characterIndexForPoint:(NSPoint)p { return 0; }

/* ---- drag and drop of files onto the window ---- */
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ] options:@{ NSPasteboardURLReadingFileURLsOnlyKey: @YES }];
    if ([urls count] == 0) return NO;
    queue_open_path([(NSURL *)[urls objectAtIndex:0] path]);
    return YES;
}
@end

/* ------------------------------------------------------------------ app + window delegate */
@interface PaintDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation PaintDelegate
- (BOOL)windowShouldClose:(id)sender { push_simple(EV_CLOSE, 0, 0, 0, 0, 0, 0); return NO; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app { push_simple(EV_CLOSE, 0, 0, 0, 0, 0, 0); return NSTerminateCancel; }
- (BOOL)application:(NSApplication *)app openFile:(NSString *)filename { queue_open_path(filename); return YES; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return NO; }
- (void)quitRequested:(id)sender { push_simple(EV_CLOSE, 0, 0, 0, 0, 0, 0); }
- (void)windowDidResignKey:(NSNotification *)n { push_simple(EV_FOCUSOUT, 0, 0, 0, 0, 0, 0); }
@end
static PaintDelegate *g_delegate;

static void build_menu(void)
{
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu *app = [[NSMenu alloc] initWithTitle:@"SmPaint"];
    [app addItemWithTitle:@"Hide SmPaint" action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem *ho = [app addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"];
    [ho setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
    [app addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [app addItem:[NSMenuItem separatorItem]];
    NSMenuItem *q = [app addItemWithTitle:@"Quit SmPaint" action:@selector(quitRequested:) keyEquivalent:@"q"];
    [q setTarget:g_delegate];
    [appItem setSubmenu:app];
    NSMenuItem *winItem = [[NSMenuItem alloc] init];
    [bar addItem:winItem];
    NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
    [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [win addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [winItem setSubmenu:win];
    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:win];
}

/* ------------------------------------------------------------------ init / events */
int plat_init(int w, int h, int minw, int minh, const char *title)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_delegate = [[PaintDelegate alloc] init];
        [NSApp setDelegate:g_delegate];
        build_menu();
        NSRect r = NSMakeRect(0, 0, w, h);
        g_window = [[NSWindow alloc] initWithContentRect:r
                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                 backing:NSBackingStoreBuffered defer:NO];
        [g_window setReleasedWhenClosed:NO];
        [g_window setDelegate:g_delegate];
        [g_window setContentMinSize:NSMakeSize(minw, minh)];
        [g_window setTitle:[NSString stringWithUTF8String:title ? title : "SmPaint"]];
        g_view = [[PaintView alloc] initWithFrame:r];
        [g_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [g_view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
        [g_window setContentView:g_view];
        [g_window center];
        [g_window makeFirstResponder:g_view];
        [g_window setAcceptsMouseMovedEvents:YES];
        g_cur_w = w; g_cur_h = h;
        [NSApp finishLaunching];
        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
    return 0;
}
void plat_shutdown(void)
{
    @autoreleasepool {
        if (g_window) { [g_window setDelegate:nil]; [g_window orderOut:nil]; [g_window close]; g_window = nil; }
        free(g_shadow); g_shadow = NULL;
    }
}
void plat_set_title(const char *t)
{
    if (!g_window) return;
    @autoreleasepool { [g_window setTitle:[NSString stringWithUTF8String:t]]; }
}
uint32_t plat_ticks_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
void plat_present(const uint32_t *px, int w, int h)
{
    if (!g_view) return;
    if (w != g_sh_w || h != g_sh_h) {
        free(g_shadow); g_shadow = (uint32_t *)malloc((size_t)w * (size_t)h * 4); g_sh_w = w; g_sh_h = h;
        if (!g_shadow) { g_sh_w = g_sh_h = 0; return; }
    }
    memcpy(g_shadow, px, (size_t)w * (size_t)h * 4);
    @autoreleasepool {
        [g_view setNeedsDisplay:YES];
        [g_view displayIfNeeded];
    }
}
int plat_wait_event(PEvent *ev, int timeout_ms)
{
    uint32_t t0 = plat_ticks_ms();
    for (;;) {
        if (q_head != q_tail) { *ev = q[q_head]; q_head = (q_head + 1) % QCAP; return 1; }
        if (in_hook) { memset(ev, 0, sizeof *ev); return 0; }
        int wait_ms;
        if (timeout_ms < 0) wait_ms = -1;
        else {
            int el = (int)(plat_ticks_ms() - t0);
            if (el >= timeout_ms) { memset(ev, 0, sizeof *ev); return 0; }
            wait_ms = timeout_ms - el;
        }
        @autoreleasepool {
            NSDate *until = wait_ms < 0 ? [NSDate distantFuture] : [NSDate dateWithTimeIntervalSinceNow:wait_ms / 1000.0];
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (e) { [NSApp sendEvent:e]; [NSApp updateWindows]; }
        }
    }
}

/* ------------------------------------------------------------------ icons & cursors */
static NSBitmapImageRep *rep_from_argb(const uint32_t *argb, int w, int h, int dbl)
{
    int W = w * dbl, H = h * dbl;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:W pixelsHigh:H bitsPerSample:8 samplesPerPixel:4
                                                                      hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                                                                   bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:W * 4 bitsPerPixel:32];
    unsigned char *d = [rep bitmapData];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint32_t p = argb[(y / dbl) * w + (x / dbl)];
        unsigned char *o = d + ((size_t)y * (size_t)W + (size_t)x) * 4;
        o[0] = (unsigned char)((p >> 16) & 255); o[1] = (unsigned char)((p >> 8) & 255); o[2] = (unsigned char)(p & 255); o[3] = (unsigned char)(p >> 24);
    }
    return rep;
}
void plat_set_icon(const uint32_t *argb, int w, int h)
{
    if (w < g_app_icon_size) return;           /* keep the biggest; the .icns in the app bundle is used by Finder anyway */
    @autoreleasepool {
        NSBitmapImageRep *rep = rep_from_argb(argb, w, h, 1);
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(w, h)];
        [img addRepresentation:rep];
        g_app_icon = img; g_app_icon_size = w;
        [NSApp setApplicationIconImage:img];
    }
}
void *plat_make_cursor(const uint32_t *argb32x32, int hx, int hy)
{
    @autoreleasepool {
        NSBitmapImageRep *rep = rep_from_argb(argb32x32, 32, 32, 2);        /* 64x64 pixels shown at 32x32 points: crisp on Retina */
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(32, 32)];
        [img addRepresentation:rep];
        NSCursor *c = [[NSCursor alloc] initWithImage:img hotSpot:NSMakePoint(hx, hy)];
        return (void *)CFBridgingRetain(c);
    }
}
void plat_set_cursor(void *c)
{
    g_cursor = c ? (__bridge NSCursor *)c : nil;
    if (g_window && g_view) { [g_window invalidateCursorRectsForView:g_view]; }
    if (g_cursor) [g_cursor set]; else [[NSCursor arrowCursor] set];
}

/* ------------------------------------------------------------------ clipboard */
void plat_clip_set_image(const uint32_t *px, int w, int h)
{
    size_t n;
    uint8_t *png = png_encode(px, w, h, &n);
    if (!png) return;
    @autoreleasepool {
        NSData *d = [NSData dataWithBytes:png length:n];
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setData:d forType:NSPasteboardTypePNG];
    }
    free(png);
}
void plat_clip_set_text(const char *utf8)
{
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:[NSString stringWithUTF8String:utf8 ? utf8 : ""] forType:NSPasteboardTypeString];
    }
}
char *plat_clip_get_text(void)
{
    char *out = NULL;
    @autoreleasepool {
        NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        if (s) out = strdup([s UTF8String]);
    }
    return out;
}
uint32_t *plat_clip_get_image(int *w, int *h)
{
    uint32_t *out = NULL;
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSData *d = [pb dataForType:NSPasteboardTypePNG];
        if (!d) {
            NSData *t = [pb dataForType:NSPasteboardTypeTIFF];
            if (t) {
                NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:t];
                d = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            }
        }
        if (d) out = png_decode((const uint8_t *)[d bytes], [d length], w, h);
    }
    return out;
}

/* ------------------------------------------------------------------ text rendering (Core Text, aliased) */
static NSString *map_family(const char *fam)
{
    if (!fam || !*fam) return @"Helvetica";
    if (!strcmp(fam, "Sans") || !strcmp(fam, "Liberation Sans") || !strcmp(fam, "DejaVu Sans")) return @"Helvetica";
    if (!strcmp(fam, "Serif") || !strcmp(fam, "Liberation Serif") || !strcmp(fam, "DejaVu Serif")) return @"Times New Roman";
    if (!strcmp(fam, "Monospace") || !strcmp(fam, "Liberation Mono") || !strcmp(fam, "DejaVu Sans Mono")) return @"Courier New";
    return [NSString stringWithUTF8String:fam];
}
uint8_t *plat_render_text(const char *utf8, const char *family, int px, int bold, int italic, int underline, int *ow, int *oh)
{
    uint8_t *mask = NULL;
    @autoreleasepool {
        CTFontRef base = CTFontCreateWithName((__bridge CFStringRef)map_family(family), (CGFloat)px, NULL);
        if (!base) base = CTFontCreateWithName(CFSTR("Helvetica"), (CGFloat)px, NULL);
        CTFontSymbolicTraits want = (bold ? kCTFontTraitBold : 0) | (italic ? kCTFontTraitItalic : 0);
        CTFontRef font = base;
        if (want) {
            CTFontRef f2 = CTFontCreateCopyWithSymbolicTraits(base, (CGFloat)px, NULL, want, want);
            if (f2) font = f2;
        }
        CGFloat asc = CTFontGetAscent(font), desc = CTFontGetDescent(font), lead = CTFontGetLeading(font);
        int lh = (int)ceil(asc + desc + lead); if (lh < 1) lh = px;
        NSString *all = [NSString stringWithUTF8String:utf8 ? utf8 : ""];
        if (!all) all = @"";
        NSArray<NSString *> *lines = [all componentsSeparatedByString:@"\n"];
        int nl = (int)[lines count]; if (nl < 1) nl = 1;
        NSMutableArray *ctlines = [NSMutableArray array];
        int maxw = 1;
        for (int i = 0; i < nl; i++) {
            NSString *s = [lines objectAtIndex:(NSUInteger)i];
            if ([s hasSuffix:@"\r"]) s = [s substringToIndex:[s length] - 1];
            NSDictionary *attr = @{ (__bridge id)kCTFontAttributeName: (__bridge id)font,
                                    (__bridge id)kCTUnderlineStyleAttributeName: @(underline ? kCTUnderlineStyleSingle : kCTUnderlineStyleNone) };
            NSAttributedString *as = [[NSAttributedString alloc] initWithString:s attributes:attr];
            CTLineRef ln = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
            double wd = CTLineGetTypographicBounds(ln, NULL, NULL, NULL);
            int wi = (int)ceil(wd) + (italic ? 2 : 0);
            if (wi > maxw) maxw = wi;
            [ctlines addObject:(__bridge_transfer id)ln];
        }
        int W = maxw, H = nl * lh;
        uint8_t *gray = (uint8_t *)malloc((size_t)W * (size_t)H);
        if (gray) {
            memset(gray, 255, (size_t)W * (size_t)H);
            CGColorSpaceRef gs = CGColorSpaceCreateDeviceGray();
            CGContextRef cg = CGBitmapContextCreate(gray, (size_t)W, (size_t)H, 8, (size_t)W, gs, kCGImageAlphaNone);
            CGColorSpaceRelease(gs);
            if (cg) {
                CGContextSetShouldAntialias(cg, false);
                CGContextSetShouldSmoothFonts(cg, false);
                CGContextSetAllowsFontSmoothing(cg, false);
                CGContextSetGrayFillColor(cg, 0.0, 1.0);
                for (int i = 0; i < nl; i++) {
                    CGFloat baseline = (CGFloat)H - ((CGFloat)i * lh + asc + lead / 2.0);   /* Core Graphics origin is bottom-left */
                    CGContextSetTextPosition(cg, 0, baseline);
                    CTLineDraw((__bridge CTLineRef)[ctlines objectAtIndex:(NSUInteger)i], cg);
                }
                CGContextRelease(cg);
                mask = (uint8_t *)malloc((size_t)W * (size_t)H);
                if (mask) for (size_t i = 0; i < (size_t)W * (size_t)H; i++) mask[i] = gray[i] < 128 ? 255 : 0;
            }
            free(gray);
        }
        *ow = W; *oh = H;
        if (font != base) CFRelease(font);
        CFRelease(base);
    }
    return mask;
}

/* ------------------------------------------------------------------ file system helpers (POSIX) */
int plat_listdir(const char *path, PDirEnt **out, int *count)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    int cap = 64, n = 0;
    PDirEnt *a = (PDirEnt *)malloc((size_t)cap * sizeof(PDirEnt));
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".")) continue;
        if (de->d_name[0] == '.' && strcmp(de->d_name, "..")) continue;       /* hide dot files */
        if (n == cap) { cap *= 2; a = (PDirEnt *)realloc(a, (size_t)cap * sizeof(PDirEnt)); }
        char full[4096]; snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        snprintf(a[n].name, sizeof a[n].name, "%s", de->d_name);
        a[n].is_dir = S_ISDIR(st.st_mode) ? 1 : 0; a[n].size = (long)st.st_size;
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
int plat_getcwd(char *buf, int n)
{
    /* an app launched from Finder has "/" as its working directory: use the home folder instead */
    if (!getcwd(buf, (size_t)n) || !strcmp(buf, "/")) { snprintf(buf, (size_t)n, "%s", plat_home_dir()); }
    return 0;
}

/* ------------------------------------------------------------------ fit window to image */
void plat_fit_window(int w, int h)
{
    if (!g_window) return;
    @autoreleasepool {
        NSScreen *scr = [g_window screen] ? [g_window screen] : [NSScreen mainScreen];
        NSRect vis = [scr visibleFrame];
        NSRect want = [g_window frameRectForContentRect:NSMakeRect(0, 0, w, h)];
        if (want.size.width > vis.size.width || want.size.height > vis.size.height) {
            if (![g_window isZoomed]) [g_window zoom:nil];
            return;
        }
        if ([g_window isZoomed]) [g_window zoom:nil];
        NSRect cur = [g_window frame];
        NSRect f = NSMakeRect(cur.origin.x, cur.origin.y + cur.size.height - want.size.height, want.size.width, want.size.height);   /* keep top-left fixed */
        if (NSMaxX(f) > NSMaxX(vis)) f.origin.x = NSMaxX(vis) - f.size.width;
        if (f.origin.x < vis.origin.x) f.origin.x = vis.origin.x;
        if (f.origin.y < vis.origin.y) f.origin.y = vis.origin.y;
        if (NSMaxY(f) > NSMaxY(vis)) f.origin.y = NSMaxY(vis) - f.size.height;
        [g_window setFrame:f display:YES];
    }
}


/* ------------------------------------------------------------------ native file dialog (NSOpenPanel / NSSavePanel) */
int plat_file_dialog(int save, const char *title, char *path, int pathn, const char *const *names, const char *const *pats, int nfilters, int *fidx)
{
    (void)names;
    int ret = 0;
    @autoreleasepool {
        NSMutableArray *exts = [NSMutableArray array];
        for (int i = 0; i < nfilters; i++) {
            if (!strcmp(pats[i], "*")) { exts = nil; break; }
            NSArray *parts = [[NSString stringWithUTF8String:pats[i]] componentsSeparatedByString:@";"];
            for (NSString *p in parts) {
                NSString *e = [p hasPrefix:@"*."] ? [p substringFromIndex:2] : p;
                if (e.length && ![exts containsObject:e]) [exts addObject:e];
            }
        }
        NSString *full = [NSString stringWithUTF8String:path];
        NSString *dir = [full stringByDeletingLastPathComponent], *file = [full lastPathComponent];
        NSSavePanel *p = save ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
        [p setTitle:[NSString stringWithUTF8String:title]];
        if (exts.count) [p setAllowedFileTypes:exts];
        if (dir.length) [p setDirectoryURL:[NSURL fileURLWithPath:dir]];
        if (save) [p setNameFieldStringValue:file];
        if ([p runModal] == NSModalResponseOK) {
            const char *u = [[[p URL] path] UTF8String];
            if (u) { snprintf(path, (size_t)pathn, "%s", u); *fidx = -1; ret = 1; }
        }
    }
    return ret;
}
