// Aspect ratio of the TV picture (Settings > Display, WWHD_ASPECT): see aspect.cpp.
#pragma once
#include <cstdint>

namespace aspect {
enum Mode : int { kOriginal = 0, kWindow, k16x10, k21x9, k32x9, kCustom, kModes };
int mode();
void set_mode(int m);
const char* mode_name(int m);
float mode_value(int m);           // fixed aspect of a mode (0 for kWindow)
void set_window_aspect(float a);   // shape of the TV window's drawable (render thread, every present)
float requested();                 // what the setting asks for now
// main thread, GX2SwapScanBuffers: latch the aspect for the next game frame, update the game's
// data; returns the aspect the render thread should switch to at this swap
float on_swap();
float game();                         // aspect the game draws with this frame
uint64_t game_frame();                // swaps so far
// HUD: the game thread tags the upload of a layout / text projection (tools/botw_aspect_patches.py);
// the render thread, which knows the target, narrows it for the TV picture (gx2_core.cpp)
bool take_projection_tag();                        // gx2: is this vertex uniform upload one (clears the tag)
void layout_root_target(uint32_t root, bool tv);   // render thread (OP_LAYOUT_ROOT; Wind Waker only)
void set_content_clip(bool clip);                  // (OP_LAYOUT_CONTENT; Wind Waker only)
bool content_clip();
void ss_reset();  // save state loaded
}  // namespace aspect
