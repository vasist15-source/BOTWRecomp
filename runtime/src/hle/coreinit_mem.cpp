// coreinit MEM heaps (expanded heap, frame heap) and memory utilities.
// Heap bookkeeping is kept host-side; the guest heap handle is the heap's start address.
#include <map>
#include <mutex>
#include <unordered_map>

#include "../runtime.h"
#include "../write_watch.h"

namespace {

struct ExpHeap {
    uint32_t start, end;                 // managed range
    std::map<uint32_t, uint32_t> free;   // addr -> size
    std::map<uint32_t, uint32_t> used;   // user addr -> (block start) ; size in used_size
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> blocks;  // user addr -> {block start, block size}

    void init(uint32_t s, uint32_t e) {
        start = s;
        end = e;
        free.clear();
        blocks.clear();
        free[s] = e - s;
    }

    uint32_t alloc(uint32_t size, int32_t align) {
        size = (size + 3) & ~3u;
        if (size == 0) size = 4;
        bool from_top = align < 0;
        uint32_t a = (uint32_t)(align < 0 ? -align : align);
        if (a < 4) a = 4;
        if (!from_top) {
            for (auto it = free.begin(); it != free.end(); ++it) {
                uint32_t bs = it->first, bsz = it->second;
                uint32_t user = (bs + a - 1) & ~(a - 1);
                if (user + size > bs + bsz) continue;
                take(it, user, size);
                return user;
            }
        } else {
            for (auto it = free.rbegin(); it != free.rend(); ++it) {
                uint32_t bs = it->first, bsz = it->second;
                if (bsz < size) continue;
                uint32_t user = (bs + bsz - size) & ~(a - 1);
                if (user < bs) continue;
                take(std::prev(it.base()), user, size);
                return user;
            }
        }
        return 0;
    }

    void take(std::map<uint32_t, uint32_t>::iterator it, uint32_t user, uint32_t size) {
        uint32_t bs = it->first, bsz = it->second;
        free.erase(it);
        if (user > bs) free[bs] = user - bs;
        uint32_t tail = bs + bsz - (user + size);
        if (tail) free[user + size] = tail;
        blocks[user] = {user, size};
    }

    bool release(uint32_t user) {
        auto it = blocks.find(user);
        if (it == blocks.end()) return false;
        uint32_t s = it->second.first, sz = it->second.second;
        blocks.erase(it);
        // insert and coalesce
        auto n = free.emplace(s, sz).first;
        if (n != free.begin()) {
            auto p = std::prev(n);
            if (p->first + p->second == n->first) {
                p->second += n->second;
                free.erase(n);
                n = p;
            }
        }
        auto nx = std::next(n);
        if (nx != free.end() && n->first + n->second == nx->first) {
            n->second += nx->second;
            free.erase(nx);
        }
        return true;
    }

    uint32_t total_free() const {
        uint32_t t = 0;
        for (auto& f : free) t += f.second;
        return t;
    }
    uint32_t largest(uint32_t a) const {
        uint32_t best = 0;
        for (auto& f : free) {
            uint32_t user = (f.first + a - 1) & ~(a - 1);
            if (user < f.first + f.second) best = std::max(best, f.first + f.second - user);
        }
        return best & ~3u;
    }
};

struct FrmHeap {
    uint32_t start, end, head, tail;
};

std::mutex g_mem_mutex;
std::unordered_map<uint32_t, ExpHeap*> g_exp;
std::unordered_map<uint32_t, FrmHeap*> g_frm;
uint32_t g_default_heap = 0, g_mem1_heap = 0, g_fg_heap = 0;
uint32_t g_mem2_start;  // start of the MEM2 arena (OSGetMemBound)

constexpr uint32_t kHeapHeader = 0x40;

uint32_t create_exp(uint32_t start, uint32_t size) {
    auto* h = new ExpHeap();
    h->init(start + kHeapHeader, start + size);
    st32(start, 0x45585048);  // "EXPH"
    g_exp[start] = h;
    return start;
}

uint32_t create_frm(uint32_t start, uint32_t size) {
    auto* h = new FrmHeap{start + kHeapHeader, start + size, start + kHeapHeader, start + size};
    st32(start, 0x46524D48);  // "FRMH"
    g_frm[start] = h;
    return start;
}

uint32_t exp_alloc(uint32_t heap, uint32_t size, int32_t align) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    auto it = g_exp.find(heap);
    if (it == g_exp.end()) {
        LOG("[mem] alloc from unknown exp heap %08X", heap);
        return 0;
    }
    uint32_t p = it->second->alloc(size, align);
    TRACE("[mem] exp alloc heap=%08X size=%X align=%d -> %08X", heap, size, align, p);
    if (!p) LOG("[mem] exp heap %08X out of memory (size %X align %d, free %X)", heap, size, align, it->second->total_free());
    return p;
}

uint32_t frm_alloc(uint32_t heap, uint32_t size, int32_t align) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    auto it = g_frm.find(heap);
    if (it == g_frm.end()) return 0;
    FrmHeap* h = it->second;
    uint32_t a = (uint32_t)(align < 0 ? -align : align);
    if (a < 4) a = 4;
    size = (size + 3) & ~3u;
    if (align >= 0) {
        uint32_t p = (h->head + a - 1) & ~(a - 1);
        if (p + size > h->tail) return 0;
        h->head = p + size;
        return p;
    }
    uint32_t p = (h->tail - size) & ~(a - 1);
    if (p < h->head) return 0;
    h->tail = p;
    return p;
}

}  // namespace

// called at boot by main
void mem_setup_heaps(uint32_t data_end) {
    uint32_t start = (data_end + 0xFFF) & ~0xFFFu;
    g_mem2_start = start;
    g_default_heap = create_exp(start, mem::kMem2End - start);
    g_mem1_heap = create_frm(mem::kMem1, mem::kMem1Size);
    g_fg_heap = create_frm(mem::kFgBucket, mem::kFgBucketSize);
    LOG("[mem] default heap %08X-%08X", start, mem::kMem2End);
}

uint32_t mem_default_alloc(uint32_t size, int32_t align) { return exp_alloc(g_default_heap, size, align); }
void mem_default_free(uint32_t p) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    g_exp[g_default_heap]->release(p);
}

// default heap function pointers (data imports)
static void default_alloc(Cpu* c) { ret(c, mem_default_alloc(arg(c, 0), 0x40)); }
static void default_alloc_ex(Cpu* c) { ret(c, mem_default_alloc(arg(c, 0), (int32_t)arg(c, 1))); }
static void default_free(Cpu* c) { if (arg(c, 0)) mem_default_free(arg(c, 0)); }

void mem_init_data_imports(uint32_t alloc_slot, uint32_t alloc_ex_slot, uint32_t free_slot) {
    if (alloc_slot) st32(alloc_slot, dispatch::register_host(default_alloc, "MEMAllocFromDefaultHeap"));
    if (alloc_ex_slot) st32(alloc_ex_slot, dispatch::register_host(default_alloc_ex, "MEMAllocFromDefaultHeapEx"));
    if (free_slot) st32(free_slot, dispatch::register_host(default_free, "MEMFreeToDefaultHeap"));
}

HLE(coreinit, MEMCreateExpHeapEx) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    TRACE("[mem] MEMCreateExpHeapEx(%08X, %X)", arg(c, 0), arg(c, 1));
    ret(c, create_exp(arg(c, 0), arg(c, 1)));
}
HLE(coreinit, MEMDestroyExpHeap) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    auto it = g_exp.find(arg(c, 0));
    if (it != g_exp.end()) { delete it->second; g_exp.erase(it); }
    ret(c, arg(c, 0));
}
HLE(coreinit, MEMAllocFromExpHeapEx) { ret(c, exp_alloc(arg(c, 0), arg(c, 1), (int32_t)arg(c, 2))); }
HLE(coreinit, MEMFreeToExpHeap) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    auto it = g_exp.find(arg(c, 0));
    if (it != g_exp.end() && arg(c, 1)) it->second->release(arg(c, 1));
}
HLE(coreinit, MEMGetAllocatableSizeForExpHeapEx) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    auto it = g_exp.find(arg(c, 0));
    int32_t a = (int32_t)arg(c, 1);
    uint32_t r = it == g_exp.end() ? 0 : it->second->largest((uint32_t)(a < 0 ? -a : a) | 4);
    TRACE("[mem] allocatable(heap=%08X align=%d) -> %X", arg(c, 0), a, r);
    ret(c, r);
}
HLE(coreinit, MEMGetTotalFreeSizeForExpHeap) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    auto it = g_exp.find(arg(c, 0));
    ret(c, it == g_exp.end() ? 0 : it->second->total_free());
}

HLE(coreinit, MEMCreateFrmHeapEx) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    TRACE("[mem] MEMCreateFrmHeapEx(%08X, %X)", arg(c, 0), arg(c, 1));
    ret(c, create_frm(arg(c, 0), arg(c, 1)));
}
HLE(coreinit, MEMDestroyFrmHeap) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    g_frm.erase(arg(c, 0));
    ret(c, arg(c, 0));
}
HLE(coreinit, MEMAllocFromFrmHeapEx) {
    uint32_t p = frm_alloc(arg(c, 0), arg(c, 1), (int32_t)arg(c, 2));
    TRACE("[mem] frm alloc heap=%08X size=%X align=%d -> %08X", arg(c, 0), arg(c, 1), (int32_t)arg(c, 2), p);
    ret(c, p);
}
HLE(coreinit, MEMGetAllocatableSizeForFrmHeapEx) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    auto it = g_frm.find(arg(c, 0));
    if (it == g_frm.end()) { ret(c, 0); return; }
    uint32_t a = (uint32_t)std::abs((int32_t)arg(c, 1));
    if (a < 4) a = 4;
    uint32_t p = (it->second->head + a - 1) & ~(a - 1);
    uint32_t r = p < it->second->tail ? it->second->tail - p : 0;
    TRACE("[mem] frm allocatable(heap=%08X) -> %X", arg(c, 0), r);
    ret(c, r);
}

// MEMAllocator (0x10 bytes): +0 MEMAllocatorFunc* {alloc(allocator, size), free(allocator, ptr)}, +4 heap,
// +8 param1 (alignment), +C param2. The function tables hold guest-callable addresses of host functions,
// so game code may call them directly as well as through MEMAllocFromAllocator / MEMFreeToAllocator.
// (BotW's movie player allocates its buffers through one; without these its threads quit at once.)
namespace {
uint32_t allocator_funcs(PpcFunc alloc, PpcFunc free_, const char* allocName, const char* freeName) {
    const uint32_t t = mem::host_alloc(8, 8);
    st32(t, dispatch::register_host(alloc, allocName));
    st32(t + 4, dispatch::register_host(free_, freeName));
    return t;
}
void exp_allocator_alloc(Cpu* c) { ret(c, exp_alloc(ld32(arg(c, 0) + 4), arg(c, 1), (int32_t)ld32(arg(c, 0) + 8))); }
void exp_allocator_free(Cpu* c) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    auto it = g_exp.find(ld32(arg(c, 0) + 4));
    if (it != g_exp.end() && arg(c, 1)) it->second->release(arg(c, 1));
}
void frm_allocator_alloc(Cpu* c) { ret(c, frm_alloc(ld32(arg(c, 0) + 4), arg(c, 1), (int32_t)ld32(arg(c, 0) + 8))); }
void frm_allocator_free(Cpu*) {}  // a frame heap frees only by state or all at once
void default_allocator_alloc(Cpu* c) { ret(c, mem_default_alloc(arg(c, 1), 0x40)); }
void default_allocator_free(Cpu* c) { if (arg(c, 1)) mem_default_free(arg(c, 1)); }
void init_allocator(uint32_t a, uint32_t funcs, uint32_t heap, uint32_t align) {
    st32(a + 0x0, funcs);
    st32(a + 0x4, heap);
    st32(a + 0x8, align);
    st32(a + 0xC, 0);
}
}  // namespace
HLE(coreinit, MEMInitAllocatorForExpHeap) {
    static const uint32_t funcs = allocator_funcs(exp_allocator_alloc, exp_allocator_free, "ExpHeapAllocator_Alloc", "ExpHeapAllocator_Free");
    init_allocator(arg(c, 0), funcs, arg(c, 1), arg(c, 2));
}
HLE(coreinit, MEMInitAllocatorForFrmHeap) {
    static const uint32_t funcs = allocator_funcs(frm_allocator_alloc, frm_allocator_free, "FrmHeapAllocator_Alloc", "FrmHeapAllocator_Free");
    init_allocator(arg(c, 0), funcs, arg(c, 1), arg(c, 2));
}
HLE(coreinit, MEMInitAllocatorForDefaultHeap) {
    static const uint32_t funcs =
        allocator_funcs(default_allocator_alloc, default_allocator_free, "DefaultHeapAllocator_Alloc", "DefaultHeapAllocator_Free");
    init_allocator(arg(c, 0), funcs, g_default_heap, 0);
}
// (allocator, size) / (allocator, ptr): through the allocator's own functions, as on Cafe OS
HLE(coreinit, MEMAllocFromAllocator) {
    const uint32_t a = arg(c, 0), funcs = a ? ld32(a) : 0;
    ret(c, funcs ? guest_call(c, ld32(funcs), {a, arg(c, 1)}) : 0);
}
HLE(coreinit, MEMFreeToAllocator) {
    const uint32_t a = arg(c, 0), funcs = a ? ld32(a) : 0;
    if (funcs) guest_call(c, ld32(funcs + 4), {a, arg(c, 1)});
}

// OSGetMemBound(type, u32* start, u32* size): 1 = MEM1, 2 = MEM2. On Cafe OS the MEM2 arena encloses
// the default heap (coreinit builds it there), so this reports the default heap's range; a game that
// builds its own heap over it would collide with malloc, so every call is logged.
HLE(coreinit, OSGetMemBound) {
    uint32_t type = arg(c, 0), start = 0, size = 0;
    if (type == 1) start = mem::kMem1, size = mem::kMem1Size;
    else if (type == 2) start = g_mem2_start, size = mem::kMem2End - g_mem2_start;
    LOG("[mem] OSGetMemBound(%u) -> %08X size %08X lr=%08X", type, start, size, c->lr);
    if (arg(c, 1)) st32(arg(c, 1), start);
    if (arg(c, 2)) st32(arg(c, 2), size);
    ret(c, start ? 0 : (uint32_t)-1);
}

// Overlay arena (BotW enables it): 0xA0000000, 0x1C000000 bytes, as in Cemu. Guest memory there is
// reserved like the rest of the 4 GiB, but is not yet mapped for the GPU (gfx/metal_main.mm).
static bool g_overlay_arena;
HLE(coreinit, OSIsEnabledOverlayArena) { ret(c, g_overlay_arena); }
HLE(coreinit, OSEnableOverlayArena) {
    g_overlay_arena = true;
    if (arg(c, 1)) st32(arg(c, 1), 0xA0000000u);
    if (arg(c, 2)) st32(arg(c, 2), 0x1C000000u);
    LOG("[mem] overlay arena enabled: A0000000 size 1C000000 lr=%08X", c->lr);
}
HLE(coreinit, OSDisableOverlayArena) { g_overlay_arena = false; }
namespace mem { bool overlay_arena_enabled() { return g_overlay_arena; } }

HLE(coreinit, MEMGetBaseHeapHandle) {
    // base heap slots: 0 = MEM1, 1 = MEM2 (default heap), 8 = foreground bucket
    uint32_t arena = arg(c, 0);
    ret(c, arena == 0 ? g_mem1_heap : arena == 1 ? g_default_heap : arena == 8 ? g_fg_heap : 0);
}

HLE(coreinit, OSBlockMove) { memmove(mem::ptr(arg(c, 0)), mem::ptr(arg(c, 1)), arg(c, 2)); ret(c, arg(c, 0)); }
HLE(coreinit, OSBlockSet) { memset(mem::ptr(arg(c, 0)), (int)arg(c, 1), arg(c, 2)); ret(c, arg(c, 0)); }
HLE(coreinit, memcpy) { memcpy(mem::ptr(arg(c, 0)), mem::ptr(arg(c, 1)), arg(c, 2)); ret(c, arg(c, 0)); }
HLE(coreinit, memmove) { memmove(mem::ptr(arg(c, 0)), mem::ptr(arg(c, 1)), arg(c, 2)); ret(c, arg(c, 0)); }
HLE(coreinit, memset) { memset(mem::ptr(arg(c, 0)), (int)arg(c, 1), arg(c, 2)); ret(c, arg(c, 0)); }

// caches are coherent on the host. Flushes and stores are the game saying "the GPU will read what I
// wrote here": a hint for the Vulkan buffer cache (write_watch.h; a no-op unless it is on).
HLE(coreinit, DCFlushRange) { wwatch::hint(arg(c, 0), arg(c, 1)); }
HLE(coreinit, DCFlushRangeNoSync) { wwatch::hint(arg(c, 0), arg(c, 1)); }
HLE(coreinit, DCInvalidateRange) {}
HLE(coreinit, DCStoreRange) { wwatch::hint(arg(c, 0), arg(c, 1)); }
HLE(coreinit, DCStoreRangeNoSync) { wwatch::hint(arg(c, 0), arg(c, 1)); }
HLE(coreinit, DCZeroRange) { memset(mem::ptr(arg(c, 0) & ~31u), 0, ((arg(c, 0) & 31) + arg(c, 1) + 31) & ~31u); }
HLE(coreinit, OSIsAddressRangeDCValid) { ret(c, 1); }

// ---------------------------------------------------------------- save states: heap bookkeeping
#include <algorithm>
#include <vector>

#include "../savestate.h"
void mem_ss_save(ss::Writer& w) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    w.u32(g_default_heap);
    w.u32(g_mem1_heap);
    w.u32(g_fg_heap);
    std::vector<uint32_t> keys;
    for (auto& [k, h] : g_exp) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    w.u32((uint32_t)keys.size());
    for (uint32_t k : keys) {
        ExpHeap* h = g_exp[k];
        w.u32(k);
        w.u32(h->start);
        w.u32(h->end);
        w.u32((uint32_t)h->free.size());
        for (auto& [a, n] : h->free) { w.u32(a); w.u32(n); }
        std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> b(h->blocks.begin(), h->blocks.end());
        std::sort(b.begin(), b.end());
        w.u32((uint32_t)b.size());
        for (auto& [u, v] : b) { w.u32(u); w.u32(v.first); w.u32(v.second); }
    }
    keys.clear();
    for (auto& [k, h] : g_frm) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    w.u32((uint32_t)keys.size());
    for (uint32_t k : keys) {
        w.u32(k);
        w.pod(*g_frm[k]);
    }
}

bool mem_ss_check(ss::Reader r, std::string& why) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    uint32_t d = r.u32(), m1 = r.u32(), fg = r.u32();
    if (d != g_default_heap || m1 != g_mem1_heap || fg != g_fg_heap) { why = "the base heaps differ (different game build?)"; return false; }
    return r.ok;
}

void mem_ss_load(ss::Reader& r) {
    std::lock_guard<std::mutex> lk(g_mem_mutex);
    g_default_heap = r.u32();
    g_mem1_heap = r.u32();
    g_fg_heap = r.u32();
    for (auto& [k, h] : g_exp) delete h;
    g_exp.clear();
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        uint32_t k = r.u32();
        auto* h = new ExpHeap();
        h->start = r.u32();
        h->end = r.u32();
        uint32_t nf = r.u32();
        for (uint32_t j = 0; j < nf && r.ok; j++) {
            uint32_t a = r.u32();
            h->free[a] = r.u32();
        }
        uint32_t nb = r.u32();
        for (uint32_t j = 0; j < nb && r.ok; j++) {
            uint32_t u = r.u32(), s = r.u32(), z = r.u32();
            h->blocks[u] = {s, z};
        }
        g_exp[k] = h;
    }
    for (auto& [k, h] : g_frm) delete h;
    g_frm.clear();
    n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        uint32_t k = r.u32();
        g_frm[k] = new FrmHeap(r.pod<FrmHeap>());
    }
}
