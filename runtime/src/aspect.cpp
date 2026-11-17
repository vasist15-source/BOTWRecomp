// Aspect ratio of the TV picture: 16:9 (original), the window's shape, 16:10, 21:9 or 32:9
// (Settings > Display; WWHD_ASPECT=21:9 / window / 2.4 for a run).
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
//    set-ups, a grass distance) is written here at the swap, as Cemu's Graphics pack patches it.
//  - 2D: layout (nw::lyt) and text (nw::font) projections drawn to the TV picture are narrowed on the
//    render thread (gx2_core.cpp OP_SET_PROJ_REGS): the HUD and menus keep their proportions, centred
//    in the wider picture.
// The aspect is latched once per frame on the main thread at GX2SwapScanBuffers; the render thread
// switches its render-target factors at the matching swap (the value travels with the swap command),
// so a change never lands in the middle of a frame. At 16:9 every site returns the game's own values.
#include "aspect.h"
#include "mods/cemu_pack.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "botw_aspect.h"
#include "runtime.h"

namespace aspect {
namespace {
constexpr float kBase = 16.0f / 9.0f;

int parse_env_mode(float& custom) {
    const char* e = getenv("WWHD_ASPECT");
    if (!e || !*e) return kOriginal;
    if (!strcmp(e, "window") || !strcmp(e, "match")) return kWindow;
    if (!strcmp(e, "16:9")) return kOriginal;
    if (!strcmp(e, "16:10")) return k16x10;
    if (!strcmp(e, "21:9")) return k21x9;
    if (!strcmp(e, "32:9")) return k32x9;
    float a = 0;
    if (const char* c = strchr(e, ':')) a = (float)atof(e) / (float)atof(c + 1);
    else a = (float)atof(e);
    if (!(a > 0.5f && a < 8.0f)) return kOriginal;
    custom = a;
    return kCustom;
}
float g_custom = kBase;
std::atomic<int> g_mode{parse_env_mode(g_custom)};
std::atomic<float> g_window{kBase};
// aspect of the frame being built: written by the main thread at the swap, read by the threads
// that compute and record the frame
std::atomic<float> g_game{kBase};

float clamp_aspect(float a) { return std::fmin(std::fmax(a, 1.0f), 4.0f); }
bool same(float a, float b) { return std::fabs(a - b) <= 1e-4f * std::fmax(1.0f, std::fabs(a)); }

// ---- the game's read-only data that holds the TV's aspect (v208 addresses; Cemu's Graphics pack,
// patch_AspectRatio.asm, writes the same). Written at the swap when the aspect changes; a value that is
// neither the game's nor ours (another build, a code mod) is left alone.
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

const char* mode_name(int m) {
    static const char* n[kModes] = {"16:9 (original)", "Match window", "16:10", "21:9", "32:9", "custom"};
    return m >= 0 && m < kModes ? n[m] : "?";
}
float mode_value(int m) {
    switch (m) {
    case kOriginal: return kBase;
    case k16x10: return 16.0f / 10.0f;
    case k21x9: return 64.0f / 27.0f;  // 2560x1080 / 3440x1440-class monitors sold as 21:9
    case k32x9: return 32.0f / 9.0f;
    case kCustom: return g_custom;
    default: return 0;
    }
}
int mode() { return g_mode.load(std::memory_order_relaxed); }
void set_mode(int m) {
    if (m < 0 || m >= kModes) return;
    g_mode = m;
    LOG("[aspect] %s", mode_name(m));
}
void set_window_aspect(float a) {
    if (!(a > 0.1f && a < 20.0f)) return;
    // window resizes are continuous; steps of 0.5% keep render targets from being remade every frame
    float q = std::round(a * 200.0f) / 200.0f;
    if (std::fabs(q - kBase) < 0.006f) q = kBase;  // a 16:9 window (rounded pixel sizes) is 16:9
    g_window.store(q, std::memory_order_relaxed);
}
float requested() {
    if (float pack = mods::cemu::aspect_ratio()) return pack;
    int m = mode();
    return clamp_aspect(m == kWindow ? g_window.load(std::memory_order_relaxed) : mode_value(m));
}
float game() { return g_game; }
static uint64_t g_swaps = 0;
uint64_t game_frame() { return g_swaps; }

float on_swap() {
    // test aid: WWHD_ASPECT_AT=frame:aspect,... switches the aspect at those swaps (aspect 0 = window)
    static std::vector<std::pair<uint64_t, float>> at = [] {
        std::vector<std::pair<uint64_t, float>> v;
        if (const char* e = getenv("WWHD_ASPECT_AT"))
            for (char* p = (char*)e; *p;) {
                uint64_t f = strtoull(p, &p, 10);
                if (*p++ != ':') break;
                v.push_back({f, (float)strtod(p, &p)});
                while (*p == ',') p++;
            }
        return v;
    }();
    g_swaps++;
    for (auto& [f, v] : at)
        if (f == g_swaps) {
            if (v == 0) set_mode(kWindow);
            else if (same(v, kBase)) set_mode(kOriginal);
            else { g_custom = v; set_mode(kCustom); }
        }
    float a = requested();
    if (same(a, kBase)) a = kBase;
    // the GPU work of the frame being submitted now was built with the camera of the previous
    // frame: render targets follow one swap later
    float render = g_game.load();
    if (!same(a, render)) {
        LOG("[aspect] game aspect %.4f from swap %llu", a, (unsigned long long)g_swaps);
        write_data(a);
    }
    g_game = a;
    return render;
}

// ---- HUD: the next vertex uniform upload of this thread is a layout / text projection
static thread_local bool t_tag = false;
bool take_projection_tag() {
    bool t = t_tag;
    t_tag = false;
    return t;
}
// Wind Waker's layout hooks reported layout roots and menu content; BotW's centred HUD needs neither
void layout_root_target(uint32_t, bool) {}
static bool g_content_clip = false;  // render thread
void set_content_clip(bool clip) { g_content_clip = clip; }
bool content_clip() { return g_content_clip; }
void ss_reset() {}
}  // namespace aspect

// ---- the sites of tools/botw_aspect_patches.py (game threads)
using aspect::kBase;
namespace {
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

extern "C" double botw_aspect_grass() {
    const float g = aspect::game();
    return g > kBase ? (double)kBase / g : 1.0;
}

extern "C" void botw_aspect_tag_projection() {
    if (aspect::game() != kBase) aspect::t_tag = true;
}
