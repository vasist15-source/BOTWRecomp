// Dynamic resolution (Settings > Graphics > Dynamic resolution): while the GPU holds the game below
// its frame rate, the Metal renderer draws at a lower resolution (down to the chosen minimum, a fraction
// of the internal resolution, never below the console's 1280x720); with room again it goes back up.
// Decision logic: dynres_core.h. Off (the default) leaves everything as before: the factor is exactly 1.
// Ported from ZeldaWWHDRecomp (d454eda, 8c8c383); idea: GreenNaugahyde's Android fork, v0.9.5.
#pragma once
#include <cstdint>
#include <string>

namespace dynres {

// the setting: the lowest factor of the internal resolution, 0 = off (0.75 or 0.5). Any thread.
void set_min(float factor);
float min_factor();
bool on();
bool min_from_env();  // WWHD_DYNRES_MIN chose it for this run (the saved setting is not applied)

// GPU timing (renderer, any thread): one frame's GPU busy time (the union of its command buffers'
// intervals). Metal measures it every frame anyway (metal_main.mm read_gpu_times); taken only while on().
void gpu_busy(double ns);

// render thread, once per frame at the frame boundary (latch_res_scale): decides and returns the factor
// for the next frame. `base` is the internal resolution (the floor at 1x, the log and the report).
float latch(uint64_t frame, float base);
float factor();  // the factor in effect (any thread)

// a render target changing scale keeps this many textures of other scales (the levels it came from)
constexpr int kSpares = 2;

// the performance report's line ("Dynamic resolution: ..."), without newline
std::string status();

// counters for the report
struct Stats {
    uint64_t changes = 0;   // scale changes
    uint64_t reused = 0;    // render target textures taken from the kept ones at a change
    uint64_t created = 0;   // render target textures created at a change
};
Stats stats();
void count_reused();
void count_created();
// render thread: the cost of this frame's scale change (logged with WWHD_DYNRES_LOG_FRAMES)
void note_rescale(bool created, double ms);

}  // namespace dynres
