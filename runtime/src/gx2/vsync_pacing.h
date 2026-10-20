// The virtual vsync's flip timing without game state (gx2_core.cpp uses it; runtime/tools/
// vsync_pacing_test.cpp tests it, ctest vsync_pacing).
//
// The game paces itself on the Wii U's vsync: a flip executes on a vsync at least `swap interval`
// vsyncs after the previous one, and the game waits until its flips have executed. The virtual
// vsync ticks on its own clock (59.94 Hz, or 2/4 times as fast for 120/240 fps interpolation). On a
// fixed grid, a frame that is ready a little after its vsync waits for the next one: a computer that
// draws 55 frames a second got 30 at the 60 fps limit (issue #125), as with double-buffered vsync.
//
// Here a flip that is ready late executes as soon as it is ready, not at the next vsync:
// - lateness under one 59.94 Hz vsync (16.7 ms, also at 120/240 fps) keeps the grid: the flip counts
//   as made on its vsync, so a frame that is a little late, and the sleep timers' wake-up delay, take
//   nothing from the next frames and the long-term rate stays exactly 59.94 Hz (119.88, 239.76). At
//   120/240 fps the logic passes, which take longer than an in-between pass, regularly swap several
//   ms after their vsync, and the in-between frames make up for it;
// - a flip later than that moves the grid: the clock is advanced so the flip's moment is a vsync,
//   and the next flip is due one interval after it. The rate becomes what the computer draws (a
//   frame never waits for a vsync it has missed), and the grid never lags more than one vsync
//   behind, so faster frames afterwards catch up with at most one vsync's worth of frames.
// Frames that are ready on time flip exactly on the grid, as before.
#pragma once
#include <cstdint>

namespace vsync_pacing {

// The clock: ticks (quarter vsyncs) since the epoch, advanced by `shift` nanoseconds. Times are
// nanoseconds since the epoch on the vsync's clock.
struct Clock {
    int64_t period;      // one vsync (59.94 Hz: 16'683'333 ns)
    uint64_t perVsync;   // ticks per vsync (4)
    uint64_t index(int64_t t, int64_t shift) const { return t + shift <= 0 ? 0 : uint64_t(t + shift) * perVsync / uint64_t(period); }
    // the time tick starts (the first time index() returns it)
    int64_t time(uint64_t tick, int64_t shift) const { return int64_t((tick * uint64_t(period) + perVsync - 1) / perVsync) - shift; }
    int64_t granule_ns(uint64_t granule) const { return int64_t(granule * uint64_t(period) / perVsync); }
};

// A flip executes at time t (ready, and its due tick has come: t >= time(due)). Returns the tick
// it counts as executed on (the next flip is due `interval` vsyncs after it) and updates shift:
// less than a vsync (`period`, at every rate) late keeps the grid (due); later moves the grid so t
// is a granule boundary (granule: ticks per vsync at the current rate).
inline uint64_t flip_executed(const Clock& c, int64_t& shift, uint64_t due, int64_t t, uint64_t granule) {
    const int64_t late = t - c.time(due, shift);
    if (late < c.period) return due;
    // the next boundary after t, and the shift that makes it start at t
    const uint64_t at = (c.index(t, shift) / granule + 1) * granule;
    shift += c.time(at, shift) - t;
    return at;
}

}  // namespace vsync_pacing
