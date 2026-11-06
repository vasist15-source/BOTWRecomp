// The merged content of the player's content mods (mod_loader.cpp), built once per set of mods:
//  - a mod's content/ or aoc/ file replaces the game's; when several mods have it, the one highest in
//    the list wins;
//  - archives (packs, actor packs, ...) that several mods change are merged file by file, each mod's
//    changes taken against the game's own archive, nested archives included;
//  - a loose file the game only has inside archives (most of Pack/TitleBG.pack and Pack/Bootup.pack,
//    which the game reads instead of loose files) is put into those archives;
//  - the resource size table takes the mods' own entries, and entries of files that grew grow too;
//  - a .bnp's BCML merge data (deepmerge.aamp, aslist.aamp, rstb.json) is applied in the mod's turn.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mods::merge {

struct Vanilla {
    std::filesystem::path update_content, base_content, aoc_content;  // .../content folders (empty: absent)
    // the game's file for a tree path ("content/Pack/TitleBG.pack", "aoc/0010/Pack/AocMainField.pack"),
    // the update's before the base game's, any case; empty if the game has none
    std::filesystem::path find(const std::string& tree) const;
};
struct Mod {
    std::string id, name;
    std::filesystem::path content, aoc;  // the mod's content/ and aoc/ folders (either may be empty)
    // BCML's merge data of a .bnp (empty: none): deepmerge.aamp and aslist.aamp (parameter changes, applied to
    // the files they name inside the merged archives) and rstb.json (sizes; 0: the entry is dropped)
    std::filesystem::path logs;
    bool option = false;  // a BCML mod's option, after its mod (counted as an option of it)
};
struct ModReport {
    size_t files = 0;       // files the mod has
    size_t replaced = 0;    // read by the game as they are
    size_t merged = 0;      // put into merged archives
    size_t overridden = 0;  // unused: a mod higher in the list has the same file
    std::vector<std::string> notes;
};
struct Result {
    std::map<std::string, std::string> files;  // tree key (lower case) -> the host file the game reads
    std::vector<ModReport> mods;               // in the order of the mods passed to build()
    std::vector<std::string> log;
    std::vector<std::filesystem::path> vanilla_used;  // game files the merged files were made from
    size_t merged_archives = 0;
    size_t size_entries = 0;  // resource size table entries that differ from the game's
};
std::string tree_key(const std::string& tree);
inline const char* const kSizeTable = "content/System/Resource/ResourceSizeTable.product.srsizetable";
// `mods` in load order (later ones win). Merged files are written below `out`. Throws
// (exception_report) when one can't be written.
Result build(const Vanilla& vanilla, const std::vector<Mod>& mods, const std::filesystem::path& out);

}  // namespace mods::merge
