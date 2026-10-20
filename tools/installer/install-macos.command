#!/bin/bash
# Breath of the Wild for Mac: double-click to set up the game (the first time: right-click > Open).
# Builds "Breath of the Wild Installer.app" next to tools/ (Apple's Command Line Tools only) and opens it.
#   --terminal ARGS   run the setup in Terminal instead (tools/installer/botw_setup.py ARGS; see its --help)
cd "$(dirname "$0")/../.." || exit 1
DIR="$(pwd)"
export PYTHONDONTWRITEBYTECODE=1
# a downloaded source folder is quarantined; you opened this script, so its own tools may run too
/usr/bin/xattr -dr com.apple.quarantine "$DIR" 2>/dev/null
clt_ok() { xcode-select -p >/dev/null 2>&1 && xcrun --find clang >/dev/null 2>&1 && xcrun --find swiftc >/dev/null 2>&1; }
if ! clt_ok; then
  echo "Breath of the Wild for Mac needs Apple's free Command Line Tools (compiler and Python)."
  echo "A system dialog opens now: click \"Install\" and accept the license (a few minutes)."
  xcode-select --install >/dev/null 2>&1
  until clt_ok; do sleep 5; done
  echo "Command Line Tools installed."
fi
if [ "$1" = "--terminal" ]; then
  shift
  exec /usr/bin/python3 "$DIR/tools/installer/botw_setup.py" "$@"
fi
APP="$DIR/Breath of the Wild Installer.app"
SRC="$DIR/tools/installer/macos/BotWInstaller.swift"
if [ ! -x "$APP/Contents/MacOS/BotWInstaller" ] || [ "$SRC" -nt "$APP/Contents/MacOS/BotWInstaller" ]; then
  echo "Building the installer app..."
  "$DIR/tools/installer/macos/build.sh" "$DIR" || { echo "The installer app could not be built (see above)."; exit 1; }
fi
open "$APP"
