// Tests the code mods' runtime rules (mods/code_patch_rules.cpp): the programs of preset values with Cemu's
// arithmetic, Cemu's preset choice and visibility, and the pack folder and fingerprint, which must be
// those of tools/botw_code_mods.py (tools/test_cemu_patches.py checks the same fixture's fingerprint).
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "mods/code_patch_rules.h"

using namespace mods;
using namespace mods::patches;
namespace fs = std::filesystem;

namespace {
double run(const std::string& program, const std::map<std::string, double>& vars = {}, bool integer = false) {
    return evaluate(json::parse(program), vars, integer);
}
void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
// tools/test_cemu_patches.py RULES, as tools/botw_code_mods.py writes it into the manifest
const char* kRules = R"({"variables":{"$mode":["int",0],"$mult":["double",1.0]},"categories":["Cycle","Length"],
 "presets":[{"name":"Enabled","category":"Cycle","default":true,"condition":null,"values":{"$mode":1.0}},
            {"name":"Disabled","category":"Cycle","default":false,"condition":null,"values":{"$mode":0.0}},
            {"name":"Fast","category":"Length","default":false,"condition":["$mode",1.0,"=="],"values":{"$mult":96.0}},
            {"name":"Normal","category":"Length","default":true,"condition":["$mode",1.0,"=="],"values":{}}]})";
}  // namespace

int main(int argc, char** argv) {
    // Cemu's double arithmetic
    assert(run(R"(["$a", 2.0, "*"])", {{"$a", 1.5}}) == 3.0);
    assert(run(R"([10.0, 0.0, "/"])") == 0.0);
    assert(run(R"([7.5, 2.0, "%"])") == 0.0);  // both rounded: 8 % 2
    assert(run(R"([-7.0, 3.0, "%"])") == -1.0);
    assert(run(R"([2.0, 3.0, 2.0, "^", "^"])") == 512.0);
    assert(run(R"([1.0, 1.00000000001, "=="])") == 1.0 && run(R"([1.0, 1.1, "!="])") == 1.0);
    assert(run(R"([3.0, 2.0, ">"])") == 1.0 && run(R"([2.0, 1.0, "<="])") == 0.0);
    assert(run(R"([305430528.0, "fn:ha"])") == 0x1235 && run(R"([305430528.0, "fn:lo"])") == 0x8000);  // 0x12348000
    assert(run(R"([305430528.0, "fn:hi"])") == 0x1234 && run(R"([-1.0, "fn:u32"])") == 4294967295.0);
    // TExpressionParser<int> for conditions
    assert(run(R"([7.0, 2.0, "/"])", {}, true) == 3 && run(R"([-7.0, 2.0, "/"])", {}, true) == -3);
    assert(run(R"(["$m", 1.0, "=="])", {{"$m", 1.9}}, true) == 1);
    assert(to_u32(-1.0) == 0xFFFFFFFFu && to_u32(4294967296.0) == 0 && to_u32(INFINITY) == 0 && to_u32(3.9) == 3);
    for (const char* bad : {R"(["$nope"])", R"(["+"])", R"([])", R"([1.0, "fn:sin"])", R"([1.0, 2.0, "&&"])"}) {
        bool threw = false;
        try {
            run(bad);
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
    }

    // presets: the defaults, a choice, a hidden category
    const Rules r = rules_of(json::parse(kRules));
    assert(r.categories.size() == 2 && r.presets.size() == 4 && r.defaults.at("$mult") == 1.0);
    Selection s = select(r, {});
    assert(s.active == std::vector<bool>({true, false, false, true}) && s.visible == std::vector<bool>(4, true));
    auto v = values(r, s);
    assert(v.at("$mode") == 1 && v.at("$mult") == 1);
    s = select(r, {{"Length", "Fast"}});
    assert(values(r, s).at("$mult") == 96);
    s = select(r, {{"Cycle", "Disabled"}, {"Length", "Fast"}});  // Length's presets hidden: the choice stays
    assert(!s.visible[2] && !s.visible[3] && s.active[2]);
    v = values(r, s);
    assert(v.at("$mode") == 0 && v.at("$mult") == 96);  // an active hidden preset still gives its values
    s = select(r, {{"Cycle", "gone"}, {"Length", "gone"}});  // saved choices that don't exist any more
    assert(s.active == std::vector<bool>({true, false, false, true}));

    // the pack folder (through a folder around it) and its fingerprint
    const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "code_patch_rules_test";
    fs::remove_all(work);
    write(work / "Mod" / "inner" / "rules.txt", "[Definition]\nname = T\n");
    write(work / "Mod" / "inner" / "patch_A.asm", "[T]\nmoduleMatches = 0x6267BFD0\n0x02000000 = nop\n");
    write(work / "Mod" / "inner" / "notes.txt", "not part of it\n");
    write(work / "Content" / "content" / "x.txt", "a content mod\n");
    write(work / "Two" / "a" / "rules.txt", "");
    write(work / "Two" / "b" / "rules.txt", "");
    assert(pack_folder(work / "Mod") == work / "Mod" / "inner");
    assert(pack_folder(work / "Content").empty() && pack_folder(work / "Two").empty());
    assert(pack_files(work / "Mod" / "inner") == std::vector<std::string>({"rules.txt", "patch_A.asm"}));
    assert(fingerprint(work / "Mod" / "inner") == "c0e372459df36742f6ffec0ec1c7d12b2df0067f3072266c6ab7cc31620668b5");
    write(work / "Mod" / "inner" / "patch_A.asm", "[T]\nmoduleMatches = 0x6267BFD0\n0x02000004 = nop\n");
    assert(fingerprint(work / "Mod" / "inner") != "c0e372459df36742f6ffec0ec1c7d12b2df0067f3072266c6ab7cc31620668b5");
    write(work / "Old" / "rules.txt", "");
    write(work / "Old" / "patches.txt", "[T]\n");
    assert(pack_files(work / "Old") == std::vector<std::string>({"rules.txt", "patches.txt"}));
    fs::remove_all(work);
    std::puts("code_patch_rules_test: ok");
    return 0;
}
