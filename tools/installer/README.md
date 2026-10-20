# Installing Breath of the Wild for Mac

This port contains no game files and no game code. The installer builds the game on your Mac from
**your own** Wii U dump of *The Legend of Zelda: Breath of the Wild* (USA): it translates the
game's code to C, compiles it with Apple's compiler and puts the result in a normal Mac app,
**Breath of the Wild.app**. Nothing is downloaded and nothing from your dump leaves your Mac.

## What you need

- A Mac with Apple silicon and macOS 13 or newer, about 3 GB of free space (plus about 15 GB if
  you let the installer copy the game files).
- Apple's free **Command Line Tools**. The installer offers to install them (or run
  `xcode-select --install`).
- **CMake**: `brew install cmake ninja` with [Homebrew](https://brew.sh), or the CMake app from
  [cmake.org](https://cmake.org/download/) (Ninja is optional; it makes the build a little faster).
- Your dump, extracted (folders with `code`, `content` and `meta` in them):
  - the **base game**, title `00050000101C9400`
  - the **update v208** (game version 1.5.0), title `0005000E101C9400`
  - optionally the **DLC** (v80: The Master Trials and The Champions' Ballad), title `0005000C101C9400`

  Dump them from your own console (for example with Dumpling). Games installed in Cemu work as
  they are: they are in `mlc01/usr/title/00050000/101c9400`, `.../0005000e/101c9400` and
  `.../0005000c/101c9400`. Disc images (`.wud`, `.wux`) and Cemu archives (`.wua`) have to be
  extracted first.

## Install

1. Double-click **Install Breath of the Wild.command** in this folder (the first time: right-click >
   Open). It builds the installer app (a few seconds) and opens it.
2. **Your game files**: click *Find in a Folder…* and choose the folder that holds your dump, or
   choose (or drag in) the base game, update and DLC folders one by one. Each one is checked: a
   green check means it is right; otherwise the row says what is wrong (another region, an older
   update, encrypted files, a missing folder, …).
3. **Save** (optional): choose a save to bring along, for example Cemu's
   `mlc01/usr/save/00050000/101c9400/user/80000001`.
4. **Where to put the game**: `~/Applications` by default; `/Applications` or any other folder if
   you prefer. *Copy the game files to this Mac* makes the app independent of where your dump is
   now (an external drive, say); otherwise the app reads them where they are.
5. Click **Build Breath of the Wild**. It takes about 5–10 minutes; the window shows each step, the
   time left and, under *Details*, the full log. When it is done, click **Open Game**.

Prefer Terminal? `tools/installer/install-macos.command --terminal install --base … --update …
[--dlc …] [--save …] [--dest …]` does the same (`--help` lists the options).

## Where things are

| what | where |
|---|---|
| the game | `Breath of the Wild.app` where you chose (`~/Applications` by default) |
| which game files the app uses | `~/Library/Application Support/BotW/install.plist` |
| saves | `~/Library/Application Support/BotW/save` (an imported save replaces it; the old one is kept as `save.backup-<date>`) |
| copied game files (if chosen) | `~/Library/Application Support/BotW/game` |
| settings, controls, screenshots | `~/Library/Application Support/BotW` |
| shader cache | `~/Library/Caches/BotW` |
| build files (if kept) | `~/Library/Caches/BotW/installer` |
| game log, crash reports, installer log | `~/Library/Logs/BotW` (`wwhd.log`, `captures/`, `installer.log`) |

Moved your dump? Open the installer again and choose the new place; it builds again (faster with
the build files kept). To uninstall, delete the app and the `BotW` folders above.

## Problems

- *"… can't be opened"* for the `.command`: right-click it and choose Open.
- A step fails: the window shows the error and the last lines of the log; **Copy Log** puts the whole
  log (`~/Library/Logs/BotW/installer.log`) on the clipboard for a bug report. It contains file
  paths, but no game data.
- The game doesn't start and says its files are missing: the folder or drive with the dump is not
  there; connect it or run the installer again.

## For developers

`tools/installer/botw_setup.py` does all the work (standard library only, the Command Line Tools'
Python 3.9): `scan`, `check`, `requirements`, `install` (`--json` for the installer app's event
stream). It translates with the same code as `tools/probe_recomp.py` (identical output to the dev
checkout's `build/gen`), configures CMake with `GEN_DIR` in the cache folder (Release, Metal) and
builds only the `wwhd` target. `tools/installer/macos/BotWInstaller.swift` is the SwiftUI front end,
built by `tools/installer/macos/build.sh [DEST]`. `tools/make_app.sh` stays the dev bundle (the dev
checkout's build, dump and `build/save`). The multi-platform setup inherited from the Wind Waker HD
port (`setup.py`, `gui/`, the Windows and Linux scripts) is not adapted to BotW yet; its notes are in
[README-wwhd.md](README-wwhd.md).
