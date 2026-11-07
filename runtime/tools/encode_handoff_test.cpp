// Encoding-thread hand-off (runtime/src/gfx/encode_handoff.h). A producer (the render thread's part) and
// a consumer (the encoding thread's) pass numbered records through a small ring, with waits for room and
// syncs (waits until everything queued is executed), in the sleeping mode, the spinning mode, and while
// the mode changes back and forth the way metal_main.mm changes it (wait, set, no-op record, wake_any,
// wait). Records must arrive complete and in order, and a watchdog fails the run if neither thread makes
// progress for 10 s (a lost wake-up).
//
// --bench [seconds]: a Hateno-like frame for each mode and wake batch: 5,100 draws of 4.5 records, a
// draw every ~2.2 us of render-thread work, 0.21 us to execute a record, pass ends every 30 draws, a
// commit every 15 passes, 10 syncs, then the render thread idles until the next 60 Hz frame. Prints each
// thread's CPU time per frame, wake-ups and sync waits.
#include "gfx/encode_handoff.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using gfx::handoff::Handoff;
using gfx::handoff::Rec;

namespace {

uint64_t now_ticks() { return mach_absolute_time(); }
uint64_t ticks_ns(uint64_t t) {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t i;
        mach_timebase_info(&i);
        return i;
    }();
    return t * tb.numer / tb.denom;
}
// stands for work (the real threads' record preparation and Metal calls), not a wait
void work_ns(uint64_t ns) {
    const uint64_t t0 = now_ticks();
    while (ticks_ns(now_ticks() - t0) < ns) {
    }
}
uint64_t thread_cpu_ns(mach_port_t t) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS) return 0;
    return (uint64_t)(info.user_time.seconds + info.system_time.seconds) * 1000000000ull +
           (uint64_t)(info.user_time.microseconds + info.system_time.microseconds) * 1000ull;
}
double process_cpu_s() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

[[noreturn]] void fail(const char* what) {
    fprintf(stderr, "encode_handoff_test: FAIL: %s\n", what);
    fflush(stderr);
    _exit(1);
}

constexpr uint64_t kNop = ~0ull, kStop = ~1ull;

// ---------------------------------------------------------------- correctness
struct Run {
    const char* name;
    bool sleeping;
    bool switching;  // change the mode every ~2,000 records
    uint32_t wakeDraws;
    uint64_t records;
    uint32_t ring;  // slots
    bool consumerDelays;
};

void run(const Run& cfg) {
    Handoff h(cfg.sleeping, cfg.wakeDraws);
    std::vector<uint64_t> ring(cfg.ring);
    std::atomic<uint64_t> produced{0}, consumed{0};
    std::atomic<bool> finished{false};

    std::thread watchdog([&] {
        uint64_t lastP = ~0ull, lastC = ~0ull;
        auto since = std::chrono::steady_clock::now();
        while (!finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const uint64_t p = produced.load(), c = consumed.load();
            if (p != lastP || c != lastC) {
                lastP = p;
                lastC = c;
                since = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - since > std::chrono::seconds(10)) {
                fprintf(stderr, "%s: no progress for 10 s: produced %llu, consumed %llu, executed %llu, sleeping %d\n", cfg.name,
                        (unsigned long long)p, (unsigned long long)c, (unsigned long long)h.executed(), (int)h.sleeping());
                fail("lost wake-up");
            }
        }
    });

    std::thread consumer([&] {
        std::mt19937 rng(7);
        uint64_t r = 0, expect = 0;
        for (;;) {
            bool mode;
            const uint64_t w = h.next(r, mode);
            if (w <= r) fail("next() returned no records");
            bool stop = false;
            while (r < w) {
                const uint64_t v = ring[r % cfg.ring];
                if (v == kStop) stop = true;
                else if (v != kNop) {
                    if (v != expect) {
                        fprintf(stderr, "%s: record %llu holds %llu, expected %llu\n", cfg.name, (unsigned long long)r,
                                (unsigned long long)v, (unsigned long long)expect);
                        fail("records out of order");
                    }
                    expect++;
                }
                r++;
                consumed.fetch_add(1, std::memory_order_relaxed);
                if (cfg.consumerDelays && (rng() & 1023) == 0) work_ns(rng() % 20000);
                h.done(r, mode);
            }
            h.batch_done(r, mode);
            if (stop) break;
        }
        if (expect != cfg.records) fail("records lost");
    });

    std::mt19937 rng(11);
    uint64_t wlocal = 0, seq = 0, sinceSwitch = 0;
    auto put = [&](uint64_t v, Rec kind) {
        if (wlocal + 1 - h.executed() > cfg.ring) {  // full: wait for room for one slot
            h.wait_executed(wlocal + 1 - cfg.ring, false);
            if (wlocal + 1 - h.executed() > cfg.ring) fail("no room after waiting for it");
        }
        ring[wlocal % cfg.ring] = v;
        wlocal++;
        produced.fetch_add(1, std::memory_order_relaxed);
        h.publish(wlocal, kind);
    };
    auto sync = [&] {
        h.wait_executed(wlocal, true);
        if (h.executed() < wlocal) fail("sync returned early");
    };
    const Rec kinds[] = {Rec::State, Rec::State, Rec::State, Rec::State, Rec::Draw, Rec::Draw, Rec::End, Rec::Commit, Rec::Block, Rec::Open};
    while (seq < cfg.records) {
        const uint32_t x = rng();
        put(seq++, kinds[x % 10]);
        if ((x >> 8) % 150 == 0) sync();
        if ((x >> 16) % 500 == 0) work_ns((x >> 4) % 30000);  // the render thread busy elsewhere
        if ((x >> 20) % 300 == 0) std::this_thread::yield();
        if (cfg.switching && ++sinceSwitch >= 2000 + (x % 1000)) {
            sinceSwitch = 0;
            sync();  // metal_main.mm eq_set_sleeping: nothing queued
            h.set_sleeping(!h.sleeping());
            put(kNop, Rec::State);
            h.wake_any();
            sync();
        }
    }
    put(kStop, Rec::Commit);
    sync();
    consumer.join();
    finished = true;
    watchdog.join();
    printf("%-34s ok: %llu records, %llu wake-ups, encoding thread slept %llu times, producer slept %llu times\n", cfg.name,
           (unsigned long long)cfg.records, (unsigned long long)h.kicks(), (unsigned long long)h.enc_waits(),
           (unsigned long long)h.rt_sleeps());
}

// a producer that publishes one record and waits for it, over and over: every wake-up is on the
// critical path, in both modes and across mode changes
void ping_pong(bool switching) {
    Handoff h(true, 32);
    std::vector<uint64_t> ring(64);
    std::atomic<uint64_t> progress{0};
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        uint64_t last = ~0ull;
        auto since = std::chrono::steady_clock::now();
        while (!finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (progress.load() != last) {
                last = progress.load();
                since = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - since > std::chrono::seconds(10))
                fail("ping-pong: lost wake-up");
        }
    });
    std::thread consumer([&] {
        uint64_t r = 0;
        for (;;) {
            bool mode;
            const uint64_t w = h.next(r, mode);
            bool stop = false;
            while (r < w) {
                stop = stop || ring[r % ring.size()] == kStop;
                r++;
                h.done(r, mode);
            }
            h.batch_done(r, mode);
            progress++;
            if (stop) break;
        }
    });
    std::mt19937 rng(3);
    uint64_t wlocal = 0;
    const Rec kinds[] = {Rec::State, Rec::Draw, Rec::End, Rec::Block};
    for (int i = 0; i < 100000; i++) {
        ring[wlocal % ring.size()] = (uint64_t)i;
        h.publish(++wlocal, kinds[rng() % 4]);
        if (rng() % 3 == 0) work_ns(rng() % 3000);
        h.wait_executed(wlocal, rng() & 1);
        if (h.executed() < wlocal) fail("ping-pong: wait returned early");
        progress++;
        if (switching && i % 97 == 0) {
            h.set_sleeping(!h.sleeping());
            ring[wlocal % ring.size()] = kNop;
            h.publish(++wlocal, Rec::State);
            h.wake_any();
            h.wait_executed(wlocal, true);
        }
    }
    ring[wlocal % ring.size()] = kStop;
    h.publish(++wlocal, Rec::Commit);
    h.wait_executed(wlocal, true);
    consumer.join();
    finished = true;
    watchdog.join();
    printf("%-34s ok: %llu wake-ups, encoding thread slept %llu times, producer slept %llu times\n",
           switching ? "ping-pong, mode changes" : "ping-pong, sleeping", (unsigned long long)h.kicks(),
           (unsigned long long)h.enc_waits(), (unsigned long long)h.rt_sleeps());
}

// ---------------------------------------------------------------- bench
struct BenchResult {
    double frames, procCpuMs, rtCpuMs, encCpuMs, encWorkMs, kicks, encWaits, rtSleeps, syncMs, activeMs;
};

BenchResult bench(bool sleeping, uint32_t wakeDraws, double seconds) {
    constexpr uint32_t kDraws = 5100, kPassDraws = 30, kCommitPasses = 15, kSyncs = 10;
    constexpr uint64_t kRing = 1 << 20;
    constexpr uint64_t kRecordNs = 210, kStateGapNs = 400, kDrawGapNs = 450, kCommitNs = 15000, kOpenNs = 5000;
    Handoff h(sleeping, wakeDraws);
    std::vector<uint8_t> ring(kRing);  // kind per slot
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> encWorkNs{0};
    std::atomic<mach_port_t> encPort{0};
    std::thread consumer([&] {
        encPort = pthread_mach_thread_np(pthread_self());
        uint64_t r = 0;
        for (;;) {
            bool mode;
            const uint64_t w = h.next(r, mode);
            const uint64_t t0 = now_ticks();
            bool quit = false;
            while (r < w) {
                const uint8_t k = ring[r % kRing];
                quit = quit || k == 0xFF;
                work_ns(kRecordNs + (k == (uint8_t)Rec::Commit ? kCommitNs : k == (uint8_t)Rec::Open ? kOpenNs : 0));
                r++;
                h.done(r, mode);
            }
            h.batch_done(r, mode);
            encWorkNs += ticks_ns(now_ticks() - t0);
            if (quit) break;
        }
    });
    while (!encPort.load()) std::this_thread::yield();
    const mach_port_t rtPort = pthread_mach_thread_np(pthread_self());
    std::mt19937 rng(5);
    uint64_t wlocal = 0;
    auto put = [&](Rec kind) {
        if (wlocal + 1 - h.executed() > kRing) h.wait_executed(wlocal + 1 - kRing, false);
        ring[wlocal % kRing] = (uint8_t)kind;
        h.publish(++wlocal, kind);
    };
    uint64_t syncNs = 0, activeNs = 0;
    auto sync = [&] {
        const uint64_t t0 = now_ticks();
        h.wait_executed(wlocal, true);
        syncNs += ticks_ns(now_ticks() - t0);
    };
    // warm up one frame, then measure
    auto frame = [&] {
        const uint64_t t0 = now_ticks();
        uint32_t syncAt[kSyncs];
        for (auto& s : syncAt) s = rng() % kDraws;
        uint32_t passes = 0;
        for (uint32_t d = 0; d < kDraws; d++) {
            if (d % kPassDraws == 0) put(passes % kCommitPasses == 0 ? Rec::Open : Rec::State);  // pass begin
            const int states = 3 + (d & 1);
            for (int s = 0; s < states; s++) {
                work_ns(kStateGapNs);
                put(Rec::State);
            }
            work_ns(kDrawGapNs);
            put(Rec::Draw);
            if (d % kPassDraws == kPassDraws - 1) put(++passes % kCommitPasses == 0 ? Rec::Commit : Rec::End);
            for (uint32_t s : syncAt)
                if (s == d) sync();
        }
        put(Rec::Commit);
        sync();
        activeNs += ticks_ns(now_ticks() - t0);
    };
    frame();
    const uint64_t frameNs = 1000000000ull / 60;
    const double cpu0 = process_cpu_s();
    const uint64_t rt0 = thread_cpu_ns(rtPort), enc0 = thread_cpu_ns(encPort.load()), work0 = encWorkNs.load();
    const uint64_t kicks0 = h.kicks(), waits0 = h.enc_waits(), sleeps0 = h.rt_sleeps();
    syncNs = activeNs = 0;
    const auto start = std::chrono::steady_clock::now();
    int frames = 0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        const auto due = start + std::chrono::nanoseconds(frameNs * (frames + 1));
        frame();
        frames++;
        std::this_thread::sleep_until(due);  // the rest of the 60 Hz frame
    }
    const double f = frames;
    BenchResult res{};
    res.frames = f;
    res.procCpuMs = (process_cpu_s() - cpu0) * 1e3 / f;
    res.rtCpuMs = (thread_cpu_ns(rtPort) - rt0) / 1e6 / f;
    res.encCpuMs = (thread_cpu_ns(encPort.load()) - enc0) / 1e6 / f;
    res.encWorkMs = (encWorkNs.load() - work0) / 1e6 / f;
    res.kicks = (h.kicks() - kicks0) / f;
    res.encWaits = (h.enc_waits() - waits0) / f;
    res.rtSleeps = (h.rt_sleeps() - sleeps0) / f;
    res.syncMs = syncNs / 1e6 / f;
    res.activeMs = activeNs / 1e6 / f;
    ring[wlocal % kRing] = 0xFF;
    h.publish(++wlocal, Rec::Commit);
    h.wait_executed(wlocal, false);
    consumer.join();
    return res;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        const double secs = argc > 2 ? atof(argv[2]) : 3.0;
        printf("%-14s %7s %8s %8s %8s %8s %8s %8s %8s %8s\n", "mode", "frames", "proc ms", "rt ms", "enc ms", "enc work", "wakes",
               "enc slp", "sync ms", "active");
        auto line = [&](const char* name, const BenchResult& r) {
            printf("%-14s %7.0f %8.2f %8.2f %8.2f %8.2f %8.1f %8.1f %8.3f %8.2f\n", name, r.frames, r.procCpuMs, r.rtCpuMs, r.encCpuMs,
                   r.encWorkMs, r.kicks, r.encWaits, r.syncMs, r.activeMs);
            fflush(stdout);
        };
        line("spinning", bench(false, 32, secs));
        for (uint32_t n : {8u, 16u, 32u, 64u, 128u}) {
            char name[32];
            snprintf(name, sizeof name, "sleeping/%u", n);
            line(name, bench(true, n, secs));
        }
        line("spinning", bench(false, 32, secs));
        return 0;
    }
    run({"sleeping, batches of 4", true, false, 4, 300000, 256, true});
    run({"sleeping, batches of 32", true, false, 32, 300000, 1024, true});
    run({"sleeping, tiny ring", true, false, 1000, 100000, 8, true});
    run({"spinning", false, false, 32, 300000, 256, true});
    run({"spinning, tiny ring", false, false, 32, 100000, 8, false});
    run({"mode changes", true, true, 16, 300000, 256, true});
    run({"mode changes, tiny ring", false, true, 8, 100000, 8, true});
    ping_pong(false);
    ping_pong(true);
    printf("encode_handoff_test: all passed\n");
    return 0;
}
