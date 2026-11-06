#!/bin/sh
# Build SmPaint.app (universal, ad-hoc signed) and a .dmg. Run on a Mac from the project root.
set -e
VER=$(sed -n 's/^#define APP_VERSION "V\(.*\)"/\1/p' src/app.h)
make -f Makefile.mac
rm -rf dist; mkdir -p dist/SmPaint.app/Contents/MacOS dist/SmPaint.app/Contents/Resources
cp build/mspaint dist/SmPaint.app/Contents/MacOS/Paint
cp packaging/macos/mspaint.icns dist/SmPaint.app/Contents/Resources/mspaint.icns
sed "s/@VERSION@/$VER/g" packaging/macos/Info.plist > dist/SmPaint.app/Contents/Info.plist
# Apple Silicon refuses unsigned code, so apply an ad-hoc signature (no developer account needed).
codesign --force --deep --sign - dist/SmPaint.app
mkdir -p dist/dmg && cp -R dist/SmPaint.app dist/dmg/ && ln -s /Applications dist/dmg/Applications
hdiutil create -volname "Paint" -srcfolder dist/dmg -ov -format UDZO "dist/SmPaintV$VER-mac.dmg"
rm -rf dist/dmg
# Unzip-and-double-click version: a zip holding just SmPaint.app (ditto keeps the signature and permissions intact).
ditto -c -k --keepParent dist/SmPaint.app "dist/SmPaintV$VER-mac.zip"
echo "Built dist/SmPaint.app, dist/SmPaintV$VER-mac.zip and dist/SmPaintV$VER-mac.dmg"
