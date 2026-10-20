#!/bin/sh
# usage: tools/installer/macos/build.sh [DEST]  -- build "Breath of the Wild Installer.app" (default: the
# source folder's root, next to tools/). Needs only Apple's Command Line Tools (swiftc).
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
DEST="${1:-$ROOT}"
APP="$DEST/Breath of the Wild Installer.app"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/c/MacOS" "$TMP/c/Resources"
/usr/bin/xcrun swiftc -O -swift-version 5 -parse-as-library -target "$(uname -m)-apple-macos13.0" \
    "$ROOT/tools/installer/macos/BotWInstaller.swift" -o "$TMP/c/MacOS/BotWInstaller"
cat > "$TMP/c/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Breath of the Wild Installer</string>
<key>CFBundleDisplayName</key><string>Breath of the Wild Installer</string>
<key>CFBundleIdentifier</key><string>local.botw-mac.installer</string>
<key>CFBundleExecutable</key><string>BotWInstaller</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo source)</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>BotWSourceRoot</key><string>$ROOT</string>
</dict></plist>
PL
rm -rf "$APP"
mkdir -p "$APP"
mv "$TMP/c" "$APP/Contents"
codesign --force -s - "$APP" >/dev/null 2>&1 || true
echo "$APP"
