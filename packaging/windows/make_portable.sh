#!/bin/sh
# Builds the portable Windows zip: unzip it, double-click SmPaint.exe. No installer, nothing written outside its folder.
# SmPaint.exe is the x64 build (also runs on Windows-on-ARM through emulation); native x86 and arm64 builds sit in other-architectures/.
# Needs llvm-mingw on PATH (see BUILDING.md). Run from the project root.
set -e
VER=$(sed -n 's/^#define APP_VERSION "V\(.*\)"/\1/p' src/app.h)
for A in x64 x86 arm64; do make -f Makefile.win ARCH=$A; done
D="dist/SmPaint-V$VER-windows"
rm -rf "$D"; mkdir -p "$D/other-architectures"
cp build/mspaint-x64.exe "$D/SmPaint.exe"
cp build/mspaint-x86.exe "$D/other-architectures/SmPaint-x86.exe"
cp build/mspaint-arm64.exe "$D/other-architectures/SmPaint-arm64.exe"
cat > "$D/READ ME.txt" <<EOF
SmPaint V$VER for Windows 10 / 11
Double-click SmPaint.exe. Nothing to install.
SmPaint.exe is 64-bit (works on Intel/AMD and on ARM PCs). For a 32-bit PC use other-architectures\\SmPaint-x86.exe;
for a native ARM build use other-architectures\\SmPaint-arm64.exe.
If Windows SmartScreen says "Windows protected your PC", click More info, then Run anyway (the app is not code-signed).
EOF
(cd dist && zip -qr "SmPaint-V$VER-windows.zip" "SmPaint-V$VER-windows")
echo "Built dist/SmPaint-V$VER-windows.zip"
