// Tests the content mod loader with synthetic files: Yaz0, SARC and resource size table formats
// (mods/botw_formats.h), AAMP, BYML with its YAML text and MSBT (mods/aamp.h, byml.h, msbt.h), merging several
// mods' content with BCML merge data (mods/content_merge.h, bcml_merge.h), mod folder layouts and the loader's
// startup, settings and cache (mods/mod_loader.h).
//   content_merge_test WORK            all tests (WORK is emptied)
//   content_merge_test WORK --reuse T  (run by the test itself) a second start must take the cached merge
//   content_merge_test WORK --options TABLE  (run by the test itself) a start with other BCML options
//   content_merge_test --dump GAME BASE AOC MODS   by hand: the loader on a real dump and Mods folder; every
//                      merged archive is read back (BOTW_MODS_DIR=MODS; its cache is MODS/../Mods cache)
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <sys/resource.h>

#include "mods/aamp.h"
#include "mods/bcml_merge.h"
#include "mods/botw_formats.h"
#include "mods/byml.h"
#include "mods/content_merge.h"
#include "mods/mod_loader.h"
#include "mods/msbt.h"

namespace config { std::string game_dir, base_dir, aoc_dir, save_dir; }
bool g_trace_hle = false;
void log_msg(const char* fmt, ...) {
    if (!std::getenv("CONTENT_MERGE_TEST_LOG")) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

using namespace mods::formats;
namespace fs = std::filesystem;
namespace merge = mods::merge;
namespace loader = mods::loader;
namespace aamp = mods::aamp;
namespace byml = mods::byml;
namespace msbt = mods::msbt;
namespace bcml = mods::bcml;
namespace json = mods::json;

namespace {

Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
std::string text(const Bytes& b) { return std::string(b.begin(), b.end()); }
Bytes repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; i++) out += s;
    return bytes(out);
}
Bytes yaz(const Bytes& b, uint32_t align = 0) { return yaz0_encode(b.data(), b.size(), align); }
Bytes unyaz(const Bytes& b) {
    Bytes out;
    assert(yaz0_decode(b.data(), b.size(), out));
    return out;
}
Bytes sarc(const std::map<std::string, Bytes>& files) {
    std::vector<SarcInput> in;
    for (const auto& [name, data] : files) in.push_back({name, data.data(), data.size(), 8});
    Bytes out;
    assert(sarc_write(in, 0x65, out));
    return out;
}
std::map<std::string, Bytes> unsarc(const Bytes& b) {
    Sarc s;
    std::string why;
    const bool ok = sarc_parse(b.data(), b.size(), s, &why);
    if (!ok) fprintf(stderr, "sarc: %s\n", why.c_str());
    assert(ok);
    std::map<std::string, Bytes> out;
    for (const auto& e : s.entries) out[e.name] = Bytes(b.begin() + long(e.offset), b.begin() + long(e.offset + e.size));
    return out;
}
void put(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    assert(f);
}
void put(const fs::path& p, const std::string& s) { put(p, bytes(s)); }
Bytes get(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    assert(f);
    return Bytes(std::istreambuf_iterator<char>(f), {});
}
Rstb table_of(const fs::path& p) {
    const Bytes body = unyaz(get(p));
    Rstb t;
    assert(rstb_parse(body.data(), body.size(), t));
    return t;
}
uint32_t entry(const Rstb& t, const std::string& name) {
    uint32_t v = 0;
    assert(rstb_get(t, name, &v));
    return v;
}
bool has_note(const merge::ModReport& r, const char* part) {
    for (const auto& n : r.notes)
        if (n.find(part) != std::string::npos) return true;
    return false;
}

void test_yaz0() {
    std::mt19937 rng(7);
    for (size_t n : {0u, 1u, 7u, 8u, 9u, 17u, 4096u, 70000u}) {
        Bytes noise(n), runs(n);
        for (size_t i = 0; i < n; i++) {
            noise[i] = uint8_t(rng());
            runs[i] = uint8_t("BotW"[(i / 13) % 4] + (i % 3));
        }
        for (const Bytes* b : {&noise, &runs}) {
            const Bytes packed = yaz(*b, 0x80);
            assert(is_yaz0(packed.data(), packed.size()) && yaz0_size(packed.data(), packed.size()) == n);
            assert(yaz0_alignment(packed.data(), packed.size()) == 0x80);
            assert(unyaz(packed) == *b);
        }
    }
    const Bytes zeros(100000, 0);
    const Bytes packed = yaz(zeros);
    assert(packed.size() < 1200 && unyaz(packed) == zeros);  // runs of 0x111 bytes
    Bytes cut(packed.begin(), packed.begin() + 40), out;
    assert(!yaz0_decode(cut.data(), cut.size(), out));
    // a back reference before the start of the data
    const uint8_t bad[] = {'Y', 'a', 'z', '0', 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x10, 0x00};
    assert(!yaz0_decode(bad, sizeof bad, out));
}

void test_sarc() {
    const Bytes a = bytes("first"), b(300, 7), c = bytes("third file");
    std::vector<SarcInput> in = {{"b.txt", a.data(), a.size(), 4}, {"a/x.bin", b.data(), b.size(), 0x80}, {"c", c.data(), c.size(), 0x2000}};
    Bytes out;
    assert(sarc_write(in, 0x65, out));
    Sarc s;
    assert(sarc_parse(out.data(), out.size(), s) && s.big_endian && s.key == 0x65);
    assert(s.entries.size() == 3 && s.data_offset % 0x2000 == 0);
    for (size_t k = 0; k < s.entries.size(); k++) {
        const auto& e = s.entries[k];
        assert(e.hash == sarc_hash(e.name));
        if (k) assert(s.entries[k - 1].hash < e.hash);
        const uint32_t want = e.name == "c" ? 0x2000 : e.name == "a/x.bin" ? 0x80 : 4;
        assert(e.offset % want == 0);
        const Bytes& data = e.name == "c" ? c : e.name == "a/x.bin" ? b : a;
        assert(e.size == data.size() && !memcmp(out.data() + e.offset, data.data(), data.size()));
    }
    // header and names only (as the pack index reads them)
    Sarc head;
    assert(!sarc_parse(out.data(), s.data_offset, head));
    assert(sarc_parse(out.data(), s.data_offset, head, nullptr, out.size()) && head.entries.size() == 3);
    std::vector<SarcInput> twice = {{"same", a.data(), a.size(), 4}, {"same", c.data(), c.size(), 4}};
    assert(!sarc_write(twice, 0x65, out));
    assert(natural_alignment(0x534C) == 4 && natural_alignment(0x100) == 0x100 && natural_alignment(0x40000) == 0x2000);
    Bytes fres(0x20, 0);
    memcpy(fres.data(), "FRES", 4);
    put32(fres.data() + 0x10, 0x1000);
    assert(data_alignment(fres.data(), fres.size()) == 0x1000 && data_alignment(a.data(), a.size()) == 8);
}

void test_rstb() {
    assert(rstb_crc("abc") == 0x352441C2u);
    Rstb t;
    rstb_set(t, "Actor/Pack/A.bactorpack", 10);
    t.names["Model/Collides.bfres"] = 20;
    const Bytes body = rstb_write(t);
    assert(body.size() == 12 + 8 + 132);
    Rstb back;
    assert(rstb_parse(body.data(), body.size(), back) && back == t);
    assert(entry(back, "Actor/Pack/A.bactorpack") == 10 && entry(back, "Model/Collides.bfres") == 20);
    rstb_set(back, "Model/Collides.bfres", 30);
    assert(back.names["Model/Collides.bfres"] == 30 && back.crc.size() == 1);
    rstb_erase(back, "Model/Collides.bfres");
    rstb_erase(back, "Actor/Pack/A.bactorpack");
    assert(back.names.empty() && back.crc.empty());
    Bytes swapped = body;  // a little-endian (Switch) table
    std::swap(swapped[4], swapped[7]);
    std::swap(swapped[5], swapped[6]);
    assert(!rstb_parse(swapped.data(), swapped.size(), back));
    assert(resource_name("Actor/Pack/X.sbactorpack") == "Actor/Pack/X.bactorpack");
    assert(resource_name("GameData/gamedata.ssarc") == "GameData/gamedata.sarc");
    assert(resource_name("Model/A.Tex1.sbfres") == "Model/A.Tex1.bfres");
    assert(resource_name("Pack/TitleBG.pack") == "Pack/TitleBG.pack" && resource_name("x/a.sarc") == "x/a.sarc");
    assert(resource_name("dir.s/file") == "dir.s/file");
    assert(grown_entry(1300, 1000, 900) == 1300);           // smaller: as it was
    assert(grown_entry(1300, 1000, 2000) == 2016 + 300);    // simple: rounded size plus the same overhead
    assert(grown_entry(9000, 1000, 2000) == 9000 + 11250);  // large overhead: the growth at 9x, and a quarter more
}

// ---------------------------------------------------------------- AAMP
Bytes le(uint32_t v) { return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; }
aamp::Param F(const char* name, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    return {aamp::hash(name), aamp::kF32, le(u)};
}
aamp::Param I(const char* name, int32_t v) { return {aamp::hash(name), aamp::kInt, le(uint32_t(v))}; }
aamp::Param S(const char* name, const std::string& v, uint8_t type = aamp::kString64) { return {aamp::hash(name), type, bytes(v)}; }
aamp::Object O(const char* name, std::vector<aamp::Param> params) { return {aamp::hash(name), std::move(params)}; }
aamp::List L(const char* name, std::vector<aamp::List> lists, std::vector<aamp::Object> objects) {
    return {aamp::hash(name), std::move(lists), std::move(objects)};
}
Bytes aamp_bytes(const aamp::List& root, const char* type = "xml") {
    aamp::File f;
    f.type = type;
    f.root = root;
    Bytes out;
    std::string why;
    const bool ok = aamp::write(f, out, &why);
    if (!ok) fprintf(stderr, "aamp write: %s\n", why.c_str());
    assert(ok);
    return out;
}
aamp::List aamp_of(const Bytes& b) {
    aamp::File f;
    std::string why;
    const bool ok = aamp::parse(b.data(), b.size(), f, &why);
    if (!ok) fprintf(stderr, "aamp parse: %s\n", why.c_str());
    assert(ok);
    return f.root;
}
const aamp::Param& value(const aamp::List& l, const char* object, const char* param) {
    const aamp::Object* o = aamp::object(l, aamp::hash(object));
    assert(o);
    const aamp::Param* p = aamp::param(*o, aamp::hash(param));
    assert(p);
    return *p;
}
// a BCML log: FileTable names the files, each file's changes are the list named by the CRC-32 of its name
Bytes bcml_log(const std::vector<std::pair<std::string, aamp::List>>& files) {
    aamp::List root{aamp::hash("param_root"), {}, {}};
    aamp::Object table{aamp::hash("FileTable"), {}};
    for (size_t i = 0; i < files.size(); i++) {
        table.params.push_back({aamp::hash("File" + std::to_string(i)), aamp::kStringRef, bytes(files[i].first)});
        aamp::List diff = files[i].second;
        diff.name = aamp::hash(files[i].first);
        root.lists.push_back(diff);
    }
    root.objects.push_back(table);
    return aamp_bytes(root, "");
}

void test_aamp() {
    // every type reads back as written, and writing is stable
    aamp::List root = L("param_root", {L("Child", {L("Grandchild", {}, {O("Deep", {I("N", -5)})})}, {O("Empty", {})})},
                        {O("Values", {{aamp::hash("bool"), aamp::kBool, le(1)}, F("f32", 2.5f), I("int", 7),
                                      {aamp::hash("vec2"), aamp::kVec2, Bytes(8, 1)}, {aamp::hash("vec3"), aamp::kVec3, Bytes(12, 2)},
                                      {aamp::hash("vec4"), aamp::kVec4, Bytes(16, 3)}, {aamp::hash("color"), aamp::kColor, Bytes(16, 4)},
                                      S("s32", "short", aamp::kString32), S("s64", "a longer string"),
                                      {aamp::hash("curve"), aamp::kCurve1, Bytes(0x80, 5)}, {aamp::hash("curve4"), aamp::kCurve4, Bytes(0x200, 6)},
                                      {aamp::hash("ints"), aamp::kBufferInt, Bytes(12, 7)}, {aamp::hash("none"), aamp::kBufferF32, {}},
                                      S("s256", "path/to/something", aamp::kString256), {aamp::hash("quat"), aamp::kQuat, Bytes(16, 8)},
                                      {aamp::hash("u32"), aamp::kU32, le(0xFFFFFFFE)}, {aamp::hash("u32s"), aamp::kBufferU32, Bytes(8, 9)},
                                      {aamp::hash("bin"), aamp::kBufferBinary, Bytes{1, 2, 3, 4, 5}}, S("ref", "", aamp::kStringRef)})});
    const Bytes b = aamp_bytes(root);
    assert(!memcmp(b.data(), "AAMP", 4) && b.size() % 4 == 0);
    const aamp::List back = aamp_of(b);
    assert(back == root && aamp_bytes(back) == b);
    assert(aamp::text(value(back, "Values", "s64")) == "a longer string" && value(back, "Values", "bin").data.size() == 5);
    Bytes bad = b;
    bad.resize(0x40);
    aamp::File f;
    assert(!aamp::parse(bad.data(), bad.size(), f) && !aamp::parse(b.data(), 0x20, f));
    // deep merge: values set, new parameters, objects and lists added, the rest kept
    aamp::List target = L("param_root", {L("L1", {}, {O("B", {I("z", 3)})})}, {O("A", {I("x", 1), I("y", 2)})});
    const aamp::List diff = L("param_root", {L("L1", {L("L2", {}, {O("D", {I("k", 9)})})}, {O("B", {I("z", 4)})}), L("L3", {}, {})},
                              {O("A", {I("y", 5), I("w", 7)}), O("C", {I("q", 1)})});
    aamp::merge(target, diff);
    assert(target.objects.size() == 2 && target.objects[0].params.size() == 3);
    assert(value(target, "A", "x") == I("x", 1) && value(target, "A", "y") == I("y", 5) && value(target, "A", "w") == I("w", 7));
    assert(value(target, "C", "q") == I("q", 1) && target.lists.size() == 2);
    const aamp::List* l1 = aamp::list(target, aamp::hash("L1"));
    assert(l1 && value(*l1, "B", "z") == I("z", 4) && aamp::list(*l1, aamp::hash("L2")));
    // AS lists: AddReses by Anim (added after the game's), ASDefines by Name, CFDefines by their CFPre's Name
    aamp::List aslist = L("param_root",
                          {L("AddReses", {}, {O("AddRes_0", {S("Anim", "Player_Animation")}), O("AddRes_1", {S("Anim", "Motorcycle_Animation")})}),
                           L("ASDefines", {}, {O("ASDefine_0", {S("Name", "Wait"), S("Filename", "Player_Wait")})}),
                           L("CFDefines", {L("CFDefine_0", {L("CFPosts", {}, {})}, {O("CFPre", {S("Name", "Jump", aamp::kString32)})})}, {})},
                          {});
    const aamp::List asdiff = L("param_root",
                                {L("AddReses", {}, {O("AddRes_0", {S("Anim", "Linkle_Extra_Animation")}), O("AddRes_1", {S("Anim", "Player_Animation")})}),
                                 L("ASDefines", {}, {O("ASDefine_0", {S("Name", "Wait"), S("Filename", "Linkle_Wait")}),
                                                     O("ASDefine_1", {S("Name", "Twirl"), S("Filename", "Linkle_Twirl")})}),
                                 L("CFDefines", {L("CFDefine_0", {}, {O("CFPre", {S("Name", "Run", aamp::kString32)})}),
                                                 L("CFDefine_1", {}, {O("CFPre", {S("Name", "Jump", aamp::kString32)}),
                                                                      O("CFExcepts", {S("Name_0", "Freeze", aamp::kString32)})})}, {})},
                                {});
    aamp::merge_aslist(aslist, asdiff);
    const aamp::List* adds = aamp::list(aslist, aamp::hash("AddReses"));
    assert(adds && adds->objects.size() == 3 && aamp::text(value(*adds, "AddRes_0", "Anim")) == "Player_Animation");
    assert(aamp::text(value(*adds, "AddRes_2", "Anim")) == "Linkle_Extra_Animation");
    const aamp::List* defs = aamp::list(aslist, aamp::hash("ASDefines"));
    assert(defs && defs->objects.size() == 2 && aamp::text(value(*defs, "ASDefine_0", "Filename")) == "Linkle_Wait");
    assert(aamp::text(value(*defs, "ASDefine_1", "Name")) == "Twirl");
    const aamp::List* cfs = aamp::list(aslist, aamp::hash("CFDefines"));
    assert(cfs && cfs->lists.size() == 2);
    const aamp::List* jump = aamp::list(*cfs, aamp::hash("CFDefine_0"));
    assert(jump && aamp::text(value(*jump, "CFExcepts", "Name_0")) == "Freeze" && aamp::list(*jump, aamp::hash("CFPosts")));
    const aamp::List* run = aamp::list(*cfs, aamp::hash("CFDefine_1"));
    assert(run && aamp::text(value(*run, "CFPre", "Name")) == "Run");
}

// ---------------------------------------------------------------- BYML and its YAML text
byml::Value S(const std::string& s) { return byml::Value::string(s); }
byml::Value N(int32_t v) { return byml::Value::s32(v); }
byml::Value Bo(bool b) { return byml::Value::boolean(b); }
byml::Value Fl(float f) { return byml::Value::f32(f); }
byml::Value Ar(std::vector<byml::Value> items) {
    byml::Value v = byml::Value::new_array();
    v.array = std::move(items);
    return v;
}
byml::Value Ha(std::vector<std::pair<std::string, byml::Value>> items) {
    byml::Value v = byml::Value::new_hash();
    for (auto& [k, x] : items) v.set(k, std::move(x));
    return v;
}
Bytes byml_bytes(const byml::Value& root, uint16_t version = 2, bool big = true) {
    byml::Document d;
    d.root = root;
    d.version = version;
    d.big_endian = big;
    Bytes out;
    std::string why;
    const bool ok = byml::write(d, out, &why);
    if (!ok) fprintf(stderr, "byml write: %s\n", why.c_str());
    assert(ok);
    return out;
}
byml::Value byml_of(const Bytes& b) {
    byml::Document d;
    std::string why;
    const Bytes u = is_yaz0(b.data(), b.size()) ? unyaz(b) : b;
    const bool ok = byml::parse(u.data(), u.size(), d, &why);
    if (!ok) fprintf(stderr, "byml parse: %s\n", why.c_str());
    assert(ok);
    return d.root;
}
byml::Value yaml(const std::string& text) {
    byml::Value v;
    std::string why;
    const bool ok = byml::parse_text(text, v, &why);
    if (!ok) fprintf(stderr, "yaml: %s\n", why.c_str());
    assert(ok);
    return v;
}

void test_byml() {
    // every type reads back as written, either byte order; 64-bit values in version 3
    const byml::Value shared = Ar({Fl(1.5f), Fl(-2.0f)});
    const byml::Value root = Ha({{"null", byml::Value::null()}, {"str", S("text")}, {"bool", Bo(true)}, {"int", N(-7)}, {"float", Fl(0.25f)},
                                 {"uint", byml::Value::u32(0xFFFFFFF0u)}, {"s64", byml::Value::s64(-5000000000ll)},
                                 {"u64", byml::Value::u64(0xFFFFFFFFFFull)}, {"f64", byml::Value::f64(3.25)},
                                 {"list", Ar({S("a"), N(1), Ha({{"k", S("v")}}), Ar({})})}, {"a", shared}, {"b", shared}, {"empty", Ha({})}});
    for (bool big : {true, false}) {
        const Bytes b = byml_bytes(root, 3, big);
        assert(byml::is_byml(b.data(), b.size()) && byml_of(b) == root);
        byml::Document d;
        assert(byml::parse(b.data(), b.size(), d) && d.version == 3 && d.big_endian == big);
        assert(byml_bytes(byml_of(b), 3, big) == b);  // stable
    }
    // equal containers are stored once (as the game's files): "a" and "b" share their array
    const Bytes one = byml_bytes(Ha({{"a", shared}, {"b", shared}}));
    const Bytes two = byml_bytes(Ha({{"a", shared}, {"b", Ar({Fl(1.5f), Fl(-3.0f)})}}));
    assert(one.size() < two.size() && one.size() % 4 == 0);
    // keys in order, values found by key
    const byml::Value h = Ha({{"zeta", N(1)}, {"alpha", N(2)}, {"mid", N(3)}});
    assert(h.hash[0].first == "alpha" && h.hash[2].first == "zeta" && h.find("mid")->as_s32() == 3 && !h.find("none"));
    // bad data: refused, not read out of range
    Bytes b = byml_bytes(root);
    byml::Document d;
    for (size_t cut : {size_t(0), size_t(8), size_t(15), b.size() / 2}) assert(!byml::parse(b.data(), cut, d));
    Bytes bad = b;
    put32(bad.data() + 12, 0x7FFFFFF0);  // root out of range
    assert(!byml::parse(bad.data(), bad.size(), d));
    bad = b;
    bad[3] = 9;  // version 9
    assert(!byml::parse(bad.data(), bad.size(), d) && !byml::is_byml(bad.data(), bad.size()));

    // oead's YAML text, as BCML's logs have it
    const byml::Value t = yaml(R"(Npc_A<Talk>:
  exist_enable_camera_input: true
  mode: Seamless
  subfile:
  - {file: InitTalk.bfevfl}
  - {file: 'Common.bfevfl'}
SignalFlowchart<<Check>: {disable_event_start_interval: true, is_timeline: false, mode: Seamless}
numbers: {f: 1.5, neg: -2.0, i: -12, hex: 0x10, u: !u 0xFFFFFFFF, l: !l -5, ul: !ul 0x10, d: !f64 2.5, e: 1.0e-05, big: 4294967295}
strings: ['true', "a: b", '123', "it's \"quoted\" \u00e9\n", plain text with spaces, 'it''s', ~, null]
nested:
  - - 1
    - 2
  - key: value  # a comment
    other: [1, 2.0, 'x', {a: [b]}]
  -
  - []
  - {}
'quoted key': 1
empty:
block: |-
  line one
  line two
last: x
)");
    const byml::Value& a = *t.find("Npc_A<Talk>");
    assert(a.find("exist_enable_camera_input")->is(byml::Type::Bool) && a.str("mode") == "Seamless");
    assert(a.find("subfile")->array.size() == 2 && a.find("subfile")->array[1].str("file") == "Common.bfevfl");
    const byml::Value& sig = *t.find("SignalFlowchart<<Check>");
    assert(sig.hash.size() == 3 && sig.find("is_timeline")->bits == 0);
    const byml::Value& n = *t.find("numbers");
    assert(*n.find("f") == Fl(1.5f) && *n.find("neg") == Fl(-2.0f) && *n.find("i") == N(-12) && *n.find("hex") == N(16));
    assert(*n.find("u") == byml::Value::u32(0xFFFFFFFFu) && *n.find("l") == byml::Value::s64(-5) && *n.find("ul") == byml::Value::u64(16));
    assert(*n.find("d") == byml::Value::f64(2.5) && *n.find("e") == Fl(1.0e-05f) && *n.find("big") == N(-1));  // s32 wraps, as oead
    const auto& s = t.find("strings")->array;
    assert(s.size() == 8 && s[0] == S("true") && s[1] == S("a: b") && s[2] == S("123") && s[3] == S("it's \"quoted\" \xC3\xA9\n"));
    assert(s[4] == S("plain text with spaces") && s[5] == S("it's") && s[6].is(byml::Type::Null) && s[7].is(byml::Type::Null));
    const auto& nested = t.find("nested")->array;
    assert(nested.size() == 5 && nested[0] == Ar({N(1), N(2)}) && nested[1].str("key") == "value");
    assert(*nested[1].find("other") == Ar({N(1), Fl(2.0f), S("x"), Ha({{"a", Ar({S("b")})}})}));
    assert(nested[2].is(byml::Type::Null) && nested[3] == Ar({}) && nested[4] == Ha({}));
    assert(*t.find("quoted key") == N(1) && t.find("empty")->is(byml::Type::Null) && t.str("block") == "line one\nline two" && t.str("last") == "x");
    byml::Value v;
    assert(byml::parse_text("", v) && v.is(byml::Type::Null));
    assert(byml::parse_text("- a\n- b\n", v) && v == Ar({S("a"), S("b")}));
    assert(!byml::parse_text("a: 'open\n", v) && !byml::parse_text("a:\n  b: 1\n c: 2\n", v) && !byml::parse_text("a: !u x\n", v));
    assert(!byml::parse_text("just a scalar", v));
}

// ---------------------------------------------------------------- MSBT texts
Bytes u16s(std::initializer_list<uint16_t> units) {
    Bytes b;
    for (uint16_t u : units) {
        b.push_back(uint8_t(u >> 8));
        b.push_back(uint8_t(u));
    }
    return b;
}
json::Value contents(const std::string& j) { return json::parse(j); }

void test_msbt() {
    std::string why;
    // texts: control sequences as msyt writes them (0x0E, group, type, fields)
    Bytes raw;
    assert(msbt::encode(contents(R"([{"text": "Hi "}, {"control": {"kind": "set_colour", "colour": "blue"}}, {"text": "you"},
        {"control": {"kind": "reset_colour"}}, {"control": {"kind": "pause", "length": "longer"}},
        {"control": {"kind": "choice", "choice_labels": [0, 1, 2], "selected_index": 0, "cancel_index": 2, "unknown": 8}},
        {"control": {"kind": "sound", "unknown": [0, 4]}}, {"control": {"kind": "sound2", "unknown": [7, 205]}}])"),
                        true, raw, &why));
    const Bytes want = u16s({'H', 'i', ' ', 0x0E, 0, 3, 2, 2, 'y', 'o', 'u', 0x0E, 0, 3, 2, 0xFFFF, 0x0E, 5, 2, 0, 0x0E, 1, 5, 8, 0, 1, 2, 0x0002,
                             0x0E, 3, 1, 2, 0x0004, 0x0E, 4, 1, 2, 0x07CD, 0});
    assert(raw == want);
    // every kind decodes back to what encodes it (msyt's high-level kinds and its raw ones)
    const std::string all = R"([{"text": "\u00e9t\u00e9 \ud83d\ude00"}, {"control": {"kind": "pause", "frames": 30}},
        {"control": {"kind": "icon", "icon": "b"}}, {"control": {"kind": "icon", "icon": {"a": 10}}},
        {"control": {"kind": "variable", "variable_kind": 11, "name": "Cmn_Money"}},
        {"control": {"kind": "choice", "choice_labels": [5, 6], "selected_index": 1, "cancel_index": 1, "unknown": 6}},
        {"control": {"kind": "choice", "choice_labels": [1, 2, 3, 4], "selected_index": 0, "cancel_index": 3, "unknown": 10}},
        {"control": {"kind": "single_choice", "label": 7}}, {"control": {"kind": "animation", "name": "Wait"}},
        {"control": {"kind": "text_size", "percent": 125}}, {"control": {"kind": "auto_advance", "frames": 60}},
        {"control": {"kind": "localisation", "localisation_kind": "gender", "options": ["he", "", "they"]}},
        {"control": {"kind": "font", "font_kind": "hylian"}},
        {"control": {"kind": "raw", "zero": {"zero": {"field_1": 1, "field_2": 2, "field_3": 3}}}},
        {"control": {"kind": "raw", "zero": {"four": {"field_1": 9}}}},
        {"control": {"kind": "raw", "one": {"two": {"field_1": 4}}}},
        {"control": {"kind": "raw", "one": {"eight": {"unknown_1": [[255, 255, 0, 0]], "field_1": [3, 4], "field_2": [1, 2, 3, 4]}}}},
        {"control": {"kind": "raw", "one": {"nine": {"unknown_1": null, "strings": [{"field_1": 1, "string": "a"}, {"field_1": 2, "string": ""},
            {"field_1": 3, "string": "cd"}, {"field_1": 4, "string": "e"}], "field_3": 5, "field_4": 6, "unknown_2": null, "field_6": [1, 205]}}}},
        {"control": {"kind": "raw", "two": {"one_field": [3, {"field_1": 7}]}}},
        {"control": {"kind": "raw", "two": {"variable": [1, {"field_1": 12, "string": "Name", "field_3": 2}]}}},
        {"control": {"kind": "raw", "three": {"field_1": 2, "field_2": [1, 2]}}},
        {"control": {"kind": "raw", "four": {"zero": {"field_1": 4, "string": "abc"}}}},
        {"control": {"kind": "raw", "four": {"three": {"field_1": 1}}}},
        {"control": {"kind": "raw", "five": {"field_1": 7, "field_2": 1}}},
        {"control": {"kind": "raw", "two_hundred_one": {"dynamic": [0, {"len": 4, "field_2": [1, 2, 3, 4]}]}}},
        {"control": {"kind": "raw", "two_hundred_one": {"one_field": [2, {"field_1": 3}]}}}, {"text": "end"}])";
    Bytes text;
    assert(msbt::encode(contents(all), true, text, &why));
    json::Value back;
    Bytes again;
    assert(msbt::decode(text, true, back, &why) && msbt::encode(back, true, again, &why) && again == text);
    assert(back.array.size() == contents(all).array.size() && back.array[2].get("control").get("icon").string() == "b");
    assert(!msbt::encode(contents(R"([{"control": {"kind": "set_colour", "colour": "purple"}}])"), true, text, &why));
    assert(!msbt::encode(contents(R"([{"control": {"kind": "choice", "choice_labels": [1], "selected_index": 0, "cancel_index": 0, "unknown": 4}}])"),
                         true, text, &why));
    // files: labels in hash buckets, attribute strings in EventFlowMsg files; written back byte for byte
    assert(msbt::label_hash("Talk013_Voe", 41) == 21 && msbt::label_hash("Talk_New", 3) == 2);
    msbt::File f = msbt::make("EventFlowMsg/Npc.msbt", true);
    f.groups = 3;
    msbt::set(f, "Talk_00", u16s({'a', 0}), msbt::encode_string("Npc", true, true));
    msbt::set(f, "Talk_New", u16s({'b', 0}), {});
    msbt::set(f, "Item_Name", u16s({'c', 0}), msbt::encode_string("Npc", true, true));
    Bytes file;
    assert(msbt::write(f, file, &why) && file.size() % 16 == 0 && !memcmp(file.data(), "MsgStdBn", 8));
    msbt::File g;
    assert(msbt::parse(file.data(), file.size(), g, &why) && g.entries.size() == 3 && g.atr_strings && g.atr_size == 4);
    assert(msbt::find(g, "Talk_New")->text == u16s({'b', 0}) && msbt::find(g, "Talk_New")->attribute == u16s({0}));
    assert(msbt::find(g, "Item_Name")->bucket == msbt::label_hash("Item_Name", 3) && msbt::find(g, "Talk_00")->attribute == u16s({'N', 'p', 'c', 0}));
    Bytes file2;
    assert(msbt::write(g, file2, &why) && file2 == file);
    msbt::set(g, "Talk_00", u16s({'z', 'z', 0}), {});  // no attribute: an empty string
    assert(msbt::find(g, "Talk_00")->attribute == u16s({0}) && msbt::find(g, "Talk_00")->text == u16s({'z', 'z', 0}));
    const msbt::File plain = msbt::make("ActorType/Item.msbt", true);
    assert(!plain.atr_strings && plain.atr_size == 0);
    Bytes cut(file.begin(), file.begin() + 40);
    assert(!msbt::parse(cut.data(), cut.size(), g, &why));
}

// ---------------------------------------------------------------- merging
struct World {
    fs::path root, update, base, aoc, out;
    merge::Vanilla vanilla() const { return {update / "content", base / "content", aoc / "content"}; }
};
const std::string kSelf = "System/Resource/ResourceSizeTable.product.rsizetable";

Bytes actor(const std::string& link, const std::string& physics) {
    return yaz(sarc({{"Actor/ActorLink/A.bxml", bytes(link)}, {"Actor/Physics/A.bphysics", bytes(physics)}}), 0x80);
}
std::map<std::string, Bytes> test_pack(const std::string& t, const Bytes& a, const Bytes& m) {
    return {{"Actor/Pack/A.sbactorpack", a}, {"Model/M.sbfres", m}, {"Text/T.txt", bytes(t)}};
}
Bytes physics() {
    return aamp_bytes(L("param_root", {L("ParamSet", {L("Cloth", {}, {O("Header", {I("num", 4)})})}, {})}, {}));
}
Bytes aslist() {
    return aamp_bytes(L("param_root", {L("AddReses", {}, {O("AddRes_0", {S("Anim", "Player_Animation")})})}, {}));
}
Rstb game_table() {
    Rstb t;
    rstb_set(t, "Actor/Physics/P.bphysics", 1000);
    rstb_set(t, "Actor/Pack/P.bactorpack", 2000);
    rstb_set(t, "Actor/ASList/P.baslist", 900);
    rstb_set(t, "Text/T.txt", 400);
    rstb_set(t, "Actor/ActorLink/A.bxml", 300);
    rstb_set(t, "Model/M.bfres", 100000);
    rstb_set(t, "Actor/Pack/A.bactorpack", 1000);
    rstb_set(t, "Loose/L.bin", 500);
    rstb_set(t, "Aoc/0010/Map/MainField/X.mubin", 200);
    rstb_set(t, kSelf, 0);
    rstb_set(t, kSelf, uint32_t(rstb_write(t).size() + 296));
    return t;
}
World make_world(const fs::path& root) {
    World w{root, root / "game" / "update", root / "game" / "base", root / "game" / "aoc", root / "out"};
    put(w.update / "content/Pack/Test.pack", sarc(test_pack("text-vanilla", actor("link-v", "phys-v"), yaz(repeat("model-vanilla", 50)))));
    put(w.update / "content/Pack/Other.pack", sarc({{"Other/O.txt", bytes("o-vanilla")}}));
    put(w.update / "content/Pack/Third.pack", sarc({{"Third/3.txt", bytes("3-vanilla")}}));
    put(w.update / "content/Pack/Fourth.pack",
        sarc({{"F/1.txt", bytes("one")}, {"F/2.txt", bytes("two")}, {"Actor/Pack/A.sbactorpack", actor("link-v", "phys-v")}}));
    put(w.update / "content/Pack/Fifth.pack", sarc({{"F/5.txt", bytes("five")}}));
    put(w.update / "content/Pack/Params.pack", sarc({{"Actor/Pack/P.sbactorpack", yaz(sarc({{"Actor/Physics/P.bphysics", physics()},
                                                                                         {"Actor/ASList/P.baslist", aslist()}}))}}));
    put(w.base / "content/Actor/Loose.bgparamlist", aamp_bytes(L("param_root", {}, {O("General", {I("Life", 10)})})));
    put(w.update / "content/System/Resource/ResourceSizeTable.product.srsizetable", yaz(rstb_write(game_table())));
    put(w.base / "content/Loose/L.bin", "loose-vanilla");
    put(w.aoc / "content/0010/Pack/AocPack.pack", sarc({{"Map/MainField/X.smubin", yaz(bytes("dlc-map"))}}));
    fs::create_directories(w.out);
    return w;
}
// the mods, lowest first; their folders below root/mods
std::vector<merge::Mod> make_mods(const World& w) {
    const fs::path m = w.root / "mods";
    // m1: Test.pack with a longer text, a loose file, its own size table
    put(m / "m1/content/Pack/Test.pack", sarc(test_pack("text-from-mod-one, longer", actor("link-v", "phys-v"), yaz(repeat("model-vanilla", 50)))));
    put(m / "m1/content/Loose/L.bin", "loose-m1");
    Rstb t1 = game_table();
    rstb_set(t1, "Text/T.txt", 777);
    rstb_erase(t1, "Loose/L.bin");
    put(m / "m1/content/System/Resource/ResourceSizeTable.product.srsizetable", yaz(rstb_write(t1)));
    put(m / "m1/content/.DS_Store", "finder");
    // m2: a changed file inside the actor pack in Test.pack (recompressed: the other file is the same data),
    // a loose model the game only has in Test.pack, a DLC map the game only has in AocPack.pack
    put(m / "m2/content/Pack/Test.pack",
        sarc(test_pack("text-vanilla", actor("link-from-mod-two-which-is-longer", "phys-v"), yaz(repeat("model-vanilla", 50)))));
    put(m / "m2/content/Model/M.sbfres", yaz(repeat("model-from-mod-two", 100)));
    put(m / "m2/aoc/0010/Map/MainField/X.smubin", yaz(bytes("dlc-map-mod2")));
    // m5 (below m3): a little-endian copy of Other.pack, which can't be merged
    Bytes little = sarc({{"Other/O.txt", bytes("o-switch")}});
    little[6] = 0xFF;
    little[7] = 0xFE;
    put(m / "m5/content/Pack/Other.pack", little);
    put(m / "m5/content/Pack/Fifth.pack", little);  // alone: not used at all
    // m3 (top): the loose file, Other.pack, Third.pack, and the text in Test.pack
    put(m / "m3/content/Loose/L.bin", "loose-m3");
    put(m / "m3/content/Pack/Other.pack", sarc({{"Other/O.txt", bytes("o-m3")}}));
    put(m / "m3/content/Pack/Third.pack", sarc({{"Third/3.txt", bytes("3-m3, a longer file")}}));
    put(m / "m3/content/Pack/Test.pack", sarc(test_pack("text-m3", actor("link-v", "phys-v"), yaz(repeat("model-vanilla", 50)))));
    // and a partial Fourth.pack, as BCML packs one: only the files it changes, also in the actor pack
    put(m / "m3/content/Pack/Fourth.pack",
        sarc({{"F/2.txt", bytes("two-m3")}, {"Actor/Pack/A.sbactorpack", yaz(sarc({{"Actor/ActorLink/A.bxml", bytes("partial-link")}}))}}));
    std::vector<merge::Mod> mods;
    for (const char* id : {"m1", "m2", "m5", "m3"}) {
        merge::Mod mod{id, std::string("Mod ") + id, m / id / "content", {}};
        if (fs::exists(m / id / "aoc")) mod.aoc = m / id / "aoc";
        mods.push_back(mod);
    }
    return mods;
}

void test_merge(const fs::path& root) {
    const World w = make_world(root);
    const std::vector<merge::Mod> mods = make_mods(w);
    const merge::Result r = merge::build(w.vanilla(), mods, w.out);
    auto file = [&](const char* tree) {
        auto it = r.files.find(merge::tree_key(tree));
        assert(it != r.files.end());
        return fs::path(it->second);
    };
    // Test.pack: merged from m1, m2, m3 and m2's loose model
    const fs::path test = file("content/Pack/Test.pack");
    assert(test.string().rfind(w.out.string(), 0) == 0);
    const auto pack = unsarc(get(test));
    assert(pack.size() == 3 && text(pack.at("Text/T.txt")) == "text-m3");  // m3 over m1
    assert(unyaz(pack.at("Model/M.sbfres")) == repeat("model-from-mod-two", 100));
    const Bytes actor_pack = pack.at("Actor/Pack/A.sbactorpack");
    assert(yaz0_alignment(actor_pack.data(), actor_pack.size()) == 0x80);  // the game's header word kept
    const auto inner = unsarc(unyaz(actor_pack));
    assert(text(inner.at("Actor/ActorLink/A.bxml")) == "link-from-mod-two-which-is-longer" && text(inner.at("Actor/Physics/A.bphysics")) == "phys-v");
    // loose files: the top mod's; Third.pack only m3 has: its own file; Other.pack: m5's can't be merged
    assert(file("content/Loose/L.bin") == w.root / "mods/m3/content/Loose/L.bin");
    assert(file("content/Pack/Third.pack") == w.root / "mods/m3/content/Pack/Third.pack");
    assert(text(unsarc(get(file("content/Pack/Other.pack"))).at("Other/O.txt")) == "o-m3");
    assert(file("content/Model/M.sbfres") == w.root / "mods/m2/content/Model/M.sbfres");
    assert(text(unyaz(unsarc(get(file("aoc/0010/Pack/AocPack.pack"))).at("Map/MainField/X.smubin"))) == "dlc-map-mod2");
    assert(r.files.count(merge::tree_key("content/.DS_Store")) == 0);
    // a partial pack of one mod goes into the game's copy (its partial actor pack too); a little-endian one is not used
    const auto fourth = unsarc(get(file("content/Pack/Fourth.pack")));
    assert(fourth.size() == 3 && text(fourth.at("F/1.txt")) == "one" && text(fourth.at("F/2.txt")) == "two-m3");
    const auto fourth_actor = unsarc(unyaz(fourth.at("Actor/Pack/A.sbactorpack")));
    assert(text(fourth_actor.at("Actor/ActorLink/A.bxml")) == "partial-link" && text(fourth_actor.at("Actor/Physics/A.bphysics")) == "phys-v");
    assert(r.files.count(merge::tree_key("content/Pack/Fifth.pack")) == 0 && has_note(r.mods[2], "Pack/Fifth.pack is not used"));
    assert(has_note(r.mods[3], "Pack/Fourth.pack has only the files it changes"));
    assert(r.merged_archives == 4);  // Test.pack, Other.pack, AocPack.pack, Fourth.pack
    // the size table
    const Rstb t = table_of(file(merge::kSizeTable));
    const Rstb game = game_table();
    assert(entry(t, "Text/T.txt") == 777);                 // m1's own value (m3's text is shorter than the game's)
    assert(entry(t, "Actor/ActorLink/A.bxml") == 64 + 294);  // grew: rounded size plus the game's overhead
    assert(entry(t, "Model/M.bfres") == 321154);           // a large overhead: 1150 bytes more at 100000 / 650, a quarter more
    assert(entry(t, "Aoc/0010/Map/MainField/X.mubin") == 32 + 193);
    assert(entry(t, "Loose/L.bin") == 500);                // m3's is smaller; m1 (whose table drops it) lost
    assert(entry(t, "Actor/Pack/A.bactorpack") >= 1000);
    assert(entry(t, kSelf) >= entry(game, kSelf));
    assert(r.size_entries >= 4);
    // what the Mods page says
    assert(r.mods.size() == 4 && r.mods[0].files == 3 && r.mods[3].merged == 3);  // .DS_Store not counted
    assert(r.mods[0].overridden == 1 && r.mods[0].merged == 2 && has_note(r.mods[0], "2 files are used from Mod m3"));
    assert(has_note(r.mods[0], "Pack/Test.pack: merged"));
    assert(has_note(r.mods[1], "go into Pack/Test.pack") || has_note(r.mods[1], "goes into Pack/Test.pack"));
    assert(has_note(r.mods[2], "can't be merged"));
    assert(r.mods[3].replaced == 2);  // L.bin and Third.pack
}

// a .bnp's merge data: parameter changes in a nested archive and in a loose file, an AS list, sizes
void test_bcml(const fs::path& root) {
    const World w = make_world(root);
    const fs::path m = w.root / "mods";
    put(m / "bnp/logs/deepmerge.aamp",
        bcml_log({{"content/Pack/Params.pack//Actor/Pack/P.sbactorpack//Actor/Physics/P.bphysics",
                   L("", {L("ParamSet", {L("Cloth", {}, {O("Header", {I("num", 5)}), O("Cloth_4", {S("Bone", "Bust_Havok", aamp::kString256)})})}, {})}, {})},
                  {"content/Actor/Loose.bgparamlist", L("", {}, {O("General", {I("Life", 30)})})},
                  {"content/Pack/Params.pack//Actor/Pack/Missing.sbactorpack//Actor/X.bxml", L("", {}, {})}}));
    put(m / "bnp/logs/aslist.aamp",
        bcml_log({{"content/Pack/Params.pack//Actor/Pack/P.sbactorpack//Actor/ASList/P.baslist",
                   L("", {L("AddReses", {}, {O("AddRes_0", {S("Anim", "Linkle_Extra_Animation")})})}, {})}}));
    // sizes: BCML's for its own rebuild of P.sbactorpack (too small for ours: The Linkle Mod's GameROMPlayer
    // loaded forever, 2026-11-06), and a dropped entry for a file that is changed here
    put(m / "bnp/logs/rstb.json", "{\"Actor/Physics/P.bphysics\": 5000, \"Model/M.bfres\": 0, \"New/Thing.bin\": 77, "
                                  "\"Actor/Pack/P.bactorpack\": 10, \"Actor/ASList/P.baslist\": 0}");
    put(m / "bnp/content/New/Thing.bin", "new thing");
    const merge::Result r = merge::build(w.vanilla(), {merge::Mod{"bnp", "A BCML mod", m / "bnp/content", {}, m / "bnp/logs"}}, w.out);
    auto file = [&](const char* tree) {
        auto it = r.files.find(merge::tree_key(tree));
        assert(it != r.files.end());
        return fs::path(it->second);
    };
    const auto params = unsarc(get(file("content/Pack/Params.pack")));
    const auto actor = unsarc(unyaz(params.at("Actor/Pack/P.sbactorpack")));
    const aamp::List phys = aamp_of(actor.at("Actor/Physics/P.bphysics"));
    const aamp::List* cloth = aamp::list(*aamp::list(phys, aamp::hash("ParamSet")), aamp::hash("Cloth"));
    assert(cloth && value(*cloth, "Header", "num") == I("num", 5) && aamp::text(value(*cloth, "Cloth_4", "Bone")) == "Bust_Havok");
    const aamp::List as = aamp_of(actor.at("Actor/ASList/P.baslist"));
    const aamp::List* adds = aamp::list(as, aamp::hash("AddReses"));
    assert(adds && adds->objects.size() == 2 && aamp::text(value(*adds, "AddRes_1", "Anim")) == "Linkle_Extra_Animation");
    const fs::path loose = file("content/Actor/Loose.bgparamlist");
    assert(loose.string().rfind(w.out.string(), 0) == 0 && value(aamp_of(get(loose)), "General", "Life") == I("Life", 30));
    const Rstb t = table_of(file(merge::kSizeTable));
    uint32_t v = 0;
    assert(entry(t, "Actor/Physics/P.bphysics") == 5000 && entry(t, "New/Thing.bin") == 77 && !rstb_get(t, "Model/M.bfres", &v));
    const size_t actor_size = unyaz(params.at("Actor/Pack/P.sbactorpack")).size();
    assert(entry(t, "Actor/Pack/P.bactorpack") >= actor_size && !rstb_get(t, "Actor/ASList/P.baslist", &v));
    assert(has_note(r.mods[0], "BCML merge data: 3 parameter files changed") && has_note(r.mods[0], "Missing.sbactorpack is not in"));
}

// BCML merge data for game data tables: events, flags, save data and texts merged over the game's copies, with a
// plain mod's own copies merged entry by entry; two languages (one only by BCML's language mapping)
byml::Value flag(const std::string& name, int32_t hash, int32_t init = 0) {
    return Ha({{"DataName", S(name)}, {"DeleteRev", N(-1)}, {"HashValue", N(hash)}, {"InitValue", N(init)}, {"IsSave", Bo(true)}, {"MaxValue", Bo(true)},
               {"MinValue", Bo(false)}, {"ResetType", N(0)}});
}
byml::Value save_entry(const std::string& name, int32_t hash) { return Ha({{"DataName", S(name)}, {"HashValue", N(hash)}}); }
byml::Value save_file(const std::string& sav, std::vector<byml::Value> entries) {
    return Ha({{"file_list", Ar({Ha({{"IsCommon", Bo(false)}, {"IsCommonAtSameAccount", Bo(false)}, {"IsSaveSecureCode", Bo(true)}, {"file_name", S(sav)}}),
                                Ar(std::move(entries))})},
               {"save_info", Ar({Ha({{"directory_num", N(8)}, {"is_build_machine", Bo(true)}, {"revision", N(18203)}})})}});
}
Bytes msbt_file(const std::string& name, std::vector<std::pair<std::string, std::string>> texts, uint32_t groups) {
    msbt::File f = msbt::make(name, true);
    f.groups = groups;
    for (const auto& [label, t] : texts) msbt::set(f, label, msbt::encode_string(t, true, true), msbt::encode_string("Npc", true, true));
    Bytes out;
    assert(msbt::write(f, out));
    return out;
}
std::string msbt_text(const Bytes& file, const std::string& label) {  // a plain text's string, or "?" if missing
    msbt::File f;
    assert(msbt::parse(file.data(), file.size(), f));
    const msbt::Entry* e = msbt::find(f, label);
    if (!e) return "?";
    json::Value c;
    assert(msbt::decode(e->text, true, c));
    std::string s;
    for (const auto& part : c.array) s += part.object.count("text") ? part.get("text").string() : "<" + part.get("control").get("kind").string() + ">";
    return s;
}
std::vector<byml::Value> flags_in(const Bytes& b) { return byml_of(b).hash[0].second.array; }

void test_bcml_tables(const fs::path& root) {
    const World w = make_world(root);
    // the game: Bootup.pack with EventInfo, gamedata and save data; text packs for two languages
    const byml::Value events = Ha({{"Ev<A>", Ha({{"mode", S("Seamless")}})}, {"Ev<B>", Ha({{"mode", S("Seamless")}, {"is_timeline", Bo(false)}})}});
    std::vector<byml::Value> full;
    for (int k = 0; k < 4096; k++) full.push_back(flag("Bulk_" + std::to_string(k), 100000 + k));
    const Bytes gamedata = sarc({{"/bool_data_0.bgdata", byml_bytes(Ha({{"bool_data", Ar({flag("Existing_Flag", 11), flag("Old_Flag", 12)})}}))},
                                 {"/revival_bool_data_0.bgdata", byml_bytes(Ha({{"bool_data", Ar(full)}}))},
                                 {"/s32_data_0.bgdata", byml_bytes(Ha({{"s32_data", Ar({flag("dummy", 1000)})}}))},
                                 {"/string32_data_0.bgdata", byml_bytes(Ha({{"string_data", Ar({flag("dummy_string", 20)})}}))}});
    const Bytes savedata = sarc({{"/saveformat_0.bgsvdata", byml_bytes(save_file("game_data.sav", {save_entry("A", -5), save_entry("B", 3), save_entry("C", 9)}))},
                                 {"/saveformat_1.bgsvdata", byml_bytes(save_file("caption.sav", {save_entry("Cap", 1)}))},
                                 {"/saveformat_2.bgsvdata", byml_bytes(save_file("option.sav", {save_entry("Opt", 2)}))}});
    const Bytes ev = byml_bytes(events);
    put(w.update / "content/Pack/Bootup.pack", sarc({{"Event/EventInfo.product.sbyml", yaz(ev)}, {"GameData/gamedata.ssarc", yaz(gamedata)},
                                                     {"GameData/savedataformat.ssarc", yaz(savedata)}, {"Other/Untouched.txt", bytes("same")}}));
    for (const char* lang : {"USen", "USfr"}) {
        const Bytes msg = sarc({{"EventFlowMsg/Npc.msbt", msbt_file("EventFlowMsg/Npc.msbt", {{"Talk_00", std::string("hello ") + lang}}, 3)},
                                {"ActorType/Item.msbt", msbt_file("ActorType/Item.msbt", {{"Item_Name", std::string("Item ") + lang}}, 2)}});
        put(w.update / (std::string("content/Pack/Bootup_") + lang + ".pack"), sarc({{std::string("Message/Msg_") + lang + ".product.ssarc", yaz(msg)}}));
    }
    Rstb t = game_table();
    rstb_set(t, "Event/EventInfo.product.byml", uint32_t(ev.size() + 264));
    rstb_set(t, "GameData/gamedata.sarc", uint32_t(gamedata.size() + 396));
    rstb_set(t, "GameData/savedataformat.sarc", uint32_t(savedata.size() + 412));
    rstb_set(t, "Message/Msg_USen.product.sarc", 100);  // smaller than the file: grows with it
    put(w.update / "content/System/Resource/ResourceSizeTable.product.srsizetable", yaz(rstb_write(t)));

    // mod A (a .bnp): logs for all four; its own EventInfo copy (the log wins); a size for EventInfo too small for the merge
    const fs::path m = w.root / "mods";
    put(m / "a/logs/eventinfo.yml", "Ev<A>:\n  mode: FromA\n  subfile:\n  - {file: A.bfevfl}\nEv<C>: {mode: New}\n");
    put(m / "a/logs/gamedata.yml",
        "bool_data:\n  add:\n    Existing_Flag:\n      DataName: Existing_Flag\n      DeleteRev: -1\n      HashValue: 11\n      InitValue: 1\n"
        "      IsSave: true\n      MaxValue: true\n      MinValue: false\n      ResetType: 0\n"
        "    Mod_Flag: {DataName: Mod_Flag, DeleteRev: -1, HashValue: 50, InitValue: 0, IsSave: true, MaxValue: true, MinValue: false, ResetType: 0}\n"
        "    MainField_Obj_X_1234: {DataName: MainField_Obj_X_1234, DeleteRev: -1, HashValue: 51, InitValue: 0, IsSave: true, MaxValue: true, "
        "MinValue: false, ResetType: 1}\n  del: [Old_Flag]\ns32_data:\n  add:\n    Mod_Count: {DataName: Mod_Count, DeleteRev: -1, HashValue: 52, "
        "InitValue: 0, IsSave: true, MaxValue: 99, MinValue: 0, ResetType: 0}\n  del: []\nf32_data:\n  add: {}\n  del: []\n");
    put(m / "a/logs/savedata.yml", "add:\n- {DataName: Mod_Flag, HashValue: 50}\n- {DataName: Neg, HashValue: -100}\ndel: [3]\n");
    put(m / "a/logs/texts.json", R"({"EUen": {"EventFlowMsg/Npc.msyt": {
        "Talk_00": {"attributes": "Npc", "contents": [{"text": "changed "}, {"control": {"kind": "pause", "length": "long"}}]},
        "Talk_New": {"attributes": "Npc", "contents": [{"text": "new line"}]}},
      "ActorType/Item.msyt": {"Item_New_Name": {"contents": [{"text": "New Item"}]}},
      "EventFlowMsg/NewNpc.msyt": {"Hello": {"attributes": "NewNpc", "contents": [{"text": "hi"}]}, "Bye": {"attributes": "NewNpc", "contents": [{"text": "bye"}]}}}})");
    put(m / "a/logs/rstb.json", "{\"Event/EventInfo.product.byml\": 10}");
    put(m / "a/content/Pack/Bootup.pack", sarc({{"Event/EventInfo.product.sbyml", byml_bytes(Ha({{"Ev<A>", Ha({{"mode", S("CopyOfA")}})}}))}}));
    // mod B (plain, higher): its own copies of EventInfo (Ev<B> changed) and of the French texts (Item_Name changed)
    byml::Value events_b = events;
    events_b.find("Ev<B>")->set("mode", S("FromB"));
    put(m / "b/content/Pack/Bootup.pack", sarc({{"Event/EventInfo.product.sbyml", yaz(byml_bytes(events_b))}}));
    const Bytes msg_fr = sarc({{"ActorType/Item.msbt", msbt_file("ActorType/Item.msbt", {{"Item_Name", "Objet de B"}}, 2)}});
    put(m / "b/content/Pack/Bootup_USfr.pack", sarc({{"Message/Msg_USfr.product.ssarc", yaz(msg_fr)}}));
    // mod C (plain, lowest): a loose copy of EventInfo (the game has it only in Bootup.pack) with an event of its own
    byml::Value events_c = events;
    events_c.set("Ev<D>", Ha({{"mode", S("FromC")}}));
    put(m / "c/content/Event/EventInfo.product.sbyml", yaz(byml_bytes(events_c)));
    const std::vector<merge::Mod> mods = {
        {"c", "Mod C", m / "c/content", {}, {}}, {"a", "Mod A", m / "a/content", {}, m / "a/logs"}, {"b", "Mod B", m / "b/content", {}, {}}};
    const auto t0 = std::chrono::steady_clock::now();
    const merge::Result r = merge::build(w.vanilla(), mods, w.out);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto file = [&](const char* tree) {
        auto it = r.files.find(merge::tree_key(tree));
        assert(it != r.files.end());
        return get(it->second);
    };
    const auto boot = unsarc(file("content/Pack/Bootup.pack"));
    assert(text(boot.at("Other/Untouched.txt")) == "same");
    // events: A's log (not its copy), B's copy's change, A's new event
    const byml::Value merged_events = byml_of(boot.at("Event/EventInfo.product.sbyml"));
    assert(is_yaz0(boot.at("Event/EventInfo.product.sbyml").data(), 16));
    assert(merged_events.find("Ev<A>")->str("mode") == "FromA" && merged_events.find("Ev<A>")->find("subfile")->array[0].str("file") == "A.bfevfl");
    assert(merged_events.find("Ev<B>")->str("mode") == "FromB" && merged_events.find("Ev<C>")->str("mode") == "New");
    assert(merged_events.find("Ev<D>")->str("mode") == "FromC" && merged_events.hash.size() == 4);
    assert(!r.files.count(merge::tree_key("content/Event/EventInfo.product.sbyml")));  // not read loose instead of the merge
    // flags: changed in place, deleted, new ones at the end of their kind; the full revival file gets a new file after it
    const auto gd = unsarc(unyaz(boot.at("GameData/gamedata.ssarc")));
    const auto bools = flags_in(gd.at("/bool_data_0.bgdata"));
    assert(bools.size() == 2 && bools[0].str("DataName") == "Existing_Flag" && *bools[0].find("InitValue") == N(1) && bools[1].str("DataName") == "Mod_Flag");
    assert(gd.at("/revival_bool_data_0.bgdata") == byml_bytes(Ha({{"bool_data", Ar(full)}})));  // untouched: the game's bytes
    const auto revival = flags_in(gd.at("/revival_bool_data_1.bgdata"));
    assert(revival.size() == 1 && revival[0].str("DataName") == "MainField_Obj_X_1234");
    assert(flags_in(gd.at("/s32_data_0.bgdata")).size() == 2 && flags_in(gd.at("/s32_data_0.bgdata"))[1].str("DataName") == "Mod_Count");
    assert(gd.size() == 5);
    // save data: added, deleted, sorted by hash (signed); the other formats keep their names and bytes
    const auto sv = unsarc(unyaz(boot.at("GameData/savedataformat.ssarc")));
    const byml::Value saved = byml_of(sv.at("/saveformat_0.bgsvdata"));
    std::vector<int32_t> hashes;
    for (const auto& e : saved.find("file_list")->array[1].array) hashes.push_back(e.find("HashValue")->as_s32());
    assert((hashes == std::vector<int32_t>{-100, -5, 9, 50}));
    assert(sv.size() == 3 && byml_of(sv.at("/saveformat_1.bgsvdata")).find("file_list")->array[0].str("file_name") == "caption.sav");
    // texts: English from A's EUen (its region differs), French too (BCML falls back to English), and B's French change
    const auto en = unsarc(unyaz(unsarc(file("content/Pack/Bootup_USen.pack")).at("Message/Msg_USen.product.ssarc")));
    assert(msbt_text(en.at("EventFlowMsg/Npc.msbt"), "Talk_00") == "changed <pause>" && msbt_text(en.at("EventFlowMsg/Npc.msbt"), "Talk_New") == "new line");
    assert(msbt_text(en.at("ActorType/Item.msbt"), "Item_New_Name") == "New Item" && msbt_text(en.at("ActorType/Item.msbt"), "Item_Name") == "Item USen");
    msbt::File npc;
    assert(msbt::parse(en.at("EventFlowMsg/NewNpc.msbt").data(), en.at("EventFlowMsg/NewNpc.msbt").size(), npc));
    assert(npc.atr_strings && npc.groups == 2 && npc.entries.size() == 2 && msbt::find(npc, "Bye")->attribute == msbt::encode_string("NewNpc", true, true));
    const auto fr = unsarc(unyaz(unsarc(file("content/Pack/Bootup_USfr.pack")).at("Message/Msg_USfr.product.ssarc")));
    assert(msbt_text(fr.at("ActorType/Item.msbt"), "Item_Name") == "Objet de B" && msbt_text(fr.at("EventFlowMsg/Npc.msbt"), "Talk_00") == "changed <pause>");
    assert(msbt_text(fr.at("ActorType/Item.msbt"), "Item_New_Name") == "New Item");
    // sizes: the rebuilt files' entries hold them (A's own EventInfo size is BCML's, too small for this merge)
    const Rstb table = table_of(w.out / merge::kSizeTable);
    assert(entry(table, "Event/EventInfo.product.byml") >= unyaz(boot.at("Event/EventInfo.product.sbyml")).size());
    assert(entry(table, "GameData/gamedata.sarc") >= unyaz(boot.at("GameData/gamedata.ssarc")).size());
    assert(entry(table, "Message/Msg_USen.product.sarc") >= unyaz(unsarc(file("content/Pack/Bootup_USen.pack")).at("Message/Msg_USen.product.ssarc")).size());
    // what the Mods page says
    assert(has_note(r.mods[1], "BCML merge data: 2 events, 5 game data flags, 3 save data flags, 5 texts in 3 files"));
    assert(has_note(r.mods[2], "Event/EventInfo.product.sbyml: its changes (1 event) are merged"));
    assert(has_note(r.mods[2], "Message/Msg_USfr.product.ssarc: its changes (texts) are merged"));
    assert(has_note(r.mods[0], "Event/EventInfo.product.sbyml: its changes (1 event) are merged") && r.mods[0].merged == 1);
    assert(r.log.size() >= 2 && r.log[1].find("Rebuilt from BCML merge data: events, game data flags, save data, texts USen, USfr") == 0);
    printf("content merge: game data tables merged in %.0f ms\n", ms);

    // the merges on their own: a full flag file gets a new one; save data that grows past a file renumbers the others
    bcml::Files game_flags{{"/bool_data_0.bgdata", byml_bytes(Ha({{"bool_data", Ar(full)}}))}}, changed;
    byml::Value add = yaml("bool_data:\n  add:\n    New_1: {DataName: New_1, HashValue: 7}\n  del: [Bulk_5]\n");
    size_t count = 0;
    std::string why;
    assert(bcml::merge_flags(game_flags, {&add}, changed, count, why) && count == 2 && changed.size() == 2);
    assert(flags_in(changed.at("/bool_data_0.bgdata")).size() == 4095 && flags_in(changed.at("/bool_data_1.bgdata")).size() == 1);
    std::vector<byml::Value> many;
    for (int k = 0; k < 8192; k++) many.push_back(save_entry("S" + std::to_string(k), k * 2));
    bcml::Files game_save{{"/saveformat_0.bgsvdata", byml_bytes(save_file("game_data.sav", many))},
                          {"/saveformat_1.bgsvdata", byml_bytes(save_file("caption.sav", {}))}},
        result;
    bcml::SaveDiff grow;
    grow.add.push_back(save_entry("Odd", 3));
    assert(bcml::merge_save(game_save, {&grow}, result, count, why) && count == 1 && result.size() == 3);
    assert(byml_of(result.at("/saveformat_0.bgsvdata")).find("file_list")->array[1].array.size() == 8192);
    assert(byml_of(result.at("/saveformat_1.bgsvdata")).find("file_list")->array[1].array.size() == 1);
    assert(result.at("/saveformat_2.bgsvdata") == game_save.at("/saveformat_1.bgsvdata"));  // caption.sav, renumbered
    bcml::SaveDiff none;
    none.add.push_back(save_entry("S1", 2));  // already there
    assert(!bcml::merge_save(game_save, {&none}, result, count, why) && why.empty());
    // BCML's language mapping
    std::map<std::string, bcml::TextFiles> langs{{"EUde", {}}, {"USen", {}}, {"JPja", {}}};
    assert(bcml::texts_for(langs, "USen") == &langs["USen"] && bcml::texts_for(langs, "USde") == &langs["EUde"]);
    assert(bcml::texts_for(langs, "USfr") == &langs["USen"] && bcml::texts_for({}, "USen") == nullptr);
}

// ---------------------------------------------------------------- folder layouts
void test_layouts(const fs::path& root) {
    const std::string usa = "00050000101C9400";
    const std::string def = "[Definition]\ntitleIds = 00050000101C9300,00050000101C9400,00050000101C9500\nname = \"My Pack\"\n"
                            "description = \"First line.|Second line.\"\nversion = 4\n";
    put(root / "cemu/rules.txt", def);
    put(root / "cemu/content/Pack/x.pack", "x");
    loader::Layout l = loader::detect(root / "cemu", usa);
    assert(l.kind == "Cemu graphic pack" && l.name == "My Pack" && l.problem.empty() && l.warning.empty());
    assert(l.description == "First line. Second line." && l.content == root / "cemu/content" && l.aoc.empty());
    put(root / "cemu-code/rules.txt", def);
    put(root / "cemu-code/patch_Something.asm", "[x]");
    put(root / "cemu-code/aoc/0010/x", "x");
    l = loader::detect(root / "cemu-code", usa);  // its code patches are a code mod (mods::patches)
    assert(l.problem.empty() && l.warning.empty() && l.code && l.aoc == root / "cemu-code/aoc");
    put(root / "cemu-only-code/rules.txt", def);
    put(root / "cemu-only-code/patches.txt", "[x]");
    l = loader::detect(root / "cemu-only-code", usa);
    assert(l.problem.empty() && l.code && l.content.empty() && l.aoc.empty());
    put(root / "cemu-shaders/rules.txt", def);
    put(root / "cemu-shaders/0123456789abcdef_0000000000000000_ps.txt", "x");
    put(root / "cemu-shaders/content/a", "a");
    l = loader::detect(root / "cemu-shaders", usa);
    assert(l.problem.empty() && !l.warning.empty() && !l.code);
    put(root / "cemu-nothing/rules.txt", def);
    put(root / "cemu-nothing/0123456789abcdef_0000000000000000_ps.txt", "x");
    assert(!loader::detect(root / "cemu-nothing", usa).problem.empty());
    put(root / "cemu-other/rules.txt", "[Definition]\ntitleIds = 0005000010143500\nname = Other\n");
    put(root / "cemu-other/content/a", "a");
    assert(!loader::detect(root / "cemu-other", usa).problem.empty());
    put(root / "wrapped/Some Mod/Readme.txt", "read me");
    put(root / "wrapped/Some Mod/Mod/content/a", "a");
    l = loader::detect(root / "wrapped", usa);
    assert(l.problem.empty() && l.kind == "Content folder" && l.content == root / "wrapped/Some Mod/Mod/content");
    put(root / "sd/sdcafiine/00050000101C9400/ModA/content/a", "a");
    l = loader::detect(root / "sd", usa);
    assert(l.problem.empty() && l.kind == "SDCafiine folder" && l.content == root / "sd/sdcafiine/00050000101C9400/ModA/content");
    put(root / "sd2/00050000101C9400/ModA/content/a", "a");
    put(root / "sd2/00050000101C9400/ModB/content/b", "b");
    assert(loader::detect(root / "sd2", usa).problem.find("several") != std::string::npos);
    put(root / "sd3/0005000010143500/ModA/content/a", "a");
    assert(loader::detect(root / "sd3", usa).problem.find("another game") != std::string::npos);
    put(root / "switch/romfs/Actor/x", "x");
    assert(loader::detect(root / "switch", usa).problem.find("Switch") != std::string::npos);
    put(root / "archive.zip", "PK");
    assert(loader::detect(root / "archive.zip", usa).problem.find("unpack") != std::string::npos);
    put(root / "bnp/Second Wind.bnp", "7z");
    assert(loader::detect(root / "bnp", usa).problem.find("BCML") != std::string::npos);
    put(root / "unpacked-bnp/content/Actor/Pack/A.sbactorpack", "x");  // an unpacked .bnp: merge data in logs/
    put(root / "unpacked-bnp/logs/deepmerge.aamp", "AAMP");
    put(root / "unpacked-bnp/logs/packs.json", "{}");
    put(root / "unpacked-bnp/info.json", "{\"name\": \"Some BCML Mod 1.0\", \"desc\": \"About it\", \"version\": \"1.0\"}");
    l = loader::detect(root / "unpacked-bnp", usa);
    assert(l.kind == "BCML mod" && l.problem.empty() && l.logs == root / "unpacked-bnp/logs");
    assert(l.name == "Some BCML Mod 1.0" && l.description == "About it");
    for (const char* log : {"eventinfo.yml", "gamedata.yml", "savedata.yml", "texts.json"}) put(root / "unpacked-bnp/logs" / log, "{}");
    assert(loader::detect(root / "unpacked-bnp", usa).problem.empty());  // merge data the loader applies
    put(root / "unpacked-bnp/logs/future.yml", "x");  // merge data the loader can't apply
    l = loader::detect(root / "unpacked-bnp", usa);
    assert(l.problem.find("future.yml") != std::string::npos && l.problem.find("gamedata.yml") == std::string::npos);
    // options as BCML's dev tools write them: switches ("multi", with a default) and groups ("single": one
    // choice, none unless required; no defaults, but a hand-written one is taken)
    put(root / "options-bnp/content/a", "a");
    put(root / "options-bnp/info.json", R"({"name": "With options", "options": {
        "multi": [{"name": "Extra", "desc": "More things", "folder": "Extra", "default": true},
                  {"name": "Off", "folder": "Off", "default": false}, {"name": "Bad", "folder": "Bad", "default": true},
                  {"name": "Gone", "folder": "Gone"}, {"name": "Outside", "folder": "../options-bnp/options/Off"}],
        "single": [{"name": "Colour", "desc": "Pick one", "required": true,
                    "options": [{"name": "Red", "folder": "Red"}, {"name": "Blue", "folder": "Blue"}, {"name": "Again", "folder": "Extra"}]},
                   {"name": "Missing", "required": true, "options": [{"name": "Nowhere", "folder": "Nowhere"}]},
                   {"name": "Hat", "required": false,
                    "options": [{"name": "Cap", "folder": "Cap"}, {"name": "Crown", "folder": "Crown", "default": true}]}]}})");
    for (const char* o : {"Extra", "Off", "Red", "Blue", "Cap", "Crown"}) put(root / "options-bnp/options" / o / "content/b", o);
    put(root / "options-bnp/options/Bad/logs/future.yml", "{}");
    l = loader::detect(root / "options-bnp", usa);
    assert(l.kind == "BCML mod" && l.problem.empty() && l.options.size() == 6 && l.groups.size() == 2);
    const char* const keys[] = {"Extra", "Off", "Red", "Blue", "Cap", "Crown"};
    const int groups[] = {-1, -1, 0, 0, 1, 1};
    for (size_t k = 0; k < 6; k++)
        assert(l.options[k].key == keys[k] && l.options[k].name == keys[k] && l.options[k].group == groups[k] &&
               l.options[k].folder == root / "options-bnp/options" / keys[k]);
    assert(l.options[0].by_default && !l.options[1].by_default && l.options[0].description == "More things");
    assert(l.groups[0].name == "Colour" && l.groups[0].required && l.groups[0].description == "Pick one");
    assert(l.groups[1].name == "Hat" && !l.groups[1].required);  // Missing: no option left
    assert(l.warning.find("Bad") != std::string::npos && l.warning.find("Extra") == std::string::npos && l.warning.find("Off") == std::string::npos);
    // what a start uses: saved choices over the defaults, one per group
    using Saved = std::map<std::string, bool>;
    auto uses = [&](const Saved& saved) {
        const std::vector<bool> on = loader::chosen(l, saved);
        assert(on.size() == l.options.size());
        std::string out;
        for (size_t k = 0; k < on.size(); k++)
            if (on[k]) out += (out.empty() ? "" : " ") + l.options[k].key;
        return out;
    };
    assert(uses({}) == "Extra Red Crown");  // a required group without a default: its first
    assert(uses({{"Extra", false}, {"Off", true}, {"Unknown", true}}) == "Off Red Crown");
    assert(uses({{"Red", false}, {"Blue", true}}) == "Extra Blue Crown");
    assert(uses({{"Red", false}, {"Blue", false}}) == "Extra Red Crown");  // required: never none
    assert(uses({{"Cap", false}, {"Crown", false}}) == "Extra Red");       // None chosen
    assert(uses({{"Cap", true}, {"Crown", false}}) == "Extra Red Cap");
    assert(uses({{"Blue", true}, {"Red", true}}) == "Extra Red Crown");  // hand-edited: the first one
    put(root / "packs-only/content/Actor/Pack/A.sbactorpack", "x");  // only the list of its packs: usable
    put(root / "packs-only/logs/packs.json", "{}");
    assert(loader::detect(root / "packs-only", usa).problem.empty());
    fs::create_directories(root / "empty");
    assert(!loader::detect(root / "empty", usa).problem.empty());
}

// ---------------------------------------------------------------- the loader
std::string guest_of(const char* p) { return std::string("/vol/content/") + p; }
loader::ModView options_mod() {
    for (const auto& m : loader::list())
        if (m.id == "f-options") return m;
    assert(!"f-options is listed");
    return {};
}

void setup_loader(const fs::path& root) {
    config::game_dir = (root / "game/update").string();
    config::base_dir = (root / "game/base").string();
    config::aoc_dir = (root / "game/aoc").string();
    config::save_dir = (root / "save").string();
    setenv("BOTW_MODS_DIR", (root / "user/Mods").c_str(), 1);
}
void test_loader(const fs::path& root, const char* self) {
    make_world(root);
    const fs::path mods = root / "user/Mods";
    put(mods / "b-cemu/rules.txt", "[Definition]\ntitleIds = 00050000101C9400\nname = \"Big Pack\"\n");
    put(mods / "b-cemu/content/Loose/L.bin", "loose-cemu");
    put(mods / "b-cemu/content/Pack/Other.pack", sarc({{"Other/O.txt", bytes("o-cemu")}}));
    put(mods / "c-plain/content/Loose/L.bin", "loose-plain");
    put(mods / "c-plain/aoc/0010/Map/MainField/X.smubin", yaz(bytes("dlc-map-plain")));
    put(mods / "d-switch/romfs/x", "x");
    // e-bnp.bnp: a 7z archive as BCML makes them (if this tar writes 7z), with a default option
    const fs::path bnp_src = root / "bnp-source";
    put(bnp_src / "content/Loose/Bnp.bin", "from the bnp");
    put(bnp_src / "logs/rstb.json", "{\"Loose/Bnp.bin\": 64}");
    put(bnp_src / "info.json", "{\"name\": \"Bnp Mod\", \"options\": {\"multi\": [{\"name\": \"Extra\", \"folder\": \"Extra\", \"default\": true}]}}");
    put(bnp_src / "options/Extra/content/Loose/Extra.bin", "from the option");
    const std::string make = "cd \"" + bnp_src.string() + "\" && tar --format 7zip -cf \"" + (mods / "e-bnp.bnp").string() +
                             "\" content logs info.json options 2>/dev/null";
    const bool with_bnp = std::system(make.c_str()) == 0;
    if (!with_bnp) fprintf(stderr, "content merge: this tar can't write 7z; the .bnp part is skipped\n");
    // f-options: an unpacked BCML mod with a switch (off by default) and a required group (Red at first);
    // Blue has its own sizes
    put(mods / "f-options/content/Loose/Options.bin", "options mod");
    put(mods / "f-options/info.json", R"({"name": "Options Mod", "options": {"multi": [{"name": "Switch", "folder": "Switch", "default": false}],
        "single": [{"name": "Colour", "required": true, "options": [{"name": "Red", "folder": "Red"}, {"name": "Blue", "folder": "Blue"}]}]}})");
    put(mods / "f-options/options/Switch/content/Loose/Switch.bin", "switch");
    put(mods / "f-options/options/Red/content/Loose/Colour.bin", "red");
    put(mods / "f-options/options/Blue/content/Loose/Colour.bin", "blue");
    put(mods / "f-options/options/Blue/logs/rstb.json", "{\"Loose/Colour.bin\": 4321}");
    put(root / "save/user/1/game_data.sav", "save data");
    setup_loader(root);
    loader::startup();
    // new mods are listed first, in name order: b-cemu wins over c-plain
    const auto list = loader::list();
    assert(list.size() == (with_bnp ? 5 : 4) && list[0].id == "b-cemu" && list[0].name == "Big Pack" && list[0].active && list[0].enabled);
    assert(list[1].id == "c-plain" && list[1].active && list[2].id == "d-switch" && !list[2].problem.empty() && !list[2].active);
    if (with_bnp) {  // unpacked next to the merge caches; its default option is used
        assert(list[3].id == "e-bnp.bnp" && list[3].name == "Bnp Mod" && list[3].kind == "BCML mod (.bnp)" && list[3].active);
        const std::string from = loader::replacement(guest_of("Loose/Bnp.bin"));
        assert(from.find("Mods unpacked") != std::string::npos && text(get(from)) == "from the bnp");
        assert(text(get(loader::replacement(guest_of("Loose/Extra.bin")))) == "from the option");
    }
    assert(list[0].files == 2 && list[0].used == 2 && list[1].used == 1);
    assert(loader::replacement(guest_of("Loose/L.bin")) == (mods / "b-cemu/content/Loose/L.bin").string());
    assert(loader::replacement("Loose/l.BIN") == (mods / "b-cemu/content/Loose/L.bin").string());  // relative, any case
    assert(loader::replacement(guest_of("Pack/Other.pack")) == (mods / "b-cemu/content/Pack/Other.pack").string());
    const std::string aoc = loader::replacement("/vol/aoc0005000c101c9400/0010/Pack/AocPack.pack");
    assert(!aoc.empty() && text(unyaz(unsarc(get(aoc)).at("Map/MainField/X.smubin"))) == "dlc-map-plain");
    assert(loader::replacement(guest_of("Pack/Third.pack")).empty() && loader::replacement("/vol/save/x").empty());
    const std::string table = loader::replacement(guest_of("System/Resource/ResourceSizeTable.product.srsizetable"));
    assert(!table.empty() && entry(table_of(table), "Aoc/0010/Map/MainField/X.mubin") == 32 + 193);
    if (with_bnp) assert(entry(table_of(table), "Loose/Bnp.bin") == 64);
    assert(!loader::restart_needed() && loader::summary().find("archives merged") != std::string::npos);
    // saves were backed up (a new set of mods), next to the Mods folder
    bool backed_up = false;
    for (const auto& e : fs::recursive_directory_iterator(root / "user/Save backups"))
        backed_up |= e.path().filename() == "game_data.sav";
    assert(backed_up);
    // choices: saved for the next start
    loader::set_enabled("c-plain", false);
    assert(loader::restart_needed());
    loader::set_enabled("c-plain", true);
    assert(!loader::restart_needed());
    loader::move("c-plain", -1);
    assert(loader::list()[0].id == "c-plain" && loader::restart_needed());
    loader::move("c-plain", +1);
    assert(!loader::restart_needed());
    put(mods / "a-new/content/Loose/Other.bin", "new");
    loader::rescan();
    assert(loader::list()[0].id == "a-new" && loader::list()[0].enabled && loader::restart_needed());
    loader::set_enabled("a-new", false);
    assert(!loader::restart_needed());
    // options: BCML's defaults at first, a group's choice merged over the mod
    {
        const loader::ModView f = options_mod();
        assert(f.kind == "BCML mod" && f.name == "Options Mod" && f.active && f.groups.size() == 1 && f.groups[0].required);
        assert(f.options.size() == 3 && f.options[0].key == "Switch" && f.options[0].group == -1 && !f.options[0].on && !f.options[0].active);
        assert(f.options[1].key == "Red" && f.options[1].on && f.options[1].active && f.options[1].files == 1 && f.options[1].used == 1);
        assert(f.options[2].key == "Blue" && f.options[2].group == 0 && !f.options[2].on && !f.options[2].active);
        assert(text(get(loader::replacement(guest_of("Loose/Options.bin")))) == "options mod");
        assert(text(get(loader::replacement(guest_of("Loose/Colour.bin")))) == "red" && loader::replacement(guest_of("Loose/Switch.bin")).empty());
    }
    const fs::path settings = root / "user/mods.json";
    loader::set_option("f-options", "Switch", true);  // saved, for the next start
    assert(loader::restart_needed() && options_mod().options[0].on && !options_mod().options[0].active);
    assert(text(get(settings)).find("\"id\":\"f-options\",\"options\":{\"Switch\":true}") != std::string::npos);
    loader::set_option("f-options", "Switch", false);
    assert(!loader::restart_needed());
    loader::set_option("f-options", "Blue", true);  // the group's other option goes off
    assert(loader::restart_needed() && !options_mod().options[1].on && options_mod().options[2].on);
    loader::set_option("f-options", "Blue", false);  // a required group keeps its choice
    assert(options_mod().options[2].on);
    loader::set_option("f-options", "Red", true);
    loader::set_option("f-options", "Unknown", true);
    assert(!loader::restart_needed() && options_mod().options[1].on && !options_mod().options[2].on);
    loader::set_enabled("f-options", false);  // a mod that is off: its options don't matter
    loader::set_option("f-options", "Switch", true);
    assert(loader::restart_needed());
    loader::set_option("f-options", "Switch", false);
    loader::set_enabled("f-options", true);
    assert(!loader::restart_needed());
    // a second start with the same mods and options takes the merged files from the cache
    const long long time = (long long)fs::last_write_time(aoc).time_since_epoch().count();
    const std::string self_work = "\"" + std::string(self) + "\" \"" + root.parent_path().string() + "\" ";
    assert(std::system((self_work + "--reuse " + std::to_string(time)).c_str()) == 0);
    // other options: merged again at the next start, with the chosen ones
    loader::set_option("f-options", "Switch", true);
    loader::set_option("f-options", "Blue", true);
    assert(loader::restart_needed());
    assert(std::system((self_work + "--options \"" + table + "\"").c_str()) == 0);
}
int reuse(const fs::path& root, const std::string& time) {
    setup_loader(root);
    loader::startup();
    const std::string aoc = loader::replacement("/vol/aoc0005000c101c9400/0010/Pack/AocPack.pack");
    assert(!aoc.empty() && std::to_string((long long)fs::last_write_time(aoc).time_since_epoch().count()) == time);
    assert(loader::list().size() >= 4 && !loader::list()[0].active);  // a-new: off
    return 0;
}
int options_start(const fs::path& root, const std::string& old_table) {  // a start with the options chosen above
    setup_loader(root);
    loader::startup();
    const loader::ModView f = options_mod();
    assert(f.active && f.options[0].on && f.options[0].active && !f.options[1].on && !f.options[1].active);
    assert(f.options[2].on && f.options[2].active && !loader::restart_needed());
    assert(loader::summary().find(" mods with ") != std::string::npos && loader::summary().find(" options, ") != std::string::npos);
    assert(text(get(loader::replacement(guest_of("Loose/Colour.bin")))) == "blue");
    assert(text(get(loader::replacement(guest_of("Loose/Switch.bin")))) == "switch");
    const std::string table = loader::replacement(guest_of("System/Resource/ResourceSizeTable.product.srsizetable"));
    assert(entry(table_of(table), "Loose/Colour.bin") == 4321);  // the option's own merge data
    // another set of mods (the options are part of the cache's fingerprint): the first start's merge is gone
    assert(fs::path(table).parent_path() != fs::path(old_table).parent_path() && !fs::exists(old_table));
    return 0;
}

// ---------------------------------------------------------------- a real dump (by hand)
// every archive in a merged file reads back, keeps all of the game's files, and how many changed
struct Check {
    size_t archives = 0, files = 0, changed = 0, lost = 0, params = 0, small = 0, tables = 0, texts = 0;
    const Rstb* table = nullptr;  // the merged size table
    std::string aoc;              // "Aoc/0010/" for DLC files
    // a changed file's size-table entry (if it has one) must hold at least the file
    void sized(const std::string& name, size_t size) {
        if (!table) return;
        uint32_t entry = 0;
        const std::string base = resource_name(name);
        if (!(!aoc.empty() && rstb_get(*table, aoc + base, &entry)) && !rstb_get(*table, base, &entry)) return;
        if (entry < size) {
            small++;
            fprintf(stderr, "size entry too small: %s %u < %zu\n", base.c_str(), entry, size);
        }
    }
};
void check_params(const Bytes& data, Check& c) {  // a parameter file: reads, writes and reads back the same
    aamp::File f, g;
    Bytes again;
    std::string why;
    const bool ok = aamp::parse(data.data(), data.size(), f, &why) && aamp::write(f, again, &why) && aamp::parse(again.data(), again.size(), g, &why);
    if (!ok) fprintf(stderr, "parameter file: %s\n", why.c_str());
    assert(ok && f.root == g.root);
    c.params++;
}
void check_table(const Bytes& data, Check& c) {  // a BYML document: reads, writes and reads back the same
    byml::Document d, again;
    Bytes out;
    std::string why;
    const bool ok = byml::parse(data.data(), data.size(), d, &why) && byml::write(d, out, &why) && byml::parse(out.data(), out.size(), again, &why);
    if (!ok) fprintf(stderr, "BYML file: %s\n", why.c_str());
    assert(ok && again.root == d.root);
    c.tables++;
}
void check_text(const Bytes& data, Check& c) {  // an MSBT file: reads and writes back byte for byte
    msbt::File f;
    Bytes out;
    std::string why;
    const bool ok = msbt::parse(data.data(), data.size(), f, &why) && msbt::write(f, out, &why);
    if (!ok) fprintf(stderr, "MSBT file: %s\n", why.c_str());
    assert(ok && out == data);
    c.texts++;
}
void check_archive(const Bytes& merged, const Bytes* game, int depth, Check& c) {
    const auto files = unsarc(merged);
    c.archives++;
    std::map<std::string, Bytes> vanilla;
    if (game) vanilla = unsarc(*game);
    for (const auto& [name, v] : vanilla)
        if (!files.count(name)) c.lost++;
    for (const auto& [name, data] : files) {
        c.files++;
        const Bytes unpacked = is_yaz0(data.data(), data.size()) ? unyaz(data) : data;
        const Bytes* old = nullptr;
        Bytes old_unpacked;
        if (auto it = vanilla.find(name); it != vanilla.end()) {
            old_unpacked = is_yaz0(it->second.data(), it->second.size()) ? unyaz(it->second) : it->second;
            old = &old_unpacked;
        }
        if (!old || *old != unpacked) {
            c.changed++;
            c.sized(name, unpacked.size());
        }
        if (unpacked.size() >= 4 && !memcmp(unpacked.data(), "AAMP", 4)) check_params(unpacked, c);
        if ((!old || *old != unpacked) && byml::is_byml(unpacked.data(), unpacked.size())) check_table(unpacked, c);
        if ((!old || *old != unpacked) && unpacked.size() >= 8 && !memcmp(unpacked.data(), "MsgStdBn", 8)) check_text(unpacked, c);
        if (depth < 4 && is_sarc(unpacked.data(), unpacked.size()) && (!old || *old != unpacked))
            check_archive(unpacked, old && is_sarc(old->data(), old->size()) ? old : nullptr, depth + 1, c);
    }
}
// BCML merge data for game data tables (the Mods folder's and its unpacked .bnp files' logs/, not their options): every
// logged event, flag, save data flag and text is in the merged files; one that only one mod logs is as logged
void check_tables(const fs::path& cache, const fs::path& mods, const merge::Vanilla& game) {
    std::vector<fs::path> logs;
    auto scan = [&](const fs::path& root) {
        if (!fs::exists(root)) return;
        for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
            const std::string n = it->path().filename().string();
            if (!it->is_directory()) continue;
            if (it.depth() > 3 || n == "options" || n == "content" || n == "aoc") {
                it.disable_recursion_pending();
                continue;
            }
            if (n == "logs") {
                logs.push_back(it->path());
                it.disable_recursion_pending();
            }
        }
    };
    scan(mods);
    scan(mods.parent_path() / "Mods unpacked");
    fs::path merged;
    for (const auto& e : fs::directory_iterator(cache))
        if (fs::exists(e.path() / "merged")) merged = e.path() / "merged";
    auto file = [&](const std::string& tree) {
        const fs::path m = merged / tree;
        return fs::exists(m) ? get(m) : get(game.find(tree));
    };
    auto read = [](const fs::path& p) {
        const Bytes b = get(p);
        return std::string(b.begin(), b.end());
    };
    std::vector<byml::Value> events, flags;
    std::vector<bcml::SaveDiff> saves;
    std::vector<std::map<std::string, bcml::TextFiles>> texts;
    for (const auto& l : logs) {
        std::string why;
        if (fs::exists(l / "eventinfo.yml")) assert(bcml::read_events(read(l / "eventinfo.yml"), events.emplace_back(), why));
        if (fs::exists(l / "gamedata.yml")) assert(bcml::read_flags(read(l / "gamedata.yml"), flags.emplace_back(), why));
        if (fs::exists(l / "savedata.yml")) assert(bcml::read_save(read(l / "savedata.yml"), saves.emplace_back(), why));
        if (fs::exists(l / "texts.json")) assert(bcml::read_texts(read(l / "texts.json"), texts.emplace_back(), why));
    }
    if (events.empty() && flags.empty() && saves.empty() && texts.empty()) return;
    const auto boot = unsarc(file("content/Pack/Bootup.pack"));
    size_t n_events = 0, n_flags = 0, n_saves = 0, n_texts = 0, n_languages = 0;
    if (!events.empty()) {
        byml::Document d;
        const Bytes u = unyaz(boot.at("Event/EventInfo.product.sbyml"));
        assert(byml::parse(u.data(), u.size(), d));
        std::map<std::string, int> count;
        for (const auto& e : events)
            for (const auto& [k, v] : e.hash) count[k]++;
        for (const auto& e : events)
            for (const auto& [k, v] : e.hash) {
                const byml::Value* m = d.root.find(k);
                assert(m && (count[k] > 1 || *m == v));
                n_events++;
            }
    }
    if (!flags.empty()) {
        std::map<std::string, std::map<std::string, byml::Value>> all;  // type -> name -> flag
        for (const auto& [name, data] : unsarc(unyaz(boot.at("GameData/gamedata.ssarc")))) {
            byml::Document d;
            assert(byml::parse(data.data(), data.size(), d));
            for (const auto& [type, list] : d.root.hash)
                for (const auto& f : list.array) all[type][f.str("DataName")] = f;
        }
        std::map<std::pair<std::string, std::string>, int> count;
        for (const auto& f : flags)
            for (const auto& [type, d] : f.hash) {
                if (const byml::Value* add = d.find("add"))
                    for (const auto& [name, flag] : add->hash) count[{type, name}]++;
                if (const byml::Value* del = d.find("del"))
                    for (const auto& name : del->array) count[{type, name.text}]++;
            }
        for (const auto& f : flags)
            for (const auto& [type, d] : f.hash) {
                if (const byml::Value* add = d.find("add"))
                    for (const auto& [name, flag] : add->hash) {
                        auto it = all[type].find(name);
                        const int logged = count[std::make_pair(type, name)];
                        assert(it != all[type].end() && (logged > 1 || it->second == flag));
                        n_flags++;
                    }
                if (const byml::Value* del = d.find("del"))
                    for (const auto& name : del->array) {
                        const int logged = count[std::make_pair(type, name.text)];
                        assert(logged > 1 || !all[type].count(name.text));
                        n_flags++;
                    }
            }
    }
    if (!saves.empty()) {
        std::vector<std::pair<int, std::string>> order;  // game_data.sav files by number
        const auto sv = unsarc(unyaz(boot.at("GameData/savedataformat.ssarc")));
        for (const auto& [name, data] : sv) order.push_back({std::atoi(name.c_str() + name.rfind('_') + 1), name});
        std::sort(order.begin(), order.end());
        std::vector<int32_t> hashes;
        for (const auto& [index, name] : order) {
            byml::Document d;
            const Bytes& data = sv.at(name);
            assert(byml::parse(data.data(), data.size(), d));
            const auto& list = d.root.find("file_list")->array;
            if (list[0].str("file_name") != "game_data.sav") continue;
            for (const auto& e : list[1].array) hashes.push_back(e.find("HashValue")->as_s32());
        }
        assert(std::is_sorted(hashes.begin(), hashes.end()));  // the game looks them up by hash
        const std::set<int32_t> have(hashes.begin(), hashes.end());
        for (const auto& s : saves)
            for (const auto& e : s.add) {
                assert(have.count(e.find("HashValue")->as_s32()));
                n_saves++;
            }
    }
    if (!texts.empty())
        for (const auto& e : fs::directory_iterator(game.update_content / "Pack")) {
            const std::string n = e.path().filename().string();
            if (n.size() != 16 || n.rfind("Bootup_", 0) != 0) continue;
            const std::string lang = n.substr(7, 4);
            const auto pack = unsarc(file("content/Pack/" + n));
            const auto msg = unsarc(unyaz(pack.at("Message/Msg_" + lang + ".product.ssarc")));
            std::map<std::pair<std::string, std::string>, int> count;
            for (const auto& t : texts)
                if (const bcml::TextFiles* f = bcml::texts_for(t, lang))
                    for (const auto& [file, entries] : *f)
                        for (const auto& [label, entry] : entries) count[{file, label}]++;
            for (const auto& t : texts)
                if (const bcml::TextFiles* f = bcml::texts_for(t, lang))
                    for (const auto& [file, entries] : *f) {
                        msbt::File m;
                        const Bytes& data = msg.at(file);
                        assert(msbt::parse(data.data(), data.size(), m));
                        for (const auto& [label, entry] : entries) {
                            const msbt::Entry* x = msbt::find(m, label);
                            const int logged = count[std::make_pair(file, label)];
                            assert(x && (logged > 1 || x->text == entry.text));
                            assert(!m.atr_strings || !entry.has_attribute || logged > 1 || x->attribute == entry.attribute);
                            n_texts++;
                        }
                    }
            n_languages++;
        }
    printf("BCML merge data in the merged files: %zu events, %zu flags, %zu save data flags, %zu texts in %zu languages\n", n_events, n_flags,
           n_saves, n_texts, n_languages);
}
int dump_check(char** argv) {
    config::game_dir = argv[2];
    config::base_dir = argv[3];
    config::aoc_dir = argv[4];
    setenv("BOTW_MODS_DIR", argv[5], 1);
    setenv("CONTENT_MERGE_TEST_LOG", "1", 1);
    const auto t0 = std::chrono::steady_clock::now();
    loader::startup();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    printf("startup: %.0f ms, peak memory %.0f MB\n%s\n", ms, double(usage.ru_maxrss) / (1 << 20), loader::summary().c_str());
    for (const auto& m : loader::list()) {
        printf("- %s [%s]%s%s: %zu files, %zu used%s%s\n", m.name.c_str(), m.kind.c_str(), m.enabled ? "" : " (off)", m.active ? " active" : "",
               m.files, m.used, m.problem.empty() ? "" : ", problem: ", m.problem.c_str());
        if (!m.warning.empty()) printf("    warning: %s\n", m.warning.c_str());
        for (const auto& n : m.notes) printf("    %s\n", n.c_str());
        for (const auto& o : m.options) {
            printf("    option %s (%s)%s%s: %zu files, %zu used\n", o.name.c_str(), o.group < 0 ? "switch" : m.groups[size_t(o.group)].name.c_str(),
                   o.on ? " on" : " off", o.active ? " active" : "", o.files, o.used);
            for (const auto& n : o.notes) printf("        %s\n", n.c_str());
        }
    }
    const merge::Vanilla game{fs::path(argv[2]) / "content", fs::path(argv[3]) / "content", fs::path(argv[4]) / "content"};
    const fs::path cache = fs::path(argv[5]).parent_path() / "Mods cache";
    if (!fs::exists(cache)) return 0;  // nothing merged
    Rstb merged_table;
    bool have_table = false;
    for (const auto& e : fs::recursive_directory_iterator(cache))
        if (e.is_regular_file() && e.path().filename() == "ResourceSizeTable.product.srsizetable") {
            merged_table = table_of(e.path());
            have_table = true;
        }
    size_t small = 0;
    for (const auto& e : fs::recursive_directory_iterator(cache)) {
        if (!e.is_regular_file()) continue;
        const std::string rel = e.path().lexically_relative(cache).generic_string();
        const size_t merged = rel.find("/merged/");
        if (merged == std::string::npos) continue;
        const std::string tree = rel.substr(merged + 8);
        const Bytes data = get(e.path());
        if (tree == merge::kSizeTable) {
            const Rstb t = table_of(e.path());
            printf("size table: %zu entries by CRC, %zu by name\n", t.crc.size(), t.names.size());
            continue;
        }
        const fs::path vanilla = game.find(tree);
        const Bytes vbytes = vanilla.empty() ? Bytes() : get(vanilla);
        const Bytes unpacked = is_yaz0(data.data(), data.size()) ? unyaz(data) : data;
        const Bytes vunpacked = is_yaz0(vbytes.data(), vbytes.size()) ? unyaz(vbytes) : vbytes;
        Check c;
        c.table = have_table ? &merged_table : nullptr;
        if (tree.rfind("aoc/", 0) == 0) c.aoc = "Aoc/" + tree.substr(4, tree.find('/', 4) - 4) + "/";
        c.sized(tree.substr(tree.find('/', tree.rfind("aoc/", 0) == 0 ? 4 : 0) + 1), unpacked.size());
        check_archive(unpacked, vanilla.empty() ? nullptr : &vunpacked, 1, c);
        small += c.small;
        printf("%s: %zu bytes (game %zu): %zu archives, %zu files read back (%zu parameter files, %zu data tables, %zu text files), %zu changed, "
               "%zu of the game's missing\n",
               tree.c_str(), data.size(), vbytes.size(), c.archives, c.files, c.params, c.tables, c.texts, c.changed, c.lost);
        assert(c.lost == 0);
    }
    printf("size entries smaller than their files: %zu\n", small);
    assert(small == 0);
    check_tables(cache, fs::path(argv[5]), game);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc >= 2);
    if (argc == 6 && std::string(argv[1]) == "--dump") return dump_check(argv);
    const fs::path work = argv[1];
    if (argc == 4 && std::string(argv[2]) == "--reuse") return reuse(work / "loader", argv[3]);
    if (argc == 4 && std::string(argv[2]) == "--options") return options_start(work / "loader", argv[3]);
    fs::remove_all(work);
    fs::create_directories(work);
    test_yaz0();
    test_sarc();
    test_rstb();
    test_aamp();
    test_byml();
    test_msbt();
    test_merge(work / "merge");
    test_bcml(work / "bcml");
    test_bcml_tables(work / "tables");
    test_layouts(work / "layouts");
    test_loader(work / "loader", argv[0]);
    printf("content merge: all tests passed\n");
    return 0;
}
