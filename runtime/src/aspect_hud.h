// HUD and menus at an aspect ratio wider than 16:9: which panes are scaled or moved, and how. No guest
// memory here (aspect_hud.cpp reads and writes the panes), so runtime/tools/aspect_hud_test.cpp can run it.
//
// The game's layouts are made for a 1280x720 screen, and at a wider aspect ratio that screen is drawn
// across the wider picture: everything 2D comes out stretched sideways by kx = aspect / (16/9). Cemu
// graphic packs' BotW Graphics pack (v208, "Edge HUD" and "Centered HUD"; CC0-1.0) undoes that pane by
// pane when a layout is built: the right containers get their x scale multiplied by 1/kx, so what is in
// them has its proportions again, and are moved so that they sit at the screen's edges (Edge) or keep
// their place around the centre (Centred). Backgrounds and fades are left stretched, so they still cover
// the picture. Which pane gets what is the pack's decision code, kept as a table (aspect_hud_rules.inc,
// made by tools/botw_hud_rules.py): it depends on the screen being loaded, the last parts pane ("Pa...")
// built before this one, and the pane's name. The routines below are the pack's, written out.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace aspect::hud {
enum Mode : int { kEdges = 0, kCentred, kStretched, kModes };  // the pack's ultrawideHUDMode 1, 2, 0

enum : uint8_t {
    kScreen,     // compare the screen's name from here on
    kParts,      // ... the last parts pane's name
    kPane,       // ... this pane's name
    kIf,         // the name compared is `name`: go to `to`
    kIfNot,      // ... is not `name`
    kIfNoParts,  // no parts pane was built in this screen yet: go to `to`
    kIfParts,    // one was
    kOffset,     // the x offset the next routine takes (the pack's f12)
    kOffsetTime, // ... the clock's, which depends on the mode
    kGo,
};
constexpr uint16_t kDo = 0x8000;  // `to` is a routine (Action), not a step
struct Op {
    uint8_t op;
    uint16_t to;
    const char* name = nullptr;
    float value = 0;
};
#include "aspect_hud_rules.inc"

// The routine the pack runs for a pane (a_exitScale: none) and the offset it passes.
inline Action decide(const char* screen, const char* parts, const char* pane, int mode, float& offset) {
    const char* subject = screen;
    offset = 0;
    constexpr uint16_t count = sizeof kOps / sizeof kOps[0];
    uint16_t pc = kEntry;
    for (int steps = 0; steps < 4096 && pc < count; steps++) {
        const Op& o = kOps[pc];
        bool go = false;
        switch (o.op) {
        case kScreen: subject = screen; break;
        case kParts: subject = parts; break;
        case kPane: subject = pane; break;
        case kIf: go = !strcmp(subject, o.name); break;
        case kIfNot: go = strcmp(subject, o.name) != 0; break;
        case kIfNoParts: go = !*parts; break;
        case kIfParts: go = *parts != 0; break;
        case kOffset: offset = o.value; break;
        case kOffsetTime: offset = mode == kEdges ? 342.0f * 0.85f : 342.0f / 0.85f; break;
        case kGo: go = true; break;
        }
        if (!go) { pc++; continue; }
        if (o.to & kDo) return Action(o.to & ~kDo);
        pc = o.to;
    }
    return a_exitScale;
}

struct Pane {
    float x;       // translation x (pane + 0x1C)
    float sx, sy;  // scale (pane + 0x34, + 0x38)
    float w;       // width in the layout the pane is built from
};
// ar: 1/kx = (16/9) / aspect. pad: how far the screen's edge is from where 1280x720 puts it, in the units
// of a pane already scaled by ar: 640 * (1 - ar) (the pack's const_PaddingRightSide; its "Alt" padding is
// the same at the 720-line size the game works in).
struct Factors {
    float ar, pad;
    int mode;
};
inline Factors factors(float aspect, int mode) {
    const float ar = (16.0f / 9.0f) / aspect;
    return {ar, 640.0f * (1.0f - ar), mode};
}
// a pane keeps its distance from the screen's edge on its side: |x| + (640 - |x|) * (1 - ar)
inline float keep(float x, float ar) {
    const float a = std::fabs(x), v = a + (640.0f - a) * (1.0f - ar);
    return x >= 0 ? v : -v;
}

inline void apply(Action a, Pane& p, const Factors& f, float offset) {
    const bool edges = f.mode == kEdges;
    if (a == a_scalePaneToLeftSideIf) a = edges ? a_scalePaneToLeftSide : a_scalePaneNormal;
    if (a == a_scalePaneAndKeepPosIf) a = edges ? a_scalePaneAndKeepPos : a_scalePaneAndPos;
    switch (a) {
    case a_scalePaneNormal: p.sx *= f.ar; break;
    case a_scalePaneReverse: p.sx /= f.ar; break;
    case a_scalePaneOnYScale: p.sy /= f.ar; break;
    case a_scalePaneAndPos: p.sx *= f.ar; p.x *= f.ar; break;
    case a_scaleOnlyPos: p.x *= f.ar; break;
    case a_scaleOnlySizeReverse: p.w /= f.ar; break;
    case a_scalePaneToLeftSide: p.sx *= f.ar; p.x -= f.pad; break;
    case a_scalePaneToRightSide: p.sx *= f.ar; p.x += f.pad; break;
    case a_movePaneToRightSide:
    case a_movePaneToRightSideAlt: p.x += f.pad; break;
    case a_scalePaneAndKeepPos: p.sx *= f.ar; p.x = keep(p.x, f.ar); break;
    case a_moveKeepPos: p.x = keep(p.x, f.ar); break;
    case a_scalePaneAndKeepCustomPosIf:  // the pane's parent sits at `offset`
        p.sx *= f.ar;
        p.x = edges ? keep(p.x + offset, f.ar) - offset : (p.x - offset) * (1.0f - f.ar);
        break;
    case a_scaleMapAndStore:  // the map: at the right edge while it is the minimap (aspect_hud.cpp moves it)
        p.sx *= f.ar;
        if (edges) p.x += f.pad;
        break;
    case a_scalePaneToLeftSideIf:
    case a_scalePaneAndKeepPosIf:
    case a_exitScale: break;
    }
}

// MainScreen's sensor, sound and temperature gauges are animated in x from 392 (the pack's
// _scaleSensorAnimationX): only the animated displacement is scaled
inline float sensor_x(float x, float ar) { return (x - 392.0f) * ar + 392.0f; }
}  // namespace aspect::hud
