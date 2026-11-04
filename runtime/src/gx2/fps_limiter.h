#pragma once
#include <chrono>

namespace gx2 {
inline int normalize_fps_target(int target) {
    return target == 0 || target == 60 || target == 90 || target == 120 ? target : 30;
}
// Host pacing owns the high-refresh cap. Display vsync would quantize 90fps
// to a lower divisor on many displays; 30/60 retain their existing sync policy.
inline bool fps_present_unsynced(int target, bool variableStep) {
    return variableStep && (target == 0 || target > 60);
}
// Fixed-step 30 fps needs a host cap even when variable-step pacing is inactive.
inline int frame_limit(int target, bool variableStep, int uncappedLimit) {
    return target == 30 ? 30 : !variableStep ? 0 : target > 0 ? target : uncappedLimit;
}

// Frames go out on a fixed schedule, one period apart, so the sleep's usual oversleep (a fraction of
// a millisecond on macOS) is made up by the next frame instead of adding up (pacing from each actual
// release ran 60 fps at 56-58 and 30 fps at 29.4, the fixed-step game 2% slow). A frame that is late,
// or oversleeps by more than a quarter period, moves the schedule to its release instead: no burst of
// catch-up frames after a hitch, which would speed up the fixed-step 30 fps game.
class FrameLimiter {
public:
    using Clock = std::chrono::steady_clock;
    Clock::time_point deadline(Clock::time_point now, int limit) {
        if (limit <= 0) { active_ = false; waited_ = false; return now; }
        const auto period = std::chrono::nanoseconds(1000000000LL / limit);
        // A cap change uses the last actual frame, not an old scheduled deadline.
        const auto base = active_ && limit == limit_ ? slot_ : last_;
        waited_ = active_ && base + period > now;
        pending_ = waited_ ? base + period : now;
        limit_ = limit;
        period_ = period;
        return pending_;
    }
    void released(Clock::time_point now) {
        slot_ = waited_ && now - pending_ < period_ / 4 ? pending_ : now;
        last_ = now;
        active_ = true;
        waited_ = false;
    }
private:
    Clock::time_point last_{}, slot_{}, pending_{};
    Clock::duration period_{};
    int limit_ = 0;
    bool active_ = false, waited_ = false;
};
}
