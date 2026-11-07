// Host objects of guest synchronization objects (OSMutex, OSFastMutex, OSSemaphore, OSEvent,
// OSMessageQueue, OSThreadQueue, OSRendezvous), keyed by the guest object's address (threads.cpp).
//
// Every OSLockMutex / OSUnlockMutex of every game thread looks its object up, so a lookup takes no lock:
// an open-addressing table of (address, object) slots, each filled once under the table's lock (the
// object first, then the address with release order) and read with acquire loads. An object is never
// replaced or deleted (a thread may be waiting on it, or about to), so what a lookup returns stays valid
// and every thread gets the same object for an address. The table is at most half full; a full one is not
// resized in place: a copy twice as large replaces it and the old one is kept (a lookup may still be
// reading it; the old copies together are smaller than the current one), so a lookup that misses in an
// old copy takes the lock and looks again in the current one before it adds an object.
//
// get_locked is the lookup as it was before (the table's one lock for every lookup, then an
// unordered_map), kept for WWHD_LOCKED_LOOKUP=1 and the in-run A/B (WWHD_LOOKUP_AB=1); its map is filled
// on its first use and kept up to date from then on, so both lookups always return the same objects.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace objtable {

template <class T>
class Table {
public:
    // what a lookup did (the A/B's counts): took the table's lock, found it held
    struct Took {
        bool lock = false, waited = false;
    };

    explicit Table(uint32_t slots = 1024) {  // a power of two
        Array* a = make(slots);
        arrays_.push_back(a);
        cur_.store(a, std::memory_order_release);
    }
    ~Table() {  // only when no lookup can run any more (tests; the runtime never destroys its tables)
        for_each([](uint32_t, T* p) { delete p; });
        for (Array* a : arrays_) {
            delete[] a->slots;
            delete a;
        }
    }
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    // the object of `addr`, made on first use
    T* get(uint32_t addr, Took* took = nullptr) {
        if (T* p = find(addr)) return p;
        return insert(addr, took);
    }

    // the object of `addr` if it has one (no lock)
    T* find(uint32_t addr) const {
        if (!addr) return zero_.load(std::memory_order_acquire);
        const Array* a = cur_.load(std::memory_order_acquire);
        for (uint32_t i = home(*a, addr);; i = (i + 1) & a->mask) {
            const uint32_t k = a->slots[i].key.load(std::memory_order_acquire);
            if (k == addr) return a->slots[i].obj.load(std::memory_order_relaxed);
            if (!k) return nullptr;
        }
    }

    // the old lookup: the table's lock, then a hash map (for the in-run A/B)
    T* get_locked(uint32_t addr, Took* took = nullptr) {
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        lock(lk, took);
        if (!mirror_on_) {
            each_locked([&](uint32_t a, T* p) { mirror_.emplace(a, p); });
            mirror_on_ = true;
        }
        auto it = mirror_.find(addr);
        if (it != mirror_.end()) return it->second;
        T* p = new T();
        add(addr, p);
        return p;
    }

    // every object with its address, under the table's lock: none is added meanwhile (f must not look up
    // an object that does not exist yet)
    template <class F>
    void for_each(F&& f) {
        std::lock_guard<std::mutex> lk(m_);
        each_locked(f);
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(m_);
        return count_ + (zero_.load(std::memory_order_relaxed) ? 1 : 0);
    }
    uint32_t slots() {
        std::lock_guard<std::mutex> lk(m_);
        return arrays_.back()->mask + 1;
    }

private:
    struct Slot {
        std::atomic<uint32_t> key{0};  // guest address; 0: empty (address 0 has a slot of its own)
        std::atomic<T*> obj{nullptr};
    };
    struct Array {
        uint32_t mask = 0;   // slots - 1
        uint32_t shift = 0;  // 32 - log2(slots)
        Slot* slots = nullptr;
    };

    static Array* make(uint32_t n) {
        Array* a = new Array;
        a->mask = n - 1;
        a->shift = 32 - (uint32_t)__builtin_ctz(n);
        a->slots = new Slot[n];
        return a;
    }
    // Fibonacci hashing: guest objects are aligned, so their low address bits say little
    static uint32_t home(const Array& a, uint32_t addr) { return (uint32_t)(addr * 0x9E3779B1u) >> a.shift; }
    static void place(Array& a, uint32_t addr, T* p) {
        uint32_t i = home(a, addr);
        while (a.slots[i].key.load(std::memory_order_relaxed)) i = (i + 1) & a.mask;
        a.slots[i].obj.store(p, std::memory_order_relaxed);
        a.slots[i].key.store(addr, std::memory_order_release);  // publishes the object
    }
    __attribute__((noinline)) T* insert(uint32_t addr, Took* took) {
        std::unique_lock<std::mutex> lk(m_, std::defer_lock);
        lock(lk, took);
        if (T* p = find(addr)) return p;  // made meanwhile
        T* p = new T();
        add(addr, p);
        return p;
    }
    void lock(std::unique_lock<std::mutex>& lk, Took* took) {
        if (!took) {
            lk.lock();
            return;
        }
        took->lock = true;
        if (!lk.try_lock()) {
            took->waited = true;
            lk.lock();
        }
    }
    // m_ held, addr has no object yet
    void add(uint32_t addr, T* p) {
        if (!addr) {
            zero_.store(p, std::memory_order_release);
        } else {
            Array* a = arrays_.back();
            if (2 * (count_ + 1) > a->mask + 1) {  // grow: a copy twice as large, then publish it
                Array* b = make(2 * (a->mask + 1));
                for (uint32_t i = 0; i <= a->mask; i++)
                    if (const uint32_t k = a->slots[i].key.load(std::memory_order_relaxed))
                        place(*b, k, a->slots[i].obj.load(std::memory_order_relaxed));
                arrays_.push_back(b);
                cur_.store(b, std::memory_order_release);
                a = b;
            }
            place(*a, addr, p);
            count_++;
        }
        if (mirror_on_) mirror_.emplace(addr, p);
    }
    template <class F>
    void each_locked(F&& f) {  // m_ held
        if (T* z = zero_.load(std::memory_order_relaxed)) f(0u, z);
        const Array* a = arrays_.back();
        for (uint32_t i = 0; i <= a->mask; i++)
            if (const uint32_t k = a->slots[i].key.load(std::memory_order_relaxed))
                f(k, a->slots[i].obj.load(std::memory_order_relaxed));
    }

    // read by every lookup: a cache line of their own, away from the lock
    alignas(128) std::atomic<const Array*> cur_{nullptr};
    std::atomic<T*> zero_{nullptr};
    alignas(128) std::mutex m_;    // adding objects, iterating, get_locked
    uint32_t count_ = 0;           // objects in the current array
    std::vector<Array*> arrays_;   // every array so far, the current one last (never freed: see above)
    bool mirror_on_ = false;       // mirror_ holds every object
    std::unordered_map<uint32_t, T*> mirror_;  // get_locked's map
};

// ---- threads.cpp: the lookup the HLE functions use, for the in-run A/B (metal_main.mm ab_frame)
enum Kind { kMutex, kSemaphore, kEvent, kMessageQueue, kThreadQueue, kRendezvous, kKinds };
struct Counts {                      // totals since start, all threads (counted only with WWHD_LOOKUP_AB=1)
    uint64_t lookups[kKinds] = {};
    uint64_t locked = 0;             // lookups that took the table's lock
    uint64_t lockWaits = 0;          // ... and found it held
    uint64_t mutexWaits = 0;         // OSMutex / OSFastMutex calls whose host object's lock was held
    uint64_t mutexBlocks = 0;        // OSLockMutex / OSFastMutex_Lock calls that waited for another thread
};
bool ab();                           // WWHD_LOOKUP_AB=1
bool locked();                       // the old lookup is in use (WWHD_LOCKED_LOOKUP=1, or the A/B's old windows)
void set_locked(bool on);
Counts counts();
uint64_t guest_cpu_ns();             // CPU time of the game's threads so far (each one up to its exit)

}  // namespace objtable
