# SmPaint (portable C port) V1.11
Core is C99; all OS access goes through src/plat.h. Linux/X11 implementation: src/plat_x11.c (libX11, cairo for text only).
To port: write src/plat_<os>.c implementing plat.h and add it to the Makefile.
Build: make mspaint   (needs libx11-dev, libcairo2-dev)
Resources: src/resdata.c is generated from the original exe by tools/ (peres.py, decode.py, gen_res.py, gen_font.py).
Needs libxi6 at runtime (touchpad pinch zoom, XInput 2.4).

See BUILDING.md for Windows and macOS builds (src/plat_win32.c, src/plat_cocoa.m).

## Unzip-and-run packages (Windows / macOS)
- Windows: unzip `SmPaint-V1.11-windows.zip`, double-click `SmPaint.exe` (no install). SmartScreen: More info, Run anyway.
- macOS: unzip `SmPaintV1.11-mac.zip`, double-click `SmPaint.app` (universal: Apple Silicon and Intel). First launch: right-click, Open, Open
  (the app is ad-hoc signed, not notarized); on macOS 15+ use System Settings, Privacy & Security, Open Anyway if asked.
- Both are produced by the GitHub workflow (`.github/workflows/build.yml`) or locally by `packaging/windows/make_portable.sh` and `packaging/macos/make_app.sh`.

## V1.11
Text box: drag its border to move, handles to resize, click/drag/double-click/shift+arrows to select, per-selection font/size/bold/italic/underline.
Save as PDF; JPEG is the default save type; 85% default zoom; outline thickness picker for rectangle/ellipse/rounded rectangle/polygon.

## V1.11
App renamed SmPaint (window title, About, launcher, Windows/macOS package names). The .deb package name stays `mspaint` so it upgrades the old one.
