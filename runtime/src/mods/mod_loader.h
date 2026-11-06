// Content mods (Settings > Mods): the Mods folder holds one folder per mod, a Cemu graphic pack
// (rules.txt with content/ and aoc/; its code patches are code mods, code_patches.h), an SDCafiine folder or
// just content/ and aoc/, made for the Wii U game, or a BCML mod: a .bnp file (unpacked at startup with the
// system's tar into the caches) or an unpacked one, whose merge data in logs/ is applied with its files (and
// with its chosen options'). At
// startup the enabled ones are merged in the list's order (content_merge.h; cached per set of mods and
// options) and the game's reads of content and DLC files go to the result (hle/fs.cpp). Changes apply at
// the next start.
//   BOTW_MODS=0          no content mods this run (nothing saved)
//   BOTW_MODS_DIR=path   another Mods folder (tests); headless runs (WWHD_NO_HOST_INPUT) use none without it
#pragma once
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mods::loader {

struct Layout {  // what a mod folder holds
    std::string kind;  // "Cemu graphic pack", "SDCafiine folder", "Content folder", "BCML mod", "BCML mod (.bnp)"
    std::string name, description;
    std::filesystem::path content, aoc;  // empty when absent
    std::filesystem::path logs;          // a BCML mod's merge data (deepmerge.aamp, rstb.json, texts.json, ...)
    // A BCML mod's optional parts (info.json "options"; options/<folder> holds content, aoc and logs). A "multi"
    // option is a switch of its own; the options of a "single" group are one choice, or none unless the
    // group is required. A chosen option is merged over its mod.
    struct Group {
        std::string name, description;
        bool required = false;
    };
    struct Option {
        std::string key;  // its folder's name (what mods.json saves)
        std::string name, description;
        std::filesystem::path folder;
        int group = -1;           // its group in `groups`; -1: a switch of its own
        bool by_default = false;  // BCML's default
    };
    std::vector<Group> groups;  // only groups with options
    std::vector<Option> options;
    bool code = false;    // a Cemu graphic pack with code patches (patch_*.asm, patches.txt): a code mod too
    std::string problem;  // why it can't be used ("": it can)
    std::string warning;  // parts of it that are not used
};
// what `folder` (or a file in the Mods folder) holds, for the game with title ID `title`
Layout detect(const std::filesystem::path& folder, const std::string& title);
// which of l.options a start uses, from the saved choices (option key -> on) over BCML's defaults. A group
// the player hasn't chosen in takes its default option; a required one without a default its first (BCML
// leaves those to the player, it has no default for them); one where the saved choice is gone, none
// unless it is required.
std::vector<bool> chosen(const Layout& l, const std::map<std::string, bool>& saved);

struct OptionView {
    std::string key, name, description;
    int group = -1;       // its group in ModView::groups; -1: a switch of its own
    bool on = false;      // the saved choice (next start)
    bool active = false;  // part of this run
    size_t files = 0, used = 0;  // this run, as for the mod
    std::vector<std::string> notes;
};
struct ModView {
    std::string id;  // its name in the Mods folder
    std::string name, kind, description;
    bool enabled = false;  // the saved choice (next start)
    bool active = false;   // part of this run
    std::string problem, warning;
    bool code = false;           // it has code patches too (mods::patches)
    bool content = false;        // it has content/, aoc/ or BCML merge data (merged at startup)
    size_t files = 0, used = 0;  // this run: files it has, files the game reads from it
    std::vector<std::string> notes;
    std::vector<Layout::Group> groups;  // a BCML mod's options (none for a mod that can't be used)
    std::vector<OptionView> options;
};

void startup();  // main thread, before the game reads files
// the host file the game reads for a guest path (/vol/content/..., /vol/aoc<title id>/..., or a path
// relative to /vol/content), or "" for the game's own
std::string replacement(const std::string& guest);

// Settings > Mods (any thread)
std::vector<ModView> list();  // in priority order: the first one wins
std::string directory();
std::string summary();  // this run's result, or why mods are off
bool restart_needed();  // the saved choices differ from this run's (options too)
void set_enabled(const std::string& id, bool on);
// a BCML option of mod `id`, by its key: a switch on or off; a group's option on is the group's choice, off
// leaves the group without one (not a required group)
void set_option(const std::string& id, const std::string& key, bool on);
void move(const std::string& id, int delta);  // -1: up (wins over more mods)
void rescan();

}  // namespace mods::loader
