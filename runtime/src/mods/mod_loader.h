// Content mods (Settings > Mods): the Mods folder holds one folder per mod, a Cemu graphic pack
// (rules.txt with content/ and aoc/), an SDCafiine folder or just content/ and aoc/, made for the Wii U
// game, or a BCML mod: a .bnp file (unpacked at startup with the system's tar into the caches) or an
// unpacked one, whose merge data in logs/ is applied with its files. At startup the enabled ones are
// merged in the list's order (content_merge.h; cached per set of mods) and the game's reads of content and
// DLC files go to the result (hle/fs.cpp). Changes apply at the next start.
//   BOTW_MODS=0          no content mods this run (nothing saved)
//   BOTW_MODS_DIR=path   another Mods folder (tests); headless runs (WWHD_NO_HOST_INPUT) use none without it
#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace mods::loader {

struct Layout {  // what a mod folder holds
    std::string kind;  // "Cemu graphic pack", "SDCafiine folder", "Content folder", "BCML mod", "BCML mod (.bnp)"
    std::string name, description;
    std::filesystem::path content, aoc;  // empty when absent
    std::filesystem::path logs;          // a BCML mod's merge data (deepmerge.aamp, aslist.aamp, rstb.json)
    struct Option {                      // a BCML mod's optional part (options/<folder>: content, aoc, logs)
        std::string name;
        std::filesystem::path folder;
        bool on = false;  // BCML's default
    };
    std::vector<Option> options;
    std::string problem;  // why it can't be used ("": it can)
    std::string warning;  // parts of it that are not used
};
// what `folder` (or a file in the Mods folder) holds, for the game with title ID `title`
Layout detect(const std::filesystem::path& folder, const std::string& title);

struct ModView {
    std::string id;  // its name in the Mods folder
    std::string name, kind, description;
    bool enabled = false;  // the saved choice (next start)
    bool active = false;   // part of this run
    std::string problem, warning;
    size_t files = 0, used = 0;  // this run: files it has, files the game reads from it
    std::vector<std::string> notes;
};

void startup();  // main thread, before the game reads files
// the host file the game reads for a guest path (/vol/content/..., /vol/aoc<title id>/..., or a path
// relative to /vol/content), or "" for the game's own
std::string replacement(const std::string& guest);

// Settings > Mods (any thread)
std::vector<ModView> list();  // in priority order: the first one wins
std::string directory();
std::string summary();  // this run's result, or why mods are off
bool restart_needed();  // the saved choices differ from this run's
void set_enabled(const std::string& id, bool on);
void move(const std::string& id, int delta);  // -1: up (wins over more mods)
void rescan();

}  // namespace mods::loader
