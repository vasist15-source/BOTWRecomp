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

### 2026-10-27: dynamic resolution (Metal)
Ported from ZeldaWWHDRecomp d454eda + 8c8c383 (`runtime/src/dynres*`, test `dynres_test`). Settings >
Graphics > Dynamic resolution: Off (default), Down to 75%, Down to 50%. Every half second the busy share
(the per-frame GPU busy time metal_main.mm already measures) and the frame rate against the game's 30/60
target decide: GPU-bound (missing the rate, busy > 90 %) steps one level down (1, 7/8, 3/4, 5/8, 1/2),
room at the next level (< 80 % predicted) for two windows and the hold (2 s, doubling to 32 s after an
undone step) steps up. BotW differences from upstream: only the targets our rules scale with the internal
resolution take the factor (plus depth buffers matched to them, `Surface::dynMatched`, so colour and
depth keep one size), never below 1x (at 1x the setting does nothing; at 1.5x the floor is 1.125x);
shadow cascades, linear/readback targets and unscaled data targets keep their size. A change rescales
every surface at once (as an internal resolution change does); each keeps its last two textures, so a
step back reuses them. Pack shaders keep `$width/$height` of the full internal resolution (their
texel-step offsets keep the same screen-space size). The layout uses the full-resolution size; a lowered
picture is always presented filtered. Logs: `[dynres]` per change, `[gfx]` report line every 300 frames;
`WWHD_DYNRES_MIN`, `WWHD_DYNRES_TEST_AT=f:factor`, `WWHD_DYNRES_TEST_BUDGET=f:ms`, `WWHD_DYNRES_LOG_FRAMES=1`.
**UNTESTED** in game: Hateno/Kakariko at 2x, frame time, CPU %, hitch at a change, memory.

### 2026-11-04: 30 fps at real-time speed (issue #1), PR #2 merged
vasist15-source/BOTWRecomp#2 (shpodg) merged as 83c1a91: a host frame limiter at GX2SwapScanBuffers
now also caps 30 fps (`fps_limiter.h`), so the frame-locked game can't run above 1x when its own vsync
fence lets frames through early (an Oct 20 log showed 40-53 swaps/s at 30 fps; the fence leak itself
is not found, b0d39a2's grid moves are a suspect). Also from the PR: 90/120 fps, VFR-scaled
menu/map/audio clocks and FPS++-derived translator patches (`tools/botw_fps_patches.py`, applied as C
text in `probe_recomp.py`, so those functions are kept out of crlive/leaflocal via
`Recompiler.plain_funcs`), per-cutscene caps, three-axis motion, ADPCM/JPEG/FPSCR.NI fixes, Stasis
picking read back at DrawDone. The PR's limiter paced from each actual release, so the sleep's
oversleep added up (60 fps 56-58, 30 fps 29.4 swaps/s); it now keeps a fixed schedule and only resets
it after a late frame (60 fps 59.7-60.0, 30 fps 30.0). Play-tested: 30 fps at normal speed.
CPU % not measured yet.

### 2026-11-04: Liquid Glass settings menu and performance HUD (Metal)
The Cmd+, / F1 overlay is restyled as a macOS window: no title bar, close button top left, a sidebar
with icon tiles in place of the tab bar, a large page title and grouped cards (`heading()` opens
them), switches instead of checkboxes, capsule segments instead of radio buttons, SF Pro. The Saves
page is gone (save states and crash recovery stay in the menu bar and on F2-F5 / Shift+F1-F5);
Screenshots moved to Display. On Metal, `overlay::glass_panels()` lists the root windows on glass and
`display.mm` draws them: the composed picture at a quarter of the target's size, blurred in two
passes, then a rounded panel per window (rim light, edge refraction, shadow) just before that window's
draw list; only while overlay windows show. Vulkan keeps dense backgrounds. Play-tested (menu opened
three times, 60.0 swaps/s, GPU busy 10-13 ms/frame); the blur's own cost is not A/B measured yet.

### 2026-11-04: MetalFX upscaling, frame generation, Metal 4 residency and compiler (Metal)
Settings > Graphics > MetalFX, both off by default (saved as `metalfxUpscale` / `metalfxFrameGen`;
`WWHD_METALFX_UPSCALE`, `WWHD_METALFX_FRAMEGEN`, `WWHD_METALFX_LOG=1`). `gfx/metalfx.mm`, called by
`present_screens`:
- Upscaling: MetalFX's spatial scaler takes the TV scan image to the size of its box in the window when
  the box is larger (in place of the Picture scaling filter); made again on a size change (resize,
  dynamic resolution step). macOS 13+.
- Frame generation: one picture between each pair of game frames, presented first; the frame's own
  picture follows `afterMinimumDuration` half a game frame later (30 -> 60, 60 -> 120 on 120 Hz, +half a
  frame of latency). Only while game frame time >= 1.8 refreshes with vsync. Motion is estimated on the
  GPU (luma pyramid 1/4..1/32, coarse-to-fine block matching, sub-pixel, 3x3 median); the halfway picture
  samples both pictures half a vector away and leans to the newer one where they disagree. Standalone
  check on moving test pictures (scratch test, not in the tree): motion exact at the centre, still HUD box 0,
  halfway picture matches t=0.5 (mean error 6 vs 32 for either real picture), clean under Metal API and
  shader validation. MetalFX's own `MTLFXFrameInterpolator` is wired up behind
  `WWHD_METALFX_INTERPOLATOR=1` but does not interpolate with estimated motion and a constant depth: on the
  same pictures (macOS 27; any constant depth, reversed or not, any motion scale, camera matrices, own
  history) it puts out the current picture. A nil depth texture fails MetalFX validation; on macOS 27
  `contentWidth/Height` must be set or the whole picture is copied.
- Metal 4 on the existing queue (the full MTL4 command model was decided against: no hazard tracking):
  guest memory buffers in an `MTLResidencySet` on the main and probe queues (macOS 15+,
  `WWHD_RESIDENCY_SET=0` off); MSL libraries built by an `MTL4Compiler` (macOS 26+, `WWHD_MTL4_COMPILER=0`
  off; a library it fails on is retried with the device). Pipelines are still made by the device.
Play-tested (user, 2026-11-04): upscaling, frame generation and the Metal 4 changes all work. Not measured
yet: GPU cost, CPU % / heat with frame generation on vs off, shader compile times with MTL4Compiler.

### 2026-11-06: save states taken out of the menus
The Save States menu in the menu bar and its shortcuts (Cmd+1-5 / Cmd+Shift+1-5, F2-F5 / Shift+F1-F5)
are gone, so F2-F5 can be bound in Controls; crash recovery lost its switch with it
(`WWHD_CRASH_RECOVERY=1` still turns it on). Why: a full state loads into the running game only while
every guest thread is still parked where it was saved, because a thread's native call stack can't be
rebuilt. Play test: saving and loading on the spot works (Great Plateau, Kakariko), but from Kakariko
a slot saved earlier elsewhere was refused every time (a thread from the save had exited, a
WorkerMgr worker waited on another object). Making it work anywhere needs resumable recompiled
functions (a check at every non-leaf call), decided against for that per-call cost. The engine stays
for scripted runs (`tools/bench/run_bench.py` loads a state with `WWHD_STATE_LOAD_AT`); this change
touches no per-frame or per-call code.

### 2026-11-06: the release kit links with older SDKs again (issue #4)
0.1.7's installer stopped at "Linking the game" for players whose Command Line Tools have an SDK older
than macOS 26 (reported on macOS 15.8.2, M1 Air): `MTL4CompilerDescriptor`, `MTL4LibraryDescriptor` and
`MTLFXFrameInterpolatorDescriptor` were undefined. The installer links the kit against the player's own
SDK, and a class newer than the deployment target (13.0) is a weak import that this SDK must still list;
`@available` only makes it optional when the game runs. `MTLResidencySetDescriptor` (macOS 15) would
have failed the same way with a macOS 13/14 SDK. The four are now looked up by name (`NEWER_CLASS` in
`gfx/metal.h`, which checks the name at compile time), and `tools/release/botw_dmg.py` refuses a kit
with any weak import (0.1.7's kit: 4, now 0). Not run on macOS 15 yet: there the guarded paths should
fall back to the device's shader compiler and our own frame generation.

### 2026-11-06: cheats in the settings menu
Settings > Cheats: infinite arrows, weapons and bows don't break, shields don't break, Revali's Gale /
Daruk's Protection / Urbosa's Fury / Mipha's Grace without recharge, infinite hearts, infinite stamina,
Master Cycle Zero fuel and summoning anywhere, amiibo more than once a day. All off by default; each
switch applies at once (no restart) and is saved (`cheatArrows`, ...; `WWHD_CHEATS=arrows,shields|all`
decides for a run without saving). The sites are those of the v208 patches in Cemu's BotW Cheats
graphic packs (CC0), in `tools/botw_cheat_patches.py`: the translator keeps the game's instruction and
puts a switch around it (`botw_cheats[]`, `runtime/include/botw_cheats.h`; a byte load and a branch),
so with a cheat off the code runs as before. The 11 functions holding the 11 sites (both durability
switches share one) are kept plain (no crlive/leaflocal), like the FPS sites; regenerating changed 9
code files and nothing else.
Durability is ours, not the pack's (its patch replaces the `mtctr` of the next virtual call): the
weapon class (vtable 100C54AC, method 024AC534) passes its durability loss in f1 at 024ACC60, which
is zeroed by the weapon's type (+0x938: 0 one-handed, 1 two-handed, 2 spear, 3 bow, 4 shield,
5 other, from the profile names at 104FBAAC), so shields have their own switch. Urbosa's count is
"decremented" by 0 so cr0 stays right (the pack's nop leaves it stale). The translator change makes
installs re-translate (`recomp_stamp`, the kit lists the new file). Play-tested by the user
(2026-11-06): works. Not checked on their own: shield-surfing wear (it may take another path) and the
last arrow of a kind (still used up, as with the pack). Frame time / CPU % not measured (11 sites,
none in per-frame code with the cheats off beyond a byte load and a branch).

### 2026-11-06: controllers back at once after the settings menu (upstream #153)
Closing Settings (or answering the text prompt) kept every game input, keyboard included, until every
input of every controller read 0.3 or less (`g_wait_release`): a trigger or stick resting above that
left the game without input until the pad was unplugged, and a button held on one pad held back the
same button on another. Ported upstream WWHD's per-device release gate (003465b4, 7fa8823a;
`overlay/release_gate.h`): `gfx/input.mm` reads each GCController on its own and gives each a gate, so
only the inputs held while the overlay had the input stay from the game, each until it reads 0.5 or
less (the game's press threshold), and everything else reaches it at once; a controller missing from
a poll loses its gate. `overlay::blocks_input()` is `captures()` now, so the gyro no longer waits
either. The overlay's navigation and the Controls window still read the ungated values.
`release_gate_test` (ctest `overlay_release_gate`) passes; play-tested by the user (2026-11-06): works.
The SDL host (`platform/input_sdl.cpp`, Vulkan-only builds, not ours) is not gated: it now has no
release wait at all (upstream's commits have that half).

### 2026-11-06: the European release accepted (untested)
The installer takes the European release (base 00050000101C9500, update 0005000E101C9500, DLC
0005000C101C9500) as well as the USA one: BotW-Native's region.cpp says both v208 updates ship the same
U-King.rpx, the SHA-256 the installer checks, so the translated code is the same. Base game, update and
DLC must be of one release; Japan stays refused. The game takes its region from MCP_GetSysProdSettings'
platform region (byte 3 read at 03C707DC; 030AC9C4 makes 1/2/4 its Japan/USA/Europe) and its language
from UCReadSysConfig `cafe.language`, checks them against `content/System/RegionLangMask.txt` (USA:
`USen USes USfr`, RomType `US`; the update's copy is in `content/Local/System`) and loads
`Pack/Bootup_<USen|EUde|...>.pack`. MCP used to report the translated build's region (USA, always), so a
European dump would have been told it is the USA one; `game_release.cpp` now reads the dump's: the title
ID in meta.xml, else the RomType, the Bootup packs, meta.xml's region bits. MCP gets that region,
`cafe.cntry_reg` the language's country in Europe (110 UK, 78 Germany, 77 France, ...; 49 for the USA as
before), and the mods' title checks (SDCafiine folders, Cemu `titleIds`) the dump's title. The languages
on Language & About come from the update's Bootup packs as before; a new install starts in English. Cemu
saves are also looked for under `101c9500` (the same save files). `tools/run.sh` / `make_app.sh` use the
European folders when there are no USA ones. `game_release_test` covers the decision. **Not run with a
European dump yet**: check the `[config] release:` log line, the text language, the DLC and a Cemu save.

### 2026-11-06: more cheats: weather, no Blood Moon, no random enemy spawns
Settings > Cheats gains an Enemies card and a Weather choice (`cheatNoBloodMoon`, `cheatNoRandomEnemies`,
`cheatWeather` = 0 for the game's own, 1-9 for weather type 0-8; `WWHD_CHEATS=weather=3` takes values).
- Weather (sites of Cemu's Weather mod, 03668FC8/03668FEC in the WeatherMgr update): the current weather
  (+0x18) is compared with the forced one and the forced one stored, so it takes over when the game's
  own transition allows (+0x14 >= 0.99); the game's wanted weather (r26) is left alone. Places with a
  weather of their own keep it.
- No Blood Moon (ours; Cemu's Day Time pack only moves its threshold): TimeMgr's bloody-day check
  0365ECBC (FirstTouchdown and the timer at +0xA8 over 2520.0 = 7 * 360, the only reader of 10301850)
  answers no. handleNewDay (0365F214) then never marks a day WM_BloodyDay, so the midnight Blood Moon
  (Demo011_0 and the BloodyMoon/Schedule event) never comes; the timer keeps counting, so a Blood Moon
  follows within two days of the switch going off. Untouched: the relief Blood Moon the game forces
  to free memory (BloodyMoonRelief, another path) and a day already marked when the switch goes on.
- No random enemy spawns (Cemu's Prevent Random Spawns, "all enemies"): the auto-placement check at
  029FFB3C reports an actor whose name (*(r1 + 0x40)) starts with Enemy_ as placed already.
Regenerating changed code_087 and code_192 only (3 functions). **Not play-tested yet.**

### 2026-11-06: content mods and BCML .bnp mods (Settings > Mods), first part of the mod loader
Mods made for the Wii U version go in `~/Library/Application Support/BotW/Mods`, one folder each: a Cemu
graphic pack (`rules.txt` with `content/` and `aoc/`; its code patches and shaders are not used), an
SDCafiine folder (`<title id>/<mod>/content`), or just `content/` and `aoc/`, also inside a wrapper folder
as unzipped. Switch mods, `.bnp`, `.zip` and code-only packs are listed with the reason they can't be used.
Settings > Mods: on/off, order (higher wins), what each mod changed; changes apply at the next start.
At startup (`mods/mod_loader.cpp`, before guest code) the enabled mods are merged (`mods/content_merge.cpp`,
formats in `mods/botw_formats.cpp`) into `~/Library/Caches/BotW/Mods/<fingerprint of the mods' files>`, and
`hle/fs.cpp` reads `/vol/content` and `/vol/aoc…` files from the result (one map lookup per open):
- a file several mods have: the higher one's. Archives several mods change (packs, actor packs, nested
  ones) are merged file by file, each mod's changes taken against the game's archive (same data
  compressed differently counts as unchanged). Rebuilt archives keep each file's alignment class and the
  Yaz0 header's alignment word, and are compressed again (own LZ encoder, about Nintendo's ratio).
- a loose file the game only has inside packs (all but 7 files of TitleBG.pack and Bootup.pack, which
  the game reads from the packs, and DLC shrine packs) is also put into those packs.
- one mod's archive is used as it is only if it has all of the game's files (also in nested archives it
  changes); a partial one, as BCML's .bnp packs keep only the files they change, goes into the game's copy.
  Checked on the dump: a 2-file TitleBG.pack (one with a 1-file actor pack) merged in 89 ms, all 820 files there.
- the resource size table (`System/Resource/ResourceSizeTable.product.srsizetable`, 67,992 entries; packs
  have none): the mods' own entries (the larger one wins), entries of files that grew grow (fixed overhead:
  rounded size plus the game's overhead; large overhead such as params and models: the growth at the
  entry's ratio to the size, plus a quarter), an entry a mod's own table drops for its file is dropped, and
  the table's own entry follows its size. DLC entries are `Aoc/0010/...` where the game has one.
- saves are copied to `Save backups/<date>` (newest 10) when a start's set of mods differs from the last.
  `BOTW_MODS=0` starts without mods; `BOTW_MODS_DIR` uses another folder (headless runs use none).
- BCML mods: a `.bnp` in the Mods folder is unpacked at startup with `/usr/bin/tar` (bsdtar reads 7z) into
  `~/Library/Caches/BotW/Mods unpacked/<name-size-time>`; an unpacked one (`content/` with `logs/` or
  `info.json`) works the same. Its merge data is applied in the mod's turn (`mods/aamp.cpp` reads and writes
  AAMP v2): `deepmerge.aamp` (values set, new objects and lists added, by name hash, into the file its
  FileTable names, `content/X.sbactorpack//Actor/Y.bphysics`, inside the merged archives), `aslist.aamp`
  (AddReses by Anim, new ones `AddRes_N` after the game's; ASDefines by Name; CFDefines by CFPre Name) and
  `rstb.json` (sizes; 0 drops the entry). Options from `info.json` use BCML's defaults (switches: next entry).
  Mods with other logs (texts, gamedata, ActorInfo, maps, ...) are refused with their names.
  Why: The Linkle Mod 3.0.1 unpacked without its merge data crashed at the title screen (null pointer in
  039742E0+0xA6C, both runs) though its 130 partial actor packs merged complete. With it (checked on the dump,
  `content_merge_test --dump`): 195 parameter files changed (Link's cloth physics count 4 -> 5 plus the
  `Bust_Havok` entry for the new `Link.hkcl`, `Linkle_Extra_Animation` as AddRes_2, 622 of 622 support-bone
  values of Armor_012_Upper), 135 archives merged, all 331 sizes exact and 59 entries dropped, 674 ms at the
  first start; all 440 parameter files of the merged TitleBG.pack read, write and read back the same.
  Then it loaded forever (the game asked for a loose `Actor/ActorLink/GameROMPlayer.bxml`): rstb.json's
  3,071,360 for GameROMPlayer.bactorpack is BCML's own rebuild, ours was 3,080,168 bytes. Now files made
  here (rebuilt archives, changed parameters) get at least their grown entry whatever a mod's sizes say
  (a 0 still drops the entry), and AAMP files are written with equal values and strings stored once, as
  the game's are (Player.baslist 32,484 bytes, the game's 32,444; GameROMPlayer 3,071,976, entry 3,072,408).
  `content_merge_test --dump` now fails on any changed file larger than its entry: 0 of them, 133 grown,
  1,539 parameter files round-trip.
Not done: merges inside files of other mods (ActorInfo, gamedata flags, texts: the higher mod's whole file),
Cemu code patches through the translator, mod-only files in directory listings. Measured with sample
mods made from the dump (scratchpad only, synthetic edits): TitleBG.pack merged from 3 mods plus a DLC
shrine pack in 221 ms, 347 MB peak at startup (freed before the game starts); the next start takes the
cache in 1 ms. `content_merge_test` (ctest `content_merge`) passes. Play-tested by the user with The Linkle
Mod 3.0.1 (2026-11-06): it works.

### 2026-11-06: BCML options in Settings > Mods
A BCML mod's options (`info.json`) are listed under it: a switch for each `multi` option and a row of choices
for each `single` group, with None unless the group is required; greyed while the mod is off. Choices are
saved in `mods.json` per mod and option folder (`"options": {"Royal Weapons": true}`, a group's choice as all
of its options) and apply at the next start. Options not chosen take BCML's default: a `multi` option's
`default`; in a group the option marked `default`, else its first when the group is required, else none. BCML
has no defaults for groups and asks the player instead (`Selects.jsx`). A chosen option is merged over its mod
as an input of its own (`mod/folder`), so its files win over the mod's, archives they share merge file by
file, its logs apply after the mod's (as BCML's install and mergers do), and each choice of options is a cache
set of its own. BCML's defaults give the same fingerprint as before (no re-merge after the update). Checked on
the dump with a copy of the Mods folder (`content_merge_test --dump`), The Linkle Mod 3.0.1: Royal Guards
Armoury: 28 of 28 files used, 7 parameter files changed (every file its `deepmerge.aamp` names), 142 archives
merged, 664 ms; with the Umbrella Glider as well, its partial TitleBG.pack merges over the mod's (its
`Player_Animation.sbfres` wins), 822 files read back, 812 ms, 265 MB peak; no size entry smaller than its
file. `content_merge_test` passes (switches, groups, saved choices, a start with other options merging a new
set). Play-tested by the user (2026-11-06) with Royal Guards Armoury and the Umbrella Glider on (merged in
837 ms at that start): it works.
Restart now (Settings > Mods, under the restart note; `hostui::restart_game`): while a game is in progress it
asks as Quit does ("Restart Breath of the Wild?", `gfx/quit_prompt.mm`), then `render::restart()` does the
quit's teardown (`render::shutdown`: audio dump, screenshots, renderer caches; settings, `mods.json` and saves
are written when they change) and `execv`s the same executable with the same arguments (the app's
`--game/--base/--save/--aoc`, same log). If that fails it says so and quits. macOS only (the SDL host has no
prompt for it). Play-tested by the user (2026-11-06): twice, once asked in gameplay and once without asking
before it; the game came back each time and played on: it works.

### 2026-11-06: code mods: Cemu graphic packs' code patches in the game code (Settings > Mods)
A Cemu graphic pack with `patch_*.asm` (or a Cemuhook `patches.txt`) in the Mods folder is a code mod. Its groups
for v208 (`moduleMatches = 0x6267BFD0`, or `rpx`) are read when the game code is translated, the way Cemu reads
them (`tools/cemu_patches.py`, after Cemu's GraphicPack2PatchesParser/Apply, ExpressionParser and PPCAssembler):
groups, `.origin = codecave`, labels that stick to the next item after its alignment, `name = expression`
variables, `<address> = ...` lines, `.float/.int/.byte/.string/.align` data, Cemu's expressions (a leading 0
without a decimal point anywhere after it is hex, one @ha/@l that applies to the whole value, division by 0 is
0, comparisons) and a PowerPC assembler for what the BotW packs use (97 encodings checked against known words,
the game's own from the FPS++ and cheat site tables among them). `tools/botw_code_mods.py` builds them in
(called from `tools/probe_recomp.py`):
- an instruction a pack changes keeps the game's own, behind the mod's switch (`botw_patch_on`,
  `runtime/include/botw_patches.h`), as the cheats do; the functions with such sites stay in plain form.
- code caves become functions of their own in `build/gen/code_patches.c`, at 0x01800000 and up (below the
  game's code, so `bla` reaches them), 256-aligned one after another; calls into the game, to its imports
  (`bl import.coreinit.OSReport`) and between cave functions are direct C calls. The runtime writes the caves'
  bytes at startup and adds their functions to the dispatcher (`runtime/src/mods/code_patches.cpp`).
- what depends on presets the runtime computes from the chosen presets, at startup and when one changes: cave
  data (`.float $timeMultiplier`), the game data a pack changes (written while the mod is on, the game's bytes
  put back when it is switched off) and 16-bit immediates (`li r12, $dayTimeEnum` reads `botw_patch_value(slot)`;
  its place in ppc2c's C is found by translating with two sentinel immediates). The translator compiles those
  expressions to postfix programs; `mods/code_patch_rules.cpp` runs them with Cemu's arithmetic and chooses
  presets by Cemu's rules (one active preset per category, the saved choice or the default; a preset whose
  condition fails is hidden; values come from active visible presets, then active hidden ones, then
  [Default]). Presets are saved in `code_mods.json` next to the Mods folder, the switch in `mods.json`.
- `code_patches.c` holds a manifest of the packs (JSON), each with a SHA-256 of its `rules.txt` and patch
  files. A pack in the Mods folder that the game code doesn't hold, or holds in another version, is shown as
  needing a rebuild: Settings > Mods > Rebuild the game code runs, in a source checkout,
  `botw_setup.py recomp` into the build's GEN_DIR and `cmake --build`; in the installed app,
  `botw_setup.py install --keep-build` (a kept build is now translated in place, so only the changed files
  compile again). Then Restart now. The installer's translation stamp includes the code mods' fingerprints.
- refused, with the reason in Settings > Mods: packs that change game code the port changes itself (FPS++,
  Weather, the cheat packs; the Durability pack's site too, Settings > Cheats has it without the pack's bug),
  a second mod at the same instruction (by name), jumps into the middle of a game function (FPS++-style
  branches back), a code word computed from presets (Master Mode Options' `.int` trick), `[RAM]` (Extended
  Memory), shaders, `[TextureRedefine]` sizes. Draw Distance's LOD bias rule is noted as not used.
Checked: all 26 BotW v208 code packs of Cemu's graphic packs parse. With 16 of them in a test Mods folder,
9 are built in (Day Time, Camera Sensitivity, Draw Distance, Xbox Controller Button Layout, Menu Navigation
Speed, Arrow Draw Speed, 9999 Map Stamps Limit, LWZX Crash, NPC Stuttering Fix: 14 cave functions, 27
instructions in 14 functions) and 7 refused as above; the 11 changed code files and `code_patches.c` compile.
With no code mods the translation is byte-identical to before (`diff -rq` of all 310 files of build/gen, the
empty `code_patches.c` included). Rebuilding the dev checkout for Day Time and Camera Sensitivity: translation
1:47, build 5 s (3 game code files, `code_patches.c`, link). Tests: `tools/test_cemu_patches.py` (ctest
`cemu_patches`), ctest `code_patch_rules` (the runtime's programs, presets and fingerprints match the
translator's) and `code_patches` (startup, presets at once, the game's data back when switched off, which
packs need a rebuild, on a hand-written manifest), `content_merge`, `tools/test_botw_translator_package.py`.
Not done: Master Mode Options (code words from presets), Menu Navigation Speed has no effect (it sets values
only Cemu's FPS++ pack reads), packs with only data patches still need the rebuild. Play-tested by the user
(2026-11-06) with Day Time and Camera Sensitivity from the Mods folder, switched and set in Settings > Mods:
they work. Frame time and CPU % not measured (the runtime does no per-frame work for code mods; built in, a
patched function loses the register passes and reads its mod's switch byte at each changed instruction).
The Rebuild button also covers an app `tools/make_app.sh` made (it runs make_app.sh again after the build).

### 2026-11-07: BCML merge data for events, flags, save data and texts (layer 2 of the mod loader, part 1)
A `.bnp`'s `eventinfo.yml`, `gamedata.yml`, `savedata.yml` and `texts.json` are applied (before, the mod was refused).
New formats: `mods/byml.{h,cpp}` (BYML v2-4, either byte order, and the YAML text oead writes, which BCML's logs use:
`!u`/`!l`/`!ul`/`!f64`, floats with a '.', s32 otherwise), `mods/msbt.{h,cpp}` (MSBT files and msyt's JSON form of
texts, all of its control kinds). The mergers (`mods/bcml_merge.{h,cpp}`) follow BCML (bcml/mergers data.py, events.py,
texts.py and its Rust texts.rs), with UKMM's uk-content as a second reference and msyt/msbt-rs (MIT) for the texts'
control sequences; our own code, as a specification only:
- events (`Event/EventInfo.product.sbyml` in Bootup.pack): whole entries added or replaced.
- flags (`GameData/gamedata.ssarc`): per type added or replaced by DataName, then deleted; diffs combine as BCML's
  dict_merge (a later mod's deletion list replaces an earlier one's). The game's files stay as they are unless a flag
  in them changes; a new flag goes to the last file of its kind (stage flags such as MainField_... to the revival files,
  UKMM's rule), and a new file when that one has 4096.
- save data (`GameData/savedataformat.ssarc`): game_data.sav's flags, the highest mod's per hash, deletions of any,
  sorted by signed hash, 8192 per file, caption.sav and option.sav renumbered after them.
- texts (`Message/Msg_<lang>.product.ssarc` in `Pack/Bootup_<lang>.pack`): per MSBT file and label, for every language
  of the dump, the mod's texts chosen as BCML maps languages (the same, another region's, English, any). Texts the
  merge doesn't change stay as raw bytes; the game's attribute strings are kept when an entry has none.
These files are rebuilt from the game's copies after the archive merge and replace any mod's copy (as BCML's Bootup
injection). A mod with its own copy and no log for it (a plain content mod) has its changes against the game's copy
merged entry by entry, and a loose copy is no longer read instead of the rebuilt one. `kCacheFormat` 4 (one re-merge).
Checked on the dump: all 1091 MSBT files of USen, USes and USfr write back byte for byte and all 29,730 texts decode and
encode back the same; every BYML in Bootup.pack and TitleBG.pack writes back at the game's size (only the order of a
hash's child nodes differs). Gerudo Hero 1.0.0 (gamebanana 733520) alone: 580 ms at the first start, 371 MB peak, 3 ms
from the cache; 8 events, 5 flags, 4 save data flags and 71 texts (x3 languages) in the merged files; the merge changes 11
files in Bootup.pack, 27 in each language pack and 2 in TitleBG.pack; no size entry smaller than its file. With The Linkle
Mod 3.0.1 and both options too: 1.16-1.39 s, about 400 MB peak, 146 archives (both change TitleBG's
SignalFlowchart.bfevfl: the higher one wins). The peak counts freed memory: on macOS 27 freed medium and large blocks
stay resident (malloc's in-use bytes drop to about 1 MB between archives, the footprint doesn't, and
`malloc_zone_pressure_relief` releases nothing); a window-sized Yaz0 match table saved only 3 MB and was left out.
`content_merge_test` covers BYML and its YAML, MSBT and every control kind, the four merges (logs, a plain mod's copies in
a pack and loose, two languages, flags and save data overflowing into new files) and `--dump` now checks rebuilt data
tables and texts and that every logged change is in the merged files. Play-tested by the user (2026-11-07) with Gerudo
Hero: Gerudo Town lets Link in without the vai outfit and the NPCs' voe lines show. Seven lines its flows use are missing
from the mod's own texts.json (BCML misses them too). Switching Linkle off worked (Link only looked like Linkle in an
outfit). Still refused by name: actorinfo.yml, areadata.yml, drops.json, dstatic.yml, effects.yml, mainstatic.yml,
map.yml, quests.yml, residents.yml, shop.aamp. No per-frame work; frame time and CPU % not measured.

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

### 2026-10-27: GX2 register classes, Metal narrow shader keys (ported from ZeldaWWHDRecomp)
- Register writes (upstream bde7fb1): one class byte and Metal key mask per register (`gx2/register_class.h`)
  instead of range walks per written word, an inline path for ALU constants, display-list recording slots
  found by `pthread_self` instead of `thread_local`, shader-bind and uniform temporaries on the stack.
  `register_class_test` checks the tables against the reference functions.
- Shader keys (upstream ba509f2, `gfx/shader_linkage.h`): linkage + variant key, with units and VS exports
  from the program's first translation. Pixel-shader keys no longer hold the VS export table, render-target
  formats the shader doesn't write or read, or sampler depth-compare bits. Framebuffer fetch is keyed
  exactly (address, format, tile mode, active targets), and graphic packs add what their names hash.
  Recipes name shaders by the narrow key; replay also maps the old wide key, and compaction renames
  pipeline recipes, so old caches and the head start still resolve.
- Found: the old key hashed texture word 1 bits 20-25 (DEPTH) as the "format"; the data format is 26-31.
- Switches: `WWHD_MTL_SHADER_KEY_VERIFY=1` (retranslate per new wide key, log `[gfx shader key] VIOLATION`),
  `WWHD_MTL_WIDE_SHADER_KEYS=1` (old keys). Not measured yet (the user skipped it):
  `measure_keys.sh` in the side-session notes folder compares before/after binaries from empty caches.
- First run with an old cache compacts it; the compacted file names pipelines by narrow keys, which an
  older build drops (it would rebuild those pipelines on demand).

### 2026-10-30: sampler LOD bias applied, cube map index fix (from upstream WWHD)

Metal samplers ignore the GX2 LOD bias; BotW sets -1 level on its textures, so everything sampled a
mip blurrier than on the Wii U. The translated shaders now add it (per-draw `texNLodBias` in the
support buffer, gfx/sampler_lod_bias.h); shaders a graphic pack replaces are translated without it.
SET_CUBEMAP_INDEX on a non-cube unit now declares its index. Hateno 1.5x, 60 fps mode, in-run A/B:
16.70 ms/frame, 297 % CPU with the bias vs 16.99 ms/frame, 327 % without; 0 shader failures,
graphic packs replaced as before. Switches: WWHD_LOD_BIAS=off|ab, WWHD_LOD_BIAS_LOG=1.

## Recompiler code-quality passes (2026-10-27)

Ported from upstream ZeldaWWHDRecomp (4b5e2a0, 79ae761, e658359, 4af135f, 2e6c6d1, f0e6fcb; our
tools/recomp and ppc.h were still byte-identical to the fork point, so they applied cleanly):
guest registers in C locals (leaflocal.py: all 203,444 functions, 125,346 leaf + 78,098 with calls),
condition-register liveness (crlive.py: 703,049 CR writers), cheaper fcmp/mfcr, paired-single fast
path, static GQR0/1 (BotW writes only GQR2-5; 210,375 of its 211,416 paired loads/stores use GQR0),
and recomp.py writes only changed files (regenerating identical code rebuilds nothing). BotW has no
mffs/mcrfs/mtfsf at all, so dropping fcmp's FPSCR FPCC update is safe. probe_recomp.py turns the
hook files off, so no function needs the plain form; save states only snapshot threads parked in HLE
calls, after which the non-leaf form reloads every register.
Game code __text 210.3 MB -> 139.9 MB. Switches when generating: WWHD_RECOMP_PLAIN=1 (all off,
output byte-identical to before), WWHD_RECOMP_CRLIVE/LEAF/NONLEAF/GQR=0 each; checking build:
WWHD_RECOMP_LEAF_POISON=1 WWHD_RECOMP_CR_CHECK=1 (aborts with "CR liveness: bit N read at ...").
