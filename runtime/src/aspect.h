// Aspect ratio of the TV picture (Settings > Display, WWHD_ASPECT): see aspect.cpp.
#pragma once
#include <cstdint>

namespace aspect {
// The choices of Settings > Display. value: the aspect; 0: the TV window's shape.
struct Choice {
    const char* id;     // as saved
    const char* label;
    float value;
};
int choices();
const Choice& choice(int i);
constexpr float kMin = 1.0f, kMax = 5.34f;  // 1:1 ... 48:9

// The saved setting. It applies when the game starts: the HUD's panes are placed for the aspect when
// their layouts are built (aspect_hud.cpp), so a run keeps the aspect it started with.
int saved();                  // index of the choice
void set_saved(int i);
void load_saved();            // main thread, before the game runs (else on first use)
bool restart_needed();        // the saved setting differs from what this run uses

void set_window_aspect(float a);   // shape of the TV window's drawable (at launch; render thread, every present)
// main thread, GX2SwapScanBuffers: the aspect the render thread uses from this swap on
float on_swap();
float game();                         // this run's aspect (fixed at its first use)
int hud_mode();                       // this run's HUD placement (aspect::hud::Mode; edges unless WWHD_HUD)
uint64_t game_frame();                // swaps so far
// Narrower than 16:9 with the HUD centred: the game thread tags the upload of a layout / text projection
// (tools/botw_aspect_patches.py); the render thread narrows it for the TV picture (gx2_core.cpp)
bool take_projection_tag();                        // gx2: is this vertex uniform upload one (clears the tag)
void layout_root_target(uint32_t root, bool tv);   // render thread (OP_LAYOUT_ROOT; Wind Waker only)
void set_content_clip(bool clip);                  // (OP_LAYOUT_CONTENT; Wind Waker only)
bool content_clip();
void ss_reset();  // save state loaded
}  // namespace aspect
