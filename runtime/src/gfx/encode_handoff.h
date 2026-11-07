// Wake-ups between the render thread and the encoding thread (Metal renderer, metal_main.mm "encoding
// thread"; tests: runtime/tools/encode_handoff_test.cpp).
//
// The render thread writes encoder records into a ring and publishes its write position; the encoding
// thread executes them and publishes its read position. One writer each.
//
// Sleeping (the default): the encoding thread sleeps as soon as it has caught up, and the render thread
// wakes it in batches: after every wakeDraws draws (WWHD_ENCODE_WAKE_DRAWS, default 32), at a pass end
// that commits, at the first pass of a command buffer (whose record makes the command buffer: the
// batched commit rule counts it), and whenever it waits for the encoding thread (a sync, a full ring).
// Records in between are only published (a store). Waking a sleeping thread is a kernel call
// (semaphore_signal); one per draw would be thousands a frame on the render thread. The render thread
// sleeps the same way while it waits, and the encoding thread wakes it once the position it waits for
// has been executed (checked after each batch and every 64 records). Neither thread spins.
//
// Spinning (WWHD_ENCODE_SPIN=1; the hand-off until 2026-11-07): caught up, the encoding thread spins up
// to 2,000 yields, then waits on the write position (libc++ polls ~4 us more before it sleeps); every
// draw, pass end and block notifies it, it notifies the read position after every record, and a sync
// spins up to 4,000 yields before it sleeps. Any record ends the spin, and one follows every ~0.5 us
// while the render thread draws, so the encoding thread slept only in the render thread's pauses (~1,700
// times a frame at Hateno, each woken by a kernel call at the next draw): its thread CPU was 10.5 ms a
// frame for 4.6 ms of encoding. Sleeping: 4.4 ms, 150 wake-ups a frame, process CPU -43 points (PLAN.md).
//
// The mode can change while the game runs (WWHD_ENCODE_AB). The render thread waits until everything
// queued is executed, sets the mode, queues a record that does nothing and wakes the encoding thread
// both ways (wake_any), then waits for that record. The encoding thread reads the mode after each write
// position it loads, so the batch with that record runs in the new mode, and neither thread is left
// asleep the other mode's way.
#pragma once
#include <mach/mach.h>

#include <atomic>
#include <cstdint>

namespace gfx::handoff {

// One sleeper and its notifier. The sleeper posts what it waits for (nonzero), checks its condition
// again and sleeps only if it still does not hold; the notifier, after making the condition true, takes
// the post and signals. A seq_cst fence on each side orders the post against the condition, so no
// wake-up is lost. Only a taken post signals the semaphore (a kernel call), and every signal is waited
// for once (by sleep(), or by withdraw() when the notifier took the post first).
class Gate {
 public:
    Gate() { semaphore_create(mach_task_self(), &sem_, SYNC_POLICY_FIFO, 0); }
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    // sleeper: announce what it waits for, then check its condition again
    void post(uint64_t want) {
        word_.store(want, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    // sleeper, its condition still unmet after post(): sleep until the notifier takes the post
    void sleep() {
        while (semaphore_wait(sem_) == KERN_ABORTED) {
        }
    }
    // sleeper, its condition met after post(): take the post back, or the notifier's signal if it took it
    void withdraw() {
        if (!word_.exchange(0, std::memory_order_acq_rel)) sleep();
    }
    // notifier, after making the condition true: signals the sleeper if it posted a want that met(want)
    // accepts; true if it signalled
    template <class F>
    bool notify(F&& met) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        uint64_t want = word_.load(std::memory_order_relaxed);
        if (!want || !met(want)) return false;
        if (!word_.compare_exchange_strong(want, 0, std::memory_order_acq_rel, std::memory_order_relaxed)) return false;
        semaphore_signal(sem_);
        return true;
    }

 private:
    alignas(128) std::atomic<uint64_t> word_{0};  // 0: nothing posted
    semaphore_t sem_ = MACH_PORT_NULL;
};

// what a published record asks of the hand-off
enum class Rec : uint8_t {
    State,   // pipeline, bindings, bytes, ...
    Draw,    // a draw
    End,     // a pass end that does not commit
    Commit,  // a pass end that commits its command buffer
    Block,   // a command-buffer block
    Open,    // the first pass of a command buffer
};

class Handoff {
 public:
    explicit Handoff(bool sleeping = true, uint32_t wakeDraws = 32) : sleep_(sleeping), wakeDraws_(wakeDraws ? wakeDraws : 1) {}
    Handoff(const Handoff&) = delete;
    Handoff& operator=(const Handoff&) = delete;

    // ---- render thread
    bool sleeping() const { return sleep_.load(std::memory_order_relaxed); }
    uint32_t wake_draws() const { return wakeDraws_; }
    // the records up to w are written: publish them, and wake the encoding thread if `kind` calls for it
    void publish(uint64_t w, Rec kind) {
        wpos_.store(w, std::memory_order_release);
        if (!sleeping()) {  // spinning: every draw, pass end and block notifies
            if (kind == Rec::Draw || kind == Rec::End || kind == Rec::Commit || kind == Rec::Block) wpos_.notify_one();
            return;
        }
        if (kind == Rec::Draw ? ++draws_ < wakeDraws_ : kind != Rec::Commit && kind != Rec::Open) return;
        kick();
    }
    uint64_t executed() const { return rpos_.load(std::memory_order_acquire); }
    // waits until the encoding thread has executed the records up to target (published already). spin:
    // a sync's short spin first (spinning mode)
    void wait_executed(uint64_t target, bool spin) {
        if (executed() >= target) return;
        if (!sleeping()) {
            wpos_.notify_one();
            uint64_t r;
            if (spin)
                for (int i = 0; i < 4000 && (r = rpos_.load(std::memory_order_acquire)) < target; i++) pause();
            while ((r = rpos_.load(std::memory_order_acquire)) < target) rpos_.wait(r, std::memory_order_acquire);
            return;
        }
        kick();
        while (executed() < target) {
            rt_.post(target);
            if (executed() >= target) {
                rt_.withdraw();
                break;
            }
            rtSleeps_.fetch_add(1, std::memory_order_relaxed);
            rt_.sleep();
        }
    }
    // mode change: only with nothing queued, followed by a no-op record, wake_any() and a wait for it
    void set_sleeping(bool sleeping) {
        draws_ = 0;
        sleep_.store(sleeping, std::memory_order_release);
    }
    void wake_any() {
        wpos_.notify_one();
        kick();
    }

    // ---- encoding thread
    // waits until the write position passes r (sleeping or spinning, per the mode) and returns it;
    // sleepMode: the mode for the records up to it
    uint64_t next(uint64_t r, bool& sleepMode) {
        for (;;) {
            uint64_t w = wpos_.load(std::memory_order_acquire);
            sleepMode = sleep_.load(std::memory_order_acquire);
            if (w != r) return w;
            if (!sleepMode) {
                for (int i = 0; i < 2000 && (w = wpos_.load(std::memory_order_acquire)) == r; i++) pause();
                if (w != r) {
                    sleepMode = sleep_.load(std::memory_order_acquire);
                    return w;
                }
                encWaits_.fetch_add(1, std::memory_order_relaxed);
                wpos_.wait(r, std::memory_order_acquire);
                continue;
            }
            enc_.post(1);
            if (wpos_.load(std::memory_order_acquire) != r) {
                enc_.withdraw();
                continue;
            }
            encWaits_.fetch_add(1, std::memory_order_relaxed);
            enc_.sleep();
        }
    }
    // after each record: the records up to r are executed
    void done(uint64_t r, bool sleepMode) {
        rpos_.store(r, std::memory_order_release);
        if (!sleepMode) {
            rpos_.notify_one();
            return;
        }
        if (++sinceCheck_ >= 64) wake_waiter(r);
    }
    // after the records next() returned
    void batch_done(uint64_t r, bool sleepMode) {
        if (sleepMode) wake_waiter(r);
    }

    // ---- counters (never reset)
    uint64_t kicks() const { return kicks_.load(std::memory_order_relaxed); }         // wake-ups of a sleeping encoding thread
    uint64_t enc_waits() const { return encWaits_.load(std::memory_order_relaxed); }  // encoding thread: sleeps (spinning: waits after the spin)
    uint64_t rt_sleeps() const { return rtSleeps_.load(std::memory_order_relaxed); }  // render thread: waits that slept (sleeping mode)

 private:
    void kick() {
        draws_ = 0;
        if (enc_.notify([](uint64_t) { return true; })) kicks_.fetch_add(1, std::memory_order_relaxed);
    }
    void wake_waiter(uint64_t r) {
        sinceCheck_ = 0;
        rt_.notify([r](uint64_t want) { return r >= want; });
    }
    static void pause() {
#if defined(__aarch64__)
        __builtin_arm_yield();
#endif
    }

    // a line each for: the render thread's position, the encoding thread's, the mode (read per batch),
    // the render thread's own state, the encoding thread's own state
    alignas(128) std::atomic<uint64_t> wpos_{0};
    alignas(128) std::atomic<uint64_t> rpos_{0};
    alignas(128) std::atomic<bool> sleep_;
    const uint32_t wakeDraws_;
    alignas(128) uint32_t draws_ = 0;  // render thread: draws since the last wake-up
    std::atomic<uint64_t> kicks_{0}, rtSleeps_{0};
    alignas(128) uint32_t sinceCheck_ = 0;  // encoding thread: records since it last looked for a waiting render thread
    std::atomic<uint64_t> encWaits_{0};
    Gate enc_;  // the encoding thread sleeps here (sleeping mode)
    Gate rt_;   // the render thread sleeps here while it waits (sleeping mode)
};

}  // namespace gfx::handoff
