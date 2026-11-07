// Texture change detection with page write tracking, and its churn rule (metal_surfaces.mm
// check_texture and check_cpu_overwrite; test: runtime/tools/texture_churn_test.cpp).
//
// A CPU texture, or a GPU-written surface outside MEM1, is write-protected at each full check
// (write_watch.h): the next CPU write to one of its pages faults once, and the texture's next use
// hashes it in full. That costs nothing for textures that rarely change. A texture the game rewrites
// every frame, or whose pages it shares with data it writes every frame, pays every frame for a fault
// on the writing thread (signal, handler, mprotect: ~6 us on an M4), a re-protect on the render
// thread (~1.5 us) and the full hash, which more often than not finds nothing new.
//
// Such a texture becomes dynamic: its pages are no longer armed and it is hashed in full at each
// frame's first use, which costs the hash alone, so nothing it shows can be older than a texture on
// the fault path would be. After kLimit checks caused by writes, each within kFrames frames of the
// previous one (the Vulkan buffer cache's rule, buffer_cache_core.h), it is dynamic for a back-off
// (kFirstBackoff frames, doubling up to kMaxBackoff, back to the first once kMaxBackoff frames pass
// without such a check), then armed and watched again.
//
// Hashing every use costs more than it saves when uses without a write come in between (a texture
// with one page written every other frame: frame interpolation draws twice per 30 Hz game step), and
// for a big texture whose writes stop once it is dynamic (BotW streams its 63 MiB terrain array a
// slice at a time while the player moves: three writes in a row, then a pause). So a streak makes a
// texture dynamic only if neither the hashing its clean checks would add nor one full hash per write
// check costs more than its write checks cost in faults and re-protects, counted in bytes hashed in the
// same time (kFaultBytes per page written, kProtectBytes per check). A dynamic texture whose writes
// stop then costs at most what its writes cost while they went on; rewritten in full, any size passes.
#pragma once
#include <algorithm>
#include <cstdint>

#include "write_watch.h"

namespace texchurn {

constexpr uint32_t kLimit = 3, kFrames = 4;
constexpr uint64_t kFirstBackoff = 64, kMaxBackoff = 2048;
// a write fault and a re-protect in bytes XXH3 hashes meanwhile: on an M4 a fault takes ~6 us, a re-protect
// ~1.5 us, and textures hashed every frame go at ~37 GB/s (Hateno: 3.6 MiB in 0.10 ms)
constexpr uint64_t kFaultBytes = 192 << 10, kProtectBytes = 48 << 10;

struct State {
    uint64_t lastWrite = 0;     // frame of the last check caused by a write
    uint64_t dynamicUntil = 0;  // dynamic (not armed) before this frame
    uint64_t saved = 0;         // the streak's faults and re-protects, in bytes hashed meanwhile
    uint32_t streak = 0;        // checks caused by writes in the streak
    uint32_t clean = 0;         // checks without a write since the streak began
    uint32_t backoffs = 0;
};

inline bool dynamic(const State& s, uint64_t frame) { return frame < s.dynamicUntil; }

// a check that found the texture's armed pages unwritten
inline void clean_check(State& s) { s.clean += s.streak != 0; }

// A check caused by writes to `pages` of the texture's armed pages; `bytes`: what a full check
// hashes. Returns true when the texture becomes dynamic now.
inline bool write_check(State& s, uint64_t frame, uint32_t pages, uint64_t bytes) {
    const uint64_t age = frame - s.lastWrite;
    s.lastWrite = frame;
    if (age > kMaxBackoff) s.backoffs = 0;  // stable for long: the back-off starts over
    if (age > kFrames) {
        s.streak = s.clean = 0;
        s.saved = 0;
    }
    s.streak++;
    s.saved += pages * kFaultBytes + kProtectBytes;
    if (s.streak < kLimit) return false;
    const bool worth = (uint64_t)std::max(s.clean, s.streak) * bytes <= s.saved;
    s.streak = s.clean = 0;
    s.saved = 0;
    if (!worth) return false;
    s.dynamicUntil = frame + std::min<uint64_t>(kFirstBackoff << std::min<uint32_t>(s.backoffs, 16), kMaxBackoff);
    s.backoffs++;
    return true;
}

// A texture's write tracking: its ranges were armed with `stamp` at its last check, unless unarmed.
struct Watch {
    uint64_t stamp = 0;
    bool unarmed = false;  // the last check left the pages unarmed (dynamic): the next one hashes in full
    State churn;
};

// what one check did
struct Check {
    bool hash = false;      // the caller hashes the ranges now (armed already, unless left unarmed)
    bool forced = false;    // the caller asked for the hash (force)
    bool written = false;   // the armed pages were written since the last arm (why it hashes, unless forced)
    bool unarmed = false;   // left unarmed (dynamic)
    bool became = false;    // became dynamic at this check
    uint32_t pages = 0;     // written: pages written since the last arm
    uint32_t edges = 0;     // ... how many of them are a range's first or last page
    uint64_t since = 0;     // the stamp of the arm the last check left (0: new, or left unarmed); what a
                            // partial update (texture_pieces.h) re-hashes was written since it
};

// One check of a texture whose guest memory is `ranges` ((address, size) pairs). force: hash whatever
// the stamps say (new, invalidated, written by our own copy or write-back). An unarmed texture is
// hashed at every check; an armed one only after a write to its pages. Writes since the last arm feed
// the churn rule whether or not the check was forced: the renderer's own writes into armed pages fault
// as the game's do (a linear render target written back every frame). Unless the rule (on) keeps the
// texture dynamic, its ranges are armed again before this returns, so the caller reads guest memory
// after the arm (write_watch.h). With the rule off, the old behaviour: every check that hashes arms.
template <class Ranges>
Check check(Watch& w, const Ranges& ranges, uint64_t frame, bool force, bool on) {
    Check c;
    c.forced = force;
    if (w.stamp && !w.unarmed) {  // armed at the last check (a stamp is never 0)
        c.since = w.stamp;
        if (!force) {
            for (auto& [a, n] : ranges)
                if (wwatch::written_since(a, n, w.stamp)) {
                    c.written = true;
                    break;
                }
            if (!c.written) {
                if (on) clean_check(w.churn);
                return c;
            }
        }
        uint64_t bytes = 0;
        for (auto& [a, n] : ranges) {
            uint32_t e = 0;
            c.pages += wwatch::written_pages(a, n, w.stamp, &e);
            c.edges += e;
            bytes += n;
        }
        c.written = c.pages != 0;
        c.became = c.written && on && write_check(w.churn, frame, c.pages, bytes);
    }
    c.hash = true;
    if (on && dynamic(w.churn, frame)) {
        w.unarmed = c.unarmed = true;
        return c;
    }
    uint64_t stamp = ~0ull;
    for (auto& [a, n] : ranges) stamp = std::min(stamp, wwatch::arm(a, n));
    w.stamp = stamp;
    w.unarmed = false;
    return c;
}

}  // namespace texchurn
