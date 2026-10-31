#include "../src/gx2/fps_limiter.h"
#include <cassert>
#include <chrono>
using namespace std::chrono;
int main() {
    for (int fps : {0,30,60,90,120}) assert(gx2::normalize_fps_target(fps)==fps);
    for (int fps : {-1,24,59,144,240}) assert(gx2::normalize_fps_target(fps)==30);
    for (int fps : {30,60,90,120}) {
        assert(gx2::frame_limit(fps,true,240)==fps);
        assert(!gx2::fps_present_unsynced(fps,false));
        assert(gx2::fps_present_unsynced(fps,true)==(fps>60));
    }
    assert(gx2::fps_present_unsynced(0,true));
    assert(gx2::frame_limit(30, false, 0) == 30);
    assert(gx2::frame_limit(30, true, 120) == 30);
    assert(gx2::frame_limit(60, true, 0) == 60);
    assert(gx2::frame_limit(60, false, 0) == 0);
    assert(gx2::frame_limit(0, true, 0) == 0);
    assert(gx2::frame_limit(0, true, 90) == 90);
    gx2::FrameLimiter l;
    auto t = gx2::FrameLimiter::Clock::time_point{};
    assert(l.deadline(t, 30) == t);
    l.released(t);
    // Simulated 144 Hz host submissions must still wait for a full 30 fps period.
    for (int i = 0; i < 300; ++i) {
        auto next = l.deadline(t + milliseconds(7), 30);
        assert(next - t == nanoseconds(33333333));
        t = next; l.released(t);
    }
    assert(l.deadline(t, 60) - t == nanoseconds(16666666));
    assert(l.deadline(t, 90) - t == nanoseconds(11111111));
    assert(l.deadline(t, 120) - t == nanoseconds(8333333));
    assert(l.deadline(t, 30) - t == nanoseconds(33333333));
    // A late frame cannot borrow time from the next frame.
    t += seconds(1);
    assert(l.deadline(t, 30) == t);
    l.released(t + milliseconds(5));
    assert(l.deadline(t + milliseconds(6), 30) == t + milliseconds(5) + nanoseconds(33333333));
    assert(l.deadline(t, 0) == t);
    assert(l.deadline(t + seconds(2), 30) == t + seconds(2));
    // Sustained caps, switching in either direction, then a long hitch: never
    // borrow a missed frame's time or inherit a previous target's deadline.
    for (int fps : {90,120,60,30,120,90}) {
        l.released(t);
        for (int i=0;i<fps;++i) {
            auto next=l.deadline(t+milliseconds(1),fps);
            assert(next-t==nanoseconds(1000000000LL/fps));
            t=next; l.released(t);
        }
        t+=seconds(1);
        assert(l.deadline(t,fps)==t);
    }
}
