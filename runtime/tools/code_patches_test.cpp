// Tests the code mods' runtime (mods/code_patches.cpp) on a hand-written manifest, as tools/botw_code_mods.py
// writes it into code_patches.c: the caves' bytes and functions at startup, the presets' values in cave data,
// instruction immediates and the game's data, switching a mod on and off at once (the game's own data put
// back), presets that apply at once and are saved, and which Mods folder packs need the game code rebuilt.
#include <sys/mman.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "botw_patches.h"
#include "mods/code_patches.h"
#include "runtime.h"

namespace fs = std::filesystem;
namespace config { std::string game_dir, base_dir, aoc_dir, save_dir; }
void log_msg(const char*, ...) {}

namespace {
std::vector<std::pair<uint32_t, PpcFunc>> g_dispatched;
void cave(Cpu*) {}
}  // namespace
namespace dispatch {
void set(uint32_t addr, PpcFunc fn) { g_dispatched.push_back({addr, fn}); }
}  // namespace dispatch

// TestMod: a cave at 0x01800000 (blr, then a float of $mult), game data at 0x10000008 ($mult * 2) while on, an
// immediate $mode + 1; Bad: refused by the translation; Old: in the game code with other files than the
// Mods folder's (its fingerprint is a placeholder)
extern "C" const BotwPatchFunc g_botw_patch_funcs[] = {{0x01800000u, cave}};
extern "C" const unsigned g_botw_patch_func_count = 1;
extern "C" const char g_botw_patch_manifest[] = R"({"format":1,"mods":[
 {"id":"TestMod","name":"Test Mod","description":"A test.","fingerprint":"ab4ae124cd18f52fe28dab967e80832ac2cfd197003f038646c7277152e2fb15",
  "switch":0,"variables":{"$mode":["int",0],"$mult":["double",1.0]},"categories":["Length"],
  "presets":[{"name":"Normal","category":"Length","default":true,"condition":null,"values":{}},
             {"name":"Fast","category":"Length","default":false,"condition":null,"values":{"$mult":96.0}}],
  "memory":[[25165824,"4e80002000000000"]],"writes":[[25165828,"float",["$mult"]]],
  "data":[[268435464,"00000000"],[268435464,"float",["$mult",2.0,"*"]]],"values":[[3,["$mode",1.0,"+"]]],
  "sites":[33554436],"notes":["A note."]},
 {"id":"Bad","name":"Bad","description":"","fingerprint":"67167af0a06b6b42a4f0378eea67ed8f995cd5161631cab7bfb2e4896adbe5bb",
  "problem":"it gives the game more memory ([RAM]), which this port can't"},
 {"id":"Old","name":"Old","description":"","fingerprint":"0000","switch":1,"variables":{},"categories":[],"presets":[],
  "memory":[],"writes":[],"data":[],"values":[],"sites":[33554440],"notes":[]}]})";

namespace {
void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
float guest_float(uint32_t a) {
    uint32_t u = __builtin_bswap32(*(const uint32_t*)mem::ptr(a));
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
void set_guest_float(uint32_t a, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    *(uint32_t*)mem::ptr(a) = __builtin_bswap32(u);
}
const char* kRules =
    "[Definition]\ntitleIds = 00050000101C9400\nname = Test Mod\nversion = 6\n\n[Default]\n$mode:int = 0\n$mult = 1.0\n\n"
    "[Preset]\nname = Normal\ncategory = Length\ndefault = 1\n\n[Preset]\nname = Fast\ncategory = Length\n$mult = 96\n";
}  // namespace

int main(int argc, char** argv) {
    // guest memory where the runtime has it
    void* p = mmap(PPC_MEM_BASE, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    assert(p == PPC_MEM_BASE);
    const fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "code_patches_test";
    fs::remove_all(work);
    const fs::path mods = work / "Mods";
    write(mods / "TestMod" / "rules.txt", kRules);
    write(mods / "TestMod" / "patch_test.asm", "[T]\nmoduleMatches = 0x6267BFD0\n");
    write(mods / "Bad" / "rules.txt", "[Definition]\nname = Bad\n");
    write(mods / "Bad" / "patch_bad.asm", "[B]\nmoduleMatches = 0x6267BFD0\n");
    write(mods / "Old" / "rules.txt", "[Definition]\nname = Old\n");
    write(mods / "Old" / "patch_old.asm", "[O]\nmoduleMatches = 0x6267BFD0\n");
    write(mods / "New" / "wrapped" / "rules.txt", "[Definition]\nname = New\n");
    write(mods / "New" / "wrapped" / "patches.txt", "[N]\n");
    write(mods / "Content" / "content" / "a.txt", "a content mod\n");
    write(mods / "Graphics" / "rules.txt", "[Definition]\nname = Shaders only\n");
    set_guest_float(0x10000008, 5.0f);  // the game's own value

    char arg0[] = "wwhd";
    char* args[] = {arg0, nullptr};
    mods::patches::startup(1, args, mods, [](const std::string& id) { return id == "TestMod" || id == "Old"; });

    // startup: the cave and its function, the default presets' values, TestMod on (Old: other files, so off)
    assert(g_dispatched.size() == 1 && g_dispatched[0].first == 0x01800000u);
    assert(*(const uint32_t*)mem::ptr(0x01800000) == __builtin_bswap32(0x4E800020u));
    assert(guest_float(0x01800004) == 1.0f && guest_float(0x10000008) == 2.0f);
    assert(botw_patch_values[3] == 1 && botw_patch_on(0) == 1 && botw_patch_on(1) == 0);

    // what Settings > Mods shows
    const auto list = mods::patches::list();
    std::vector<std::string> ids;
    for (const auto& v : list) ids.push_back(v.id);
    assert((ids == std::vector<std::string>{"Bad", "New", "Old", "TestMod"}));  // packs with code patches only
    mods::patches::View v;
    assert(mods::patches::find("TestMod", v) && v.build == mods::patches::Build::Current && v.on && v.name == "Test Mod");
    assert(v.changes == 1 && v.notes == std::vector<std::string>{"A note."} && v.categories.size() == 1);
    assert(v.categories[0].name == "Length" && v.categories[0].presets.size() == 2 && v.categories[0].active == 0);
    assert(mods::patches::find("Bad", v) && v.build == mods::patches::Build::Refused && v.problem.find("[RAM]") != std::string::npos);
    assert(mods::patches::find("Old", v) && v.build == mods::patches::Build::Changed && !v.on);
    assert(mods::patches::find("New", v) && v.build == mods::patches::Build::Missing);
    assert(!mods::patches::find("Content", v) && !mods::patches::is_code_mod("Graphics"));
    assert(mods::patches::rebuild_needed());

    // a preset at once: cave data and the game data while on; saved for the next start
    mods::patches::set_preset("TestMod", "Length", "Fast");
    assert(guest_float(0x01800004) == 96.0f && guest_float(0x10000008) == 192.0f);
    assert(mods::patches::find("TestMod", v) && v.categories[0].active == 1);
    std::ifstream saved(work / "code_mods.json");
    const std::string text{std::istreambuf_iterator<char>(saved), {}};
    assert(text.find("\"TestMod\":{\"Length\":\"Fast\"}") != std::string::npos);

    // off and on at once: the game's data comes back, the switch follows
    mods::patches::set_on("TestMod", false);
    assert(botw_patch_on(0) == 0 && guest_float(0x10000008) == 5.0f && guest_float(0x01800004) == 96.0f);
    mods::patches::set_preset("TestMod", "Length", "Normal");  // while off: the game's data stays its own
    assert(guest_float(0x01800004) == 1.0f && guest_float(0x10000008) == 5.0f);
    mods::patches::set_on("TestMod", true);
    assert(botw_patch_on(0) == 1 && guest_float(0x10000008) == 2.0f);
    mods::patches::set_on("Old", true);  // another version of it in the game code: stays off
    assert(botw_patch_on(1) == 0);

    // the pack's files change: the game code holds another version now, so it can be switched off but not on
    write(mods / "TestMod" / "patch_test.asm", "[T]\nmoduleMatches = 0x6267BFD0\n0x02000004 = nop\n");
    mods::patches::rescan();
    assert(mods::patches::find("TestMod", v) && v.build == mods::patches::Build::Changed && v.on);
    mods::patches::set_on("TestMod", false);
    assert(botw_patch_on(0) == 0 && guest_float(0x10000008) == 5.0f);
    mods::patches::set_on("TestMod", true);
    assert(botw_patch_on(0) == 0);

    fs::remove_all(work);
    std::puts("code_patches_test: ok");
    return 0;
}
