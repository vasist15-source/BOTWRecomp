// Cheats (Settings > Cheats): switches the translated game reads at the sites of
// tools/botw_cheat_patches.py (ids in runtime/include/botw_cheats.h). All off by default; a change
// applies at once and is remembered in the host settings ("cheat" + id, e.g. cheatArrows).
// Start-up switch: WWHD_CHEATS=arrows,shields,... (ids below, or "all") decides for the run instead
// of the saved choices; changes made in the menu then are not saved.
#pragma once
#include "botw_cheats.h"

namespace mods::cheats {
struct Info {
    const char* id;    // WWHD_CHEATS name; the settings key is "cheat" + id with a capital
    const char* name;  // menu label
    const char* help;  // menu tooltip
};
const Info& info(int cheat);  // cheat: BOTW_CHEAT_*
bool on(int cheat);
void set(int cheat, bool on);  // from the overlay (render thread); the save is posted to the main thread
bool from_env();               // WWHD_CHEATS decides this run
void load_saved();             // overlay start-up: the saved choices (unless WWHD_CHEATS decides)
}  // namespace mods::cheats
