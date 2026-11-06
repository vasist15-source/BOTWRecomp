// BCML's merge data for game data tables (a .bnp's logs/), applied to the game's own files as BCML's mergers
// apply them (NiceneNerd/BCML bcml/mergers, GPL-3.0; UKMM's uk-content as a second reference; this is our own
// implementation of their formats and rules):
//   eventinfo.yml  Event/EventInfo.product.sbyml in Pack/Bootup.pack: events added or replaced whole
//   gamedata.yml   GameData/gamedata.ssarc: per flag type, flags added or replaced by DataName, then deleted
//   savedata.yml   GameData/savedataformat.ssarc: game_data.sav's flags (name and hash) added or deleted
//   texts.json     Message/Msg_<language>.product.ssarc in Pack/Bootup_<language>.pack: texts added or
//                  replaced by label, per MSBT file, in msyt's JSON form
// A mod's diff is merged over the game's file in load order (later mods win). A mod that ships its own copy of
// such a file and no log for it has its changes against the game's copy taken as its diff, as BCML does when
// it installs a mod. Files the merge leaves alone stay as the game has them; new flags go where the game keeps
// flags of their kind (MainField_... and the other stage flags with the revival ones).
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "byml.h"

namespace mods::bcml {

using Bytes = std::vector<uint8_t>;
using Files = std::map<std::string, Bytes>;  // an archive's files by name, as stored

// ---- EventInfo: a Hash of event entries ("Npc_oasis001<Talk>": {...})
bool read_events(const std::string& yaml, byml::Value& diff, std::string& why);
bool events_of_copy(const Bytes& game, const Bytes& copy, byml::Value& diff, std::string& why);
// `out` is the merged file (unpacked BYML); `changed` counts entries that differ from the game's
bool merge_events(const Bytes& game, const std::vector<const byml::Value*>& diffs, Bytes& out, size_t& changed, std::string& why);

// ---- gamedata: {type: {add: {DataName: flag}, del: [DataName]}}
bool read_flags(const std::string& yaml, byml::Value& diff, std::string& why);
bool flags_of_copy(const Files& game, const Files& copy, byml::Value& diff, std::string& why);
// `changed`: the archive's files that differ (new ones included); `flags` counts flags added, changed or deleted
bool merge_flags(const Files& game, const std::vector<const byml::Value*>& diffs, Files& changed, size_t& flags, std::string& why);

// ---- savedata: game_data.sav flags
struct SaveDiff {
    std::vector<byml::Value> add;  // {DataName, HashValue}
    std::vector<int32_t> del;      // hashes
};
bool read_save(const std::string& yaml, SaveDiff& diff, std::string& why);
bool save_of_copy(const Files& game, const Files& copy, SaveDiff& diff, std::string& why);
// `result`: the archive's files after the merge (all of them: files are renumbered when game_data.sav grows);
// false with an empty `why` when nothing changes
bool merge_save(const Files& game, const std::vector<const SaveDiff*>& diffs, Files& result, size_t& flags, std::string& why);

// ---- texts
struct Text {
    Bytes text;                  // raw (msbt.h), big-endian
    bool has_attribute = false;  // its attribute string is given
    Bytes attribute;             // raw, with its 0 unit
    bool operator==(const Text& o) const { return text == o.text && has_attribute == o.has_attribute && attribute == o.attribute; }
};
using TextFiles = std::map<std::string, std::map<std::string, Text>>;  // "EventFlowMsg/X.msbt" -> label -> text
// texts.json: language ("USen") -> files
bool read_texts(const std::string& json, std::map<std::string, TextFiles>& diff, std::string& why);
bool texts_of_copy(const Files& game, const Files& copy, TextFiles& diff, std::string& why);
// the language of a mod's texts that goes into the game's `language`, as BCML maps them: the same one, else one
// of the same language in another region, else English, else any (null: the mod has none)
const TextFiles* texts_for(const std::map<std::string, TextFiles>& diff, const std::string& language);
bool merge_texts(const Files& game, const std::vector<const TextFiles*>& diffs, Files& changed, size_t& texts, std::string& why);

}  // namespace mods::bcml
