// overlay/controls_view.cpp: the Controls drawing for every controller it can show (the two Wii U
// controllers and each host controller), laid out and drawn through Dear ImGui without a renderer, a
// controller or the game. Checks that every Wii U input keeps its three chips whatever is drawn, that
// a host controller's parts stand for the inputs bound to them (hover, click, held), that the callouts
// stay inside the view and apart, and that an unbound input loses its line and nothing else.
//
//   controls_view_test                 the checks
//   controls_view_test --bmp <prefix>  also writes <prefix>_<controller>.bmp (a plain software raster
//                                      of the draw lists, to look at the drawings)
//   ... --pictures <dir>                 with --bmp: the picture controllers with their pictures, read from
//                                      <dir>/<controller>.rgba (int32 width, int32 height, RGBA8 pixels)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "overlay/controls_view.h"

using namespace input_map;

namespace {
int g_fail = 0, g_pass = 0;
#define CHECK(x)                                                              \
    do {                                                                      \
        if (x) g_pass++;                                                      \
        else { g_fail++; fprintf(stderr, "%s:%d: FAILED: %s (%s)\n", __FILE__, __LINE__, #x, g_case.c_str()); } \
    } while (0)
std::string g_case;

const int kW = 1280, kH = 760;
const ImVec2 kViewPos(20, 60);
const float kViewW = 1240, kViewH = 600;

// ---- a software raster of ImGui's draw data (only for --bmp)
std::vector<uint8_t> g_fb;  // RGB
void honor_textures() {
    for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
            t->SetTexID((ImTextureID)(intptr_t)1);
            t->SetStatus(ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantDestroy) {
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}
void raster(ImDrawData* dd) {
    g_fb.assign((size_t)kW * kH * 3, 0);
    for (size_t i = 0; i < g_fb.size(); i += 3) g_fb[i] = 14, g_fb[i + 1] = 30, g_fb[i + 2] = 46;  // the overlay's blue-black
    ImTextureData* font_atlas = ImGui::GetIO().Fonts->TexData;
    for (ImDrawList* dl : dd->CmdLists) {
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            ImTextureData* atlas = cmd.TexRef._TexData ? cmd.TexRef._TexData : font_atlas;  // the font atlas or a picture
            const int cx0 = std::max(0, (int)cmd.ClipRect.x), cy0 = std::max(0, (int)cmd.ClipRect.y);
            const int cx1 = std::min(kW, (int)cmd.ClipRect.z), cy1 = std::min(kH, (int)cmd.ClipRect.w);
            for (unsigned e = 0; e + 2 < cmd.ElemCount; e += 3) {
                const ImDrawVert* v[3];
                for (int q = 0; q < 3; q++) v[q] = &dl->VtxBuffer[cmd.VtxOffset + dl->IdxBuffer[cmd.IdxOffset + e + q]];
                const float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) -
                                   (v[2]->pos.x - v[0]->pos.x) * (v[1]->pos.y - v[0]->pos.y);
                if (std::fabs(area) < 1e-6f) continue;
                const int x0 = std::max(cx0, (int)std::floor(std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x})));
                const int x1 = std::min(cx1 - 1, (int)std::ceil(std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x})));
                const int y0 = std::max(cy0, (int)std::floor(std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y})));
                const int y1 = std::min(cy1 - 1, (int)std::ceil(std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y})));
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) {
                        const float px = x + 0.5f, py = y + 0.5f;
                        float w[3];
                        w[0] = ((v[1]->pos.x - px) * (v[2]->pos.y - py) - (v[2]->pos.x - px) * (v[1]->pos.y - py)) / area;
                        w[1] = ((v[2]->pos.x - px) * (v[0]->pos.y - py) - (v[0]->pos.x - px) * (v[2]->pos.y - py)) / area;
                        w[2] = 1 - w[0] - w[1];
                        if (w[0] < 0 || w[1] < 0 || w[2] < 0) continue;
                        float c[4] = {}, u = 0, vv = 0;
                        for (int q = 0; q < 3; q++) {
                            for (int ch = 0; ch < 4; ch++) c[ch] += w[q] * ((v[q]->col >> (ch * 8)) & 255);
                            u += w[q] * v[q]->uv.x;
                            vv += w[q] * v[q]->uv.y;
                        }
                        float ta = 1;
                        if (atlas && atlas->Pixels) {
                            const int tx = std::clamp((int)(u * atlas->Width), 0, atlas->Width - 1);
                            const int ty = std::clamp((int)(vv * atlas->Height), 0, atlas->Height - 1);
                            const uint8_t* tp = atlas->Pixels + (ty * atlas->Width + tx) * atlas->BytesPerPixel;
                            ta = (atlas->BytesPerPixel == 4 ? tp[3] : tp[0]) / 255.0f;
                            if (atlas->BytesPerPixel == 4)
                                for (int ch = 0; ch < 3; ch++) c[ch] *= tp[ch] / 255.0f;
                        }
                        const float a = c[3] / 255.0f * ta;
                        uint8_t* out = &g_fb[((size_t)y * kW + x) * 3];
                        for (int ch = 0; ch < 3; ch++) out[ch] = (uint8_t)(out[ch] * (1 - a) + c[ch] * a);
                    }
            }
        }
    }
}
void write_bmp(const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    const int row = (kW * 3 + 3) & ~3, size = 54 + row * kH;
    uint8_t h[54] = {'B', 'M'};
    auto put = [&](int at, int v) { memcpy(h + at, &v, 4); };
    put(2, size); put(10, 54); put(14, 40); put(18, kW); put(22, kH);
    h[26] = 1; h[28] = 24;
    put(34, row * kH);
    fwrite(h, 1, 54, f);
    std::vector<uint8_t> line(row);
    for (int y = kH - 1; y >= 0; y--) {
        for (int x = 0; x < kW; x++) {
            const uint8_t* p = &g_fb[((size_t)y * kW + x) * 3];
            line[x * 3] = p[2]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[0];
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
}

// ---- one frame of the Controls drawing
float g_pad[kPadCount];
bool g_keys[256];
ImVec2 g_mouse(-1000, -1000);
bool g_click = false;

bool g_picture = false;             // the picture layout for the controllers that have one
ImTextureData* g_picture_tex = nullptr;  // and its picture (--pictures)

overlay::ControlsView frame(const Mapping& m, bool pro, HostPad host, bool raster_it = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)kW, (float)kH);
    io.DeltaTime = 1.0f / 60.0f;
    io.AddMousePosEvent(g_mouse.x, g_mouse.y);
    io.AddMouseButtonEvent(0, g_click);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("controls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImGui::SetCursorScreenPos(kViewPos);
    overlay::ControlsView cv;
    cv.m = m;
    cv.pro = pro;
    cv.host = host;
    cv.picture = g_picture;
    cv.picture_tex = g_picture_tex;
    cv.pad = g_pad;
    cv.keys = g_keys;
    for (int a = 0; a < kActionCount; a++) {
        float x = 0;
        for (int key : m.keys[a])
            if (key != kNoKey && g_keys[key]) x = 1;
        if (int p = m.pad[a]; p != kPadNone) x = std::max(x, g_pad[p]);
        cv.act[a] = x;
    }
    overlay::draw_controls(cv, kViewW, kViewH);
    ImGui::End();
    ImGui::Render();
    honor_textures();
    if (raster_it) raster(ImGui::GetDrawData());
    return cv;
}

// every vertex the drawing made, to check it stays inside the view (a little slack for shadows and focus rings)
bool drawing_inside() {
    for (ImDrawList* dl : ImGui::GetDrawData()->CmdLists)
        for (const ImDrawVert& v : dl->VtxBuffer)
            if (v.pos.x < kViewPos.x - 6 || v.pos.x > kViewPos.x + kViewW + 6 || v.pos.y < kViewPos.y - 6 || v.pos.y > kViewPos.y + kViewH + 6)
                return false;
    return true;
}

const struct { const char* id; PadKind kind; const char* name; } kHosts[] = {
    {"xbox", PadKind::kXbox, "Xbox Wireless Controller"},
    {"dualsense", PadKind::kDualSense, "DualSense Wireless Controller"},
    {"dualshock4", PadKind::kDualShock4, "DUALSHOCK 4 Wireless Controller"},
    {"switchpro", PadKind::kSwitchPro, "Pro Controller"},
    {"steam", PadKind::kSteam, "Steam Controller"},
    {"generic", PadKind::kGeneric, "Controller"},
};

// sweeps the mouse over the view and collects, per hovered Wii U input, where it was hovered: on a
// chip (hover_col >= 0) or on the drawing (hover_col < 0)
struct Sweep {
    bool part[kActionCount] = {};   // hovered on the drawing
    bool chip[kActionCount][3] = {};
};
Sweep sweep(const Mapping& m, bool pro, HostPad host) {
    Sweep s;
    for (float y = kViewPos.y + 2; y < kViewPos.y + kViewH; y += 7)
        for (float x = kViewPos.x + 2; x < kViewPos.x + kViewW; x += 7) {
            g_mouse = ImVec2(x, y);
            const overlay::ControlsView cv = frame(m, pro, host);
            if (cv.hover_action < 0) continue;
            if (cv.hover_col >= 0) s.chip[cv.hover_action][cv.hover_col] = true;
            else s.part[cv.hover_action] = true;
        }
    g_mouse = ImVec2(-1000, -1000);
    frame(m, pro, host);
    return s;
}

// <dir>/<name>.rgba as a registered texture (null when there is none)
ImTextureData* load_picture(const char* dir, const std::string& name) {
    FILE* f = fopen((std::string(dir) + "/" + name + ".rgba").c_str(), "rb");
    if (!f) return nullptr;
    int32_t size[2] = {};
    ImTextureData* t = nullptr;
    if (fread(size, 4, 2, f) == 2 && size[0] > 0 && size[1] > 0 && size[0] <= 4096 && size[1] <= 4096) {
        t = new ImTextureData();
        t->Create(ImTextureFormat_RGBA32, size[0], size[1]);
        t->UseColors = true;
        if (fread(t->GetPixels(), 4, (size_t)size[0] * size[1], f) != (size_t)size[0] * size[1]) fprintf(stderr, "%s.rgba: short\n", name.c_str());
        ImGui::RegisterUserTexture(t);
    }
    fclose(f);
    return t;
}

void test_layouts(const char* bmp, const char* pictures) {
    Mapping m = Mapping::defaults();
    struct Case { std::string name; bool pro; HostPad host; bool picture; };
    std::vector<Case> cases = {{"gamepad", false, {}, false}, {"pro", true, {}, false}};
    for (auto& h : kHosts) cases.push_back({h.id, false, {h.kind, h.name}, false});
    // the controllers there is a picture of: the same checks on the picture's layout (the others: no change)
    for (auto& h : kHosts)
        if (h.kind == PadKind::kXbox || h.kind == PadKind::kDualSense || h.kind == PadKind::kDualShock4 || h.kind == PadKind::kSwitchPro)
            cases.push_back({std::string(h.id) + "_picture", false, {h.kind, h.name}, true});
    for (const Case& c : cases) {
        g_case = c.name;
        g_picture = c.picture;
        g_picture_tex = c.picture && pictures ? load_picture(pictures, c.host.kind == PadKind::kXbox ? "xbox" : c.host.kind == PadKind::kDualSense ? "dualsense"
                                                              : c.host.kind == PadKind::kDualShock4 ? "dualshock4" : "switchpro") : nullptr;
        std::fill(g_pad, g_pad + kPadCount, 0.0f);
        if (c.picture && bmp) {  // the picture with something to see on it: A and the left trigger held, the right stick pushed
            g_pad[kPadA] = g_pad[kPadLT] = 1;
            g_pad[kPadRSRight] = 0.8f;
            g_pad[kPadRSUp] = 0.5f;
        }
        frame(m, c.pro, c.host);
        frame(m, c.pro, c.host, bmp != nullptr);
        if (bmp) write_bmp(std::string(bmp) + "_" + c.name + ".bmp");
        std::fill(g_pad, g_pad + kPadCount, 0.0f);
        frame(m, c.pro, c.host);
        CHECK(drawing_inside());
        const Sweep s = sweep(m, c.pro, c.host);
        // every Wii U input has its three chips, whatever is drawn
        for (int a = 0; a < kActionCount; a++)
            for (int col = 0; col < 3; col++) CHECK(s.chip[a][col]);
        // and every input the defaults bind to the controller can be picked on the drawing itself
        for (int a = 0; a < kActionCount; a++)
            if (m.pad[a] != kPadNone) CHECK(s.part[a]);
    }
    g_picture = false;
    g_picture_tex = nullptr;
}

// where the mouse hovers `action` on the drawing itself (not a chip); x < 0 when nowhere
ImVec2 find_part(const Mapping& m, bool pro, HostPad host, int action) {
    for (float y = kViewPos.y + 2; y < kViewPos.y + kViewH; y += 3)
        for (float x = kViewPos.x + 300; x < kViewPos.x + kViewW - 300; x += 3) {
            g_mouse = ImVec2(x, y);
            const overlay::ControlsView cv = frame(m, pro, host);
            if (cv.hover_action == action && cv.hover_col < 0) return g_mouse;
        }
    return ImVec2(-1, -1);
}
// a click at the mouse position: the capture it starts (-1 none). ImGui takes queued mouse events a
// frame apart, so a few frames each way; a button acts on the release.
int click(const Mapping& m, bool pro, HostPad host, int* col) {
    g_click = true;
    for (int i = 0; i < 3; i++) CHECK(frame(m, pro, host).start_action < 0);
    g_click = false;
    int started = -1;
    for (int i = 0; i < 3; i++) {
        const overlay::ControlsView cv = frame(m, pro, host);
        if (cv.start_action >= 0) started = cv.start_action, *col = cv.start_col;
    }
    return started;
}

// a click on a part of the drawing starts a capture for its input, key or controller (it did nothing
// before 2026-11-17: the part under the mouse was not known while the button was down)
void test_click_wiiu() {
    g_case = "click, Wii U GamePad";
    const Mapping m = Mapping::defaults();
    g_mouse = find_part(m, false, {}, kZL);
    CHECK(g_mouse.x >= 0);
    int col = -1;
    CHECK(click(m, false, {}, &col) == kZL && col == overlay::kColAny);
    g_mouse = ImVec2(-1000, -1000);
    frame(m, false, {});
}

// a host controller's part is the host input: it stands for whatever Wii U input is bound to it
void test_host_parts() {
    g_case = "host parts";
    const HostPad xbox = {PadKind::kXbox, "Xbox Wireless Controller"};
    Mapping m = Mapping::defaults();
    // the left trigger's tab: the part that hovers ZL by default
    CHECK(m.pad[kZL] == kPadLT);
    const ImVec2 at = find_part(m, false, xbox, kZL);
    CHECK(at.x >= 0);
    // rebind: the same tab now stands for R (and ZL has no part: nothing hovers it on the drawing)
    Mapping swapped = m;
    swapped.pad[kR] = kPadLT;
    swapped.pad[kZL] = kPadNone;
    g_mouse = at;
    frame(swapped, false, xbox);
    overlay::ControlsView cv = frame(swapped, false, xbox);
    CHECK(cv.hover_action == kR && cv.hover_col < 0);
    int col = -1;
    CHECK(click(swapped, false, xbox, &col) == kR && col == overlay::kColAny);
    g_mouse = ImVec2(-1000, -1000);
    frame(swapped, false, xbox);
    // unbound inputs keep their chips (to bind them again)
    const Sweep s = sweep(swapped, false, xbox);
    for (int c = 0; c < 3; c++) CHECK(s.chip[kZL][c]);
    CHECK(!s.part[kZL] && s.part[kR]);
    // an input bound to a stick direction is picked on that stick
    Mapping stick = m;
    stick.pad[kHome] = kPadRSUp;
    stick.pad[kRUp] = kPadNone;
    const Sweep s2 = sweep(stick, false, xbox);
    CHECK(s2.part[kHome] && !s2.part[kRUp]);
}

// the names: what each controller calls the inputs, and what a controller is taken for
void test_names() {
    g_case = "names";
    CHECK(pad_kind("Xbox One", "Xbox Wireless Controller") == PadKind::kXbox);
    CHECK(pad_kind("Xbox Series X", "Xbox Wireless Controller") == PadKind::kXbox);
    CHECK(pad_kind("DualSense", "DualSense Wireless Controller") == PadKind::kDualSense);
    CHECK(pad_kind("DualSense", "DualSense Edge Wireless Controller") == PadKind::kDualSense);
    CHECK(pad_kind("DualShock 4", "DUALSHOCK 4 Wireless Controller") == PadKind::kDualShock4);
    CHECK(pad_kind("Switch Pro Controller", "Pro Controller") == PadKind::kSwitchPro);
    CHECK(pad_kind("Nintendo Switch Joy-Con (L/R)", "Joy-Con (L/R)") == PadKind::kSwitchPro);
    CHECK(pad_kind("Nintendo Switch Joy-Con (L)", "Joy-Con (L)") == PadKind::kGeneric);
    CHECK(pad_kind("Nintendo Switch Online SNES Controller", "SNES Controller") == PadKind::kGeneric);
    CHECK(pad_kind("HID", "Steam Controller") == PadKind::kSteam);
    CHECK(pad_kind("HID", "Steam Virtual Gamepad") == PadKind::kGeneric);
    CHECK(pad_kind("Xbox One", "8BitDo Pro 2") == PadKind::kXbox);  // the profile the system recognised wins
    CHECK(pad_kind("MFi", "Nimbus+") == PadKind::kGeneric);
    CHECK(pad_kind(nullptr, nullptr) == PadKind::kGeneric);
    CHECK(pad_kind_from_id("dualsense") == PadKind::kDualSense && pad_kind_from_id("") == PadKind::kNone);
    // by position: kPadA is the bottom face button on every controller
    CHECK(!strcmp(pad_short_label(PadKind::kXbox, kPadA), "A"));
    CHECK(!strcmp(pad_short_label(PadKind::kSwitchPro, kPadA), "B"));
    CHECK(!strcmp(pad_short_label(PadKind::kDualSense, kPadA), "\xC3\x97"));  // ×
    CHECK(!strcmp(pad_short_label(PadKind::kSwitchPro, kPadLT), "ZL"));
    CHECK(!strcmp(pad_short_label(PadKind::kDualSense, kPadLT), "L2"));
    CHECK(!strcmp(pad_short_label(PadKind::kDualSense, kPadOptions), "Create"));
    CHECK(!strcmp(pad_short_label(PadKind::kDualShock4, kPadOptions), "Share"));
    CHECK(!strcmp(pad_short_label(PadKind::kSwitchPro, kPadMenu), "+"));
    for (PadKind k : {PadKind::kNone, PadKind::kGeneric, PadKind::kXbox, PadKind::kDualSense, PadKind::kDualShock4,
                      PadKind::kSwitchPro, PadKind::kSteam})
        for (int p = 1; p < kPadCount; p++) {
            CHECK(*pad_short_label(k, p) && *pad_label(k, p));
            if (k == PadKind::kNone || k == PadKind::kGeneric || p > kPadR3) {  // the names all controllers share
                CHECK(!strcmp(pad_short_label(k, p), pad_short_label(p)));
                CHECK(!strcmp(pad_label(k, p), pad_label(p)));
            }
        }
    // the shown controller: what the host last reported
    note_host_pad(PadKind::kDualSense, "DualSense Wireless Controller");
    CHECK(host_pad().kind == PadKind::kDualSense && host_pad().name == "DualSense Wireless Controller");
    note_host_pad(PadKind::kNone, "left over");
    CHECK(host_pad().kind == PadKind::kNone && host_pad().name.empty());
}
}  // namespace

int main(int argc, char** argv) {
    const char* bmp = argc > 2 && !strcmp(argv[1], "--bmp") ? argv[2] : nullptr;
    const char* pictures = bmp && argc > 4 && !strcmp(argv[3], "--pictures") ? argv[4] : nullptr;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    if (bmp) {  // the overlay's fonts (overlay.cpp setup_fonts), where the system has them
        const char* sf = "/System/Library/Fonts/SFNS.ttf";
        const char* fallback = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf";
        if (FILE* f = fopen(sf, "rb")) {
            fclose(f);
            io.Fonts->AddFontFromFileTTF(sf, 17.0f);
            if (FILE* g = fopen(fallback, "rb")) {
                fclose(g);
                ImFontConfig merge;
                merge.MergeMode = true;
                io.Fonts->AddFontFromFileTTF(fallback, 17.0f, &merge);
            }
        }
    }
    test_names();
    test_layouts(bmp, pictures);
    test_click_wiiu();
    test_host_parts();
    ImGui::DestroyContext();
    printf("controls_view_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
