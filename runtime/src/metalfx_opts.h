// MetalFX options (Metal renderer only): the settings overlay and the Metal display code share these.
// The work itself is in gfx/metalfx.mm; this part builds everywhere (the overlay is in every build).
//
//   Upscaling: the TV picture is scaled to the window with MetalFX's spatial scaler (in place of the
//              bilinear / sharp / integer filter) whenever the window is larger than the picture.
//   Frame generation: one frame drawn between each pair of game frames (30 -> 60 fps, or 60 -> 120 on
//              a 120 Hz display), motion-compensated: the game gives no motion vectors, so they are
//              estimated from the pictures on the GPU (block matching on a luma pyramid). Only while
//              twice the game's frame rate fits the display; otherwise frames pass through.
//              (WWHD_METALFX_INTERPOLATOR=1: MetalFX's frame interpolator instead, experimental; see
//              gfx/metalfx.mm for why it is not the default.)
//
// Saved in the host settings (metalfxUpscale, metalfxFrameGen). Environment (wins over the saved choice):
//   WWHD_METALFX_UPSCALE=0|1      WWHD_METALFX_FRAMEGEN=0|1
//   WWHD_METALFX_LOG=1            log scaler/interpolator creation and per-300-frame framegen counts
#pragma once
#include <string>

namespace mfx {
bool upscale();               // chosen (the window may still not need it)
void set_upscale(bool on);
bool framegen();
void set_framegen(bool on);
bool from_env();              // an environment variable decides: the overlay's switches are disabled

// what this Mac supports (set by the Metal renderer at start-up; false before / on Vulkan)
bool upscale_available();     // MetalFX spatial scaler (macOS 13+)
bool framegen_available();    // frame generation (MetalFX's interpolator only with WWHD_METALFX_INTERPOLATOR=1: macOS 26+)
void set_available(bool upscale, bool framegen);

// status line for the overlay ("Frame generation: 30 -> 60 fps", "waiting: game above half the display rate")
std::string status();
void set_status(const std::string& s);
void load_saved();            // overlay start-up: the saved choices (unless the environment decides)
}  // namespace mfx
