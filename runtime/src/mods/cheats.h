// Cheats (Settings > Cheats): switches the translated game reads at the sites of
// tools/botw_cheat_patches.py (ids in runtime/include/botw_cheats.h). All off by default; a change
// applies at once and is remembered in the host settings ("cheat" + id, e.g. cheatArrows = 1; the
// weather as its number).
// Start-up switch: WWHD_CHEATS=arrows,shields,weather=3 (ids below; "all" turns on every switch)
// decides for the run instead of the saved choices; changes made in the menu then are not saved.
#pragma once
#include <cstdint>

#include "botw_cheats.h"

namespace mods::cheats {
struct Info {
    const char* id;    // WWHD_CHEATS name; the settings key is "cheat" + id with a capital
    const char* name;  // menu label
    const char* help;  // menu tooltip
    uint8_t max = 1;   // highest value (1: an on/off switch)
};
extern const char* const kWeatherNames[10];  // BOTW_CHEAT_WEATHER: 0 the game's own, then types 0-8
const Info& info(int cheat);  // cheat: BOTW_CHEAT_*
bool on(int cheat);
uint8_t value(int cheat);
void set(int cheat, bool on);
void set_value(int cheat, uint8_t v);  // from the overlay (render thread); the save is posted to the main thread
bool from_env();                       // WWHD_CHEATS decides this run
void load_saved();                     // overlay start-up: the saved choices (unless WWHD_CHEATS decides)
}  // namespace mods::cheats
