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

class FrameLimiter {
public:
    using Clock = std::chrono::steady_clock;
    Clock::time_point deadline(Clock::time_point now, int limit) {
        if (limit <= 0) { active_ = false; return now; }
        // A cap change uses the last actual frame, not an old scheduled deadline.
        const auto period = std::chrono::nanoseconds(1000000000LL / limit);
        return active_ && last_ + period > now ? last_ + period : now;
    }
    void released(Clock::time_point now) { last_ = now; active_ = true; }
private:
    Clock::time_point last_{};
    bool active_ = false;
};
}
