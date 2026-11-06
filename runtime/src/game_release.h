// Which release of the game the player installed (USA or Europe) and the region and country it is told.
//
// The USA and European v208 updates ship the same U-King.rpx (the installer checks its SHA-256), so one
// translation runs both. The game asks the system which release it is: MCP_GetSysProdSettings' platform
// region (0x03C707DC reads its byte 3; 0x030AC9C4 makes 1, 2, 4 its Japan, USA, Europe; coreinit_misc.cpp)
// and UCReadSysConfig("cafe.language"). It checks the two against its content/System/RegionLangMask.txt
// (the update's is in content/Local/System; line 1 the region-languages it has, "USen USes USfr", line 2
// its RomType, "US"), and they name its text packs, content/Pack/Bootup_<USen|EUde|EUnl|...>.pack: the
// region must be the dump's.
// The release is read once from the dump. The title ID decides: the update's meta/meta.xml carries the
// base game's (its code/app.xml has the update's own), then the base game's metadata. Without a known
// title: the RomType, the Bootup packs present, meta.xml's region bits, else USA.
// The European release is UNTESTED (no European dump has run yet); the Japanese one is refused by the
// installer (its update's U-King.rpx is not known to be the same).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace game_release {

// Wii U region bits, as MCP and meta.xml give them (game_lang::kJapan / kUsa / kEurope)
enum Region : int { kNone = 0, kJapan = 1, kUsa = 2, kEurope = 4 };

struct Release {
    int region = kUsa;
    std::string title_id = "00050000101C9400";  // the base game's, 16 upper-case hex digits
    std::string from = "nothing names it";      // what decided it, for the log
};

// The installed release, read once from config::game_dir and config::base_dir (logged then); any thread
const Release& current();
const char* name(int region);  // "USA", "Europe", "Japan"; "?" for others

// cafe.cntry_reg (the Wii U's country codes) for a region and console language: in Europe the country of
// the language (United Kingdom for English), else the United States or Japan. The game itself reads only
// cafe.language; this is for anything else that asks.
uint32_t country(int region, int language);

// ---- the parts, for the tests (runtime/tools/game_release_test.cpp)
std::string tag_text(const std::string& xml, const char* tag);  // trimmed text of <tag ...>, "" if absent
// the base game's title ID that an app.xml or meta.xml names (an update's 0005000E... becomes
// 00050000...), 16 upper-case hex digits; "" if none
std::string title_of(const std::string& xml);
int region_of_title(const std::string& title_id);  // BotW's 101C9300 / 101C9400 / 101C9500; kNone else
int region_of_bits(uint32_t bits);  // one region bit; kNone for none or several (a region-free title)
// RegionLangMask.txt's lines without comments ('#') and blank lines; [1] is the RomType
std::vector<std::string> mask_lines(const std::string& text);
int region_of_rom_type(const std::string& rom_type);  // "US", "EU", "JP"; kNone else
// the one region whose Bootup_<RR><ll>.pack files are among `names` (US, EU, JP); kNone for none or several
int region_of_packs(const std::vector<std::string>& names);
struct Source {
    std::string what;  // "<update>/meta/meta.xml"
    std::string text;  // "" when the file isn't there
};
// metadata and RegionLangMask.txt files in the order they are trusted, and the file names in content/Pack
// of the update and base game
Release decide(const std::vector<Source>& metadata, const std::vector<Source>& masks,
               const std::vector<std::string>& packs);

}  // namespace game_release

// current().title_id for C and for code linked without this file: the weak default in
// guest_addr_identity.c (unit tests) returns the build's title, g_guest_build_title_id
extern "C" const char* game_title_id(void);
