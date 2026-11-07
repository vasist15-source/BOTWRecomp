// Texture change detection's churn rule (runtime/src/gfx/texture_churn.h) on real page write tracking
// (runtime/src/write_watch.cpp): the rule's streaks, cost guard and back-off; then a texture cache's
// checks (texchurn::check, as metal_surfaces.mm's check_texture calls it) over write patterns the game
// has: rewritten every frame, a page shared with data written every frame, one page every other frame
// (big and small), rarely, never, a burst, written every frame but used every few frames. After every
// check the uploaded contents must equal guest memory (no stale texture), with the rule on, off, and
// switched between the two as the in-run A/B does; and a writer thread racing the checks.
#include "gfx/texture_churn.h"

#include <sys/mman.h>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#define XXH_INLINE_ALL
#include "../third_party/xxhash/xxhash.h"

using Ranges = std::vector<std::pair<uint32_t, uint32_t>>;

static uint8_t* g_mem;
constexpr uint64_t kSize = 64ull << 20;
constexpr uint32_t kPage = 16384;  // the M4's page size; on 4 KiB hosts the patterns still hold

static uint64_t hash(const Ranges& r) {
    uint64_t h = 0;
    for (auto& [a, n] : r) h = XXH3_64bits_withSeed(g_mem + a, n, h);
    return h;
}

static uint64_t faults_now() {
    uint64_t f, p;
    wwatch::totals(f, p);
    return f;
}

// 1. the rule alone
static void rule() {
    using namespace texchurn;
    // rewritten at every check: dynamic at the third, for 64 frames; then 128, 256, ... up to 2048
    {
        State s;
        uint64_t f = 10000;
        assert(!write_check(s, f++, 1, 16384) && !write_check(s, f++, 1, 16384));
        assert(write_check(s, f, 1, 16384) && dynamic(s, f) && s.dynamicUntil == f + 64);
        uint64_t expect = 64;
        for (int i = 0; i < 8; i++) {
            f = s.dynamicUntil;  // armed again: three more write checks
            assert(!dynamic(s, f));
            assert(!write_check(s, f, 1, 16384) && !write_check(s, f + 1, 1, 16384) && write_check(s, f + 2, 1, 16384));
            expect = std::min<uint64_t>(expect * 2, kMaxBackoff);
            assert(s.dynamicUntil == f + 2 + expect);
        }
        // stable for longer than the longest back-off: the next streak starts at 64 again
        f = s.dynamicUntil + kMaxBackoff + 10;
        assert(!write_check(s, f, 1, 16384) && !write_check(s, f + 1, 1, 16384) && write_check(s, f + 2, 1, 16384));
        assert(s.dynamicUntil == f + 2 + kFirstBackoff);
    }
    // written every 5 frames: never a streak
    {
        State s;
        for (uint64_t f = 100; f < 5000; f += 5) assert(!write_check(s, f, 4, 65536));
    }
    // every other frame with a clean check between: a 16 KiB texture is worth it, a 1 MiB one with one page written is not
    for (uint64_t bytes : {16384ull, 1ull << 20}) {
        State s;
        bool became = false;
        for (uint64_t f = 100; f < 400; f += 2) {
            if (dynamic(s, f)) continue;
            became |= write_check(s, f, 1, bytes);
            clean_check(s);
        }
        assert(became == (bytes == 16384));
    }
    // ... but a 1 MiB texture with all 64 pages rewritten every other frame is (64 faults cost more than a hash)
    {
        State s;
        bool became = false;
        for (uint64_t f = 100; f < 400 && !became; f += 2) {
            became = write_check(s, f, 64, 1u << 20);
            clean_check(s);
        }
        assert(became);
    }
    // written at every check, as at Hateno: a hash per frame must not cost more than the faults it saves, so
    // a texture whose writes stop costs no more dynamic than they did
    auto streak = [](uint32_t pages, uint64_t bytes) {
        State s;
        bool became = false;
        for (uint64_t f = 100; f < 112; f++) became |= write_check(s, f, pages, bytes);
        return became;
    };
    assert(streak(65, 1u << 20));     // a 512x512 render target written back every frame: all 65 pages fault
    assert(streak(1, 200u << 10));    // one page shared with data written every frame
    assert(!streak(1, 900u << 10));   // ... of a 900 KiB surface: one fault is cheaper than the hash
    assert(!streak(20, 64640u << 10));  // the 63 MiB terrain array streamed a slice at a time
}

// 2. a texture cache over guest memory
struct Tex {
    const char* name;
    Ranges ranges;
    std::function<void(uint64_t)> write;  // the game's writes in a frame
    uint32_t useEvery = 1;                // used (checked) every n-th frame
    bool ownWrites = false;               // written by the renderer (a write-back): every check forced
    texchurn::Watch watch;
    bool watched = false;
    uint64_t uploaded = 0;
    uint64_t hashes = 0, uploads = 0, became = 0, unarmed = 0, written = 0, lastHash = 0;
    void check(uint64_t frame, bool on) {  // check_texture with write tracking
        const texchurn::Check c = texchurn::check(watch, ranges, frame, !watched || ownWrites, on);
        written += c.written;
        unarmed += c.unarmed;
        became += c.became;
        assert(c.hash || (!c.written && !c.unarmed && !c.became && !c.forced));
        if (c.hash) {
            watched = true;
            hashes++;
            lastHash = frame;
            const uint64_t h = hash(ranges);
            if (h != uploaded) uploaded = h, uploads++;
        }
        assert(uploaded == hash(ranges));  // never stale
    }
};

static void poke(uint32_t addr, uint64_t frame) { g_mem[addr] = (uint8_t)(frame * 31 + 7); }

struct Run {
    std::vector<Tex> tex;
    uint64_t faults = 0;
};

// base: a page-aligned guest address for this run's textures
static Run make(uint32_t base) {
    Run r;
    uint32_t a = base;
    auto next = [&](uint32_t size) { uint32_t at = a; a += (size + kPage - 1) / kPage * kPage + kPage; return at; };
    {  // 64 KiB, one byte of every page rewritten every frame
        uint32_t t = next(65536);
        r.tex.push_back({"every frame", {{t, 65536}}, [t](uint64_t f) { for (uint32_t o = 0; o < 65536; o += kPage) poke(t + o + 100, f); }});
    }
    {  // 192 KiB ending 4 KiB into a page whose rest holds a variable written every frame: the texture never changes
        uint32_t t = next(192u << 10) + kPage - 4096;
        uint32_t var = t + (192u << 10) + 64;
        r.tex.push_back({"shared page", {{t, 192u << 10}}, [var](uint64_t f) { poke(var, f); }});
    }
    {  // the same at 1 MiB: a fault a frame is cheaper than hashing it every frame
        uint32_t t = next(1u << 20) + kPage - 4096;
        uint32_t var = t + (1u << 20) + 64;
        r.tex.push_back({"big shared page", {{t, 1u << 20}}, [var](uint64_t f) { poke(var, f); }});
    }
    {  // a 1.5 MiB array streamed in bursts: two pages in each of 3 frames, then 20 quiet ones (the terrain array)
        uint32_t t = next(3u << 19);
        r.tex.push_back({"streamed array", {{t, 3u << 19}}, [t](uint64_t f) {
                             if (f % 23 < 3) {
                                 poke(t + (uint32_t)(f % 90) * kPage + 11, f);
                                 poke(t + (uint32_t)(f % 90 + 1) * kPage + 11, f);
                             }
                         }});
    }
    {  // 1 MiB with a mip chain, one page of the base level changed every other frame
        uint32_t t = next(1u << 20), m = next(349528);
        r.tex.push_back({"big, every other frame", {{t, 1u << 20}, {m, 349528}}, [t](uint64_t f) { if (f % 2 == 0) poke(t + 5 * kPage + 9, f); }});
    }
    {  // 16 KiB, every other frame
        uint32_t t = next(16384);
        r.tex.push_back({"small, every other frame", {{t, 16384}}, [t](uint64_t f) { if (f % 2 == 1) poke(t + 77, f); }});
    }
    {  // every 50 frames
        uint32_t t = next(262144);
        r.tex.push_back({"every 50 frames", {{t, 262144}}, [t](uint64_t f) { if (f % 50 == 0) poke(t + 3 * kPage, f); }});
    }
    {  // never written
        uint32_t t = next(131072);
        r.tex.push_back({"static", {{t, 131072}}, [](uint64_t) {}});
    }
    {  // rewritten every frame for 200 frames, then never again
        uint32_t t = next(32768);
        r.tex.push_back({"burst", {{t, 32768}}, [t](uint64_t f) { if (f >= 100 && f < 300) poke(t + 1000 + f % 20000, f); }});
    }
    {  // rewritten every frame, used every third frame
        uint32_t t = next(65536);
        Tex x{"every frame, used every 3rd", {{t, 65536}}, [t](uint64_t f) { poke(t + kPage + 5, f); }};
        x.useEvery = 3;
        r.tex.push_back(x);
    }
    {  // rewritten every frame, used every 6th: too far apart for a streak, stays on the fault path
        uint32_t t = next(65536);
        Tex x{"every frame, used every 6th", {{t, 65536}}, [t](uint64_t f) { poke(t + 2 * kPage + 5, f); }};
        x.useEvery = 6;
        r.tex.push_back(x);
    }
    {  // written back by the renderer every frame (a linear render target): each check is forced
        uint32_t t = next(16384);
        Tex x{"own write-back", {{t, 16384}}, [t](uint64_t f) { poke(t + 300, f); }};
        x.ownWrites = true;
        r.tex.push_back(x);
    }
    return r;
}

// frames [first, last): game writes, then each texture used this frame is checked once
static void play(Run& r, uint64_t first, uint64_t last, const std::function<bool(uint64_t)>& on) {
    const uint64_t f0 = faults_now();
    for (uint64_t f = first; f < last; f++) {
        for (Tex& t : r.tex) t.write(f);
        for (Tex& t : r.tex)
            if (f % t.useEvery == 0) t.check(f, on(f));
    }
    r.faults += faults_now() - f0;
}

static const Tex& by(const Run& r, const char* name) {
    for (const Tex& t : r.tex)
        if (!strcmp(t.name, name)) return t;
    assert(false);
    return r.tex[0];
}

static void cache() {
    constexpr uint64_t kFrames = 4000;
    Run off = make(1u << 20), on = make(24u << 20);
    play(off, 1, kFrames, [](uint64_t) { return false; });
    play(on, 1, kFrames, [](uint64_t) { return true; });
    printf("%-30s %26s %26s\n", "", "rule off: hashes/uploads", "on: hashes/uploads/became");
    for (size_t i = 0; i < on.tex.size(); i++)
        printf("%-30s %17llu/%-8llu %15llu/%llu/%llu\n", on.tex[i].name, (unsigned long long)off.tex[i].hashes,
               (unsigned long long)off.tex[i].uploads, (unsigned long long)on.tex[i].hashes, (unsigned long long)on.tex[i].uploads,
               (unsigned long long)on.tex[i].became);
    printf("write faults: %llu with the rule off, %llu on\n", (unsigned long long)off.faults, (unsigned long long)on.faults);
    // off: the old behaviour, nothing unarmed
    for (const Tex& t : off.tex) assert(!t.became && !t.unarmed);
    // rewritten at every check: dynamic, a few faults per back-off instead of one per page per frame
    for (const char* n : {"every frame", "shared page", "small, every other frame", "every frame, used every 3rd", "own write-back"}) {
        assert(by(on, n).became >= 4);
        assert(by(on, n).written * 20 < by(off, n).written);
    }
    // the big ones (written every other frame, a shared page, streamed in bursts), the rare one and the one used
    // every 6th frame stay armed, exactly as without the rule
    for (const char* n : {"big, every other frame", "big shared page", "streamed array", "every 50 frames", "every frame, used every 6th",
                          "static"}) {
        assert(!by(on, n).became && !by(on, n).unarmed);
        assert(by(on, n).hashes == by(off, n).hashes && by(on, n).uploads == by(off, n).uploads);
    }
    assert(by(on, "static").hashes == 1);
    // the shared page: the texture never changed, so nothing is uploaded after the first check
    assert(by(on, "shared page").uploads == 1 && by(off, "shared page").uploads == 1);
    // the burst: dynamic during it, armed and quiet again within the longest back-off after it
    const Tex& burst = by(on, "burst");
    assert(burst.became >= 1 && !burst.watch.unarmed && burst.lastHash < 300 + 2 * texchurn::kMaxBackoff);
    assert(on.faults * 2 < off.faults);  // the big textures and the one used every 6th frame keep faulting

    // the A/B: the rule switched on and off every 97 frames; never stale (Tex::check), all armed when it ends off
    Run ab = make(48u << 20);
    play(ab, 1, kFrames, [](uint64_t f) { return (f / 97) % 2 == 0; });
    play(ab, kFrames, kFrames + 5, [](uint64_t) { return false; });
    for (const Tex& t : ab.tex) assert(!t.watch.unarmed);
    printf("switched every 97 frames: %llu write faults\n", (unsigned long long)ab.faults);
}

// 3. a writer thread racing the checks: whenever a check is done, a later check after the writer stops
// must see the final contents (armed: its write faulted after the arm; unarmed: hashed again)
static void race() {
    Ranges r = {{40u << 20, 4 * kPage}, {(40u << 20) + 6 * kPage, kPage}};
    std::mt19937 rng(11);
    uint64_t frame = 1, becameAll = 0, unarmedAll = 0;
    texchurn::Watch w;
    uint64_t uploaded = 0;
    bool watched = false;
    auto check = [&](bool on) {
        const bool force = !watched || frame % 7 == 0;  // now and then forced, as a write-back's check is
        const texchurn::Check c = texchurn::check(w, r, frame++, force, on);
        becameAll += c.became;
        unarmedAll += c.unarmed;
        if (c.hash) {
            watched = true;
            uploaded = hash(r);
        }
    };
    for (int round = 0; round < 300; round++) {
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> writes{0};
        std::thread writer([&, seed = (unsigned)rng()] {
            std::mt19937 x(seed);
            while (!stop.load(std::memory_order_relaxed)) {
                const auto& [a, n] = r[x() % r.size()];
                g_mem[a + x() % n] = (uint8_t)x();
                writes.fetch_add(1, std::memory_order_relaxed);
            }
        });
        const bool on = round % 3 != 2;
        const int checks = 1 + (int)(rng() % 40);
        for (int i = 0; i < checks; i++) {
            // a frame's worth of writes between checks (a few, or many: the check lands anywhere in them)
            const uint64_t until = writes.load(std::memory_order_relaxed) + 1 + rng() % 200;
            while (writes.load(std::memory_order_relaxed) < until) std::this_thread::yield();
            check(on);
        }
        stop = true;
        writer.join();
        check(on);  // the writer's last writes: faulted after the arm, or the texture is unarmed
        assert(uploaded == hash(r));
        check(on);
        assert(uploaded == hash(r));
    }
    assert(becameAll > 0 && unarmedAll > 0);
    printf("racing writer: 300 rounds, %llu checks left it unarmed, became dynamic %llu times\n", (unsigned long long)unarmedAll,
           (unsigned long long)becameAll);
}

int main() {
    g_mem = (uint8_t*)mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(g_mem != (uint8_t*)MAP_FAILED);
    assert(wwatch::init(g_mem, kSize) && wwatch::active());
    for (uint64_t i = 0; i < kSize; i++) g_mem[i] = (uint8_t)(i * 13);
    rule();
    cache();
    race();
    printf("texture_churn_test: rule, cache patterns (on, off, switched) and racing writer passed\n");
    return 0;
}
