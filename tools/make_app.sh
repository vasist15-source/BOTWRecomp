#!/bin/sh
# usage: tools/make_app.sh [DEST]  -- bundle build/rt/wwhd as "Breath of the Wild.app" (default /Applications)
# The app uses the same shader cache and dump as tools/run.sh; its log goes to ~/Library/Logs/BotW/wwhd.log.
# Its save folder: BOTW_SAVE if set, else the installed game's (install.plist's save, so a dev build plays
# the player's own saves), else build/save as tools/run.sh. wwhd is the bundle executable and finds them in
# Info.plist's BotWLaunch (runtime/src/platform/app_launch.mm). The icon comes from the user's own dump
# (meta/iconTex.tga).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-/Applications}"
APP="$DEST/Breath of the Wild.app"
DUMP="${BOTW_DUMP:-$HOME/Hermes/Wii U ROM}/The Legend of Zelda Breath of the Wild"
# as tools/run.sh: the USA release's folders (101c9400), else the European release's (BOTW_TITLE= chooses)
T="${BOTW_TITLE:-101c9400}"
[ -n "$BOTW_TITLE" ] || [ -d "$DUMP [Game] [00050000$T]" ] || T=101c9500
SAVE="$BOTW_SAVE"
INSTALL="$HOME/Library/Application Support/BotW/install.plist"
[ -n "$SAVE" ] || [ ! -f "$INSTALL" ] || SAVE="$(plutil -extract save raw -o - "$INSTALL" 2>/dev/null)"
[ -n "$SAVE" ] || SAVE="$ROOT/build/save"
TMP="$(mktemp -d)"
mkdir -p "$TMP/c/MacOS" "$TMP/c/Resources"
cp "$ROOT/build/rt/wwhd" "$TMP/c/MacOS/wwhd"
ICON="$DUMP [Game] [00050000$T]/meta/iconTex.tga"
if [ -f "$ICON" ]; then
    mkdir -p "$TMP/i.iconset"
    sips -s format png "$ICON" --out "$TMP/icon.png" >/dev/null
    for s in 16 32 128 256 512; do
        sips -z $s $s "$TMP/icon.png" --out "$TMP/i.iconset/icon_${s}x${s}.png" >/dev/null
        sips -z $((s * 2)) $((s * 2)) "$TMP/icon.png" --out "$TMP/i.iconset/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$TMP/i.iconset" -o "$TMP/c/Resources/icon.icns" || true
fi
cat > "$TMP/c/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Breath of the Wild</string>
<key>CFBundleDisplayName</key><string>Breath of the Wild</string>
<key>CFBundleIdentifier</key><string>local.botw-mac</string>
<key>CFBundleExecutable</key><string>wwhd</string>
<key>CFBundleIconFile</key><string>icon</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>$(cd "$ROOT" && git rev-parse --short HEAD)</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>GCSupportsControllerUserInteraction</key><true/>
<key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
<key>LSSupportsGameMode</key><true/>
<key>GCSupportsGameMode</key><true/>
<key>BotWLaunch</key><dict>
<key>game</key><string>$DUMP [Update] [0005000e$T]</string>
<key>base</key><string>$DUMP [Game] [00050000$T]</string>
<key>aoc</key><string>$DUMP [DLC] [0005000c$T]</string>
<key>save</key><string>$SAVE</string>
<key>dir</key><string>$ROOT</string>
</dict>
</dict></plist>
PL
rm -rf "$APP"
mkdir -p "$APP"
mv "$TMP/c" "$APP/Contents"
rm -rf "$TMP"
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || true
echo "$APP (save: $SAVE)"
