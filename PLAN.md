# Plan and findings

## Dump (2026-10-11)
`~/Hermes/Wii U ROM`: decrypted (loadiine layout), base + update v208 (1.5.0) + DLC v80.
`U-King.rpx` (update) = 23.3 MB.

## Probe 1: WWHD recompiler on U-King.rpx (unmodified)
| | |
|---|---|
| functions discovered | 203,444 |
| generated C | 304 files, ~690 MB |
| unhandled instructions | 53, all `op=0` words (data or padding in .text, e.g. 0x0421211C) |
| function imports | 437 used; ~310 already implemented by WWHD runtime, ~127 missing |
| test compile | `code_100.c` compiles cleanly with Apple clang -O1 in ~2.5 s |

The CPU side is essentially solved by the existing translator. The work is in the runtime.

### Missing imports by area (see build/missing_imports.txt)
- **gx2 (25), the hard part:** compute shaders (`GX2SetComputeShader`), stream-out / transform
  feedback (`GX2SetStreamOut*`; Metal has no transform feedback, so emulate by having the vertex
  function write to a buffer), occlusion queries + conditional render (`GX2Query*`), display-list
  copy/patch, depth->texture conversion, misc state regs.
- **coreinit (31):** semaphores, fast mutexes, `__tls_get_addr` (TLS!), allocator API, FS
  write ops (saves), overlay arena, MCP.
- **h264 (13):** cutscene/movie decode, so use VideoToolbox.
- **nn_nfp (19):** amiibo, stub as "no amiibo" first.
- **nn_aoc (7):** DLC mounting, needed to load the `[DLC]` content.
- **zlib125 (6):** link real zlib.
- **vpad gyro (6), snd_core (5), nn_boss (8), sysapp, padscore:** small and mostly stubs.

## Roadmap
1. **Fork the runtime.** Copy WWHD `runtime/` + CMake into this repo, make the guest ABI
   game-agnostic (strip WWHD-only hooks, `wwhd_*` names), point the loader at U-King.rpx/RPLs and
   BotW content paths (base + update overlay + DLC).
2. **Build everything.** Compile all 304 files plus the runtime with stubs for every missing import
   (log on first call). `brew install cmake ninja sdl3 glslang` first.
3. **Boot to the first GX2 frame.** Implement coreinit gaps (TLS, semaphores, mutexes) as they're
   hit. Name functions with zeldaret/botw (Switch decomp: match by strings/RTTI/call shape, not
   bytes) so crash logs are readable.
4. **Graphics bring-up.** Add GX2-level tracing to *both* Cemu and our runtime and diff frame by
   frame. Do occlusion queries, compute, then stream-out. Treat Cemu's BotW graphic packs as the quirk list.
5. **Gameplay.** Get Great Plateau playable, then movies (h264), saves, DLC, audio polish.
6. **Ports and mods.** Resolution, 60 fps (FPS++ logic: physics is tied to frame time), mouse/keyboard, gyro.

## Progress log
### 2026-10-11: boots to the title screen
Forked the WWHD runtime into this repo (upstream commit in UPSTREAM_WWHD). Changes:
- `core.cpp`: dispatch table 16 to 36 MiB (U-King .text ends at 04347C2C); TLS template from
  SHF_RPL_TLS sections + `__tls_get_addr`.
- `main.cpp` / `hle/fs.cpp`: loads U-King.rpx; `--game` (update), `--base` (base game, content/meta
  fallback), `--aoc` (DLC at /vol/aoc<id>).
- `threads.cpp`: OSFastMutex_*, OSSemaphore, OSPeekMessage.
- `hle/coreinit_mem.cpp`: OSGetMemBound, overlay arena (A0000000).
- `gx2/gx2_core.cpp`: GX2DrawIndexedEx2 (base instance ignored, logged).
- `wwhd_unlinked.cpp` (tools/wwhd_stubs.py): 172 WWHD-only hook symbols, which stop the game if called.

Result: all game threads start (Havok, ActorCreate, Resource*, audio over CoreAudio), Metal presents
at 60 fps, and the title shows "(A) Start" on a black background (logo / 3D scene missing).
Run: `tools/run.sh [seconds] [log]`; screenshots: `WWHD_TEST_SCREENSHOT=600,1500 WWHD_SCREENSHOT_DIR=build/shots`.

Open from the logs: GX2SetStencilMaskReg / SetTargetChannelMasksReg / SetAlphaToMaskReg,
GX2SetGPUFence, GX2SetEventCallback, nn_aoc (DLC list), nn_nfp (amiibo), OSGetThreadAffinity, gyro, AX aux.
- Later the same day: with a Switch Pro controller over Bluetooth, A at the title starts a new game.
  The "Nintendo presents" intro plays, then the Shrine of Resurrection with Zelda's voice, and **Link
  renders**. About 290 draws/frame. Missing: music/SFX (only voice plays), plus the base-instance /
  query / depth-only GX2 calls above.
