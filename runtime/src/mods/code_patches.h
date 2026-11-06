// Code mods (Settings > Mods): Cemu graphic packs' code patches (patch_*.asm), built into the game code
// when it is translated (tools/botw_code_mods.py: the patched instructions behind each pack's switch, the
// code caves and a manifest of the packs in code_patches.c).
//
// At startup the caves' bytes go to guest memory (0x01800000 up) and their functions to the dispatcher;
// each pack's presets (the saved choices, else its defaults) give its cave data and the immediates its
// instructions read; an enabled pack is switched on: its game data is written (the game's own kept) and
// its switch set. Switching a pack on or off, or choosing other presets, applies at once; the presets are
// saved in code_mods.json next to the Mods folder, the switch with the content mods (mods::loader).
//
// A pack the game code doesn't hold, or holds in another version (its rules.txt or patch files changed),
// needs the game code rebuilt: rebuild_start() translates and builds it again (a source checkout:
// botw_setup.py recomp into the build's GEN_DIR, then cmake --build; the installed app: botw_setup.py
// install --keep-build), into the same executable, so a restart (hostui::restart_game) starts the rebuilt game.
//   BOTW_MODS=0, headless runs without BOTW_MODS_DIR: no code mod is switched on (as the content mods)
#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mods::patches {

struct Category {
    std::string name;
    std::vector<std::string> presets;  // the ones shown (visible)
    int active = -1;                   // in presets
};
enum class Build {
    Current,  // the game code holds this version
    Changed,  // the game code holds another version of it: its files changed since
    Missing,  // the game code doesn't hold it yet
    Refused,  // the translation left it out (problem says why)
};
struct View {
    std::string id;  // its folder's name in the Mods folder
    std::string name, description;
    Build build = Build::Missing;
    std::string problem;
    std::vector<std::string> notes;
    bool on = false;                   // switched on now
    std::vector<Category> categories;  // the categories with presets shown
    size_t changes = 0;                // game instructions it changes
};

// main thread, after the game's code is loaded and dispatch::init, before the game runs. enabled(id): the
// saved choice of mods::loader
void startup(int argc, char** argv, const std::filesystem::path& mods_dir,
             const std::function<bool(const std::string&)>& enabled);
std::vector<View> list();  // the Mods folder's code mods (and the refused ones), by id
bool find(const std::string& id, View& out);
bool is_code_mod(const std::string& id);  // the Mods folder's `id` has code patches
void set_on(const std::string& id, bool on);  // at once (on: Current only)
void set_preset(const std::string& id, const std::string& category, const std::string& preset);
void rescan();  // the Mods folder again (new or changed packs)

bool rebuild_needed();  // a code mod in the Mods folder is Missing or Changed
struct RebuildState {
    bool possible = false;  // this game can rebuild itself (how: the reason if not)
    bool running = false, done = false;
    double progress = 0;  // 0-1
    std::string how, stage, error;
};
RebuildState rebuild_state();
void rebuild_start();
void rebuild_cancel();

}  // namespace mods::patches
