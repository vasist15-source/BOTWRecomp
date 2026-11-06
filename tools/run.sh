#!/bin/sh
# usage: tools/run.sh [SECONDS] [LOG]  -- boot BotW from the dump, killing it after SECONDS (default: run until closed)
DUMP="${BOTW_DUMP:-$HOME/Hermes/Wii U ROM}/The Legend of Zelda Breath of the Wild"
# the title's low half: the USA release's folders (101c9400), else the European release's (BOTW_TITLE= chooses)
T="${BOTW_TITLE:-101c9400}"
[ -n "$BOTW_TITLE" ] || [ -d "$DUMP [Game] [00050000$T]" ] || T=101c9500
LOG="${2:-build/boot.log}"
mkdir -p build/save
build/rt/wwhd --game "$DUMP [Update] [0005000e$T]" --base "$DUMP [Game] [00050000$T]" \
    --aoc "$DUMP [DLC] [0005000c$T]" --save build/save > "$LOG" 2>&1 &
P=$!
if [ -n "$1" ]; then
    i=0; while [ $i -lt "$1" ] && kill -0 $P 2>/dev/null; do sleep 1; i=$((i + 1)); done
    kill $P 2>/dev/null
fi
wait $P; echo "exit $?" >> "$LOG"
