# BOTWRecomp: Breath of the Wild, native for Apple Silicon Macs

A native macOS build of *The Legend of Zelda: Breath of the Wild* (Wii U, update v208 / game
version 1.5.0), made by static recompilation: the game's own PowerPC code is translated to C ahead of
time and compiled for Apple Silicon, Cafe OS is reimplemented natively, and GX2 graphics go straight
to Metal. There is no emulator in between.

You need your own copy of the game. This repository contains no game code, executables, artwork,
sound or video. The installer takes the files from your dump and builds the app on your Mac. Nothing
it builds is meant to be shared.

This project is not affiliated with or endorsed by Nintendo. The Legend of Zelda and Breath of the
Wild are trademarks of Nintendo.

**Status: early, but playable.** It boots, plays the intro and the Great Plateau, with menus, NPCs,
sound, movie cutscenes, saving and the DLC, at the game's own 30 fps on an M4 MacBook Air. Expect
rough edges: stutter the first time new effects are drawn (shaders are compiled then, and cached),
and some lighting glitches.

## Play it on your Mac

1. Have your own dump of the game (see below).
2. Download **BOTWRecomp-&lt;version&gt;.dmg** from the [latest release](../../releases/latest), open it
   and double-click **BOTWRecomp Installer**. The app is not notarized: the first time, open it,
   click Done, then System Settings → Privacy & Security → **Open Anyway**.
3. Click **Find in a Folder…** and choose the folder with your dump (or pick the base game, update
   and DLC one by one; each one is checked). Optionally import a save, for example from Cemu.
4. Click **Build Breath of the Wild**. It takes about 5 minutes; then the game is in
   `~/Applications` (or wherever you chose), and **Open Game** starts it.

The installer holds no game files: it builds the game on your Mac from your copy. Details and
troubleshooting: [tools/installer/README.md](tools/installer/README.md).

## What works

- Native Apple Silicon app, signed on your Mac, with the icon from your dump; saves, settings and
  screenshots in `~/Library/Application Support/BotW` like any Mac game, logs in `~/Library/Logs/BotW`.
- Metal renderer (GX2 → Metal, shaders translated with Cemu's Latte decompiler) with a shader cache,
  so a second visit doesn't stutter.
- Sound, movie cutscenes (H.264 on VideoToolbox), saving, the DLC (The Master Trials and The
  Champions' Ballad).
- Controllers through macOS GameController (Xbox, PlayStation, Switch Pro and Joy-Con, with Nintendo
  buttons by position), keyboard and mouse.
- Settings overlay: graphics, audio (master volume), controls.
- Save import from Cemu (`mlc01/usr/save/00050000/101c9400/user/80000001`).

## What you need

- An Apple Silicon Mac with macOS 13 or newer and about 3 GB free.
- Apple's Command Line Tools (`xcode-select --install`; the installer offers it).
- Your dump of the USA release, extracted (folders with `code`, `content` and `meta`):
  base game `00050000101C9400`, update v208 `0005000E101C9400` (its `U-King.rpx` has SHA-256
  `ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30`; the installer checks it) and
  optionally the DLC `0005000C101C9400`. Titles installed in Cemu (`mlc01/usr/title/...`) work as they
  are. Other regions are not supported yet.

## Build it by hand (for development)

```
brew install cmake ninja
DUMP="$HOME/path/to/dump/The Legend of Zelda Breath of the Wild"
python3 tools/probe_recomp.py "$DUMP [Update] [0005000e101c9400]/code/U-King.rpx" build/gen
cmake -S . -B build/rt -G Ninja -DCMAKE_BUILD_TYPE=Release -DWWHD_RENDERER=METAL
cmake --build build/rt --target wwhd
tools/run.sh                  # runs build/rt/wwhd on the dump (BOTW_DUMP= sets its folder)
tools/make_app.sh ~/Desktop   # bundles that build as Breath of the Wild.app
```

`build/` is generated from your dump and git-ignored; never commit it. A release DMG comes from
`tools/release/botw_dmg.py VERSION`.

## How it works

- `tools/recomp/`: the PowerPC → C translator of ZeldaWWHDRecomp, run unchanged on `U-King.rpx`
  (203,444 functions, about 690 MB of C).
- `runtime/`: the ZeldaWWHDRecomp runtime with this port's changes: Cafe OS (threads, scheduler,
  file system, saves, DLC), GX2 → Metal, audio (AX), input, H.264, the settings overlay.
- `tools/installer/`: `botw_setup.py` (checks, translation, build, bundle) and the SwiftUI installer
  app in `macos/`.
- [PLAN.md](PLAN.md): the bring-up notes, problems found and their fixes.

## Credits

- [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp): translator, runtime and
  approach this port is built on (MPL-2.0)
- [Cemu](https://github.com/cemu-project/Cemu): Latte shader decompiler and GPU address library,
  vendored in `runtime/third_party/cemu` (MPL-2.0)
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT), [fmt](https://github.com/fmtlib/fmt) (MIT),
  [metal-cpp](https://developer.apple.com/metal/cpp/) (Apache-2.0), [stb](https://github.com/nothings/stb)
  (MIT / public domain), [xxHash](https://github.com/Cyan4973/xxHash) (BSD-2-Clause)
- The installer follows the setup app of the
  [Need for Speed: Carbon Mac port](https://github.com/vasist15-source/nfs-carbon-mac).

Licence: MPL-2.0 (see [LICENSE](LICENSE)); the third-party licences are next to their code in
`runtime/third_party/`.
