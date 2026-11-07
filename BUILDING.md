# Building Paint (V1.12)

The program is a portable C99 core (`src/`) plus one small platform layer per OS. Nothing is downloaded at run time,
there is no telemetry, and the app works fully offline.

| OS | Platform layer | Output |
|----|----------------|--------|
| Linux | `src/plat_x11.c` (Xlib, cairo, XInput2) | `make` -> `mspaint` |
| Windows 10+ (x86, x64, ARM64) | `src/plat_win32.c` (Win32 only) | `make -f Makefile.win ARCH=x64|x86|arm64` |
| macOS 11+ (Apple Silicon + Intel) | `src/plat_cocoa.m` (Cocoa; Objective-C is required to call AppKit) | `make -f Makefile.mac` |

## Windows
Install an LLVM-MinGW or MinGW-w64 toolchain, then:

    make -f Makefile.win ARCH=x64      # build/mspaint-x64.exe
    make -f Makefile.win ARCH=x86      # build/mspaint-x86.exe   (32-bit)
    make -f Makefile.win ARCH=arm64    # build/mspaint-arm64.exe (needs llvm-mingw for the ARM64 target)

* llvm-mingw (https://github.com/mstorsjo/llvm-mingw) cross-compiles all three from Linux, macOS or Windows.
* The result is a single static `.exe` (no DLLs, no installer needed). It embeds the icon, version info and a
  manifest that enables sharp rendering on high-DPI screens and UTF-8 paths.
* Installer (per-user, no admin rights): `makensis -DARCH=x64 -DVERSION=1.09 -DEXE=../../build/mspaint-x64.exe -DOUTFILE=../../dist/setup-x64.exe packaging/windows/installer.nsi`

## macOS
On a Mac with the Command Line Tools (`xcode-select --install`):

    sh packaging/macos/make_app.sh

This builds one universal binary (arm64 + x86_64, minimum macOS 11), wraps it in `Paint.app`, applies an ad-hoc
code signature (required for Apple Silicon to run it) and creates `dist/mspaintV1.12-mac.dmg`.
The app is not notarized (that needs a paid Apple developer account). On first launch macOS may say it can't verify the
developer: right-click Paint.app > Open > Open, or run `xattr -dr com.apple.quarantine /Applications/Paint.app`.

## Automatic builds (optional)
`.github/workflows/build.yml` builds every Windows architecture, the NSIS installers and the macOS dmg in the cloud.
It is only used if you put the project on GitHub yourself.

## Porting to another OS
Implement every function in `src/plat.h` in a new `src/plat_<os>.c`, and add it to a Makefile. Nothing else changes.
