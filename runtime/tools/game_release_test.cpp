// Tests the installed release's detection (game_release.cpp): metadata, RegionLangMask.txt's RomType,
// the Bootup packs and region bits, their order, the country table, and a synthetic European dump.
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "game_release.h"

namespace config { std::string game_dir, base_dir; }
void log_msg(const char*, ...) {}

using namespace game_release;

namespace {

std::string meta(const char* title, const char* region) {
    return std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<menu type=\"complex\" access=\"777\">\n"
                       "  <title_version type=\"unsignedInt\" length=\"4\">208</title_version>\n"
                       "  <title_id type=\"hexBinary\" length=\"8\">") + title + "</title_id>\n"
           "  <region type=\"hexBinary\" length=\"4\">" + region + "</region>\n</menu>\n";
}
// as on the disc: Shift-JIS comments, CR LF
std::string mask(const char* languages, const char* rom_type) {
    return std::string("# \x83\x56\x83\x83\x81\x5b\x83\x76\r\n# JPja USen USes USfr EUen EUde\r\n") + languages +
           "\r\n# \x82\xc8\r\n" + rom_type + "\r\n";
}

}  // namespace

int main(int argc, char** argv) {
    // the pieces
    assert(tag_text(meta("00050000101C9400", "00000002"), "title_id") == "00050000101C9400");
    assert(tag_text("<title_id>\n  0005000E101C9500 \n</title_id>", "title_id") == "0005000E101C9500");
    assert(tag_text("<title_idx>1</title_idx><title_id>2</title_id>", "title_id") == "2");
    assert(tag_text("<title_version>208</title_version>", "title_id").empty() && tag_text("", "region").empty());
    assert(title_of("<title_id type=\"hexBinary\" length=\"8\">0005000e101c9500</title_id>") == "00050000101C9500");
    assert(title_of("<title_id>00050000101C9400</title_id>") == "00050000101C9400");
    assert(title_of("<title_id>101C9400</title_id>").empty() && title_of("<title_id>00050000101C94ZZ</title_id>").empty());
    assert(region_of_title("00050000101C9400") == kUsa && region_of_title("00050000101c9500") == kEurope);
    assert(region_of_title("00050000101C9300") == kJapan && region_of_title("0005000010143500") == kNone);
    assert(region_of_title("0005000E101C9400") == kNone && region_of_title("") == kNone);
    assert(region_of_bits(2) == kUsa && region_of_bits(4) == kEurope && region_of_bits(1) == kJapan);
    assert(region_of_bits(0) == kNone && region_of_bits(6) == kNone && region_of_bits(0xFF) == kNone);
    const std::vector<std::string> us = mask_lines(mask("USen USes USfr", "US"));
    assert((us == std::vector<std::string>{"USen USes USfr", "US"}));
    assert(mask_lines(mask("EUen EUes EUfr EUde EUit EUnl EUru", "EU")).at(1) == "EU");
    assert(mask_lines("# only comments\r\n\r\n").empty() && mask_lines("USen").size() == 1);
    assert(region_of_rom_type("EU") == kEurope && region_of_rom_type("us") == kUsa && region_of_rom_type("JP") == kJapan);
    assert(region_of_rom_type("") == kNone && region_of_rom_type("KR") == kNone);
    assert(region_of_packs({"Bootup.pack", "Bootup_Graphics.pack", "Bootup_EUde.pack", "Bootup_EUen.pack", "Title.pack"}) ==
           kEurope);
    assert(region_of_packs({"bootup_usen.pack", "Bootup_USfr.pack"}) == kUsa);
    assert(region_of_packs({"Bootup_USen.pack", "Bootup_EUen.pack"}) == kNone);
    assert(region_of_packs({"Bootup.pack", "TitleBG_EUen.pack"}) == kNone && region_of_packs({}) == kNone);

    // the decision: the title ID first (the update's meta.xml carries the base game's)
    const std::vector<std::string> no_packs;
    Release r = decide({{"update meta", meta("00050000101C9400", "00000002")}, {"base meta", ""}},
                       {{"update mask", mask("USen USes USfr", "US")}}, {"Bootup_USen.pack"});
    assert(r.region == kUsa && r.title_id == "00050000101C9400" && r.from == "title in update meta");
    r = decide({{"update meta", meta("00050000101C9500", "00000004")}}, {}, no_packs);
    assert(r.region == kEurope && r.title_id == "00050000101C9500");
    // no meta.xml: the update's app.xml (its own ID) names the base game
    r = decide({{"update meta", ""}, {"base meta", ""}, {"update app", "<title_id>0005000E101C9500</title_id>"}}, {},
               no_packs);
    assert(r.region == kEurope && r.title_id == "00050000101C9500" && r.from == "title in update app");
    // a region-free meta.xml (patched region bits) keeps the title's region
    r = decide({{"update meta", meta("00050000101C9500", "000000FF")}}, {}, {"Bootup_EUde.pack"});
    assert(r.region == kEurope);
    // the title wins over a RomType that disagrees, and the log says so
    r = decide({{"update meta", meta("00050000101C9400", "00000002")}}, {{"m", mask("EUen", "EU")}}, no_packs);
    assert(r.region == kUsa && r.from == "title in update meta; the RomType says Europe (m)");
    // no title: the RomType, then the packs, then the region bits, else USA
    r = decide({{"update meta", ""}}, {{"local mask", ""}, {"system mask", mask("EUen EUde", "EU")}},
               {"Bootup_USen.pack"});
    assert(r.region == kEurope && r.title_id == "00050000101C9500");
    assert(r.from == "no title ID, region from the RomType in system mask");
    r = decide({}, {{"mask", "USen\r\n"}}, {"Bootup_EUen.pack", "Bootup_EUru.pack"});
    assert(r.region == kEurope && r.from == "no title ID, region from its Bootup packs");
    r = decide({{"meta", meta("0005000010143500", "00000004")}}, {}, no_packs);  // a title we don't know
    assert(r.region == kEurope && r.title_id == "0005000010143500");
    assert(r.from == "unknown title in meta, region from the region in meta");
    r = decide({{"meta", meta("0005000010143500", "00000006")}}, {}, no_packs);
    assert(r.region == kUsa && r.from == "unknown title in meta, region from nothing");
    r = decide({}, {}, no_packs);
    assert(r.region == kUsa && r.title_id == "00050000101C9400" && r.from == "no title ID, region from nothing");
    assert(std::string(name(kEurope)) == "Europe" && std::string(name(kUsa)) == "USA" && std::string(name(3)) == "?");

    // cafe.cntry_reg: the United States for the USA release whatever the language, the language's country in Europe
    assert(country(kUsa, 1) == 49 && country(kUsa, 2) == 49 && country(kUsa, 5) == 49 && country(kJapan, 0) == 1);
    assert(country(kEurope, 1) == 110 && country(kEurope, 2) == 77 && country(kEurope, 3) == 78);
    assert(country(kEurope, 4) == 83 && country(kEurope, 5) == 105 && country(kEurope, 8) == 94);
    assert(country(kEurope, 10) == 100 && country(kEurope, 11) == 110);

    // a synthetic European dump (update without meta.xml region, base with it): read once by current()
    namespace fs = std::filesystem;
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "wwhd_game_release_test";
    fs::remove_all(root);
    fs::create_directories(root / "update" / "code");
    fs::create_directories(root / "update" / "content" / "Local" / "System");
    fs::create_directories(root / "update" / "content" / "Pack");
    fs::create_directories(root / "base" / "meta");
    std::ofstream(root / "update" / "code" / "app.xml") << "<app><title_id>0005000E101C9500</title_id></app>";
    std::ofstream(root / "update" / "content" / "Local" / "System" / "RegionLangMask.txt")
        << mask("EUen EUes EUfr EUde EUit EUnl EUru", "EU");
    std::ofstream(root / "update" / "content" / "Pack" / "Bootup_EUnl.pack") << "x";
    std::ofstream(root / "base" / "meta" / "meta.xml") << meta("00050000101C9500", "00000004");
    config::game_dir = (root / "update").string();
    config::base_dir = (root / "base").string();
    const Release& now = current();
    assert(now.region == kEurope && now.title_id == "00050000101C9500");
    assert(now.from == "title in " + config::base_dir + "/meta/meta.xml");
    assert(std::string(game_title_id()) == "00050000101C9500" && &current() == &now);
    fs::remove_all(root);
    puts("game_release_test: title IDs, RomType, packs, region bits, countries, a European dump passed");
}
