#!/bin/sh
# usage: tools/installer/macos/build.sh [DEST [--release VERSION]]  -- build "BOTWRecomp Installer.app"
# (default DEST: the source folder's root, next to tools/). Needs only Apple's Command Line Tools (swiftc).
# A source build points the app at this checkout (BotWSourceRoot); --release leaves that out, and
# tools/release/botw_dmg.py puts the installer's files into Contents/Resources/botwrecomp.
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DEST="${1:-$ROOT}"
APP="$DEST/BOTWRecomp Installer.app"
VERSION="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo source)"
SOURCE_KEY="$SOURCE_KEY"
if [ "$2" = "--release" ]; then VERSION="$3"; SOURCE_KEY=""; fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/c/MacOS" "$TMP/c/Resources"
/usr/bin/xcrun swiftc -O -swift-version 5 -parse-as-library -target "$(uname -m)-apple-macos13.0" \
    "$ROOT/tools/installer/macos/BotWInstaller.swift" -o "$TMP/c/MacOS/BotWInstaller"
/usr/bin/xcrun swiftc -O -swift-version 5 "$ROOT/tools/installer/macos/make_icon.swift" -o "$TMP/make_icon"
"$TMP/make_icon" "$TMP/icon.png"
mkdir -p "$TMP/AppIcon.iconset"
for s in 16 32 128 256 512; do
    sips -z $s $s "$TMP/icon.png" --out "$TMP/AppIcon.iconset/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$TMP/icon.png" --out "$TMP/AppIcon.iconset/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$TMP/AppIcon.iconset" -o "$TMP/c/Resources/AppIcon.icns"
cat > "$TMP/c/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>BOTWRecomp Installer</string>
<key>CFBundleDisplayName</key><string>BOTWRecomp Installer</string>
<key>CFBundleIdentifier</key><string>local.botw-mac.installer</string>
<key>CFBundleExecutable</key><string>BotWInstaller</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>$VERSION</string>
<key>CFBundleShortVersionString</key><string>$VERSION</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>CFBundleIconFile</key><string>AppIcon</string>
$SOURCE_KEY
</dict></plist>
PL
rm -rf "$APP"
mkdir -p "$APP"
mv "$TMP/c" "$APP/Contents"
codesign --force -s - "$APP" >/dev/null 2>&1 || true
echo "$APP"
