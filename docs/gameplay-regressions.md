# v208 gameplay regressions

These fixes target the USA v208 executable already documented by this project
(SHA-256 `ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30`).
The command-line translator and installer share a checked recompiler factory.
Patches check executable identity and the original instructions before emission;
the RPX and content files on disk are unchanged.

## Changes

- Keep the screenshot key mapping alive while iterating its keys under C++20.
- Honor FPSCR.NI single-precision underflow before rounding, including values just
  below `FLT_MIN`. Preserve signed zero; leave load/store conversions alone.
  Native audio arithmetic and the translator use the same helper.
- Publish 4x4 RGBA8/RGBA32F actor-picking targets when DrawDone returns, avoiding
  stale references to destroyed actors during Stasis aiming. Exposure readback
  remains asynchronous and the independent CPU-probe queue is retained.
- Interpret ADPCM offsets as signed nibble displacements when restoring voices or
  changing sample bases, preserving high absolute address bits and PCM behavior.
- Pair JPEG guest setjmp/longjmp with thread-local host frames, so recovery leaves
  the old native codec call chain as well as restoring guest registers.
- Pace fixed-step 30 FPS without catch-up after stalls; add 90/120 FPS caps without
  changing the upstream 30 FPS default. Higher caps use host pacing without vsync;
  they do not promise achievable performance.
- Scale audio minimum ticks, controller repeat/countdown, map panning and separate
  fixed-frame gameplay clocks by measured VFR intervals: bow stamina, regeneration,
  combat movement, arrows, shield surfing, rune movement, chasing Koroks, the Master
  Cycle, water/impact physics and the Thunderblight movement-timer workaround.
- Send the loading-pane close request even if its opening animation is unfinished.
- Cap sensitive cutscenes temporarily. Nested event/object lifetimes retain the
  strictest active cap; completion restores the user's preference.
- Preserve all three motion axes at sensor cadence, independently of VPAD polling.
  Native orientation stays unscaled; sensitivity/inversion applies to aiming rate.
  Valid gravity samples correct tilt; pause/disconnection/stale input stop rates.
  Yaw/roll modes and custom sensitivity remain available. Old player-space settings
  migrate once to native, with the untouched horizontal 0.5 default becoming 1.0;
  vertical and other custom values remain. The upstream source default stays off.

Menu changes only expose the additional FPS choices and explain native motion.
No cheats, localization, new region support, resource importer, activation,
packaging customization or save-path changes are included.

## Sources

FPS exceptions and the cutscene list are adapted from the
[Cemu graphic packs FPS++ patches](https://github.com/cemu-project/cemu_graphic_packs/tree/efa14afb2f6b25080c19878bb6c2ea722d78655b/src/BreathOfTheWild/Mods/FPS%2B%2B),
licensed [CC0-1.0](https://github.com/cemu-project/cemu_graphic_packs/blob/efa14afb2f6b25080c19878bb6c2ea722d78655b/LICENSE.md).
This port uses guarded native hooks rather than distributing patched binaries.
Controller repeat retains the original acceleration routine. The Thunderblight
workaround changes only its two reads, not the shared constant. The water formula
is an empirical compatibility adjustment, not exact 30 FPS equivalence.

Motion conventions follow Cemu's MPL-2.0
[MotionSample reference](https://github.com/cemu-project/Cemu/blob/42876aa86d2b92f9c620a76959a792bf392b89d3/src/input/motion/MotionSample.h).
ADPCM signed offsets can also be compared with Cemu's
[AX decoder](https://github.com/cemu-project/Cemu/blob/main/src/Cafe/OS/libs/snd_core/ax_mix.cpp).

## Regression commands

Use your own legal local v208 dump. Regenerate after translator changes. Generated
source and JPEG fixture data must stay local, outside Git. Optional guest-function
tests need a POSIX host and the standard 30,000-instruction output batching.

```sh
python3 tools/probe_recomp.py "$BOTW_RPX" build/gen
python3 - "$BOTW_RPX" build/jpeg-rodata.bin <<'PY'
import pathlib, sys
sys.path.insert(0, 'tools')
from rpx import Rpx
section = Rpx(sys.argv[1]).by_name['.rodata']
assert section.addr == 0x10000000
pathlib.Path(sys.argv[2]).write_bytes(section.data)
PY
cmake -S . -B build/rt -G Ninja -DWWHD_RENDERER=METAL -DWWHD_EXTRACTOR=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DBOTW_GUEST_TESTS=ON -DBOTW_JPEG_TEST_DATA="$PWD/build/jpeg-rodata.bin"
cmake --build build/rt --target wwhd screenshot_test float_mode_test ax_resume_offset_test \
  surround_output_test fps_limiter_test botw_timing_test botw_loading_test botw_fps_test \
  botw_jpeg_test motion_test botw_motion_test
ctest --test-dir build/rt --output-on-failure \
  -R '^(screenshot|float_mode|ax_resume_offset|surround_ax_stage_[01]|fps_limiter|botw_timing|botw_loading|botw_fps|botw_jpeg|motion|botw_motion_(30|60|90|120))$'
python3 -B -m unittest discover -s tools -p test_botw_translator_package.py -v
build/rt/wwhd --metal-readback-test
```

The GPU check uses synthetic textures and exits without starting gameplay; unset
`WWHD_SYNC_WRITE_BACK` to check the default policy. Installer tests stage allowlisted
scripts, verify isolated import and unknown-program rejection, check patch-module
cache invalidation, and reject game executables in the payload. They do not build a DMG.

## Evidence boundaries

Historical downstream reports confirmed the specific Stasis crash, bomb noise,
camera failure and repeated-audio/menu symptoms no longer reproduced after their
fixes. These reports are separate from this branch's automated checks and are not
a full playthrough or proof of all target platforms.

Guest fixtures execute extracted functions/instruction blocks with synthetic
peripheral state. They cover FPS changes, JPEG error cleanup/retry and registers,
loading states, cutscene nesting, and motion matrices/rates at 30/60/90/120 FPS.
They do not replace in-game verification of all bosses, cutscenes, apparatus puzzles
or sustained motion feel. The final gravity correction still needs extended in-game
controller testing. Correctness tests do not establish a performance improvement.
