// obj_table.h: the host objects of guest sync objects, found by guest address without a lock.
//   (no arguments) the checks: one object per address for good, address 0, growth with the objects
//     kept, the old lookup (get_locked) and the new one agreeing however they are mixed, iteration;
//     threads racing to make and find objects while the table grows, switching between the lookups.
//   --bench [seconds]: OSLockMutex + OSUnlockMutex pairs (lookup, the object's lock, owner check) from
//     1-8 threads on 512 mutexes, with each lookup in turn: ns per pair and how often the table's lock
//     was found held.
#include "obj_table.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kMagic = 0x6D557458;  // "mUtX"
std::atomic<int> g_made{0};
struct Obj {
    uint32_t magic = kMagic;  // set by the constructor: a lookup must see it (the object is published)
    std::mutex m;
    const void* owner = nullptr;
    int count = 0;
    Obj() { g_made.fetch_add(1, std::memory_order_relaxed); }
};
using Table = objtable::Table<Obj>;

// guest-like addresses: aligned objects spread over MEM2
uint32_t addr_of(uint32_t i) { return 0x10000000u + i * 0x2Cu * 4u; }

void basic() {
    Table t(16);
    assert(t.find(0x10000040) == nullptr);
    Obj* a = t.get(0x10000040);
    assert(a && a->magic == kMagic);
    assert(t.get(0x10000040) == a && t.find(0x10000040) == a);
    Obj* b = t.get(0x10000044);
    assert(b != a && t.find(0x10000044) == b);
    // address 0 (a null GHS lock handle): an object of its own
    assert(t.find(0) == nullptr);
    Obj* z = t.get(0);
    assert(z && z != a && z != b && t.get(0) == z && t.find(0) == z);
    assert(t.size() == 3);
    // the same high bits (one home slot): probing
    Obj* c = t.get(0xFFFFFFFC);
    assert(t.get(0xFFFFFFFC) == c && t.find(0x10000040) == a);
    int seen = 0;
    t.for_each([&](uint32_t addr, Obj* p) {
        assert(p == t.find(addr));
        seen++;
    });
    assert(seen == 4);
    puts("basic: one object per address, address 0, iteration");
}

void growth() {
    Table t(16);
    constexpr uint32_t n = 200000;
    std::vector<Obj*> objs(n);
    for (uint32_t i = 0; i < n; i++) {
        objs[i] = t.get(addr_of(i));
        if (i % 997 == 0)  // earlier objects survive every growth
            for (uint32_t j = 0; j <= i; j += 101) assert(t.find(addr_of(j)) == objs[j]);
    }
    for (uint32_t i = 0; i < n; i++) assert(t.get(addr_of(i)) == objs[i] && objs[i]->magic == kMagic);
    std::unordered_set<Obj*> distinct(objs.begin(), objs.end());
    assert(distinct.size() == n && t.size() == n);
    assert(t.slots() >= 2 * n && (t.slots() & (t.slots() - 1)) == 0);
    for (uint32_t i = n; i < n + 1000; i++) assert(t.find(addr_of(i)) == nullptr);
    size_t seen = 0;
    t.for_each([&](uint32_t, Obj*) { seen++; });
    assert(seen == n);
    printf("growth: %u objects in %u slots, all kept\n", n, t.slots());
}

void locked_and_lockfree() {
    Table t(16);
    Obj* a = t.get(addr_of(1));        // made before the old lookup's map exists
    assert(t.get_locked(addr_of(1)) == a);
    Obj* b = t.get_locked(addr_of(2));  // made by the old lookup
    assert(t.get(addr_of(2)) == b && t.find(addr_of(2)) == b);
    Obj* c = t.get(addr_of(3));         // made by the new one after the map exists
    assert(t.get_locked(addr_of(3)) == c);
    Obj* z = t.get_locked(0);
    assert(t.get(0) == z);
    for (uint32_t i = 4; i < 5000; i++) {  // alternating while the table grows
        Obj* p = (i & 1) ? t.get(addr_of(i)) : t.get_locked(addr_of(i));
        assert(t.get(addr_of(i)) == p && t.get_locked(addr_of(i)) == p);
    }
    Table::Took took;
    t.get(addr_of(7), &took);
    assert(!took.lock);  // found without the lock
    t.get(addr_of(90000), &took);
    assert(took.lock && !took.waited);
    Table::Took took2;
    t.get_locked(addr_of(7), &took2);
    assert(took2.lock);
    assert(t.size() == 5001);
    puts("locked and lock-free lookups: the same objects, however mixed");
}

// threads look up a shared set of addresses in different orders while the table grows (most addresses
// are new to all of them at first), some through the old lookup, switched while they run; every thread
// must get the same object for an address, constructed
void racing() {
    for (int round = 0; round < 6; round++) {
        Table t(16);
        constexpr uint32_t n = 60000;
        const int nthreads = 8;
        std::vector<std::atomic<Obj*>> first(n);
        for (auto& f : first) f.store(nullptr);
        std::atomic<bool> locked{false}, stop{false};
        std::atomic<int> bad{0};
        std::thread flip([&] {
            while (!stop.load()) {
                if (round & 1) locked.store(!locked.load());
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        std::thread iter([&] {  // iteration meanwhile: every object it sees is published
            while (!stop.load()) {
                t.for_each([&](uint32_t, Obj* p) {
                    if (p->magic != kMagic) bad.fetch_add(1);
                });
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
        std::vector<std::thread> ts;
        for (int k = 0; k < nthreads; k++)
            ts.emplace_back([&, k] {
                std::vector<uint32_t> order(n);
                for (uint32_t i = 0; i < n; i++) order[i] = i;
                std::mt19937 rng(round * 100 + k);
                std::shuffle(order.begin(), order.end(), rng);
                for (int pass = 0; pass < 2; pass++)
                    for (uint32_t i : order) {
                        Obj* p = locked.load(std::memory_order_relaxed) ? t.get_locked(addr_of(i)) : t.get(addr_of(i));
                        if (!p || p->magic != kMagic) {
                            bad.fetch_add(1);
                            continue;
                        }
                        Obj* expected = nullptr;
                        if (!first[i].compare_exchange_strong(expected, p) && expected != p) bad.fetch_add(1);
                    }
            });
        for (auto& th : ts) th.join();
        stop.store(true);
        flip.join();
        iter.join();
        assert(bad.load() == 0);
        assert(t.size() == n);
        for (uint32_t i = 0; i < n; i++) assert(t.find(addr_of(i)) == first[i].load());
    }
    puts("racing: 8 threads, 60,000 new addresses, growth and switched lookups: one constructed object each");
}

// ---------------------------------------------------------------- bench
struct BenchResult {
    double nsPerPair;
    double lockWaitShare;  // lookups that found the table's lock held
};
BenchResult bench(bool lockedLookup, int nthreads, double seconds) {
    Table t;
    constexpr uint32_t kMutexes = 512;
    for (uint32_t i = 0; i < kMutexes; i++) t.get(addr_of(i));
    std::atomic<bool> go{false}, stop{false};
    std::atomic<uint64_t> pairs{0}, lookups{0}, waits{0};
    std::vector<std::thread> ts;
    for (int k = 0; k < nthreads; k++)
        ts.emplace_back([&, k] {
            std::mt19937 rng(k);
            uint64_t n = 0, w = 0;
            const void* self = &n;
            while (!go.load()) std::this_thread::yield();
            while (!stop.load(std::memory_order_relaxed)) {
                for (int r = 0; r < 64; r++) {
                    const uint32_t a = addr_of(rng() % kMutexes);
                    for (int half = 0; half < 2; half++) {  // OSLockMutex, then OSUnlockMutex
                        Table::Took took;
                        Obj* mx = lockedLookup ? t.get_locked(a, &took) : t.get(a, &took);
                        w += took.waited;
                        std::lock_guard<std::mutex> lk(mx->m);
                        if (!half) {
                            if (mx->owner == self) mx->count++;
                            else if (!mx->owner) mx->owner = self, mx->count = 1;
                        } else if (mx->owner == self && --mx->count == 0) {
                            mx->owner = nullptr;
                        }
                    }
                    n++;
                }
            }
            pairs.fetch_add(n);
            lookups.fetch_add(2 * n);
            waits.fetch_add(w);
        });
    const auto start = std::chrono::steady_clock::now();
    go.store(true);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop.store(true);
    for (auto& th : ts) th.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // thread time per pair: what the game's threads would spend on it
    return {1e9 * secs * nthreads / (double)pairs.load(), (double)waits.load() / (double)std::max<uint64_t>(1, lookups.load())};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        const double secs = argc > 2 ? atof(argv[2]) : 1.0;
        printf("OSLockMutex + OSUnlockMutex pairs on 512 mutexes (%.1f s each): thread time per pair\n", secs);
        for (int n : {1, 2, 4, 6, 8}) {
            const BenchResult old = bench(true, n, secs), now = bench(false, n, secs);
            printf("  %d thread%s: locked lookup %6.1f ns (%4.1f%% of lookups found the lock held), lock-free %6.1f ns\n", n,
                   n == 1 ? " " : "s", old.nsPerPair, 100 * old.lockWaitShare, now.nsPerPair);
        }
        return 0;
    }
    basic();
    growth();
    locked_and_lockfree();
    racing();
    printf("obj_table: all checks pass (%d objects made)\n", g_made.load());
    return 0;
}
