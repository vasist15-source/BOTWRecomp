// Aspect ratio of the TV picture: 16:9 (original), the window's shape, or one of the fixed shapes from
// 5:4 to 48:9 (Settings > Display; WWHD_ASPECT=21:9 / window / 2.4 and WWHD_HUD=edges / centred /
// stretched for a run).
//
// The game keeps drawing its 1280x720 guest screen. What changes:
//  - Render targets (metal_surfaces.mm): the targets that follow the internal resolution (the BotW
//    Graphics pack's sizes, not the GamePad's) are allocated A/(16/9) times wider (or taller) than their
//    guest size; draws keep their guest viewports, which now cover the wider texture, so every
//    full-screen pass (post effects, depth sampling) lines up.
//  - 3D (tools/botw_aspect_patches.py): cameras with a 16:9 viewport project with the new aspect and
//    the same vertical field of view (Hor+: wider screens see more to the sides). Narrower screens keep
//    the horizontal field of view and widen the vertical one (Vert+), so nothing of the 16:9 view is
//    cut off. Grass culling reaches the new edges. Data the game reads for the TV's aspect (camera
//    set-ups, a grass distance) is written here, as Cemu's Graphics pack patches it.
//  - 2D, wider than 16:9 (aspect_hud.h, aspect_hud.cpp): Cemu's Graphics pack's HUD rules. The panes of
//    the HUD and the menus get their proportions back when their layouts are built and sit at the
//    screen's edges or around the centre; backgrounds and fades stay stretched over the picture.
//  - 2D, narrower than 16:9: stretched to the taller picture as in Cemu (its pane rules would push menus
//    off the sides), or with the HUD centred, layout (nw::lyt) and text (nw::font) projections drawn to
//    the TV picture narrowed on the render thread (gx2_core.cpp OP_SET_PROJ_REGS): a 16:9 band.
// The aspect and the HUD placement are fixed when the game first asks (latch): panes are changed as
// their layouts are built, so a change of the setting applies at the next start (Restart now in the
// settings). At 16:9 every site returns the game's own values.
#include "aspect.h"
#include "aspect_hud.h"
#include "mods/cemu_pack.h"
#include "overlay/hostui.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "botw_aspect.h"
#include "runtime.h"

namespace aspect {
namespace {
constexpr float kBase = 16.0f / 9.0f;

const Choice kChoices[] = {
    {"16:9", "16:9 (original)", kBase},
    {"window", "Match window", 0},
    {"4:3", "4:3", 4.0f / 3.0f},
    {"5:4", "5:4", 5.0f / 4.0f},
    {"3:2", "3:2", 3.0f / 2.0f},
    {"16:10", "16:10", 16.0f / 10.0f},
    {"21:9", "21:9 (2560x1080, 5120x2160)", 64.0f / 27.0f},
    {"43:18", "21:9 (3440x1440)", 43.0f / 18.0f},
    {"24:10", "24:10 (3840x1600)", 24.0f / 10.0f},
    {"32:10", "32:10", 32.0f / 10.0f},
    {"32:9", "32:9", 32.0f / 9.0f},
    {"48:9", "48:9 (three screens)", 48.0f / 9.0f},
    {"custom", "Custom", -1},
};
constexpr int kChoiceCount = sizeof kChoices / sizeof kChoices[0];
const char* const kHudIds[hud::kModes] = {"edges", "centred", "stretched"};

float clamp_aspect(float a) { return std::fmin(std::fmax(a, kMin), kMax); }
bool same(float a, float b) { return std::fabs(a - b) <= 1e-4f * std::fmax(1.0f, std::fabs(a)); }

// WWHD_ASPECT=21:9 / 2.4 / window and WWHD_HUD=edges / centred / stretched: this run's, whatever is saved
struct Env {
    bool set = false, window = false;
    float value = kBase;
    int hud = -1;
    Env() {
        if (const char* e = getenv("WWHD_HUD"))
            for (int i = 0; i < hud::kModes; i++)
                if (!strcmp(e, kHudIds[i])) hud = i;
        const char* e = getenv("WWHD_ASPECT");
        if (!e || !*e) return;
        set = true;
        if (!strcmp(e, "window") || !strcmp(e, "match")) { window = true; return; }
        float a = 0;
        if (const char* c = strchr(e, ':')) a = (float)atof(e) / (float)atof(c + 1);
        else a = (float)atof(e);
        if (!strcmp(e, "21:9")) a = 64.0f / 27.0f;
        if (a > 0.5f && a < 8.0f) value = a;
        else set = false;
    }
};
const Env g_env;

// the saved setting (Settings > Display)
std::atomic<int> g_saved{0}, g_saved_hud{hud::kEdges};
std::atomic<float> g_saved_custom{kBase};
std::atomic<float> g_window{kBase};
// this run's, fixed at the first use (latch): read by the threads that compute and record frames
std::atomic<float> g_game{kBase};
std::atomic<int> g_hud{hud::kEdges}, g_active_choice{0};
std::atomic<bool> g_latched{false};

// ---- the game's read-only data that holds the TV's aspect (v208 addresses; Cemu's Graphics pack,
// patch_AspectRatio.asm, writes the same). Written once, when the run's aspect is fixed; a value that is
// not the game's (another build, a code mod) is left alone.
struct Datum { uint32_t addr, original; float (*value)(float a); };
float tv_aspect(float a) { return a; }
float grass_distance(float a) { return a > kBase ? 60.0f : 120.0f; }  // the pack: half for wider pictures
const Datum kData[] = {
    {0x101BF8E8, 0x3FE374BC, tv_aspect},  // 1.777 (camera set-up)
    {0x1030A57C, 0x3FE38E39, tv_aspect},  // 16/9
    {0x1036DD4C, 0x3FE38E39, tv_aspect},  // 16/9
    {0x1047BFB8, 0x42F00000, grass_distance},  // 120: a second grass culling distance
};
uint32_t g_written[sizeof kData / sizeof kData[0]];  // what each holds since our last write (0: the game's)
uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
void write_data(float a) {
    for (size_t i = 0; i < sizeof kData / sizeof kData[0]; i++) {
        const Datum& d = kData[i];
        const uint32_t now = ld32(d.addr), want = a == kBase ? d.original : bits(d.value(a));
        if (now != d.original && now != g_written[i]) {
            static bool told = false;
            if (!told) LOG("[aspect] %08X holds %08X, not the v208 value: left alone", d.addr, now);
            told = true;
            continue;
        }
        if (now != want) st32(d.addr, want);
        g_written[i] = want;
    }
}
}  // namespace

int choices() { return kChoiceCount; }
const Choice& choice(int i) { return kChoices[i >= 0 && i < kChoiceCount ? i : 0]; }

void load_saved() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::string v;
        if (hostui::get("aspect", v)) {
            for (int i = 0; i < kChoiceCount; i++)
                if (v == kChoices[i].id) g_saved = i;
        } else if (hostui::get("aspectMode", v)) {  // as saved before there were more choices
            static const char* const kOld[] = {"16:9", "window", "16:10", "21:9", "32:9"};
            const int m = atoi(v.c_str());
            for (int i = 0; m >= 0 && m < 5 && i < kChoiceCount; i++)
                if (!strcmp(kOld[m], kChoices[i].id)) g_saved = i;
        }
        if (hostui::get("aspectCustom", v)) g_saved_custom = clamp_aspect(strtof(v.c_str(), nullptr));
        if (hostui::get("aspectHud", v))
            for (int i = 0; i < hud::kModes; i++)
                if (v == kHudIds[i]) g_saved_hud = i;
    });
}
int saved() { return g_saved; }
void set_saved(int i) {
    if (i < 0 || i >= kChoiceCount) return;
    g_saved = i;
    hostui::set("aspect", kChoices[i].id);
    LOG("[aspect] saved: %s", kChoices[i].label);
}
float saved_custom() { return g_saved_custom; }
void set_saved_custom(float a) {
    g_saved_custom = clamp_aspect(a);
    char text[32];
    snprintf(text, sizeof text, "%.4f", g_saved_custom.load());
    hostui::set("aspectCustom", text);
}
int saved_hud() { return g_saved_hud; }
void set_saved_hud(int mode) {
    if (mode < 0 || mode >= hud::kModes) return;
    g_saved_hud = mode;
    hostui::set("aspectHud", kHudIds[mode]);
}

void set_window_aspect(float a) {
    if (!(a > 0.1f && a < 20.0f)) return;
    // window resizes are continuous; steps of 0.5% keep Match window from asking for a restart at every pixel
    float q = std::round(a * 200.0f) / 200.0f;
    if (std::fabs(q - kBase) < 0.006f) q = kBase;  // a 16:9 window (rounded pixel sizes) is 16:9
    g_window.store(q, std::memory_order_relaxed);
}
namespace {
float value_of(int i) {
    const float v = kChoices[i].value;
    float a = v > 0 ? v : v < 0 ? g_saved_custom.load() : g_window.load(std::memory_order_relaxed);
    a = clamp_aspect(a);
    return same(a, kBase) ? kBase : a;
}
// The run's aspect and HUD placement, once: the first time the game asks (a layout being built, a
// camera, the first swap). The windows exist by then, so Match window has the window's shape.
void latch() {
    static std::once_flag once;
    std::call_once(once, [] {
        load_saved();
        float a;
        int chosen = -1;
        if (float pack = mods::cemu::aspect_ratio()) a = pack;
        else if (g_env.set) a = g_env.window ? g_window.load() : g_env.value;
        else a = value_of(chosen = g_saved);
        a = clamp_aspect(a);
        if (same(a, kBase)) a = kBase;
        const int mode = g_env.hud >= 0 ? g_env.hud : g_saved_hud.load();
        if (a != kBase) write_data(a);
        g_hud = mode;
        g_active_choice = chosen;
        g_game = a;
        g_latched = true;
        LOG("[aspect] this run: %.4f (%s), HUD %s", a, chosen >= 0 ? kChoices[chosen].label : "set for the run", kHudIds[mode]);
    });
}
}  // namespace
bool restart_needed() {
    if (!g_latched || g_env.set || mods::cemu::aspect_ratio()) return false;
    // another shape than the run's: another choice, Custom's value changed, or the window's shape did (by
    // more than a sliver of letterbox)
    const int s = g_saved, was = g_active_choice;
    const float now = value_of(s), run = g_game.load();
    const bool window = kChoices[s].value == 0 || (was >= 0 && kChoices[was].value == 0);
    if (window ? std::fabs(now - run) > 0.012f * run : now != run) return true;
    return run != kBase && g_env.hud < 0 && g_saved_hud != g_hud;  // the HUD's placement matters off 16:9
}
float game() {
    latch();
    return g_game.load(std::memory_order_relaxed);
}
int hud_mode() {
    latch();
    return g_hud.load(std::memory_order_relaxed);
}
static uint64_t g_swaps = 0;
uint64_t game_frame() { return g_swaps; }
float on_swap() {
    g_swaps++;
    return game();
}

// ---- narrower than 16:9, HUD centred: the next vertex uniform upload of this thread is a layout / text
// projection, which the render thread narrows: the HUD and menus keep their shape in a 16:9 band
static thread_local bool t_tag = false;
bool take_projection_tag() {
    bool t = t_tag;
    t_tag = false;
    return t;
}
// Wind Waker's layout hooks reported layout roots and menu content; BotW needs neither
void layout_root_target(uint32_t, bool) {}
static bool g_content_clip = false;  // render thread
void set_content_clip(bool clip) { g_content_clip = clip; }
bool content_clip() { return g_content_clip; }
void ss_reset() {}
}  // namespace aspect

// ---- the sites of tools/botw_aspect_patches.py (game threads)
namespace {
constexpr float kBase = 16.0f / 9.0f;
// a viewport of 16:9 guest pixels (1280x720 and its scaled copies), not the GamePad's 854x480
bool tv_shaped(double a) { return std::fabs(a - (double)kBase) < 5e-4; }
}  // namespace

extern "C" double botw_aspect_view(double a) {
    const float g = aspect::game();
    return g == kBase || !tv_shaped(a) ? a : (double)g;
}

extern "C" double botw_aspect_fovy(double fovy, uint32_t viewport) {
    const float g = aspect::game();
    if (g >= kBase) return fovy;  // Hor+ (or 16:9): the vertical field of view stays
    const double w = ldf32(viewport + 0x20) - ldf32(viewport + 0x18), h = ldf32(viewport + 0x24) - ldf32(viewport + 0x1C);
    if (!(h != 0) || !tv_shaped(w / h) || !(fovy > 0 && fovy < 3.0)) return fovy;
    return 2.0 * std::atan(std::tan(fovy * 0.5) * (double)kBase / g);
}

extern "C" double botw_aspect_scaled(double a) {
    const float g = aspect::game();
    return g == kBase ? a : a * (double)g / (double)kBase;
}

extern "C" double botw_aspect_grass() {
    const float g = aspect::game();
    return g > kBase ? (double)kBase / g : 1.0;
}

extern "C" void botw_aspect_tag_projection() {
    if (aspect::game() < kBase && aspect::hud_mode() == aspect::hud::kCentred) aspect::t_tag = true;
}
