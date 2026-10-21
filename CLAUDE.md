# BOTWRecomp: working notes for Claude

Native Apple Silicon port of Breath of the Wild (Wii U v208) by static recompilation: PowerPC → C
(`tools/recomp/`), Cafe OS reimplemented in `runtime/` (forked from ZeldaWWHDRecomp, see
`UPSTREAM_WWHD`), GX2 → Metal. Overview and build steps in [README.md](README.md), bring-up history and
debug switches in [PLAN.md](PLAN.md).

## Workflow

- **One task, one fresh session.** Don't fork the original port conversation (its transcript is
  over 120 MB). Start a fresh session with a short brief; this file and PLAN.md give the context.
- **Title sessions by area:** `Core · …`, `Graphics · …`, `Perf · …`, `Input · …`, `Release · …`.
  All BotW sessions live in the sidebar group "BotW Mac port".
- **Don't launch the game yourself.** Build, then let the user play-test and report back. Automated
  runs (`tools/autotest.sh`) only when the user asks.
- **Perf changes:** report frame time *and* CPU % / heat (test machine is a fanless M4 MacBook Air
  that throttles, so compare early in a run). No busy-waits.
- **When a fix lands:** commit with the measurement in the message (e.g. "Hateno 2x: 20.7 → 17.4
  ms/frame"), add a dated entry to PLAN.md's progress log if it changes the state of the port, and
  tell the user the session can be archived.
- **Captures** (`captures/<timestamp>/`, F12) are ~850 MB each. Keep the latest one or two, delete
  the rest once the bug they were for is fixed.

## Build and run

```
cmake --build build/rt --target wwhd    # runtime (build/gen holds the translated C)
tools/run.sh [SECONDS] [LOG]            # boots build/rt/wwhd on the dump (BOTW_DUMP= overrides)
tools/autotest.sh LOG SECONDS [ENV...]  # scripted load-save run; restores build/save afterwards
tools/make_app.sh ~/Desktop             # bundle as Breath of the Wild.app
tools/release/botw_dmg.py VERSION       # release DMG
```

Dump: `~/Hermes/Wii U ROM`. `build/`, `captures/` and `ref/` are git-ignored; never commit anything
generated from the game.

## Where things are

- `runtime/src/gx2/`, `runtime/src/gfx/`: GX2 → Metal renderer, shader cache, readback probes.
- `runtime/src/hle/`: Cafe OS (coreinit, fs, AX audio, vpad input).
- `runtime/src/motion/`, `mouse_look.cpp`, `input_map.cpp`: gyro, mouse camera, remapping.
- `runtime/src/mods/`: graphic packs. `runtime/src/overlay/`: settings overlay.
- `runtime/third_party/cemu/`: Latte shader decompiler (MSL output).
- `tools/installer/`: `botw_setup.py` and the SwiftUI installer app.
- `tools/bench/`, `tools/verify/`: benchmarks and translator verification.
