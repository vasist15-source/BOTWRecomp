#!/bin/sh
# usage: tools/run.sh [SECONDS] [LOG]  -- boot BotW from the dump, killing it after SECONDS (default: run until closed)
DUMP="${BOTW_DUMP:-$HOME/Hermes/Wii U ROM}/The Legend of Zelda Breath of the Wild"
LOG="${2:-build/boot.log}"
mkdir -p build/save
build/rt/wwhd --game "$DUMP [Update] [0005000e101c9400]" --base "$DUMP [Game] [00050000101c9400]" \
    --aoc "$DUMP [DLC] [0005000c101c9400]" --save build/save > "$LOG" 2>&1 &
P=$!
if [ -n "$1" ]; then
    i=0; while [ $i -lt "$1" ] && kill -0 $P 2>/dev/null; do sleep 1; i=$((i + 1)); done
    kill $P 2>/dev/null
fi
wait $P; echo "exit $?" >> "$LOG"
