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

## Audio investigation (parked 2026-10-11)
Symptom: no world audio. AX stats (`WWHD_AX_STATS=1`): zero voices ever acquired outside the menu.
Menu cursor sounds untested.
- Cemu reference log (`build/cemu_audio.log`, Audio API + PPC context): setup calls are identical to
  ours; Cemu then calls `AXSetAuxReturnVolume` + `AXRegisterAuxCallback(..., 03C1D838)` from
  LR 03C1DE5C (f_03C1DCE4) ~1.5 s after AXInit, and acquires voices from the AX frame callback.
- That code is the handler `03C1EB34` of aal command channels at 105983B0 / 10598870 (two
  OSMessageQueues each, consumer `03C1525C`). In ours nothing is ever submitted to them.
- Submit to 10598870 is `03C1942C`, called each frame from `03BE6920` only if
  `ld32(foundation+0x890)` (here 3DC11910, holder 3DC1190C) is non-null. It is always null; a
  hardware watchpoint shows nothing ever writes it. The only static setter found (`03C18D04`) is
  reached only via destructors (037DB2C8, 03BB11B8) or an unconstructed class (vtable 1027BA28).
- Next: find what writes foundation+0x890 in Cemu (Cemu PPC debugger memory-write breakpoint; an
  execution breakpoint on 03C18D04 crashed Cemu). Tools: WWHD_TRACE_LIB, WWHD_TRACE_MSG,
  WWHD_WATCH / WWHD_WATCH_R3, tools/callgraph.py, tools/cemu_gdb_stack.py, lldb recipes in build/*.lldb.

## Session 1 end state (2026-10-11)
Done after the first commit: point limits, nn_aoc (DLC loads), overlay arena mapped for the GPU,
stream-out (GX2 regs + OP_STREAMOUT_UPDATE + Metal sb binding, useTFViaSSBO). The last
stream-out fix (useTFViaSSBO) is built but not play-tested yet.
Next: play-test stream-out (look for "stream-out draws so far" in the log and shader-failed staying
at 0), save thumbnails are black (GPU-to-guest readback), performance (~25 fps at 1x), audio (above).

### Audio, later lead (2026-10-12)
Comparing with Cemu showed the AX frame callback (03C23E1C) is fine: it runs every frame and the init flags
(1047F1DC, 1047F1DF) are set. Inside it, f_03C23BF8 loops over 96 voice slots and calls f_03C23B50
(the whole voice/mix subtree, ~30 functions) only for slots whose flag byte (array at 1047F1FC) is nonzero.
No slot ever becomes active, so the gap is upstream: the game never *requests* a sound. Tools:
`WWHD_PEEK=addr:len`, build/axcb.json (reachable set), lldb recipe build/axcb.lldb.

## State at the end of session 1 (2026-10-12)
Playable: boots, intro, Great Plateau, menus, NPCs, saving, DLC mounted, ~30 fps at 1x on an M4.
Not working / open, in the order I would take them:
1. **Hitches.** `[hitch]` log lines (frames over 45 ms, WWHD_HITCH_MS) showed 47 of 62 hitches came with
   bursts of new pipelines (hundreds in one frame). Cause: the cache replay queues ~60k pipelines but
   built 16 + 16 shaders per frame. cache_warm_step now has a time budget scaled by frame slack
   (WWHD_WARM_MS=0 restores the old fixed counts, WWHD_BG_COMPILES caps compiles in flight).
   **UNTESTED**: compare `[hitch]` counts with the previous session (62 hitches, 47 with >=20 pipelines).
2. **Vertex shader variants.** The remaining shader-variant growth is vertex shaders split by the pixel
   shader's input layout (WWHD_KEY_DIFF=1 prints the fields). Fix idea: name VS outputs / PS inputs by
   semantic id so the two stages do not depend on each other (Metal links `[[user(...)]]` by name);
   unfed PS inputs become constants (see linkPSInputsToVS, GLSL header). Touches LatteDecompilerEmitMSLHeader.hpp.
3. **Audio** (see "Audio investigation"): the game never requests a sound.
4. Black save thumbnails (GPU -> guest readback), real occlusion queries, 60 fps / resolution options.
Done this session beyond the first commits: base instance, point limits, DLC, stream-out (also without a
render target), per-page surface index for texture invalidation, shader cache writer thread, shader keys
limited to the texture units and stage registers a program reads (cache 1.1 GB -> 0.25 GB), Cemu's fast
tile decode written straight into staging memory (verified on 27k textures).
Debug switches: WWHD_TRACE_LIB, WWHD_PEEK, WWHD_AX_STATS, WWHD_KEY_DIFF, WWHD_VERIFY_DETILE, WWHD_HITCH_MS.

## Audio status (2026-10-12)

Working: menus, title and gameplay sound. Two fixes: OSWaitRendezvous counted callers instead of
cores (aal initialized nw::snd's SoundArchivePlayer before its launcher thread set up the sound
system, so the game never sent a real command), and AX voice positions overflowed 32 bits (ADPCM
nibble addresses of sample data at 0xA5xxxxxx). Open: a burst of noise was heard once after loading
a save; if it recurs, suspects are the per-voice biquad/LPF filters (BotW updates them constantly,
WWHD never used them) and stream voices (type 1).

## State 2026-10-12 (end of day)

Fixed today: GX2DrawDone stall (2x 10 -> ~28 fps), audio (rendezvous per core, 64-bit AX positions),
save thumbnails, 463 failed pipelines, async linear write-back, render-pass merging (-25% attachment
traffic), GX2 vsync/flip callbacks + GX2SetGPUFence (menus ran at 60 fps, now the game's own 30 cap),
OSGetThreadAffinity.
Open: white pond water (lighting pass PS 3C44E300 adds it; its depth/normal/albedo inputs check out,
cube-array index fine; next: compare that shader's MSL with Cemu's GLSL / render it in Cemu);
foliage looks flat/bright; first-visit shader hitches; GPU ~30 ms/frame at 2x.
Tools: F12 capture (draws.log, full-screen pass targets, their inputs and PS MSL), WWHD_PASS_DUMP,
WWHD_FUNC_COUNT, WWHD_ORDER, WWHD_MSG_DUMP, WWHD_PROFILE=1.
