// v208 menu clocks. Input edges still run every frame; only repeat/acceleration
// counters advance in the game's original 30 Hz units. Guest counter layout is
// preserved (including all 32 bits); fractional remainders live outside saves.
#include "ppc.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace {
struct Clock { double fraction = 0; uint32_t expected = 0; };
std::mutex clock_mutex;
std::unordered_map<uint32_t, Clock> clocks;

double frame_ratio() {
    const uint32_t instance = ld32(0x1047C258u);
    if (!instance || ld32(instance + 0x10) != 0x10319D4Cu) return 1;
    const double ratio = ldf32(instance + 0x1C);
    return std::isfinite(ratio) && ratio > 0 ? std::clamp(ratio, 0.125, 1.5) : 1;
}

Clock& clock_for(uint32_t address, uint32_t value, bool reset = false) {
    // Bound stale object addresses across repeated loads. Normal use is tens of
    // entries; eviction only discards sub-frame residuals, never guest counters.
    if (clocks.size() >= 4096 && !clocks.count(address)) clocks.clear();
    auto& clock = clocks[address];
    if (reset || clock.expected != value) clock.fraction = 0;
    return clock;
}
unsigned ticks(Clock& clock, double ratio) {
    clock.fraction += ratio;
    const auto whole = static_cast<unsigned>(clock.fraction + 1e-7);
    clock.fraction = std::max(0.0, clock.fraction - whole);
    return whole;
}
}

// Map panning shares the frame-time source, but not the integer repeat clock.
extern "C" double botw_map_frame_ratio() { return frame_ratio(); }
extern "C" double botw_frame_ratio() { return frame_ratio(); }

extern "C" void botw_controller_repeat(uint32_t controller) {
    const double ratio = frame_ratio();
    const uint32_t held = ld32(controller + 0x10C);
    const uint32_t pressed = ld32(controller);
    const unsigned count = std::min(ld32(controller + 0xF8), 32u);
    uint32_t repeat = 0;
    std::lock_guard<std::mutex> lock(clock_mutex);
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t bit = 1u << i, address = controller + 0x28 + 4 * i;
        uint32_t value = ld32(address);
        auto& clock = clock_for(address, value, (pressed & bit) || !(held & bit));
        if (held & bit) {
            const unsigned delay = ld8(controller + 0xA8 + i);
            const unsigned interval = ld8(controller + 0xC8 + i);
            for (unsigned n = ticks(clock, ratio); n; --n) {
                if (interval && value >= delay && (value - delay) % interval == 0) repeat |= bit;
                ++value;
            }
        } else {
            value = 0;
        }
        st32(address, value);
        clock.expected = value;
    }
    st32(controller + 8, repeat);
}

extern "C" uint32_t botw_menu_countdown(uint32_t address, uint32_t value) {
    const double ratio = frame_ratio();
    std::lock_guard<std::mutex> lock(clock_mutex);
    auto& clock = clock_for(address, value);
    // The game only calls this for nonnegative countdowns and tests <0 after.
    const unsigned step = ticks(clock, ratio);
    clock.expected = value - std::min(step, value + 1);
    return clock.expected;
}
