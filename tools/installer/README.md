# BOTWRecomp: Breath of the Wild for Mac

This DMG holds no game files. **BOTWRecomp Installer** builds *Breath of the Wild.app* on your Mac
from your own Wii U dump. Nothing is downloaded, and nothing from your dump leaves your Mac.

## You need

- An Apple silicon Mac, macOS 13 or newer, about 3 GB free (about 15 GB more if you copy the game files).
- Apple's **Command Line Tools**. The installer offers to install them (or run `xcode-select --install`).
  Xcode is not needed.
- Your dump of the **USA** release, extracted (folders with `code`, `content` and `meta`):

  | | title ID |
  |---|---|
  | base game | `00050000101C9400` |
  | update **v208** (1.5.0) | `0005000E101C9400` |
  | DLC (optional) | `0005000C101C9400` |

  Games installed in Cemu work as they are (`mlc01/usr/title/…/101c9400`). Extract `.wud`, `.wux` and
  `.wua` files first. The **European** release (`…101C9500`) installs too but hasn't been play-tested.
  The Japanese release is not supported.

## Install or update

1. Double-click **BOTWRecomp Installer**. The first time, macOS blocks it because it isn't notarized:
   click Done, then go to System Settings > Privacy & Security and click **Open Anyway**.
2. Click **Find in a Folder…** and choose the folder with your dump. A green check next to each part
   means it's right; otherwise the row says what's wrong.
3. Optionally pick a save to import, such as Cemu's
   `mlc01/usr/save/00050000/101c9400/user/80000001`. Your current save is backed up first.
4. Click **Build Breath of the Wild** (5–10 minutes), then **Open Game**.

To update, run the new installer the same way. Your saves, settings and mods are kept.

## In the game

| | |
|---|---|
| Settings | Cmd+, |
| Home menu (Resume, Settings, Quit; pauses the game) | H, or Home on a controller |
| Full screen | Cmd+F |
| Aspect ratio (16:10, 21:9, 32:9, …) | Settings > Display, then **Restart now** |
| Mods | put them in `~/Library/Application Support/BotW/Mods`, switch them on in Settings > Mods |
| Cheats | Settings > Cheats |

Mods can be BCML `.bnp`, `.zip`, `.7z` or `.rar` files as downloaded, or folders (Cemu graphic
packs, SDCafiine folders, `content`/`aoc` folders). Switch versions of mods don't work with the Wii U
game.

## Where things are

All in `~/Library`:

| | |
|---|---|
| saves | `Application Support/BotW/save` |
| settings, mods, screenshots | `Application Support/BotW` |
| shader cache | `Caches/BotW` |
| logs and crash reports | `Logs/BotW` |

The app goes in `~/Applications` unless you choose another folder. To uninstall, delete the app
and the `BotW` folders above.

## Problems

- **A build step fails:** click **Copy Log** and paste it into a GitHub issue. The log has file
  paths but no game data.
- **"Game files missing" at launch:** the drive or folder with your dump moved. Reconnect it, or run
  the installer again. Tick *Copy the game files to this Mac* if you want the app to work without it.
- **Stutter the first time an effect appears:** its shaders are compiling. They're cached, so it
  won't happen again.

Bug reports: https://github.com/vasist15-source/BOTWRecomp/issues (include your Mac model).

## From source

From a checkout, double-click **Install Breath of the Wild.command**, which also needs CMake
(`brew install cmake ninja`). `tools/installer/botw_setup.py --help` lists the Terminal options.
`tools/release/botw_dmg.py VERSION` builds a release DMG: a prebuilt runtime kit, allow-list and
symbol checks so that nothing game-derived ships.
