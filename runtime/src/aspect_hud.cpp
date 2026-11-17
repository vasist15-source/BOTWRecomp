// HUD and menus at an aspect ratio wider than 16:9: Cemu's pane rules (aspect_hud.h) on the game's panes.
// The sites are the pack's (tools/botw_aspect_patches.py puts them into the translated game):
//   03C496B8  nw::lyt::Pane's constructor from a layout (03C4952C), after translation, rotation and scale
//             are copied from the layout: r31 the pane (name at + 0x80, translation + 0x1C, scale + 0x34),
//             r27 the size it reads next.
//   03A3EDC4  a screen is created (03A3EC0C): its name is the caller's r24, as the pack takes it.
//   03C46298  an animation writes a pane's transform (pane r30, component r27, value f1).
//   02F60204  the map opens or closes (r31: open): the map layout moves between the corner and the centre.
//   03C56CD0  a pane-based projection's parameters are copied (r4: the copy; x scale at + 8).
// Panes are changed once, as they are built, so everything here follows the run's aspect (aspect::game).
#include "aspect_hud.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#include "aspect.h"
#include "botw_aspect.h"
#include "runtime.h"

namespace {
using namespace aspect::hud;
constexpr uint32_t kPaneParent = 0x0C, kPaneX = 0x1C, kPaneScaleX = 0x34, kPaneScaleY = 0x38, kPaneName = 0x80;
constexpr size_t kName = 36;  // the pack compares up to 35 characters

std::mutex g_mu;
char g_screen[kName] = "", g_parts[kName] = "";  // the screen being built, its last parts pane ("Pa...")
uint32_t g_map_pane = 0;                         // AppMap_00's N_InOut_00, which the map hook moves
std::unordered_map<uint32_t, uint32_t> g_widths; // layout widths made wider: address -> the value written

// the pack's rules are on: wider than 16:9 and the HUD not left stretched
bool rules(Factors& f) {
    const float a = aspect::game();
    const int mode = aspect::hud_mode();
    if (!(a > 16.0f / 9.0f) || mode == kStretched) return false;
    f = factors(a, mode);
    return true;
}
void read_name(uint32_t addr, char out[kName]) {
    size_t i = 0;
    if (addr >= 0x10000 && addr < 0xFFFFFF00u)
        for (; i < kName - 1; i++)
            if (!(out[i] = (char)ld8(addr + (uint32_t)i))) break;
    out[i] = 0;
}
bool logging() {
    static const bool on = getenv("WWHD_ASPECT_LOG") != nullptr;
    return on;
}
}  // namespace

extern "C" void botw_hud_screen(uint32_t name) {
    Factors f;
    if (!rules(f)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    read_name(name, g_screen);
    g_parts[0] = 0;
    if (logging()) LOG("[hud] screen '%s'", g_screen);
}

extern "C" void botw_hud_pane(uint32_t pane, uint32_t size) {
    Factors f;
    if (!rules(f)) return;
    char name[kName];
    read_name(pane + kPaneName, name);
    std::lock_guard<std::mutex> lk(g_mu);
    if (name[0] == 'P' && name[1] == 'a') memcpy(g_parts, name, kName);
    float offset;
    const Action a = decide(g_screen, g_parts, name, f.mode, offset);
    if (a == a_exitScale) return;
    const Pane was{(float)ldf32(pane + kPaneX), (float)ldf32(pane + kPaneScaleX), (float)ldf32(pane + kPaneScaleY), (float)ldf32(size)};
    Pane p = was;
    apply(a, p, f, offset);
    if (p.x != was.x) stf32(pane + kPaneX, p.x);
    if (p.sx != was.sx) stf32(pane + kPaneScaleX, p.sx);
    if (p.sy != was.sy) stf32(pane + kPaneScaleY, p.sy);
    if (p.w != was.w) {
        // the width is the layout's own data (the pack writes it there too): once per loaded layout, not
        // again each time a screen is built from it
        auto it = g_widths.find(size);
        if (it == g_widths.end() || it->second != ld32(size)) {
            stf32(size, p.w);
            if (g_widths.size() > 4096) g_widths.clear();
            g_widths[size] = ld32(size);
        }
    }
    if (a == a_scaleMapAndStore) g_map_pane = pane;
    if (logging())
        LOG("[hud] %s / %s / %s: x %.1f -> %.1f, scale %.3f,%.3f -> %.3f,%.3f%s", g_screen, g_parts, name, was.x, p.x, was.sx,
            was.sy, p.sx, p.sy, p.w != was.w ? ", wider" : "");
}

extern "C" double botw_hud_anim(double value, uint32_t pane, uint32_t component) {
    if (component != 0) return value;  // x translation only
    Factors f;
    if (!rules(f)) return value;
    if (ld8(pane + kPaneName) != 'P' || ld8(pane + kPaneName + 1) != 'a') return value;
    char name[kName], parent[kName];
    read_name(pane + kPaneName, name);
    if (strcmp(name, "Pa_TempMeter_00") && strcmp(name, "Pa_SoundGauge_00") && strcmp(name, "Pa_Sensor_00")) return value;
    // the main HUD's gauges are children of N_All_00; the inventory's temperature pane has the name too
    const uint32_t up = ld32(pane + kPaneParent);
    if (!up) return value;
    read_name(up + kPaneName, parent);
    return strcmp(parent, "N_All_00") ? value : (double)sensor_x((float)value, f.ar);
}

extern "C" void botw_hud_map(uint32_t open) {
    Factors f;
    if (!rules(f)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_map_pane) return;
    char name[kName];
    read_name(g_map_pane + kPaneName, name);
    if (strcmp(name, "N_InOut_00")) { g_map_pane = 0; return; }  // that layout is gone
    // the minimap sits at the right edge (Edges); the opened map, and both when centred, in the middle
    stf32(g_map_pane + kPaneX, f.mode == kEdges && !open ? f.pad : 0.0f);
}

extern "C" void botw_hud_projection(uint32_t copy) {
    Factors f;
    if (!rules(f)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!strcmp(g_parts, "Pa_Weather_00")) stf32(copy + 8, f.ar);
}
