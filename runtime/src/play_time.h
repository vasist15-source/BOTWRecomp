// Time played (Home menu; Settings > Language & About).
//
// Costs nothing while the game runs: no thread, no timer, nothing per frame. The clock is read once at
// the start, and the time since the last note is added to the file when the game writes a save (the
// game is writing a megabyte to disk then anyway), when it quits or restarts, and when the game's
// exit() runs. A crash loses at most the time since the last in-game save. The menus read the clock
// only while they are drawn. Time the Mac sleeps does not count (CLOCK_UPTIME_RAW).
//
// <config dir>/playtime.txt, two lines of whole seconds:
//   played 14294       in this port
//   earlier 290912     carried over from other versions of the game (Cemu, a Switch emulator, a
//                      console): written by hand or by the installer, the port only reads it
// Test and headless runs (hidden windows, WWHD_NO_HOST_INPUT, scripted keys, WWHD_EXIT_AT_FRAME, the
// controls self-test) and WWHD_PLAY_TIME=0 do not count. WWHD_PLAY_TIME_FILE=path uses another file (tests).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#ifdef __APPLE__
#include <time.h>
#else
#include <chrono>
#endif
#include "platform/host.h"

namespace play_time {
namespace detail {
struct State {
    std::mutex mutex;
    std::string path;
    bool counting = false;
    int64_t mark = 0;     // clock at the last note (nanoseconds)
    int64_t carry = 0;    // nanoseconds below a whole second, kept for the next note
    int64_t played = 0;   // seconds, as last read from or written to the file
    int64_t earlier = 0;
};
inline State& state() {
    static State* s = new State;  // never destroyed: exit() may run while other threads still play
    return *s;
}
inline int64_t clock_ns() {
#ifdef __APPLE__
    return (int64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
inline void read(State& s) {
    FILE* f = fopen(s.path.c_str(), "r");
    if (!f) return;
    char key[32];
    long long v;
    while (fscanf(f, "%31s %lld", key, &v) == 2) {
        if (v < 0) continue;
        if (!strcmp(key, "played")) s.played = v;
        else if (!strcmp(key, "earlier")) s.earlier = v;
    }
    fclose(f);
}
inline void write(const State& s) {
    const std::string tmp = s.path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    const bool ok = fprintf(f, "played %lld\nearlier %lld\n", (long long)s.played, (long long)s.earlier) > 0;
    if (fclose(f) != 0 || !ok) { remove(tmp.c_str()); return; }
    std::error_code ec;
    std::filesystem::rename(tmp, s.path, ec);  // replaces the old file in one step
}
}  // namespace detail

// adds the time since the last note to the file (any thread; nothing to do in a run that does not count)
inline void note() {
    detail::State& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.counting) return;
    const int64_t now = detail::clock_ns();
    const int64_t ns = now - s.mark + s.carry;
    s.mark = now;
    if (ns < 1000000000) { s.carry = ns > 0 ? ns : 0; return; }
    s.carry = ns % 1000000000;
    detail::read(s);  // another copy of the game may have added its time meanwhile
    s.played += ns / 1000000000;
    detail::write(s);
}

// the game is about to start (main thread, once)
inline void start() {
    detail::State& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const char* file = getenv("WWHD_PLAY_TIME_FILE");
    s.path = file && *file ? std::string(file) : host::config_dir() + "/playtime.txt";
    detail::read(s);
    auto on = [](const char* name) { const char* v = getenv(name); return v && *v && strcmp(v, "0") != 0; };
    const char* opt = getenv("WWHD_PLAY_TIME");
    if (on("WWHD_HIDDEN_WINDOWS") || on("WWHD_EXIT_AT_FRAME") || getenv("WWHD_NO_HOST_INPUT") ||
        getenv("WWHD_KEYS") || getenv("WWHD_CONTROLS_SELFTEST") || (opt && !on("WWHD_PLAY_TIME")))
        return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(s.path).parent_path(), ec);
    s.mark = detail::clock_ns();
    s.counting = true;
    std::atexit([] { note(); });  // the game's own exit(); Quit and Restart note in render::shutdown
}

struct Total { int64_t played, earlier; };  // seconds; the total is their sum
inline Total total() {  // now, to the second: reads only the clock
    detail::State& s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Total t{s.played, s.earlier};
    if (s.counting) t.played += (detail::clock_ns() - s.mark + s.carry) / 1000000000;
    return t;
}

// "84 h 46 min", "12 min"
inline std::string text(int64_t seconds) {
    const long long min = seconds / 60;
    char b[48];
    if (min >= 60) snprintf(b, sizeof b, "%lld h %02lld min", min / 60, min % 60);
    else snprintf(b, sizeof b, "%lld min", min);
    return b;
}
}  // namespace play_time
