#!/bin/sh
# usage: tools/autotest.sh LOG SECONDS [extra env...]  -- loads the save from the title screen (scripted
# presses), runs SECONDS, and restores build/save afterwards so automated runs never change it
LOG="$1"; SECS="$2"; shift 2
B=build/save.autotest-backup
rm -rf "$B" && cp -Rp build/save "$B" || exit 1
env WWHD_KEYS="${WWHD_KEYS:-600-606:K,800-806:K,1000-1006:K,1200-1206:K}" "$@" tools/run.sh "$SECS" "$LOG" >/dev/null 2>&1
rm -rf build/save && mv "$B" build/save
