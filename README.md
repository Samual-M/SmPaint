# SmPaint (portable C port) V1.24
Core is C99; all OS access goes through src/plat.h. Linux/X11 implementation: src/plat_x11.c (libX11, cairo for text only).
To port: write src/plat_<os>.c implementing plat.h and add it to the Makefile.
Build: make mspaint   (needs libx11-dev, libcairo2-dev)
Resources: src/resdata.c is generated from the original exe by tools/ (peres.py, decode.py, gen_res.py, gen_font.py).
Needs libxi6 at runtime (touchpad pinch zoom, XInput 2.4).

See BUILDING.md for Windows and macOS builds (src/plat_win32.c, src/plat_cocoa.m).

## V1.18-V1.24 (latest)
No-tool mode: click the selected tool again to deselect it. With no tool you can move/edit finished text boxes and move/reshape lines, curves, rectangles, polygons, ellipses and rounded rectangles (Delete removes the selected shape; undo/redo covers it). Select tools keep the opaque/transparent options and gain a Crop button (also Image > Crop to Selection, Ctrl+Shift+X). Opened images zoom to fit the window (up to 100%). Text size labels are drawn at 3.5x (size 10 draws as a 35pt font), default size 10, sizes 4 and 6 added.

## Unzip-and-run packages (Windows / macOS)
- Windows: unzip `SmPaint-V1.24-windows.zip`, double-click `SmPaint.exe` (no install). SmartScreen: More info, Run anyway.
- macOS: unzip `SmPaintV1.24-mac.zip`, double-click `SmPaint.app` (universal: Apple Silicon and Intel). First launch: right-click, Open, Open
  (the app is ad-hoc signed, not notarized); on macOS 15+ use System Settings, Privacy & Security, Open Anyway if asked.
- Both are produced by the GitHub workflow (`.github/workflows/build.yml`) or locally by `packaging/windows/make_portable.sh` and `packaging/macos/make_app.sh`.

## V1.10
Text box: drag its border to move, handles to resize, click/drag/double-click/shift+arrows to select, per-selection font/size/bold/italic/underline.
Save as PDF; JPEG is the default save type; 85% default zoom; outline thickness picker for rectangle/ellipse/rounded rectangle/polygon.

## V1.11
App renamed SmPaint (window title, About, launcher, Windows/macOS package names). The .deb package name stays `mspaint` so it upgrades the old one.

## V1.12
Default text size 14; -/+ zoom buttons in the menu bar (5% steps); colour palette moved to the left column (rotated 90 degrees clockwise); default new image 1920x1080; default window 1024x768.

## V1.13
Ctrl +/- zoom in 5% steps. Text boxes: copy/cut/paste/select-all, and finished boxes stay live objects (click one with the Text tool to edit it, drag its border to move it) until you draw or change the picture with another tool. Size panels (pencil-type tools, brush, eraser, airbrush, line, curve, shape outlines) start on the middle option. Open/Save use the desktop's own file dialog (KDE Dolphin dialog via kdialog, GNOME/others via zenity, Windows and macOS system dialogs); the built-in dialog is the fallback.

## V1.14
Default zoom 70%. Window resizing no longer flashes (the window background is never cleared) and bursts of resize events are coalesced so dragging an edge or corner stays in step with the pointer.

## V1.15
Trackpad pinch-to-zoom (Linux): the XInput 2.4 pinch event is now decoded independently of the struct layout (it detects where the scale value sits), and `SMPAINT_DEBUG_PINCH=1 smpaint` prints the pinch events it receives. Two-finger scroll pans and Ctrl + two-finger scroll zooms as before.

## V1.16
File > Copy Image to Clipboard (Ctrl+Shift+C): copies the whole picture (text boxes included) to the clipboard so it can be pasted straight into another app, no saving needed.

## V1.17
Larger, clearer text in the menu bar, menus, status bar, text bar and drop-down lists (dialogs keep the original small font because their layouts are fixed). Shrinking the canvas and then enlarging it again restores the picture that was cut off (it is remembered until you flip/rotate/stretch/clear or open another file). Text size now follows the picture: on pictures larger than 1920x1080 the same point size is scaled up so text looks the same relative to the picture; smaller pictures are unchanged.

