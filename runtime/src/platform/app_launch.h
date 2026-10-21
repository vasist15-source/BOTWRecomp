#pragma once
// macOS: started as the app bundle's executable (Breath of the Wild.app, from Finder or the Dock) with no
// --game argument, wwhd does what the shell launcher used to: finds the game files, sends its output to
// ~/Library/Logs/BotW/wwhd.log (the previous run's to wwhd.previous.log) and adds --game/--base/--aoc/--save.
// The launcher had to go: macOS (RunningBoard) never saw the window of a process a script had exec'd as
// visible, so Game Mode never started. Where the files are:
//   Info.plist BotWLaunch {game, base, aoc, save, dir}   tools/make_app.sh (a dev checkout; dir = its cwd)
//   ~/Library/Application Support/BotW/install.plist {update, base, dlc, save}   the installer
// A missing file or folder shows an alert and exits. Anywhere else (run.sh, a terminal) it does nothing.
void app_bundle_launch(int& argc, char**& argv);
