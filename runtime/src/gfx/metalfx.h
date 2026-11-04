// MetalFX upscaling and frame generation of the TV picture (metalfx.mm; options: ../metalfx_opts.h).
#pragma once
#import <Metal/Metal.h>

namespace gfx {
namespace metalfx {
struct Picture {
    id<MTLTexture> tex = nil;
    bool linear = false;  // holds linear values (sRGB scan buffer)
    float dyn = 1.0f;     // dynamic resolution factor of the picture (1 once upscaled to the window)
};
void init();  // after the Metal device exists: what this Mac supports (mfx::set_available)
// This frame's TV picture for a box of boxW x boxH drawable pixels, encoded into this frame's command
// buffer: *real is the picture to show (upscaled, or R.tv's own image). True: frame generation made the
// in-between picture *mid, to be presented first, and *real after second_present_delay() seconds.
bool frame(float boxW, float boxH, Picture* real, Picture* mid);
double second_present_delay();
}  // namespace metalfx
}  // namespace gfx
