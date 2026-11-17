// overlay/pane_focus.h: the controller moves between the settings window's sidebar and its page.
// A window laid out as overlay.cpp's settings_window (sidebar and page as children, cards as flattened
// children of the page) is driven through overlay/gamepad_nav.h, without a renderer, a controller or
// the game. --plain is the window as it was (sidebar and page flattened into one scope, no PaneFocus):
// Right and A leave the focus in the sidebar, the report of 2026-11-17.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "imgui.h"
#include "imgui_internal.h"
#include "overlay/gamepad_nav.h"
#include "overlay/pane_focus.h"

using namespace input_map;

namespace {
float g_values[kPadCount], g_prev[kPadCount];
overlay::GamepadNavigation g_nav;
overlay::PaneFocus g_pane;
bool g_plain = false;    // --plain: no PaneFocus (what the window did before)
int g_tab = 3;
bool g_closed = false;   // B reached the window: it would close
float g_slider = 0.5f;
std::string g_focus;     // the item with the focus, as of the last frame

bool pressed(int p) { return g_nav.pressed(g_values, g_prev, p); }

void frame(int press = -1) {
    ImGuiIO& io = ImGui::GetIO();
    std::copy(g_values, g_values + kPadCount, g_prev);
    std::fill(g_values, g_values + kPadCount, 0.0f);
    if (press >= 0) g_values[press] = 1.0f;
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0f / 60.0f;
    g_nav.feed(io, g_values, g_prev, true, false);
    ImGui::NewFrame();
    g_focus = "(none)";
    ImGui::SetNextWindowPos(ImVec2(640, 400), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(940, 640), ImGuiCond_Always);
    static bool first = true;
    if (first) {
        ImGui::SetNextWindowFocus();
        if (!g_plain) g_pane.focus_sidebar();
        first = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Settings###settings", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const bool right = pressed(kPadDRight) || pressed(kPadLSRight), left = pressed(kPadDLeft) || pressed(kPadLSLeft);
    if (!g_plain) g_pane.begin(true);
    const ImVec2 ws = ImGui::GetWindowSize();
    const float inset = 10, sidebar = 236;
    ImGui::SetCursorPos(ImVec2(inset, inset));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 3));
    ImGui::BeginChild("sidebar", ImVec2(sidebar - inset, ws.y - 2 * inset),
                      ImGuiChildFlags_AlwaysUseWindowPadding | (g_plain ? ImGuiChildFlags_NavFlattened : 0),
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(16, 16));
    ImGui::InvisibleButton("##close", ImVec2(13, 13));
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::TextUnformatted("Breath of the Wild");
    ImGui::Dummy(ImVec2(0, 2));
    int want = -1;
    for (int i = 0; i < 8; i++) {
        if (!g_plain) g_pane.before_row(g_tab == i);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##page", ImVec2(ImGui::GetContentRegionAvail().x, 32), ImGuiButtonFlags_EnableNav);
        ImGui::PopID();
        if (ImGui::IsItemFocused()) g_focus = "row " + std::to_string(i);
        const bool opened = !g_plain && g_pane.after_row(clicked, right);
        if (clicked || opened) want = i;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    if (want >= 0) g_tab = want;
    const float px = sidebar + 22;
    ImGui::SetCursorPos(ImVec2(px, 22));
    ImGui::TextUnformatted("Title");
    const float py = ImGui::GetCursorPosY() + 6;
    ImGui::SetCursorPos(ImVec2(px, py));
    ImGui::BeginChild("page", ImVec2(ws.x - px - 14, ws.y - py - 16), g_plain ? ImGuiChildFlags_NavFlattened : 0);
    if (!g_plain) g_pane.begin_page(left);
    ImGui::PushID(g_tab);
    for (int card = 0; card < 3; card++) {
        ImGui::TextUnformatted("Section");
        if (g_tab == 7) continue;  // a page of text only: nothing takes the focus
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
        ImGui::BeginChild(("card" + std::to_string(card)).c_str(), ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        for (int b = 0; b < 3; b++) {
            if (b) ImGui::SameLine();
            ImGui::Button(("b" + std::to_string(card) + std::to_string(b)).c_str());
            if (ImGui::IsItemFocused()) g_focus = "page " + std::to_string(g_tab) + " card " + std::to_string(card) + " button " + std::to_string(b);
        }
        if (card == 1) {
            ImGui::SliderFloat("slider", &g_slider, 0.0f, 1.0f);
            if (ImGui::IsItemFocused()) g_focus = "page " + std::to_string(g_tab) + " slider";
        }
        ImGui::EndChild();
    }
    ImGui::PopID();
    ImGui::EndChild();
    ImGui::End();
    if (pressed(kPadB) && (g_plain || !g_pane.back())) g_closed = true;
    ImGui::Render();
}

int g_failed = 0;
// one press, then the frames the focus needs to settle
void press(const char* what, int pad, const char* focus, int tab, bool closed = false) {
    g_closed = false;
    bool closed_now = false;
    frame(pad);
    closed_now |= g_closed;
    for (int i = 0; i < 4; i++) frame(), closed_now |= g_closed;
    const bool ok = g_focus == focus && g_tab == tab && closed_now == closed;
    printf("%s %-12s focus %-26s page %d%s", ok ? "ok  " : "FAIL", what, g_focus.c_str(), g_tab, closed_now ? "  (closes)" : "");
    if (!ok) printf("   expected: focus %s, page %d%s", focus, tab, closed ? ", closes" : "");
    printf("\n");
    g_failed += !ok;
}
}  // namespace

int main(int argc, char** argv) {
    g_plain = argc > 1 && !strcmp(argv[1], "--plain");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad | ImGuiBackendFlags_RendererHasTextures;
    io.ConfigNavCaptureKeyboard = true;
    if (g_plain) {
        // what the report describes: the focus never leaves the sidebar
        press("(opens)", -1, "row 0", 3);
        press("Down", kPadDDown, "row 1", 3);
        press("A", kPadA, "row 1", 1);
        press("Right", kPadDRight, "row 1", 1);
        press("stick right", kPadLSRight, "row 1", 1);
        printf(g_failed ? "FAILED\n" : "plain window: Right and A stay in the sidebar, as reported\n");
        return g_failed ? 1 : 0;
    }
    press("(opens)", -1, "row 3", 3);                       // the row of the page that is shown
    press("Down", kPadDDown, "row 4", 3);                   // moving in the list does not change the page
    press("A", kPadA, "page 4 card 0 button 0", 4);         // A opens the page, focus on its first setting
    press("Right", kPadDRight, "page 4 card 0 button 1", 4);
    press("Left", kPadDLeft, "page 4 card 0 button 0", 4);  // a setting to the left: stays in the page
    press("Left", kPadDLeft, "row 4", 4);                   // nothing to the left: back to the page's row
    press("Right", kPadDRight, "page 4 card 0 button 0", 4);
    press("Down", kPadDDown, "page 4 card 1 button 0", 4);
    press("B", kPadB, "row 4", 4);                          // B in the page: back to the list, not closed
    press("Up", kPadDUp, "row 3", 4);
    press("stick right", kPadLSRight, "page 3 card 0 button 0", 3);  // Right opens the focused row's page
    press("Down", kPadDDown, "page 3 card 1 button 0", 3);
    press("Down", kPadDDown, "page 3 slider", 3);
    press("A", kPadA, "page 3 slider", 3);                  // edits the slider
    press("B", kPadB, "page 3 slider", 3);                  // ends the edit only
    press("B", kPadB, "row 3", 3);
    for (int i = 4; i <= 7; i++) press("Down", kPadDDown, ("row " + std::to_string(i)).c_str(), 3);
    press("A", kPadA, "row 7", 7);                          // a page with nothing to focus: stays on its row
    press("Up", kPadDUp, "row 6", 7);                       // and the list still moves
    // B in the list: the window closes (ImGui's own cancel leaves the panes; the focus returns to the shown page's row)
    press("B", kPadB, "row 7", 7, true);
    printf(g_failed ? "FAILED\n" : "all passed\n");
    return g_failed ? 1 : 0;
}
