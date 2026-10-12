#!/bin/sh
# usage: tools/make_app.sh [DEST]  -- bundle build/rt/wwhd as "Breath of the Wild.app" (default /Applications)
# The app uses the same save (build/save), shader cache and dump as tools/run.sh; its log goes to
# ~/Library/Logs/BotW/wwhd.log. The icon comes from the user's own dump (meta/iconTex.tga).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-/Applications}"
APP="$DEST/Breath of the Wild.app"
DUMP="${BOTW_DUMP:-$HOME/Hermes/Wii U ROM}/The Legend of Zelda Breath of the Wild"
TMP="$(mktemp -d)"
mkdir -p "$TMP/c/MacOS" "$TMP/c/Resources"
cp "$ROOT/build/rt/wwhd" "$TMP/c/MacOS/wwhd"
cat > "$TMP/c/MacOS/launch" <<SH
#!/bin/sh
mkdir -p "\$HOME/Library/Logs/BotW" "$ROOT/build/save"
cd "$ROOT"
exec "\$(dirname "\$0")/wwhd" --game "$DUMP [Update] [0005000e101c9400]" --base "$DUMP [Game] [00050000101c9400]" \\
    --aoc "$DUMP [DLC] [0005000c101c9400]" --save "$ROOT/build/save" > "\$HOME/Library/Logs/BotW/wwhd.log" 2>&1
SH
chmod +x "$TMP/c/MacOS/launch"
ICON="$DUMP [Game] [00050000101c9400]/meta/iconTex.tga"
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
<key>CFBundleExecutable</key><string>launch</string>
<key>CFBundleIconFile</key><string>icon</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>$(cd "$ROOT" && git rev-parse --short HEAD)</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>GCSupportsControllerUserInteraction</key><true/>
</dict></plist>
PL
rm -rf "$APP"
mkdir -p "$APP"
mv "$TMP/c" "$APP/Contents"
rm -rf "$TMP"
codesign --force --deep -s - "$APP" >/dev/null 2>&1 || true
echo "$APP"
