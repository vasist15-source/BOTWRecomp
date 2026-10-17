#include <mutex>
// In-game settings overlay: the Dear ImGui user interface (see overlay.h). The renderers draw the
// resulting ImDrawData (gfx/overlay_metal.mm, gfx/vulkan/overlay.cpp); the hosts feed input and apply
// changes on their main thread (hostui.h).
#include "overlay.h"
#include "../audio_out.h"
#include "guest_hud.h"
#include "gamepad_nav.h"
#include "perf_average.h"
#ifdef __ANDROID__
#include "android_telemetry.h"
#endif

#include <algorithm>
#include <cstdarg>
#include <functional>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <set>
#include <thread>
#include <vector>

#include "imgui.h"
#include "trace.h"
#include "hostui.h"
#include "controls_view.h"
#include "text_entry.h"
#include "../crashrec.h"
#include "../game_languages.h"
#include "../gfx/renderer.h"
#ifdef __ANDROID__
#include "../gfx/vulkan/android_driver.h"
#endif
#ifdef WWHD_HAS_VULKAN
#include "../gfx/vulkan/settings.h"
namespace gfxvk { bool buffer_cache_enabled(); }  // gfx/vulkan/buffer_cache.h
#endif
#include "../input.h"
#include "../input_map.h"
#include "../mods/mods.h"
#include "../mods/manager.h"
#include "../mods/packages.h"
#include "../mods/catalogue_client.h"
#include "../platform/https.h"
#include "guest_addr.h"
#include "../motion/motion.h"
#include "../platform/keycodes.h"
#include "../rumble.h"
#include "../interp.h"
#include "../runtime.h"
#include "../savestate.h"
#include "../screenshot.h"
#include "../render_prof.h"
#include "../perf_metrics.h"
#include "../build_info.h"
#include "../report_header.h"

namespace gx2 { uint64_t flips_presented(); bool uncapped(); void set_uncapped(bool on); int fps_target(); void set_fps_target(int fps); }

namespace overlay {
namespace {

using clock = std::chrono::steady_clock;
double now_s() { return std::chrono::duration<double>(clock::now().time_since_epoch()).count(); }

std::atomic<bool> g_open{false};
std::atomic<bool> g_perf{false};
std::atomic<bool> g_metal_hud{false};
PerfAverage g_average;
std::atomic<float> g_density{1.0f};
std::atomic<bool> g_wait_release{false};  // just closed: the game sees no buttons until all are released
std::atomic<double> g_last_frame{0};      // frame() ran (alive(): the game's text prompt can show)
const bool g_no_host = getenv("WWHD_NO_HOST_INPUT") != nullptr;  // test runs ignore the user's input
// ... except keys a test posts itself (WWHD_TEST_POST_KEYS, gfx/input.mm; the hidden test window never
// has the user's keyboard)
const bool g_no_host_keys = g_no_host && !getenv("WWHD_TEST_POST_KEYS");
bool g_pad_b_used = false;  // B answered a dialog this frame: it does not also close the menu

// input events from the host's main thread, replayed into ImGui on the render thread
struct Event {
    enum Kind { Key, MousePos, MouseButton, Wheel, Focus, Text } kind;
    int code = 0;
    bool down = false;
    float x = 0, y = 0;
    int mods = 0;
    std::string text = {};  // Text: typed characters (UTF-8)
};
std::mutex g_mu;
std::vector<Event> g_events;
// the system clipboard as the host last read it (main thread, before it passes the paste shortcut on);
// Dear ImGui reads it on the render thread when Ctrl/Cmd+V reaches a text field
std::string g_clipboard;
std::atomic<bool> g_wants_text{false};  // a text field of the overlay has the keyboard
std::atomic<int> g_capture_key{-2};  // remap: key pressed while capturing (-2 none, -1 cancel, -3 clear)
std::atomic<bool> g_capturing_keys{false};
// keys held while the overlay is open (the Controls tab lights them; the game never sees them)
std::atomic<bool> g_held_keys[256];

void push(const Event& e) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_events.size() < 4096) g_events.push_back(e);
}

ImGuiKey imgui_key(int code) {
    switch (code) {
    case kVK_ANSI_A: return ImGuiKey_A; case kVK_ANSI_B: return ImGuiKey_B; case kVK_ANSI_C: return ImGuiKey_C;
    case kVK_ANSI_D: return ImGuiKey_D; case kVK_ANSI_E: return ImGuiKey_E; case kVK_ANSI_F: return ImGuiKey_F;
    case kVK_ANSI_G: return ImGuiKey_G; case kVK_ANSI_H: return ImGuiKey_H; case kVK_ANSI_I: return ImGuiKey_I;
    case kVK_ANSI_J: return ImGuiKey_J; case kVK_ANSI_K: return ImGuiKey_K; case kVK_ANSI_L: return ImGuiKey_L;
    case kVK_ANSI_M: return ImGuiKey_M; case kVK_ANSI_N: return ImGuiKey_N; case kVK_ANSI_O: return ImGuiKey_O;
    case kVK_ANSI_P: return ImGuiKey_P; case kVK_ANSI_Q: return ImGuiKey_Q; case kVK_ANSI_R: return ImGuiKey_R;
    case kVK_ANSI_S: return ImGuiKey_S; case kVK_ANSI_T: return ImGuiKey_T; case kVK_ANSI_U: return ImGuiKey_U;
    case kVK_ANSI_V: return ImGuiKey_V; case kVK_ANSI_W: return ImGuiKey_W; case kVK_ANSI_X: return ImGuiKey_X;
    case kVK_ANSI_Y: return ImGuiKey_Y; case kVK_ANSI_Z: return ImGuiKey_Z;
    case kVK_ANSI_0: return ImGuiKey_0; case kVK_ANSI_1: return ImGuiKey_1; case kVK_ANSI_2: return ImGuiKey_2;
    case kVK_ANSI_3: return ImGuiKey_3; case kVK_ANSI_4: return ImGuiKey_4; case kVK_ANSI_5: return ImGuiKey_5;
    case kVK_ANSI_6: return ImGuiKey_6; case kVK_ANSI_7: return ImGuiKey_7; case kVK_ANSI_8: return ImGuiKey_8;
    case kVK_ANSI_9: return ImGuiKey_9;
    case kVK_Return: case kVK_ANSI_KeypadEnter: return ImGuiKey_Enter;
    case kVK_Tab: return ImGuiKey_Tab;
    case kVK_Space: return ImGuiKey_Space;
    case kVK_Delete: return ImGuiKey_Backspace;
    case kVK_ForwardDelete: return ImGuiKey_Delete;
    case kVK_Escape: return ImGuiKey_Escape;
    case kVK_LeftArrow: return ImGuiKey_LeftArrow; case kVK_RightArrow: return ImGuiKey_RightArrow;
    case kVK_UpArrow: return ImGuiKey_UpArrow; case kVK_DownArrow: return ImGuiKey_DownArrow;
    case kVK_Home: return ImGuiKey_Home; case kVK_End: return ImGuiKey_End;
    case kVK_PageUp: return ImGuiKey_PageUp; case kVK_PageDown: return ImGuiKey_PageDown;
    case kVK_Shift: return ImGuiKey_LeftShift; case kVK_RightShift: return ImGuiKey_RightShift;
    case kVK_Control: return ImGuiKey_LeftCtrl; case kVK_RightControl: return ImGuiKey_RightCtrl;
    case kVK_Option: return ImGuiKey_LeftAlt; case kVK_RightOption: return ImGuiKey_RightAlt;
    case kVK_Command: return ImGuiKey_LeftSuper; case kVK_RightCommand: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

// ---------------------------------------------------------------- UI state (render thread)
enum Tab { kSaves, kGraphics, kDisplay, kAudio, kControls, kAbout, kTabs };  // no Mods: Wind Waker code mods and cheats
const char* const kTabNames[kTabs] = {"Saves", "Graphics", "Display", "Audio", "Controls", "Language / About"};
const char* const kTabIds[kTabs] = {"saves", "graphics", "display", "audio", "controls", "about"};

struct Ui {
    bool init = false;
    int tab = kSaves, select_tab = -1;  // select_tab: switch to this tab next frame (shoulder buttons, tests)
    double last_time = 0;
    // controller
    float values[input_map::kPadCount] = {};
    float prev[input_map::kPadCount] = {};
    GamepadNavigation gamepad_navigation;
    ImVec2 mouse_position = {-FLT_MAX, -FLT_MAX};
    double options_since = -1;
    bool options_latched = false;
    // remap capture: action, column (0, 1 keys; 2 controller)
    int cap_action = -1, cap_col = 0;
    bool cap_pad_released = false;
    double cap_started = 0;
    std::string cap_note;
    // save state slots, refreshed twice a second while the Saves tab is shown
    ss::SlotInfo slots[ss::kSlots + 1];
    crashrec::AutoInfo autos[crashrec::kAutoSlots + 1];
    double slots_time = -1;
    // performance
    double fps_t0 = 0, fps = 0;
    uint64_t fps_n0 = 0;
    float frame_ms[120] = {};
    int frame_i = 0;
    double last_present = 0;
    // language (applies on the next start)
    int language = -1;
    int language_at_start = -1;  // the language this start runs with (game_lang: the one on the disc)
    int language_region = 0;     // game_lang region of a language source (0: the installed game)
    int language_region_at_start = 0;
    bool linearized = false;
    bool just_opened = false;
    bool list_view = false;  // Controls: the table instead of the drawing
    int hover_action = -1;   // Controls: input under the cursor (status line)
};
Ui U;

bool controller_down(int p) { return U.values[p] > 0.5f; }
bool controller_pressed(int p) { return U.values[p] > 0.5f && U.prev[p] <= 0.5f; }
bool navigation_pressed(int p) { return U.gamepad_navigation.pressed(U.values, U.prev, p); }

void post_changed(std::function<void()> fn) {
    hostui::post([fn] {
        fn();
        hostui::graphics_changed();
    });
}

// ---------------------------------------------------------------- style
void setup_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.PopupRounding = 6;
    s.GrabRounding = 5;
    s.TabRounding = 6;
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(10, 7);
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 1;  // unchecked boxes and radio buttons stay visible on the dark background
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    ImVec4* c = s.Colors;
    // deep sea blue with a teal accent
    c[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.07f, 0.12f, 0.975f);  // sRGB targets blend in linear light: keep it dense
    c[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.13f, 0.20f, 0.35f);
    c[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.11f, 0.18f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.30f, 0.62f, 0.70f, 0.45f);
    c[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.20f, 0.30f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.27f, 0.38f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.22f, 0.32f, 0.85f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.34f, 0.46f, 0.90f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.42f, 0.54f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.12f, 0.36f, 0.48f, 0.90f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.50f, 0.62f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.60f, 0.70f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.12f, 0.36f, 0.48f, 0.70f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.50f, 0.62f, 0.85f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.60f, 0.70f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.08f, 0.24f, 0.34f, 0.90f);
    c[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.52f, 0.64f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.16f, 0.44f, 0.56f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.95f, 0.85f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.80f, 0.80f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.55f, 0.95f, 0.90f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.62f, 0.70f, 0.35f);
    c[ImGuiCol_NavCursor] = ImVec4(1.0f, 0.85f, 0.35f, 1.0f);  // controller cursor: gold, easy to follow
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.08f, 0.24f, 0.34f, 1.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
}

// a font with arrows and accents if the system has one; Dear ImGui's own scalable font otherwise
void setup_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    static const char* const candidates[] = {
#if defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
#elif defined(_WIN32)
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
#endif
    };
    for (const char* path : candidates) {
        if (FILE* f = fopen(path, "rb")) {
            fclose(f);
            if (io.Fonts->AddFontFromFileTTF(path, 17.0f)) {
                LOG("[overlay] font %s", path);
                return;
            }
        }
    }
    ImFontConfig cfg;
    cfg.SizePixels = 17.0f;
    io.Fonts->AddFontDefaultVector(&cfg);
}

void init_context() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written next to the game
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "wwhd";
    io.ConfigNavCaptureKeyboard = true;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        static std::string copy;
        std::lock_guard<std::mutex> lk(g_mu);
        copy = g_clipboard;
        return copy.c_str();
    };
    pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
        std::string copy = text ? text : "";
        { std::lock_guard<std::mutex> lk(g_mu); g_clipboard = copy; }
        hostui::post([copy] { hostui::set_clipboard(copy); });
    };
    setup_style();
    setup_fonts();
    ImGui::GetStyle().FontSizeBase = 17.0f;
    U.init = true;
}

// ---------------------------------------------------------------- test switch
struct TestSwitch {
    bool open = false, perf = false;
    int tab = -1;
    uint64_t at = 1;
    bool done = false;
};
TestSwitch parse_test() {
    TestSwitch t;
    const char* e = getenv("WWHD_TEST_OVERLAY");
    if (!e || !*e) { t.done = true; return t; }
    std::string s = e;
    if (size_t at = s.find('@'); at != std::string::npos) {
        t.at = strtoull(s.c_str() + at + 1, nullptr, 10);
        s.resize(at);
    }
    std::string what = s, tab;
    if (size_t c = s.find(':'); c != std::string::npos) { what = s.substr(0, c); tab = s.substr(c + 1); }
    t.open = what == "open";
    t.perf = what == "perf";
    for (int i = 0; i < kTabs; i++)
        if (tab == kTabIds[i]) t.tab = i;
    return t;
}

// ---------------------------------------------------------------- controller
// debug: WWHD_TEST_PAD=320-400:B+LeftStickRight:0.8+LeftStickUp:0.4 holds host controller inputs
// (controls.json names, value 1 unless given) during TV frames 320..400. They reach only the
// Controls tab's live display and the game's text prompt (not the menu navigation, capture or the game).
void test_pad(float* v) {
    struct Hold { uint64_t from, to; int pad; float value; };
    static const std::vector<Hold> holds = [] {
        std::vector<Hold> out;
        const char* e = getenv("WWHD_TEST_PAD");
        while (e && *e) {
            unsigned long long a, b;
            int n;
            if (sscanf(e, "%llu-%llu:%n", &a, &b, &n) != 2) break;
            e += n;
            for (;;) {
                size_t len = strcspn(e, "+,");
                std::string item(e, len), id = item;
                float value = 1;
                if (size_t c = item.find(':'); c != std::string::npos) { id = item.substr(0, c); value = (float)atof(item.c_str() + c + 1); }
                int p = input_map::pad_from_id(id);
                if (p > 0) out.push_back({a, b, p, value});
                else LOG("[overlay] WWHD_TEST_PAD: unknown controller input %s", id.c_str());
                e += len;
                if (*e != '+') break;
                e++;
            }
            if (*e != ',') break;
            e++;
        }
        return out;
    }();
    const uint64_t f = render::frame_count();
    for (const Hold& h : holds)
        if (f >= h.from && f <= h.to) v[h.pad] = std::max(v[h.pad], h.value);
}

void read_controller() {
    std::copy(std::begin(U.values), std::end(U.values), std::begin(U.prev));
    if (g_no_host) {
        std::fill(std::begin(U.values), std::end(U.values), 0.0f);
        if (g_wait_release.load()) g_wait_release = false;
        return;
    }
    input::host_controller_values(U.values);
    using namespace input_map;
    const double t = now_s();
    // Home: toggles; Select / Minus (View / Share): held half a second opens, a press closes
    if (controller_pressed(kPadHome)) set_open(!is_open());
    if (controller_down(kPadOptions)) {
        if (!U.options_latched) {
            if (is_open()) {
                set_open(false);
                U.options_latched = true;
            } else {
                if (U.options_since < 0) U.options_since = t;
                if (t - U.options_since >= 0.5) {
                    set_open(true);
                    U.options_latched = true;
                }
            }
        }
    } else {
        U.options_since = -1;
        U.options_latched = false;
    }
    if (g_wait_release.load()) {
        bool any = false;
        for (int p = 1; p < kPadCount; p++) any |= U.values[p] > 0.3f;
        if (!any) g_wait_release = false;
    }
}

// ---------------------------------------------------------------- widgets
void help(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", text);
}
void heading(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.95f, 0.85f, 1.0f));
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}
void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.78f, 0.84f, 1.0f));
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}
void warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.35f, 1.0f));
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}
// a radio button that runs `set` on the main thread when chosen
bool radio(const char* label, bool active, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::RadioButton(label, active) && !active;
    ImGui::EndDisabled();
    return clicked;
}
bool check(const char* label, bool value, bool* out, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    bool v = value;
    bool changed = ImGui::Checkbox(label, &v);
    ImGui::EndDisabled();
    *out = v;
    return changed;
}

// ---------------------------------------------------------------- tabs
void refresh_slots(bool force) {
    double t = now_s();
    if (!force && U.slots_time >= 0 && t - U.slots_time < 0.5) return;
    U.slots_time = t;
    for (int i = 1; i <= ss::kSlots; i++) U.slots[i] = ss::slot_info(i);
    for (int i = 1; i <= crashrec::kAutoSlots; i++) U.autos[i] = crashrec::auto_info(i);
}

void tab_saves() {
    refresh_slots(false);
    std::string msg = ss::last_message();
    if (!msg.empty()) note("%s", msg.c_str());
    heading("Save states");
    note("A save state is the whole running game (large, contains game data: never share it). Shift+F1..F5 save in "
         "game, F2..F5 load (F1 opens this menu).");
    if (ImGui::BeginTable("slots", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Saved");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        for (int i = 1; i <= ss::kSlots; i++) {
            const ss::SlotInfo& s = U.slots[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Slot %d", i);
            ImGui::TableNextColumn();
            if (!s.used) ImGui::TextDisabled("empty");
            else {
                std::string d = s.when + (s.area.empty() ? "" : "  -  " + s.area);
                if (!s.controller.empty()) d += "  -  " + s.controller;
                if (!s.portable) d += "  (full)";
                if (!s.compatible) d += "  (incompatible)";
                ImGui::TextUnformatted(d.c_str());
                if (s.older_other && s.portable) {
                    // decision: an older full state stays on disk; say so (it is large and must not be shared)
                    ImGui::TextDisabled("also holds an older full state (slot%d.bin, %.0f MB)", i, s.older_bytes / 1048576.0);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Kept on disk, not loaded: the newer portable state is. It contains game data: "
                                          "don't share it. Delete it in the states folder if you no longer need it.");
                } else if (s.older_other) {
                    ImGui::TextDisabled("also holds an older portable state (slot%d.wwstate)", i);
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::Button("Save")) {
                ss::request_save(i);
                U.slots_time = -1;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!s.used || !s.compatible);
            if (ImGui::Button("Load")) {
                ss::request_load(i);
                set_open(false);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    note("States folder: %s", ss::states_dir().c_str());
    heading("Screenshots");
    {
        // the Screenshot binding (Controls tab): its keys and controller input
        const input_map::Mapping m = input_map::current();
        std::string keys;
        for (int k : m.keys[input_map::kScreenshot])
            if (k != input_map::kNoKey) keys += (keys.empty() ? "" : " or ") + input_map::key_label(k);
        if (m.pad[input_map::kScreenshot] != input_map::kPadNone)
            keys += (keys.empty() ? "controller " : " or controller ") + std::string(input_map::pad_short_label(m.pad[input_map::kScreenshot]));
        if (keys.empty()) note("Screenshot is not bound: set a key in the Controls tab (Screenshot).");
        else note("%s in game saves the TV picture as a PNG at the internal resolution, without this menu (change the key in Controls).", keys.c_str());
    }
    if (hostui::can_open_folder()) {
        if (ImGui::Button("Open screenshots folder")) {
            std::string d = screenshot::dir();
            hostui::post([d] { hostui::open_folder(d); });
        }
        ImGui::SameLine();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", screenshot::dir().c_str());
    heading("Crash Recovery");
    bool on;
    if (check("Crash Recovery (automatic state every few minutes)", crashrec::enabled(), &on)) crashrec::set_enabled(on);
    help("Saves the game into automatic states in the background and records the controller input since the latest "
         "one. After a crash, the crash log says how to load it. Saving freezes the game for a moment.");
    for (int i = 1; i <= crashrec::kAutoSlots; i++) {
        const crashrec::AutoInfo& a = U.autos[i];
        ImGui::PushID(100 + i);
        ImGui::BeginDisabled(!a.used);
        if (ImGui::Button("Load")) {
            crashrec::request_load(i);
            set_open(false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (a.used) ImGui::Text("Automatic state %d:  %s%s%s", i, a.when.c_str(), a.area.empty() ? "" : "  -  ", a.area.c_str());
        else ImGui::TextDisabled("Automatic state %d:  empty", i);
        ImGui::PopID();
    }
}

std::string performance_device() {
    std::string device = render::device();
#ifdef __APPLE__
    if (!render::vulkan()) device += ", driver: " + reporthdr::os_description();
#endif
#ifdef __ANDROID__
    const auto driver = gfxvk::drivers::active_name();
    if (!driver.empty()) device += " (" + driver + ")";
#endif
    return device.empty() ? "GPU/driver: n/a" : device;
}

std::string performance_report() {
    float sum = 0, worst = 0;
    for (float f : U.frame_ms) { sum += f; worst = std::max(worst, f); }
    char frame[180];
    snprintf(frame, sizeof frame, "FPS %.1f (average %.1f), frame %.2f ms (worst %.2f), average logic steps/s %.1f\n",
             U.fps, g_average.fps, sum / 120.0f, worst, g_average.logic);
    std::string report = std::string(frame) + perf::summary();
#ifdef __ANDROID__
    report += android_telemetry() + "\n";
#endif
    report += "CPU time is thread running time per output frame. GPU submission intervals may include stalls; not GPU load.\n";
    const std::string detailed = rprof::latest_report();
    report += detailed.empty() ? "No detailed report yet: play for a few seconds, then copy again.\n" : detailed;
    // which build, system, GPU and rendering-path switches (report_header.h)
    reporthdr::Info h;
    h.version = build::version();
    h.commit = build::commit();
    h.os = reporthdr::os_description();
    h.gpu = performance_device();
    h.renderer = render::vulkan() ? "Vulkan" : "Metal";
    h.host = hostui::name();
    h.fps = interp::mode() == 2 ? "true 60 fps" : interp::mode() == 1 ? "60 fps interpolation" : "30 fps";
    h.scale = hostui::res_scale();
#ifdef WWHD_HAS_VULKAN
    if (render::vulkan()) {
        h.bufferCache = gfxvk::buffer_cache_enabled();
        h.overrides = reporthdr::vulkan_overrides([](const char* n) -> const char* { return getenv(n); });
    }
#endif
    if (const int g = motion::settings().source; g != motion::kOff) h.gyro = motion::source_id(g);
    report = reporthdr::format(h) + report;
    return report;
}

void tab_graphics() {
#ifdef __ANDROID__
    heading("GPU driver (Snapdragon / Adreno)");
    note("Active: %s", gfxvk::drivers::active_name().c_str());
    auto driver_action = [](auto fn) { hostui::post([fn] { try { fn(); } catch (const std::exception& e) { LOG("[vulkan driver] %s", e.what()); } }); };
    if (ImGui::Button("Install driver ZIP...")) driver_action([] { gfxvk::drivers::request_install(); });
    auto selected = gfxvk::drivers::selection();
    if (radio("System driver", selected.empty())) driver_action([] { gfxvk::drivers::select(""); });
    for (const auto& driver : gfxvk::drivers::installed()) {
        ImGui::PushID(driver.id.c_str());
        auto id = driver.id;
        if (radio((driver.name + " " + driver.version).c_str(), selected == id))
            driver_action([id] { gfxvk::drivers::select(id); });
        ImGui::SameLine();
        if (ImGui::Button("Remove")) driver_action([id] { gfxvk::drivers::remove(id); });
        ImGui::PopID();
    }
    note("%s", gfxvk::drivers::message().c_str());
    help("Driver changes take effect on restart. An unfinished first 120-frame probe selects the system driver on the next start.");
#endif
    heading("Frame rate");
    {
        const int t = gx2::fps_target();
        if (radio("30 fps (original)", t == 30)) post_changed([] { gx2::set_fps_target(30); });
        ImGui::SameLine();
        if (radio("60 fps", t == 60)) post_changed([] { gx2::set_fps_target(60); });
        ImGui::SameLine();
        if (radio("Uncapped", t == 0)) post_changed([] { gx2::set_fps_target(0); });
        help("Above 30 fps the game's own frame-time scaling follows the real frame time, so it keeps its\n"
             "normal speed at any frame rate (and no longer slows down when frames take longer than 1/30 s,\n"
             "down to 20 fps). Uncapped: as many frames as this computer can draw, without vsync.");
    }
    // debug only, not saved (gx2::uncapped)
    bool unc;
    if (check("Uncapped (debug: the game runs too fast)", gx2::uncapped(), &unc)) hostui::post([unc] { gx2::set_uncapped(unc); });
    help("Debug only, to see how many frames a second this computer can draw: no frame limit and no\n"
         "vsync. The game counts frames, so it runs faster than normal. The frame rate is in the window\n"
         "title and the performance overlay. Not saved. (With frame interpolation and Keep game speed\n"
         "on, the game logic still keeps 30 steps a second.)");

    heading("Internal resolution");
    static const float scales[] = {1.0f, 1.5f, 2.0f, 3.0f};
    static const char* const names[] = {"1x  (1280x720)", "1.5x  (1920x1080)", "2x  (2560x1440)", "3x  (3840x2160)"};
    float cur = hostui::res_scale();
    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine();
        float s = scales[i];
        if (radio(names[i], std::fabs(cur - s) < 0.01f)) post_changed([s] { hostui::set_res_scale(s); });
    }

    heading("Effects");
    bool v;
    if (check("16x anisotropic filtering", render::aniso(), &v, render::feature_available(render::kFeatureAniso)))
        post_changed([v] { render::set_aniso(v); });
    ImGui::SameLine(0, 30);
    if (check("Edge smoothing (FXAA)", render::fxaa(), &v, render::feature_available(render::kFeatureFXAA)))
        post_changed([v] { render::set_fxaa(v); });
    heading("Overlay");
    if (check("Performance overlay (FPS, frame time)", perf_shown(), &v)) set_perf_shown(v);
    if (!render::vulkan()) {
        bool hud;
        if (check("Metal HUD instead (Apple's: FPS, GPU time, memory)", metal_hud_shown(), &hud)) set_metal_hud_shown(hud);
        help("Apple's Metal performance HUD in the corner of the game window, in place of the overlay above.\n"
             "Turning it on takes effect after a restart (Metal starts it with the app).");
        if (metal_hud_shown() && !getenv("MTL_HUD_ENABLED")) note("Restart the game to show the Metal HUD.");
    }
    if (ImGui::Button("Reset performance averages")) { g_average.reset(); perf::reset(); }
    // the render-thread profiler's latest report (render_prof.h), for performance bug reports
    static double copiedAt = -10;
    if (ImGui::Button("Copy performance report")) {
        std::string report = performance_report();
        hostui::post([report] { hostui::set_clipboard(report); });
        copiedAt = ImGui::GetTime();
    }
    if (ImGui::GetTime() - copiedAt < 2.0) {
        ImGui::SameLine();
        ImGui::TextDisabled("Copied");
    }
    help("Where the renderer spends its time over the last few seconds, as text for a bug report");
    heading("Bug reports");
    // read once at start-up (main.cpp start_log_file); WWHD_LOG_FILE decides when set
    static bool logFile = [] { std::string v; return hostui::get("logFile", v) && v == "1"; }();
    if (check("Write a log file", logFile, &v, !getenv("WWHD_LOG_FILE"))) {
        logFile = v;
        hostui::post([v] { hostui::set("logFile", v ? "1" : "0"); });
    }
    help("From the next start: captures/wwhd.log in the game's data folder (the previous run's is kept as "
         "wwhd-previous.log), with your user paths removed. Turn it on before reproducing a bug and attach "
         "the file to the bug report.");
}

void tab_display() {
    bool v;
    heading("Window");
    if (check("Full screen", hostui::fullscreen(), &v)) hostui::post([v] { hostui::set_fullscreen(v); });
#ifndef __ANDROID__  // always full screen there
    help(!strcmp(hostui::name(), "AppKit") ? "The TV window (Cmd+F); remembered for the next start"
                                           : "The TV window (F11 or Alt+Enter); remembered for the next start");
#endif
    heading("Picture scaling");
    static const char* const f[] = {"Smooth", "Sharp", "Integer scale (pixel exact)"};
    const bool fok = hostui::scale_filter_available();
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        if (radio(f[i], hostui::scale_filter() == i, fok)) hostui::post([i] { hostui::set_scale_filter(i); });
    }
}

void start_capture(int a, int col) {
    U.cap_action = a;
    U.cap_col = col;
    U.cap_pad_released = false;
    U.cap_started = now_s();
    U.cap_note.clear();
    g_capture_key = -2;
    g_capturing_keys = col != kColPad;
}

void clear_binding(int a, int col) {
    input_map::Mapping m = input_map::current();
    if (col == kColPad) input_map::set_pad_binding(m, a, input_map::kPadNone);
    else m.keys[a][col] = input_map::kNoKey;
    input_map::set_current(m);
}

void apply_capture() {
    if (U.cap_action < 0) return;
    using namespace input_map;
    input_map::Mapping m = input_map::current();
    const int a = U.cap_action, colm = U.cap_col;
    int code = g_capture_key.exchange(-2);
    bool done = false;
    if (code == -1) done = true;  // Esc: cancel
    else if (code == -3) {         // Backspace / Delete: clear
        if (colm == kColPad) set_pad_binding(m, a, kPadNone);
        else m.keys[a][colm == kColAny ? 0 : colm] = kNoKey;
        input_map::set_current(m);
        done = true;
    } else if (code >= 0 && colm != kColPad) {
        if (const char* r = input_map::reserved_key(code)) U.cap_note = std::string("That key is used for ") + r;
        else {
            m.keys[a][colm == kColAny ? 0 : colm] = code;
            input_map::set_current(m);
            done = true;
        }
    }
    if (!done) {
        // controller: wait for all inputs to be released, then take the first one pressed (controller
        // slots); for a key slot, B cancels
        int pressed = -1;
        bool any = false;
        for (int p = 1; p < kPadCount; p++) {
            any |= U.values[p] > 0.3f;
            if (U.values[p] > 0.6f && pressed < 0) pressed = p;
        }
        if (!U.cap_pad_released) U.cap_pad_released = !any;
        else if (pressed > 0) {
            if (colm == kColPad || colm == kColAny) {
                set_pad_binding(m, a, pressed);
                input_map::set_current(m);
            }
            done = true;  // (key slots: any controller input cancels)
            g_pad_b_used = true;  // issue #111: the input just assigned (B too) does not also close the menu this frame
        }
    }
    if (!done && now_s() - U.cap_started > 8) done = true;  // nothing pressed: give up
    if (done) {
        U.cap_action = -1;
        g_capturing_keys = false;
        g_wait_release = true;  // the controller input just assigned does not also act in the menu
    }
}

std::string summary(const input_map::Mapping& m, int a) {
    std::string out;
    for (int k : m.keys[a])
        if (k != input_map::kNoKey) out += (out.empty() ? "" : "  -  ") + input_map::key_label(k);
    if (m.pad[a] != input_map::kPadNone) out += (out.empty() ? "controller " : "  -  controller ") + std::string(input_map::pad_label(m.pad[a]));
    return out.empty() ? "not bound" : out;
}

// the table (List view)
void controls_list(input_map::Mapping& m, float h) {
    if (ImGui::BeginTable("map", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame,
                          ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Wii U input", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Key 2");
        ImGui::TableSetupColumn("Controller");
        ImGui::TableHeadersRow();
        for (int a = 0; a < input_map::kActionCount; a++) {
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const bool conflict = input_map::has_conflict(m, a);
            if (conflict) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.20f, 1.0f));
            ImGui::TextUnformatted(input_map::action_label(a));
            if (conflict) ImGui::PopStyleColor();
            for (int col = 0; col < 3; col++) {
                ImGui::TableNextColumn();
                std::string label;
                if (U.cap_action == a && (U.cap_col == col || (U.cap_col == kColAny && col != kColKey1))) label = "...";
                else if (col < 2) label = m.keys[a][col] == input_map::kNoKey ? "-" : input_map::key_label(m.keys[a][col]);
                else label = m.pad[a] == input_map::kPadNone ? "-" : input_map::pad_label(m.pad[a]);
                label += "##c" + std::to_string(col);
                if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0)) && U.cap_action < 0) start_capture(a, col);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                    (ImGui::IsItemFocused() && U.cap_action < 0 &&
                     (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))))
                    clear_binding(a, col);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------- gyro (motion/motion.h)
void save_gyro(const motion::Settings& g) {
    hostui::post([g] {
        motion::set_settings(g);
        for (const char* k : motion::kKeys) hostui::set(k, motion::value_of(g, k));
    });
}
void load_gyro() {
    motion::Settings g;
    std::string v;
    bool saved = false, axis = false;
    for (const char* k : motion::kKeys)
        if (hostui::get(k, v)) {
            motion::from_kv(g, k, v);
            saved = true;
            axis |= !strcmp(k, "gyro.axis");
        }
    if (saved && !axis) motion::upgrade_from_first_release(g);
    motion::set_settings(g);
}
// the Gyro window (Controls tab > Gyro...): source, axis, sensitivity, invert, recalibrate, Cemuhook server
void gyro_window(bool& open) {
    const char* title = "Gyro aiming##gyro";
    if (open) { ImGui::OpenPopup(title); open = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    motion::Settings g = motion::settings(), before = g;
    bool v;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
    note("On the Wii U you aim the bow and the Sheikah Slate's scope by moving the controller. This works with the "
         "GamePad and the Pro Controller choice alike. The game's own Options > Motion Controls still decides "
         "whether it uses the motion.");
    for (int i = 0; i < motion::kSourceCount; i++)
        if (radio(motion::source_label(i), g.source == i)) g.source = i;
    if (motion::env_override()) note("WWHD_GYRO=%s overrides the saved source.", getenv("WWHD_GYRO"));
    if (g.source == motion::kOff && motion::gyro_controllers() > 0) note("A controller with a gyro is connected: choose Controller gyro to use it.");
    if (g.source == motion::kController || g.source == motion::kCemuhook) {
        ImGui::TextUnformatted("Turn left/right by");
        for (int a = 0; a < motion::kAxisModeCount; a++) {
            ImGui::SameLine();
            if (radio(motion::axis_label(a), g.tuning.axis == a)) g.tuning.axis = a;
        }
        help("Player space: turning the controller left or right about the real vertical, however you hold it "
             "(recommended). Yaw: turning it about its own vertical axis (as if it lay flat). Roll: tilting it to "
             "the side like a steering wheel. Tilting its top up or down always looks up or down.");
    }
    const float lo = motion::Tuning::kMinSensitivity, hi = motion::Tuning::kMaxSensitivity;
    ImGui::SetNextItemWidth(220);
    ImGui::SliderFloat("Sensitivity left/right", &g.tuning.sensitivity_x, lo, hi, "%.2fx", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine(0, 16);
    if (check("Invert##x", g.tuning.invert_x, &v)) g.tuning.invert_x = v;
    ImGui::SetNextItemWidth(220);
    ImGui::SliderFloat("Sensitivity up/down", &g.tuning.sensitivity_y, lo, hi, "%.2fx", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine(0, 16);
    if (check("Invert##y", g.tuning.invert_y, &v)) g.tuning.invert_y = v;
    if (ImGui::Button("Default sensitivity"))
        g.tuning.sensitivity_x = g.tuning.sensitivity_y = motion::Tuning::kDefaultSensitivity;
    help("1.0 turns the view as far as moving a real Wii U GamePad by the same angle would (about twice the "
         "controller's turn); the default 0.5 lets the view follow the controller about one to one.");
    if (g.source == motion::kMouse) {
        ImGui::SetNextItemWidth(220);
        ImGui::SliderFloat("Mouse: degrees per point", &g.mouse_degrees, 0.01f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        help("How far one point of mouse movement turns the GamePad (before the sensitivity above). With Steam "
             "Input's gyro to mouse, tune this and Steam's own sensitivity together.");
        note("While the game aims, the pointer is captured and the mouse turns the GamePad (the mouse camera mod "
             "leaves it alone then).");
    }
    if (g.source == motion::kCemuhook) {
        static char host[256] = "";
        static int port = 0;
        static bool init = false;
        if (!init || ImGui::IsWindowAppearing()) { snprintf(host, sizeof host, "%s", g.dsu_host.c_str()); port = g.dsu_port; init = true; }
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputText("Server", host, sizeof host, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
            g.dsu_host = host[0] ? host : "127.0.0.1";
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputInt("Port", &port, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
            g.dsu_port = std::clamp(port, 1, 65535);
        ImGui::SetNextItemWidth(120);
        int slot = g.dsu_slot + 1;
        if (ImGui::SliderInt("Controller slot", &slot, 1, 4)) g.dsu_slot = slot - 1;
        note("A Cemuhook (DSU) server: DS4Windows, BetterJoy, SteamDeckGyroDSU or a phone app; default 127.0.0.1, port 26760.");
    }
    // recalibrate: a controller input and/or a key
    const char* pad_name = g.recenter_pad > 0 ? input_map::pad_label(g.recenter_pad) : "None";
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Recalibrate: controller", pad_name)) {
        for (int p = 0; p < input_map::kPadCount; p++)
            if (ImGui::Selectable(p ? input_map::pad_label(p) : "None", g.recenter_pad == p)) g.recenter_pad = p;
        ImGui::EndCombo();
    }
    std::string key_name = g.recenter_key >= 0 ? input_map::key_label(g.recenter_key) : "None";
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("Recalibrate: key", key_name.c_str())) {
        if (ImGui::Selectable("None", g.recenter_key < 0)) g.recenter_key = -1;
        for (int k = 0; k < 256; k++) {
            std::string id = input_map::key_id(k);
            if (id.rfind("Key", 0) == 0) continue;  // unnamed codes
            if (ImGui::Selectable(input_map::key_label(k).c_str(), g.recenter_key == k)) g.recenter_key = k;
        }
        ImGui::EndCombo();
    }
    help("If the view drifts while the controller rests, recalibrate and put the controller down for a second: "
         "the gyro's offset is learnt anew. The button or key also reaches the game if the controls use it; pick "
         "a free one.");
    if (ImGui::Button("Recalibrate now")) motion::recalibrate();
    ImGui::SameLine();
    ImGui::TextUnformatted(motion::status().c_str());
    ImGui::PopTextWrapPos();
    if (!(g == before)) save_gyro(g);
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void tab_controls() {
    bool v;
    input_map::Mapping m = input_map::current();
    // top: which controller the keyboard and controllers act as (the drawing follows), view switch
    const bool pro = input::pro_controller();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Keyboard and controllers act as");
    ImGui::SameLine();
    // the choice is saved (issue #26: every start went back to the GamePad and showed its screen)
    if (radio("Wii U GamePad", !pro))
        hostui::post([] { hostui::set_pro_controller(false); hostui::set("proController", "0"); });
    ImGui::SameLine();
    if (radio("Wii U Pro Controller", pro))
        hostui::post([] { hostui::set_pro_controller(true); hostui::set("proController", "1"); });
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize("List view").x -
                    ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::Checkbox("List view", &U.list_view);
    help("A plain table of all inputs instead of the controller drawing");
    // face-button preset (issue #78): which host face buttons drive A/B/X/Y
    const input_map::FaceLayout fl = input_map::face_layout(m);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Face buttons");
    ImGui::SameLine();
    if (radio(input_map::face_layout_label(input_map::FaceLayout::kPosition),
              !m.face_auto && fl == input_map::FaceLayout::kPosition)) {
        input_map::apply_face_layout(m, input_map::FaceLayout::kPosition);
        input_map::set_current(m);
    }
    ImGui::SameLine();
    if (radio(input_map::face_layout_label(input_map::FaceLayout::kLabels),
              !m.face_auto && fl == input_map::FaceLayout::kLabels)) {
        input_map::apply_face_layout(m, input_map::FaceLayout::kLabels);
        input_map::set_current(m);
    }
    ImGui::SameLine();
    if (radio("automatic", m.face_auto)) {
        input_map::set_face_auto(m, true);
        input_map::set_current(m);
    }
    help("How the controller's face buttons drive the Wii U's A/B/X/Y. By position: the bottom "
         "button is B (Nintendo layout). By label: the button named A is A — on an Xbox pad that "
         "makes A accept/act and B go back (issue #78). Automatic reads the labels printed on the "
         "pad that was plugged in first and follows them. Only these four bindings are rewritten; "
         "keyboard keys and the other inputs stay as they are.");
    if (m.face_auto) {
        ImGui::SameLine();
        ImGui::TextDisabled("(-> %s)", input_map::face_layout_label(fl));
    } else if (fl == input_map::FaceLayout::kCustom) {
        ImGui::SameLine();
        ImGui::TextDisabled("(custom)");
    }
    // status line: capture prompt > note > hovered input > duplicates > help
    std::string status;
    ImVec4 sc(0.70f, 0.78f, 0.84f, 1.0f);
    if (U.cap_action >= 0) {
        sc = ImVec4(1.0f, 0.85f, 0.35f, 1.0f);
        status = std::string(input_map::action_label(U.cap_action)) + ": " +
                 (U.cap_col == kColPad   ? "press a controller button or stick direction (Esc cancels)"
                  : U.cap_col == kColAny ? "press a key or a controller button (Esc cancels, Backspace clears)"
                                         : "press a key (Esc cancels, Backspace clears)");
    } else if (!U.cap_note.empty()) {
        status = U.cap_note;
    } else if (U.hover_action >= 0) {
        status = std::string(input_map::action_label(U.hover_action)) + "  -  " + summary(m, U.hover_action) +
                 ".  A / Enter / click: rebind; X / Delete / right-click: clear.";
    } else if (int n = input_map::conflict_count(m)) {
        sc = ImVec4(1.0f, 0.62f, 0.20f, 1.0f);
        status = std::to_string(n) + " input(s) are bound to more than one action (orange).";
    } else {
        status = "Select a button on the controller or a chip to rebind it. Changes are saved to controls.json.";
    }
    const float bottom = ImGui::GetFrameHeightWithSpacing() * 2 + 4;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float h = std::max(160.0f, avail.y - bottom);
    U.hover_action = -1;
    if (U.list_view) {
        controls_list(m, h);
    } else {
        ControlsView cv;
        cv.m = m;
        cv.pro = pro;
        cv.cap_action = U.cap_action;
        cv.cap_col = U.cap_col;
        static float live[input_map::kPadCount];
        std::copy(std::begin(U.values), std::end(U.values), live);
        test_pad(live);
        cv.pad = live;
        cv.t = now_s();
        // live display: pressed inputs and stick deflection through the current mapping, from the raw
        // host state (as the Controls window's poll; camera inversion left out)
        static bool keys[256];
        for (int i = 0; i < 256; i++) keys[i] = g_held_keys[i].load(std::memory_order_relaxed);
        cv.keys = keys;
        for (int a = 0; a < input_map::kActionCount; a++) {
            float x = 0;
            for (int key : m.keys[a])
                if (key != input_map::kNoKey && keys[key]) x = 1;
            if (int p = m.pad[a]; p != input_map::kPadNone) x = std::max(x, live[p]);
            cv.act[a] = x;
        }
        input_map::Mapping mm = m;
        mm.invert_camera_y = false;
        input::PadState ks = input_map::keyboard_state(mm, keys), cs = input_map::controller_state(mm, live);
        const bool kl = ks.lx || ks.ly, kr = ks.rx || ks.ry;
        cv.stick[0][0] = kl ? ks.lx : cs.lx;
        cv.stick[0][1] = kl ? ks.ly : cs.ly;
        cv.stick[1][0] = kr ? ks.rx : cs.rx;
        cv.stick[1][1] = kr ? ks.ry : cs.ry;
        draw_controls(cv, avail.x, h);
        U.hover_action = cv.hover_action;
        if (cv.start_action >= 0) start_capture(cv.start_action, cv.start_col);
        if (cv.clear_action >= 0) clear_binding(cv.clear_action, cv.clear_col);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, sc);
    ImGui::TextUnformatted(status.c_str());
    ImGui::PopStyleColor();
    float dz = m.deadzone * 100.0f;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("Stick dead zone", &dz, 0.0f, 90.0f, "%.0f%%")) {
        m.deadzone = std::clamp(dz / 100.0f, 0.0f, 0.9f);
        input_map::set_current(m);
    }
    ImGui::SameLine(0, 24);
    if (check("Invert camera up/down", m.invert_camera_y, &v)) {
        m.invert_camera_y = v;
        input_map::set_current(m);
    }
    ImGui::SameLine(0, 24);
    // issue #35: a way to keep the controller motors still (saved; WWHD_RUMBLE=0 starts with it off)
    if (check("Rumble", rumble::enabled(), &v, input::has_rumble())) {
        rumble::set_enabled(v);
        hostui::post([v] { hostui::set("rumble", v ? "1" : "0"); });
    }
    help(input::has_rumble() ? "Controller vibration when the game asks for it. Off keeps the motors still."
                             : "Controller vibration: this host does not drive controller motors yet.");
    ImGui::SameLine(0, 24);
    static bool gyro_open = false;
    {
        const motion::Settings g = motion::settings();
        std::string label = std::string("Gyro: ") + (g.source == motion::kOff ? "off" : motion::source_label(g.source)) + "...";
        if (ImGui::Button(label.c_str())) gyro_open = true;
        help("Aim in first person by moving a controller with a gyro, a Cemuhook (DSU) source or the mouse (Steam Input)");
    }
    gyro_window(gyro_open);
    ImGui::SameLine(0, 24);
    if (ImGui::Button("Reset to defaults")) input_map::set_current(input_map::Mapping::defaults());
}

int saved_language() {
    std::string v;
    if (hostui::get("language", v) && !v.empty()) {
        int l = atoi(v.c_str());
        if (l >= 0 && l < game_lang::kLanguages) return l;
    }
    return 1;
}

// the region of the saved language when it comes from a language source (game_lang::kNoRegion: the game's own)
int saved_language_region() {
    std::string v;
    if (hostui::get("language_region", v)) {
        const int r = game_lang::region_from_code(v);
        if (r != game_lang::kUsa) return r;
    }
    return game_lang::kNoRegion;
}

// "English, French and Spanish"
std::string language_list(const std::vector<int>& langs) {
    std::string s;
    for (size_t k = 0; k < langs.size(); k++)
        s += std::string(k == 0 ? "" : k + 1 == langs.size() ? " and " : ", ") + game_lang::name(langs[k]);
    return s;
}

void choose_language(int language, int region) {
    U.language = language;
    U.language_region = region;
    hostui::post([language, region] {
        hostui::set("language", std::to_string(language));
        hostui::set("language_region", game_lang::region_code(region));
    });
}

void tab_about() {
    const char* env = getenv("WWHD_LANGUAGE");
    const bool env_set = env && *env;
    char* end = nullptr;
    const long env_value = env_set ? strtol(env, &end, 10) : 1;
    const int env_language = env_set && !*end && env_value >= 0 && env_value < game_lang::kLanguages ? (int)env_value : 1;
    if (U.language < 0) {
        U.language = saved_language();
        U.language_region = saved_language_region();
        const game_lang::Start now = game_lang::current();
        U.language_at_start = now.language;  // the game reads it early in the boot
        U.language_region_at_start = now.pack ? now.region : game_lang::kNoRegion;
        if (U.language_at_start < 0) {
            const game_lang::Start s = game_lang::choose(env_set ? env_language : U.language,
                                                         env_set ? game_lang::kNoRegion : U.language_region);
            U.language_at_start = s.language;
            U.language_region_at_start = s.pack ? s.region : game_lang::kNoRegion;
        }
    }
    const std::vector<int>& avail = game_lang::available();
    heading("Console language (applies on the next start)");
    if (avail.empty())
        note("The game picks its text language from the console language. Its language packs (content/Common/Pack) "
             "were not found, so every language is offered; the game shows only those its disc carries.");
    else
        note("The game picks its text language from the console language. This game (%s) contains %s; the other "
             "languages need a disc that carries them.", game_lang::region().c_str(), language_list(avail).c_str());
    ImGui::BeginDisabled(env_set);
    for (int i : {1, 2, 5, 3, 4, 8, 9, 10, 7, 0, 6, 11}) {
        if (i != 1 && i != 3 && i != 9 && i != 0) ImGui::SameLine();  // rows of three
        if (radio(game_lang::name(i), U.language == i && U.language_region == game_lang::kNoRegion,
                  game_lang::is_available(i)))
            choose_language(i, game_lang::kNoRegion);
    }
    // the languages of the language sources (a European or Japanese disc of the player's, set up with
    // the setup's --language-source): docs/language-packs.md
    for (int region : {(int)game_lang::kEurope, (int)game_lang::kJapan}) {
        const std::vector<int> langs = game_lang::source_languages(region);
        if (langs.empty()) continue;
        heading(region == game_lang::kEurope ? "From your European game (language source, experimental)"
                                             : "From your Japanese game (language source, experimental)");
        for (size_t k = 0; k < langs.size(); k++) {
            if (k % 3) ImGui::SameLine();
            const std::string label = std::string(game_lang::name(langs[k])) + " (" + game_lang::region_name(region) + ")";
            if (radio(label.c_str(), U.language == langs[k] && U.language_region == region))
                choose_language(langs[k], region);
        }
    }
    if (!game_lang::source_languages(game_lang::kJapan).empty())
        note("Japanese language sources are untested so far: report what looks wrong.");
    ImGui::EndDisabled();
    const bool from_source = U.language_region != game_lang::kNoRegion &&
                             game_lang::source_pack(U.language, U.language_region) != nullptr;
    if (env_set) {
        note("WWHD_LANGUAGE=%s is set for this start and takes precedence.", env);
        if (!game_lang::is_available(env_language))
            warn("%s is not on this disc: the game runs in %s.", game_lang::name(env_language),
                 game_lang::name(game_lang::usable(env_language)));
    } else if (U.language_region != game_lang::kNoRegion && !from_source) {
        warn("%s (%s) is saved but its language source is missing (%s): the game runs in %s.",
             game_lang::name(U.language), game_lang::region_name(U.language_region),
             game_lang::sources_dir().c_str(), game_lang::name(game_lang::usable(U.language)));
    } else if (!from_source && !game_lang::is_available(U.language)) {
        warn("%s is saved but is not on this disc: the game runs in %s. Choose one of the languages above.",
             game_lang::name(U.language), game_lang::name(game_lang::usable(U.language)));
    } else if (U.language != U.language_at_start || (from_source ? U.language_region : 0) != U.language_region_at_start) {
        warn("%s is saved. Restart the game to apply it: the game reads the console language only when it starts.",
             game_lang::name(U.language));
    }
    heading("About");
    ImGui::Text("The Legend of Zelda: Breath of the Wild - native port (%s host, %s renderer)", hostui::name(),
                render::api_name(render::active()));
#ifdef __APPLE__
    note("Settings overlay: F1 (Fn+F1 on most Mac keyboards), Cmd+, or Settings... in the app menu, or hold Select / "
         "press Home on a controller. Esc, F1 or B closes. "
#else
    note("Settings overlay: F1, or hold Select / press Home on a controller. Esc, F1 or B closes. "
#endif
         "L / R switch tabs on a controller. The game keeps running and sees no input while this menu is open.");
    note("When the game asks for text (your name), a text window appears over the game: type on the keyboard "
         "(Enter = OK, Esc = Cancel), click the on-screen keys, or use a controller: D-pad / left stick choose a key, "
         "A types it, B deletes, X adds a space, Y is Shift, L / R switch between letters, accents and symbols, "
         "Start confirms.");
    note("Built with Dear ImGui %s (MIT License, Omar Cornut and contributors).", IMGUI_VERSION);
}

void perf_window(bool menu_open) {
    const double t = now_s();
    if (U.fps_t0 == 0) { U.fps_t0 = t; U.fps_n0 = gx2::flips_presented(); }
    if (t - U.fps_t0 >= 0.5) {
        uint64_t n = gx2::flips_presented();
        U.fps = (double)(n - U.fps_n0) / (t - U.fps_t0);
        U.fps_t0 = t;
        U.fps_n0 = n;
    }
    float worst = 0, sum = 0;
    for (float f : U.frame_ms) { worst = std::max(worst, f); sum += f; }
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##perf", nullptr, fl)) {
        ImGui::Text("%.0f fps   %.1f ms (worst %.1f)", U.fps, sum / 120.0f, worst);
        ImGui::Text("Average %.1f fps   %.1f logic steps/s", g_average.fps, g_average.logic);
        {
            // slow motion: frame interpolation without Keep game speed below its frame target
            static double t0 = 0; static uint64_t s0 = 0; static double rate = 30;
            if (t0 == 0 || t - t0 < 0) { t0 = t; s0 = interp::executed_steps(); }
            else if (t - t0 >= 2.0) { rate = (double)(interp::executed_steps() - s0) / (t - t0); t0 = t; s0 = interp::executed_steps(); }
            if (interp::mode() == 1 && !interp::paced_interpolation() && rate > 1 && rate < 26)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Slow motion: %.0f of 30 logic steps/s. Turn on Keep game speed", rate);
        }
#ifdef __ANDROID__
        ImGui::TextUnformatted(android_telemetry().c_str());
#endif
        ImGui::Separator();
        ImGui::TextUnformatted(performance_device().c_str());
        ImGui::TextUnformatted(perf::summary().c_str());
        ImGui::PlotLines("##ft", U.frame_ms, 120, U.frame_i, nullptr, 0.0f, 50.0f, ImVec2(220, 36));
        ImGui::TextDisabled("%s  %gx  %s", render::api_name(render::active()), hostui::res_scale(), interp::mode_name());
        if (float share = interp::paced_drawn_share(); share >= 0)
            ImGui::TextDisabled("in-between frames drawn: %.0f%%", share * 100.0f);
    }
    ImGui::End();
    (void)menu_open;
}

void tab_audio() {
    int speakers = audio::requested_surround() ? 1 : 0;
    if (ImGui::Combo("Speakers", &speakers, "Stereo\0Surround 5.1\0"))
        hostui::post([speakers] { audio::set_requested_surround(speakers == 1); });
    note("Restart the game after changing Speakers.");
    if (audio::surround_fallback())
        note("Surround 5.1 unavailable on this output. Using stereo; restart after connecting a 5.1 device.");
    if (audio::output_device_changed())
        note("Output device changed. Audio is muted; restart the game to use the current device.");
    note("Current output: %s", audio::channels() == 6 ? "Surround 5.1" : "Stereo");
    if (const char *ch = audio::speaker_test_channel()) {
        note("Speaker test: %s", ch);
        if (ImGui::Button("Stop speaker test"))
            audio::stop_speaker_test();
    } else if (ImGui::Button("Test speakers"))
        audio::start_speaker_test();
    note("Plays each active channel in turn at a low level. Subwoofer uses an 80 Hz tone.");
}

void settings_window() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    // the Controls drawing wants room: that tab gets (almost) the whole picture
    const ImVec2 size = U.tab == kControls ? ImVec2(std::min(1240.0f, ds.x * 0.98f), std::min(720.0f, ds.y * 0.97f))
                                           : ImVec2(std::min(860.0f, ds.x * 0.94f), std::min(620.0f, ds.y * 0.92f));
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    if (U.just_opened) {
        ImGui::SetNextWindowFocus();  // keyboard / controller navigation starts in the menu
        U.just_opened = false;
    }
    if (ImGui::Begin("Breath of the Wild  -  Settings", &open, fl)) {
        // L / R on a controller switch tabs
        if (U.cap_action < 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            if (navigation_pressed(input_map::kPadLB)) U.select_tab = (U.tab + kTabs - 1) % kTabs;
            if (navigation_pressed(input_map::kPadRB)) U.select_tab = (U.tab + 1) % kTabs;
        }
        if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
            for (int i = 0; i < kTabs; i++) {
                ImGuiTabItemFlags f = U.select_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(kTabNames[i], nullptr, f)) {
                    if (U.tab != i) U.cap_action = -1, g_capturing_keys = false, U.cap_note.clear();
                    U.tab = i;
                    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                    switch (i) {
                    case kSaves: tab_saves(); break;
                    case kGraphics: tab_graphics(); break;
                    case kDisplay: tab_display(); break;
                    case kAudio: tab_audio(); break;
                    case kControls: tab_controls(); break;
                    default: tab_about(); break;
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            U.select_tab = -1;
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    // B (not while choosing an input or in a list) or the close button closes the menu
    if (!open) set_open(false);
    if (U.cap_action < 0 && navigation_pressed(input_map::face_input(input_map::kB)) && !g_pad_b_used && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        set_open(false);
    g_pad_b_used = false;
}

}  // namespace

// ---------------------------------------------------------------- API
bool is_open() { return g_open.load(std::memory_order_relaxed); }
void set_open(bool open) {
    if (g_open.exchange(open) == open) return;
    for (auto& k : g_held_keys) k = false;
    input::release_keys();  // keys held now belong to the menu (opening) or are not stuck in the game (closing)
    if (!open) g_wait_release = true;
    if (open) {
        mods::fast_forward_reset();
        mods::mouse_release();
        U.just_opened = true;
        U.slots_time = -1;
    }
    LOG("[overlay] %s", open ? "opened" : "closed");
}
bool blocks_input() { return captures() || g_wait_release.load(std::memory_order_relaxed); }
bool captures() { return is_open() || text_entry::active(); }
bool alive() { return now_s() - g_last_frame.load(std::memory_order_relaxed) < 1.0; }
bool perf_shown() { return g_perf.load(std::memory_order_relaxed); }
void set_perf_shown(bool on) {
    g_perf = on;
    hostui::post([on] { hostui::set("perfOverlay", on ? "1" : "0"); });
    if (on && g_metal_hud) set_metal_hud_shown(false);
}
bool metal_hud_shown() { return g_metal_hud.load(std::memory_order_relaxed); }
void set_metal_hud_shown(bool on) {
    g_metal_hud = on;
    hostui::post([on] { hostui::set("metalHud", on ? "1" : "0"); });
    if (on && g_perf) set_perf_shown(false);
}

bool key(int code, bool down, bool repeat, int mods) {
    if (g_no_host_keys) return false;
#ifdef __APPLE__
    // Cmd+, (the macOS settings shortcut; the AppKit host gets it as the app menu's Settings... item)
    if (code == kVK_ANSI_Comma && (mods & kSuper) && !g_capturing_keys.load()) {
        if (down && !repeat) set_open(!is_open());
        return true;
    }
#endif
    // F1 (no modifiers; Shift+F1 saves slot 1) toggles; Esc closes
    if (code == kVK_F1 && !(mods & (kShift | kCtrl | kAlt | kSuper))) {
        if (down && !repeat && !g_capturing_keys.load()) set_open(!is_open());
        if (!g_capturing_keys.load()) return true;
    }
    if (!is_open()) {
        // the game's text prompt (text_entry.h) has the keyboard while it shows
        if (!text_entry::active()) return false;
        text_entry::key(code, down, repeat);
        return true;
    }
    if (code >= 0 && code < 256 && !repeat) g_held_keys[code] = down;
    if (g_capturing_keys.load()) {
        if (down && !repeat) {
            if (code == kVK_Escape) g_capture_key = -1;
            else if (code == kVK_Delete || code == kVK_ForwardDelete) g_capture_key = -3;
            else g_capture_key = code;
        }
        return true;
    }
    if (code == kVK_Escape && down && !repeat) {
        set_open(false);
        return true;
    }
    push({Event::Key, code, down, 0, 0, mods});
    return true;
}
bool text(const char* utf8) {
    if (!is_open() || g_no_host_keys || !utf8 || !*utf8) return false;
    Event e{Event::Text};
    e.text = utf8;
    push(e);
    return true;
}
void set_clipboard_text(const char* utf8) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_clipboard = utf8 ? utf8 : "";
}
bool wants_text() { return is_open() && g_wants_text.load(); }
bool mouse_move(float nx, float ny) {
    if (!captures() || g_no_host) return false;
    push({Event::MousePos, 0, false, nx, ny, 0});
    return true;
}
bool mouse_button(int button, bool down) {
    if (!captures() || g_no_host) return false;
    push({Event::MouseButton, button, down, 0, 0, 0});
    return true;
}
bool mouse_wheel(float dx, float dy) {
    if (!captures() || g_no_host) return false;
    push({Event::Wheel, 0, false, dx, dy, 0});
    return true;
}
void set_density(float d) { g_density = d > 0.5f ? d : 1.0f; }

ImDrawData* frame(float pw, float ph, void (*renderer_init)()) {
    static TestSwitch test = parse_test();
    static bool prefs_read = false;
    if (!prefs_read) {
        prefs_read = true;
        std::string v;
        if (hostui::get("perfOverlay", v)) g_perf = v == "1";
        if (hostui::get("metalHud", v)) g_metal_hud = v == "1";
        // the saved controller choice (WWHD_PRO_CONTROLLER wins); it also hides or shows the GamePad screen
        if (!getenv("WWHD_PRO_CONTROLLER") && hostui::get("proController", v))
            hostui::post([pro = v == "1"] { hostui::set_pro_controller(pro); });
        // the saved rumble choice (WWHD_RUMBLE wins)
        if (!rumble::env_override() && hostui::get("rumble", v)) rumble::set_enabled(v != "0");
        // (no saved aspect ratio: the wider pictures need Wind Waker's camera hooks)
        // the saved gyro settings (WWHD_GYRO overrides the source)
        hostui::post([] { load_gyro(); });
    }
    if (!test.done && render::frame_count() + 1 >= test.at) {
        test.done = true;
        if (test.tab >= 0) U.select_tab = U.tab = test.tab;
        if (test.open) set_open(true);
        if (test.perf) g_perf = true;
        LOG("[overlay] test switch: %s", getenv("WWHD_TEST_OVERLAY"));
    }
#ifdef WWHD_HAS_VULKAN
    // debug: WWHD_TEST_PRESENT_MODE=mailbox@330 picks Graphics > Presentation at TV frame 330, as the
    // radio button does (the swapchains are recreated)
    static const char* tpm = getenv("WWHD_TEST_PRESENT_MODE");
    if (tpm) {
        const char* at = strchr(tpm, '@');
        if (render::frame_count() + 1 >= (at ? strtoull(at + 1, nullptr, 10) : 1)) {
            std::string m(tpm, at ? at - tpm : strlen(tpm));
            int mode = m == "mailbox" ? gfxvk::kPresentMailbox : m == "immediate" ? gfxvk::kPresentImmediate : gfxvk::kPresentFifo;
            LOG("[overlay] test: presentation %s", m.c_str());
            post_changed([mode] { gfxvk::set_present_mode(mode); });
            tpm = nullptr;
        }
    }
#endif
    const double t = now_s();
    if (U.last_present > 0) {
        U.frame_ms[U.frame_i] = (float)((t - U.last_present) * 1000.0);
        U.frame_i = (U.frame_i + 1) % 120;
    }
    U.last_present = t;
    g_last_frame = t;
    g_average.sample(t, gx2::flips_presented(), interp::executed_steps(), int(render::active()),
                     interp::mode(), interp::fps(), hostui::res_scale());
    read_controller();
    // the game's text prompt shows unless the menu is open over it (the menu has the input then)
    const bool open = is_open(), perf = perf_shown(), text = !open && text_entry::active();
    if (wwhd_trace::enabled()) {
        wwhd_trace::sink = [](const char* line) { LOG("[overlay-trace] %s",line); };
        wwhd_trace::open = open;
        static bool was_open = false;
        if (open && !was_open) { U.gamepad_navigation.reset_trace(); wwhd_trace::emit("open frame=%d", U.init ? ImGui::GetFrameCount() : 0); }
        was_open = open;
    }
    perf::set_demand(perf, open && U.tab == kGraphics);
    // Headless report QA uses the same formatter as the clipboard button; no clipboard mutation.
    static const char* testReport = getenv("WWHD_TEST_PERF_REPORT");
    static const uint64_t reportAt = getenv("WWHD_TEST_PERF_REPORT_AT")
        ? strtoull(getenv("WWHD_TEST_PERF_REPORT_AT"), nullptr, 10) : 300;
    static bool reportWritten = false;
    if (testReport && !reportWritten && gx2::flips_presented() >= reportAt) {
        if (FILE* f = fopen(testReport, "w")) {
            const std::string report = performance_report();
            fwrite(report.data(), 1, report.size(), f); fclose(f);
            reportWritten = true;
        }
    }
    // save state notices (saved, refused during a cutscene, loaded into another Quest Log) show over the
    // game for a few seconds while the menu is closed (the window title is not visible everywhere)
    const std::string toast = open ? std::string() : ss::last_message();
    U.linearized = false;
    if (!open && !perf && !text && toast.empty() && !guesthud::active()) {
        if (U.init) {  // forget events and pressed keys while nothing is shown
            std::lock_guard<std::mutex> lk(g_mu);
            g_events.clear();
        }
        return nullptr;
    }
    if (pw < 16 || ph < 16) return nullptr;
    if (!U.init) {
        init_context();
        if (renderer_init) renderer_init();
    }
    ImGuiIO& io = ImGui::GetIO();
    // UI scale: HiDPI backing scale, and larger on large windows (a TV across the room)
    const float scale = std::max({1.0f, g_density.load(), ph / 820.0f});
    io.DisplaySize = ImVec2(pw / scale, ph / scale);
    io.DisplayFramebufferScale = ImVec2(scale, scale);
    io.DeltaTime = U.last_time > 0 ? (float)std::clamp(t - U.last_time, 1e-4, 0.25) : 1.0f / 60.0f;
    U.last_time = t;
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        events.swap(g_events);
    }
    bool mouse_used = false;
    for (const Event& e : events) {
        if (wwhd_trace::enabled() && open && e.kind == Event::Key)
            wwhd_trace::emit("event kind=%d code=%d down=%d x=%.6f y=%.6f mods=%d", int(e.kind), e.code, e.down, e.x, e.y, e.mods);
        switch (e.kind) {
        case Event::Key:
            io.AddKeyEvent(ImGuiMod_Shift, e.mods & kShift);
            io.AddKeyEvent(ImGuiMod_Ctrl, e.mods & kCtrl);
            io.AddKeyEvent(ImGuiMod_Alt, e.mods & kAlt);
            io.AddKeyEvent(ImGuiMod_Super, e.mods & kSuper);
            if (ImGuiKey k = imgui_key(e.code); k != ImGuiKey_None) io.AddKeyEvent(k, e.down);
            break;
        case Event::MousePos:
            mouse_used |= e.x != U.mouse_position.x || e.y != U.mouse_position.y;
            U.mouse_position = ImVec2(e.x, e.y);
            overlay_trace::mouse_pos(io, e.x * io.DisplaySize.x, e.y * io.DisplaySize.y);
            break;
        case Event::MouseButton: mouse_used = true; overlay_trace::mouse_button(io, e.code, e.down); break;
        case Event::Wheel: mouse_used |= e.x != 0 || e.y != 0; overlay_trace::wheel(io, e.x, e.y); break;
        case Event::Text: io.AddInputCharactersUTF8(e.text.c_str()); break;
        default: break;
        }
    }
    apply_capture();
    const int face[4] = {input_map::face_input(input_map::kA), input_map::face_input(input_map::kB),
                          input_map::face_input(input_map::kX), input_map::face_input(input_map::kY)};
    U.gamepad_navigation.feed(io, U.values, U.prev, open && U.cap_action < 0, mouse_used, face);  // the text prompt reads the controller itself
    ImGui::NewFrame();
    g_wants_text = open && io.WantTextInput;
    guesthud::frame();
    if (open) settings_window();
    if (text) {
        float pad[input_map::kPadCount];
        std::copy(std::begin(U.values), std::end(U.values), pad);
        if (g_no_host) test_pad(pad);  // WWHD_TEST_PAD drives the on-screen keyboard in test runs
        if (text_entry::draw(pad)) {
            // answered: the game sees no buttons until the one that confirmed is released
            for (auto& k : g_held_keys) k = false;
            g_wait_release = true;
        }
    }
    if (perf) perf_window(open);
    if (!toast.empty()) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 24), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(io.DisplaySize.x * 0.8f, FLT_MAX));
        const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##savestate_notice", nullptr, fl)) {
            ImGui::PushTextWrapPos(io.DisplaySize.x * 0.75f);
            ImGui::TextUnformatted(toast.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
    }
    ImGui::Render();
    overlay_trace::state();
    return ImGui::GetDrawData();
}

void linearize_colors(ImDrawData* d,bool fresh_copy) {
    if (!d || (!fresh_copy && U.linearized)) return;
    if (!fresh_copy) U.linearized = true;
    static uint8_t table[256];
    static bool made = false;
    if (!made) {
        for (int i = 0; i < 256; i++) {
            float c = i / 255.0f;
            c = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
            table[i] = (uint8_t)std::lround(c * 255.0f);
        }
        made = true;
    }
    for (ImDrawList* l : d->CmdLists)
        for (ImDrawVert& v : l->VtxBuffer) {
            uint32_t c = v.col;
            v.col = (c & 0xFF000000u) | (uint32_t)table[(c >> 16) & 0xFF] << 16 | (uint32_t)table[(c >> 8) & 0xFF] << 8 |
                    table[c & 0xFF];
        }
}

}  // namespace overlay
