#!/bin/bash
# Attach Instruments to the running game and record a trace (no new game window).
# usage: tools/profile.sh [template] [seconds]
#   template: "Game Performance" (default), "Metal System Trace", "Time Profiler"
set -e
TEMPLATE="${1:-Game Performance}"
SECS="${2:-15}"
XCTRACE=$(xcrun -f xctrace 2>/dev/null || echo /Applications/Xcode-beta.app/Contents/Developer/usr/bin/xctrace)
PID=$(pgrep -x wwhd | head -1)
[ -z "$PID" ] && { echo "game not running: start it, get to a heavy scene, then rerun"; exit 1; }
OUT="$(cd "$(dirname "$0")/.." && pwd)/build/profiles"
mkdir -p "$OUT"
NAME="$OUT/$(date +%Y%m%d-%H%M%S)-${TEMPLATE// /_}.trace"
echo "recording '$TEMPLATE' for ${SECS}s on pid $PID -> $NAME (keep playing normally)"
"$XCTRACE" record --template "$TEMPLATE" --attach "$PID" --time-limit "${SECS}s" --output "$NAME"
echo "done: $NAME"
