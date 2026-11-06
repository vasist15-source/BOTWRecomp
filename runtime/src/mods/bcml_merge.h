// BCML's merge data for game data tables (a .bnp's logs/), applied to the game's own files as BCML's mergers
// apply them (NiceneNerd/BCML bcml/mergers, GPL-3.0; UKMM's uk-content as a second reference; this is our own
// implementation of their formats and rules):
//   eventinfo.yml  Event/EventInfo.product.sbyml in Pack/Bootup.pack: events added or replaced whole
//   gamedata.yml   GameData/gamedata.ssarc: per flag type, flags added or replaced by DataName, then deleted
//   savedata.yml   GameData/savedataformat.ssarc: game_data.sav's flags (name and hash) added or deleted
//   texts.json     Message/Msg_<language>.product.ssarc in Pack/Bootup_<language>.pack: texts added or
//                  replaced by label, per MSBT file, in msyt's JSON form
// and the tables of one file each below (bcml_tables.cpp). A mod's diff is merged over the game's file in load order
// (later mods win). A mod that ships its own copy of such a file and no log for it has its changes against the game's
// copy taken as its diff, as BCML does when it installs a mod. Files the merge leaves alone stay as the game has them;
// new flags go where the game keeps flags of their kind (MainField_... and the other stage flags with the revival ones).
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "aamp.h"
#include "byml.h"

namespace mods::bcml {

using Bytes = std::vector<uint8_t>;
using Files = std::map<std::string, Bytes>;  // an archive's files by name, as stored

// BCML's util.dict_merge: hashes merge key by key, lists are replaced (overwrite_lists) or extended
void dict_merge(byml::Value& dst, const byml::Value& src, bool overwrite_lists);

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

// ---------------------------------------------------------------- tables of one file each (bcml_tables.cpp)
//   actorinfo.yml   Actor/ActorInfo.product.sbyml: actors by the CRC-32 of their name, fields merged
//   map.yml         Map/MainField/<A-1>/<A-1>_Static.smubin and _Dynamic: objects and rails by HashId
//   shop.aamp       Actor/ShopData/*.bshop in actor packs: items added to or removed from a shop's tables
//   drops.json      Actor/DropTable/*.bdrop in actor packs: drop tables replaced whole
//   quests.yml      Quest/QuestProduct.sbquestpack in Pack/TitleBG.pack: quests by Name
//   residents.yml   Actor/ResidentActors.byml in Pack/Bootup.pack: resident actors by name
//   areadata.yml    Ecosystem/AreaData.sbyml in Pack/Bootup.pack: areas by AreaNumber, fields merged
//   effects.yml     Ecosystem/StatusEffectList.sbyml in Pack/Bootup.pack: status effects by name
//   mainstatic.yml  Map/MainField/Static.smubin: markers, start positions, ... by position and name
//   dstatic.yml     Map/CDungeon/Static.smubin: shrine entrances by map and position name
enum class Kind { ActorInfo, Map, Shop, Drop, Quests, Residents, AreaData, Effects, MainStatic, DungeonStatic };
struct DropTable {
    int32_t repeat_min = 0, repeat_max = 0, approach = 0, speed = 0;  // RepeatNumMin/Max, ApproachType, OccurrenceSpeedType
    std::vector<std::pair<std::string, std::optional<float>>> items;  // in order; no value: BCML's UNDERRIDE (the game's)
    bool operator==(const DropTable& o) const {
        return repeat_min == o.repeat_min && repeat_max == o.repeat_max && approach == o.approach && speed == o.speed && items == o.items;
    }
};
using Drops = std::vector<std::pair<std::string, DropTable>>;  // tables in order
// a mod's changes to one file: as its log has them (a BYML value; shops an AAMP list, drops their tables)
struct Diff {
    byml::Value value;
    aamp::List shop;
    Drops drops;
};
// logs (one file each, or a file for each map unit / shop / drop table: "A-1_Static",
// "content/Actor/Pack/X.sbactorpack//Actor/ShopData/X.bshop")
bool read_log(Kind k, const std::string& text, Diff& diff, std::string& why);  // a log of a single file
bool read_maps(const std::string& yaml, std::map<std::string, Diff>& units, std::string& why);
bool read_shops(const Bytes& aamp, std::map<std::string, Diff>& files, std::string& why);
bool read_drops(const std::string& json, std::map<std::string, Diff>& files, std::string& why);
// the entries a diff changes (actors, objects, items, ...), for the Mods page
size_t entries(Kind k, const Diff& d);
// a mod's own copy of the file (unpacked) as its diff against the game's (unpacked), as BCML logs a mod it installs
bool diff_of_copy(Kind k, const Bytes& game, const Bytes& copy, Diff& diff, std::string& why);
// the game's file (unpacked) with the diffs, in load order; `out` (unpacked) and the entries that differ from the
// game's. False with an empty `why` when nothing changes.
bool merge_file(Kind k, const Bytes& game, const std::vector<const Diff*>& diffs, Bytes& out, size_t& changed, std::string& why);

}  // namespace mods::bcml
