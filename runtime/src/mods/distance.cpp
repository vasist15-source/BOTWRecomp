// View distances: see distance.h.
#include "distance.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../overlay/hostui.h"
#include "../runtime.h"
#include "botw_distance.h"
#include "botw_memory.h"

float botw_distance[BOTW_DIST_COUNT] = {};  // C linkage from botw_distance.h; 0 = the game's own
int botw_more_memory_on = 0;                 // botw_memory.h; set at boot (more_memory_at_boot)

namespace mods::distance {
const char* const kTreeNames[kTreeLevels] = {"Potato", "Low", "Game's own", "High", "Ultra", "Extreme"};
const char* const kGrassNames[kGrassLevels] = {"Potato", "Low", "Game's own", "High", "Ultra", "Extreme"};
namespace {
// the Draw Distance pack's values (Mods/DrawDistance/rules.txt)
const float kTreeValues[kTreeLevels] = {0.75f, 0.625f, 0.5f, 0.375f, 0.25f, 0.125f};
const float kGrassValues[kGrassLevels] = {2000.0f, 1500.0f, 1000.0f, 900.0f, 700.0f, 500.0f};
// game data the sites rely on (v208): checked before anything is changed
constexpr uint32_t kTreeConst = 0x102BC4A0, kGrassWord = 0x1030A774;

std::atomic<float> g_actors{1.0f}, g_objects{1.0f};
std::atomic<int> g_trees{kTreeDefault}, g_grass{kGrassDefault}, g_texture{0};
std::atomic<bool> g_started{false}, g_tree_ok{false}, g_grass_ok{false};
bool g_more_run = false, g_more_saved = false, g_more_env = false;
const bool g_off = [] {
    const char* e = getenv("WWHD_DISTANCE");
    return e && (!strcmp(e, "off") || !strcmp(e, "0"));
}();

void store(int id, float v) { __atomic_store(&botw_distance[id], &v, __ATOMIC_RELAXED); }
bool near1(float v) { return std::fabs(v - 1.0f) < 0.005f; }

void apply() {
    if (g_off || !g_started.load()) return;
    const float a = g_actors.load(), o = g_objects.load();
    store(BOTW_DIST_ACTOR, near1(a) ? 0.0f : a);
    store(BOTW_DIST_OBJECT, near1(o) ? 0.0f : o);
    store(BOTW_DIST_OBJECT_CULL, near1(o) ? 0.0f : 2.0f - o);
    const int t = g_trees.load();
    store(BOTW_DIST_TREE, g_tree_ok && t != kTreeDefault ? kTreeValues[t] : 0.0f);
    if (g_grass_ok) st32(kGrassWord, f32_as_u32(kGrassValues[g_grass.load()]));
}
void save(const char* key, const std::string& v) {
    if (!g_off) hostui::post([key, v] { hostui::set(key, v); });
}
std::string fmt(float v) {
    char b[32];
    snprintf(b, sizeof b, "%.3f", v);
    return b;
}
}  // namespace

bool more_memory_at_boot() {
    std::string s;
    g_more_saved = hostui::get("moreMemory", s) && s == "1";
    const char* e = getenv("WWHD_MORE_MEMORY");
    g_more_env = e && *e;
    g_more_run = g_more_env ? strcmp(e, "0") != 0 : g_more_saved;
    return g_more_run;
}
bool more_memory() { return g_more_run; }
bool more_memory_saved() { return g_more_saved; }
bool more_memory_from_env() { return g_more_env; }
void set_more_memory(bool on) {
    g_more_saved = on;
    hostui::set("moreMemory", on ? "1" : "0");
}
float actor_max() { return g_more_run ? kActorMaxMore : kActorMax; }
float object_max() { return g_more_run ? kObjectMaxMore : kObjectMax; }

void startup() {
    g_tree_ok = ld32(kTreeConst) == f32_as_u32(0.5f);
    g_grass_ok = ld32(kGrassWord) == f32_as_u32(1000.0f);
    if (!g_tree_ok) LOG("[distance] tree billboard constant %08X is %08X, not 0.5: trees left as they are", kTreeConst, ld32(kTreeConst));
    if (!g_grass_ok) LOG("[distance] grass word %08X is %08X, not 1000: grass left as it is", kGrassWord, ld32(kGrassWord));
    std::string s;
    if (hostui::get("drawActors", s)) g_actors = std::clamp(strtof(s.c_str(), nullptr), kActorMin, actor_max());
    if (hostui::get("drawObjects", s)) g_objects = std::clamp(strtof(s.c_str(), nullptr), kObjectMin, object_max());
    if (hostui::get("drawTrees", s)) g_trees = std::clamp(atoi(s.c_str()), 0, kTreeLevels - 1);
    if (hostui::get("drawGrass", s)) g_grass = std::clamp(atoi(s.c_str()), 0, kGrassLevels - 1);
    if (hostui::get("textureDetail", s)) g_texture = std::clamp(atoi(s.c_str()), kTextureMin, kTextureMax);
    g_started = true;
    if (g_off) {
        LOG("[distance] WWHD_DISTANCE=off: the game's own view distances");
        g_texture = 0;
        return;
    }
    apply();
    LOG("[distance] actors %.2fx, objects %.2fx, trees %s, grass %s, texture detail %+d%s", g_actors.load(),
        g_objects.load(), kTreeNames[g_trees.load()], kGrassNames[g_grass.load()], g_texture.load(),
        g_more_run ? ", More memory" : "");
}

float actors() { return g_actors; }
float objects() { return g_objects; }
int trees() { return g_trees; }
int grass() { return g_grass; }
int texture_detail() { return g_texture; }
float texture_bias() { return (float)g_texture.load(std::memory_order_relaxed); }
bool disabled() { return g_off; }

void set_actors(float v, bool s) {
    g_actors = std::clamp(v, kActorMin, actor_max());
    apply();
    if (s) save("drawActors", fmt(g_actors));
}
void set_objects(float v, bool s) {
    g_objects = std::clamp(v, kObjectMin, object_max());
    apply();
    if (s) save("drawObjects", fmt(g_objects));
}
void set_trees(int l, bool s) {
    g_trees = std::clamp(l, 0, kTreeLevels - 1);
    apply();
    if (s) save("drawTrees", std::to_string(g_trees.load()));
}
void set_grass(int l, bool s) {
    g_grass = std::clamp(l, 0, kGrassLevels - 1);
    apply();
    if (s) save("drawGrass", std::to_string(g_grass.load()));
}
void set_texture_detail(int l, bool s) {
    if (g_off) return;
    g_texture = std::clamp(l, kTextureMin, kTextureMax);
    if (s) save("textureDetail", std::to_string(g_texture.load()));
}
void reset() {
    set_actors(1.0f, true);
    set_objects(1.0f, true);
    set_trees(kTreeDefault, true);
    set_grass(kGrassDefault, true);
    set_texture_detail(0, true);
    LOG("[distance] reset to the game's own");
}
}  // namespace mods::distance
