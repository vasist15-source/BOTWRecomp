#include "../game_clock.h"
#include "vsync_pacing.h"
#include "../mods/fast_forward.h"
#include "../perf_metrics.h"
#include "../platform/host.h"
#include "../savestate.h"
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <sys/resource.h>
#include <time.h>
#include <condition_variable>
#include <deque>
// GX2 core: command execution, display lists, context states, draws, clears,
// copies and presentation.
#include <algorithm>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gx2.h"
#include "gx2_cmd.h"
#include "submitted_uniforms.h"
#include "gx2_regs.h"
#include "register_blocks.h"
#include "gx2_texture_regs.h"
#ifdef WWHD_HAS_VULKAN
#include "shader_key_dirty.h"
#include "gfx/vulkan/api.h"
#endif
#include "runtime.h"
#include "savestate.h"
#include "guest_addr.h"
#include "../aspect.h"
#include "gfx/renderer.h"
#include "platform/perf_hint.h"
#include "render_prof.h"

using namespace Latte;

namespace gx2 {

// ---------------------------------------------------------------- register file and context states
static uint32 g_regs[kNumRegs];
static uint32* g_shadow = nullptr;  // register copy of the active GX2ContextState
static std::unordered_map<uint32, std::vector<uint32>> g_contexts;
static std::recursive_mutex g_exec_mutex;
static RegisterBlocks<kNumRegs> g_register_blocks;

uint32* regs() { return g_regs; }

// Register writes that can change how shaders are translated bump g_shader_state_gen; the
// renderer reuses its last shader lookup while it is unchanged. Uniforms, uniform/vertex buffer
// addresses and rewrites of an identical value don't count.
extern "C" { uint64_t g_shader_state_gen = 1; }

static bool shader_irrelevant(uint32 reg) {
#ifdef WWHD_HAS_VULKAN
    // Vulkan resolves texture addresses freshly in bind_stage. These words do
    // not participate in shader translation; keep Metal's broader dirty gate.
    static const bool addressMemo = [] {
        const char* e = getenv("WWHD_VK_SHADER_ADDRESS_MEMO");
        return render::vulkan() && e && !strcmp(e, "1");
    }();
    if (addressMemo)
        for (uint32 base : {uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS),
                            uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS),
                            uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS)})
            if (reg >= base && reg < base + 7 * 18) {
                uint32 word = (reg - base) % 7;
                if (word == 2 || word == 3) return true;
            }
#endif
    if (reg >= mmSQ_ALU_CONSTANT0_0 && reg < mmSQ_ALU_CONSTANT0_0 + 0x1000) return true;
    for (uint32 base : {(uint32)mmSQ_VTX_UNIFORM_BLOCK_START, (uint32)mmSQ_PS_UNIFORM_BLOCK_START, (uint32)mmSQ_GS_UNIFORM_BLOCK_START})
        if (reg >= base && reg < base + 7 * 16) return true;
    if (reg >= mmSQ_VTX_ATTRIBUTE_BLOCK_START && reg < mmSQ_VTX_ATTRIBUTE_BLOCK_START + 7 * 16) {
        uint32 w = (reg - mmSQ_VTX_ATTRIBUTE_BLOCK_START) % 7;
        return w != 2;  // word 2 holds the stride
    }
    return false;
}
// Metal: whether a texture or sampler register write can change a shader key (metal_draw.mm's
// stage_state_hash reads, for texture units 0-17 of the vertex and pixel stages: the type and format
// bits, and whether the texture's address is a bound colour buffer; for their samplers, the
// depth-compare bits). Draws rebind textures constantly, and every rebind used to make the next draw
// recompute both shader keys (~9% of the render thread in busy scenes). WWHD_SHADER_KEY_ALL_TEX=1:
// every texture write counts, as before.
static bool metal_key_changes(uint32 reg, uint32 old, uint32 val) {
    static const bool all = getenv("WWHD_SHADER_KEY_ALL_TEX") != nullptr;
    if (all) return true;
    for (uint32 base : {uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS), uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS)}) {
        if (reg < base || reg >= base + 7 * 160) continue;
        const uint32 unit = (reg - base) / 7, word = (reg - base) % 7;
        if (unit >= 18) return false;
        switch (word) {
        case 0: return ((old ^ val) & 7) != 0;
        case 1: return ((old ^ val) & 0x3F00000) != 0;
        case 4: return ((old ^ val) & 0x300) != 0;
        case 2: {  // framebuffer fetch: the address against the bound colour buffers (as stage_state_hash)
            auto fb = [](uint32 w2) {
                const uint32 b = w2 << 8;
                if (!b) return 0u;
                for (uint32 i = 0; i < 8; i++)
                    if (b == (g_regs[mmCB_COLOR0_BASE + i] & ~0xFFu)) return i + 1;
                return 0u;
            };
            return fb(old) != fb(val);
        }
        default: return false;  // mip address, swizzle/LOD words
        }
    }
    if (reg >= uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS) && reg < uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS) + 7 * 160)
        return false;  // geometry shaders aren't translated
    if (reg >= uint32(REGADDR::SQ_TEX_SAMPLER_WORD0_0) && reg < uint32(REGADDR::SQ_TEX_SAMPLER_WORD0_0) + 3 * 18 * 3) {
        const uint32 word = (reg - uint32(REGADDR::SQ_TEX_SAMPLER_WORD0_0)) % 3;
        return word == 0 && ((old ^ val) & 0xF8000000) != 0;
    }
    // everything else: only the registers the shader key reads (metal_draw.mm get_shader_uncached and
    // stage_state_hash). Viewport, scissor, blend, stencil and the like changed on most draws and made
    // every draw recompute both keys. WWHD_SHADER_KEY_ALL_REGS=1: every register counts, as before.
    static const bool allRegs = getenv("WWHD_SHADER_KEY_ALL_REGS") != nullptr;
    if (allRegs) return true;
    static const std::vector<bool> keyRegs = [] {
        std::vector<bool> k(kNumRegs);
        auto add = [&](uint32 first, uint32 count) { for (uint32 r = first; r < first + count && r < kNumRegs; r++) k[r] = true; };
        add(mmSQ_PGM_START_VS, 2);
        add(mmSQ_PGM_START_PS, 2);
        add(mmSQ_PGM_START_FS, 2);
        add(mmVGT_STRMOUT_EN, 1);
        for (uint32 b = 0; b < 4; b++) add(mmVGT_STRMOUT_VTX_STRIDE_0 + b * 4, 1);
        add(mmSQ_VTX_SEMANTIC_0, 32);
        add(mmSPI_VS_OUT_ID_0, 10);
        add(mmSPI_VS_OUT_CONFIG, 1);
        add(mmPA_CL_VS_OUT_CNTL, 1);
        add(mmSPI_PS_IN_CONTROL_0, 2);
        add(mmSPI_PS_INPUT_CNTL_0, 32);
        add(uint32(REGADDR::SQ_CONFIG), 1);
        add(mmCB_SHADER_MASK, 1);
        add(mmCB_SHADER_CONTROL, 1);
        add(mmDB_SHADER_CONTROL, 1);
        add(mmSPI_INPUT_Z, 1);
        add(uint32(REGADDR::SX_ALPHA_TEST_CONTROL), 1);
        add(uint32(REGADDR::PA_CL_VTE_CNTL), 1);
        add(uint32(REGADDR::PA_CL_CLIP_CNTL), 1);
        add(uint32(REGADDR::PA_SU_SC_MODE_CNTL), 1);  // both faces culled: rasterization off
        add(uint32(REGADDR::CB_COLOR_CONTROL), 1);
        add(uint32(REGADDR::CB_TARGET_MASK), 1);
        add(mmCB_COLOR0_INFO, 8);
        add(mmCB_COLOR0_BASE, 8);
        add(uint32(REGADDR::DB_DEPTH_CONTROL), 1);
        return k;
    }();
    return reg < kNumRegs && keyRegs[reg];
}
// debug: WWHD_GEN_STATS=1 logs, every 20000 bumps, which registers bumped the shader generation
static void note_gen_bump(uint32 reg) {
    static const bool on = getenv("WWHD_GEN_STATS") != nullptr;
    if (!on) return;
    static std::unordered_map<uint32, uint32> tally;
    static uint32 total = 0;
    tally[reg]++;
    if (++total % 20000) return;
    std::vector<std::pair<uint32, uint32>> v(tally.begin(), tally.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string line;
    for (size_t i = 0; i < v.size() && i < 16; i++) {
        char b[32];
        snprintf(b, sizeof b, " %04X=%u", v[i].first, v[i].second);
        line += b;
    }
    LOG("[genstats] %u bumps; by register:%s", total, line.c_str());
    tally.clear();
}
static ShaderKeyDirtyStats shaderKeyDirtyStats;
ShaderKeyDirtyStats shader_key_dirty_stats() { return shaderKeyDirtyStats; }

#ifdef WWHD_HAS_VULKAN
static void apply_small_regs(uint32 first, const uint32* v, uint32 n) {
    static const bool keyDirty = [] {
        const char* e = getenv("WWHD_VK_SHADER_KEY_DIRTY");
        return e && !strcmp(e, "1");
    }();
    static const bool collectStats = getenv("WWHD_VK_STATS") != nullptr;
    const bool classify = rprof::classify_on();
    bool changed = false, baselineBump = false, actualBump = false;
    uint64_t maskedWords = 0;
    for (uint32 i = 0; i < n; ++i) {
        const uint32 reg = first + i, value = v[i], old = g_regs[reg];
        if (old != value) {
            changed = true;
            if (classify) {
                if (rprof::fast_class_reg(reg)) rprof::g_reg_dirty |= 1;
                else rprof::note_other_reg(reg);
            }
            if ((collectStats || !actualBump) && !shader_irrelevant(reg)) {
                baselineBump = true;
                uint32 mask;
                if (keyDirty && vulkan_shader_key_mask(reg, mask) && !((old ^ value) & mask)) {
                    if (collectStats) ++maskedWords;
                } else actualBump = true;
            }
            g_regs[reg] = value;
        }
        // Shadow can differ even when registers already match: draw mutates
        // primitive type directly, and context setup initializes only shadow.
        if (g_shadow) g_shadow[reg] = value;
    }
    if (actualBump) ++g_shader_state_gen;
    if (changed && collectStats) {
        ++shaderKeyDirtyStats.changedBatches;
        shaderKeyDirtyStats.baselineWouldBumps += baselineBump;
        shaderKeyDirtyStats.actualBumps += actualBump;
        shaderKeyDirtyStats.avoidedBumps += baselineBump && !actualBump;
        shaderKeyDirtyStats.maskedWords += maskedWords;
    }
}
#endif

static void apply_regs(uint32 first, const uint32* v, uint32 n) {
    if (first >= kNumRegs || n > kNumRegs - first) return;
    g_register_blocks.touch(first, n);
#ifdef WWHD_HAS_VULKAN
    // Vulkan renderer only (the Metal renderer keeps the original bulk path)
    static const bool fusedSmall = [] {
        const char* e = getenv("WWHD_VK_FUSE_SMALL_REGS");
        // Enabled by default; explicit zero retains the original bulk path.
        return render::vulkan() && (!e || strcmp(e, "0") != 0);
    }();
    if (fusedSmall && n <= 16) {
        apply_small_regs(first, v, n);
        return;
    }
#endif
    if (memcmp(&g_regs[first], v, n * 4) != 0) {
        if (rprof::classify_on())  // draw classifier (render_prof.h)
            for (uint32 i = 0; i < n; i++)
                if (g_regs[first + i] != v[i]) {
                    if (rprof::fast_class_reg(first + i)) rprof::g_reg_dirty |= 1;
                    else rprof::note_other_reg(first + i);
                }
#ifdef WWHD_HAS_VULKAN
        static const bool keyDirty = [] {
            const char* value = getenv("WWHD_VK_SHADER_KEY_DIRTY");
            return render::vulkan() && value && !strcmp(value, "1");
        }();
        static const bool collectStats = render::vulkan() && getenv("WWHD_VK_STATS") != nullptr;
        if(keyDirty || collectStats) {
            bool baselineBump = false, actualBump = false;
            uint64_t maskedWords = 0;
            for(uint32 i = 0; i < n; ++i) {
                uint32 reg = first + i;
                if(g_regs[reg] == v[i] || shader_irrelevant(reg)) continue;
                baselineBump = true;
                uint32 mask;
                if(keyDirty && vulkan_shader_key_mask(reg, mask) && !((g_regs[reg] ^ v[i]) & mask)) {
                    if(collectStats) ++maskedWords;
                } else actualBump = true;
                if(actualBump && !collectStats) break;
            }
            if(actualBump) ++g_shader_state_gen;
            if(collectStats) {
                ++shaderKeyDirtyStats.changedBatches;
                shaderKeyDirtyStats.baselineWouldBumps += baselineBump;
                shaderKeyDirtyStats.actualBumps += actualBump;
                shaderKeyDirtyStats.avoidedBumps += baselineBump && !actualBump;
                shaderKeyDirtyStats.maskedWords += maskedWords;
            }
            memcpy(&g_regs[first], v, n * 4);
        } else
#endif
        {
            // one pass over the changed span: the shader-generation check (constants and uniform/attribute
            // block words skip it by table) and only the changed words copied
            static uint8_t keyReg[kNumRegs];
            static const bool keyRegInit = [] {
                for (uint32 r = 0; r < kNumRegs; r++) keyReg[r] = !shader_irrelevant(r);
                return true;
            }();
            (void)keyRegInit;
            uint32 lo = 0;
            while (g_regs[first + lo] == v[lo]) lo++;  // memcmp found a difference
            uint32 hi = n - 1;
            while (g_regs[first + hi] == v[hi]) hi--;
            bool bumped = false;
            for (uint32 i = lo; i <= hi && !bumped; i++)
                if (g_regs[first + i] != v[i] && keyReg[first + i] && metal_key_changes(first + i, g_regs[first + i], v[i])) {
                    g_shader_state_gen++;
                    note_gen_bump(first + i);
                    bumped = true;
                }
            memcpy(&g_regs[first + lo], v + lo, (hi - lo + 1) * 4);
        }
    }
    if (g_shadow && g_shadow != v) memcpy(&g_shadow[first], v, n * 4);
}

// ---------------------------------------------------------------- display list recording
struct Recording {
    uint32 start = 0, pos = 0, end = 0;
};
static thread_local Recording t_rec;

static void execute_one(Op op, const uint32* p, uint32 n);

// ---------------------------------------------------------------- copies of guest data the CPU rewrites
// WWHD_GUEST_COPIES: 1 (default) display-list draws read their uniform blocks as submitted (below) and
// Metal copies vertex buffers and uniform blocks the CPU rewrites at record time (metal_draw.mm,
// rewritten_copy); 0 turns all of it off (draws read guest memory when the GPU runs them, as before);
// 2 alternates every 10 s and logs process CPU, ms/frame and the render and GX2 threads' CPU per
// frame for each window ([copies-ab]; an A/B in one run, heat drifts too much between runs)
static int guest_copies_mode() {
    static const int mode = getenv("WWHD_GUEST_COPIES") ? atoi(getenv("WWHD_GUEST_COPIES")) : 1;
    return mode;
}
static std::atomic<bool> g_guest_copies{guest_copies_mode() != 0};
bool guest_copies_on() { return g_guest_copies.load(std::memory_order_relaxed); }
static std::atomic<int64_t> g_render_cpu_ns{0};  // render thread's CPU time at its last swap (A/B only)
static std::atomic<uint64_t> g_submitted_bytes{0};  // submitted uniform bytes copied (A/B only)
static int64_t thread_cpu_ns() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
static void guest_copies_ab() {  // the GX2 thread, once per swap
    if (guest_copies_mode() != 2) return;
    static uint64_t swaps = 0;
    swaps++;
    static auto windowStart = std::chrono::steady_clock::now();
    static double cpuStart = 0;
    static int64_t renderStart = 0, gx2Start = 0;
    static uint64_t swapsStart = 0, bytesStart = 0;
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - windowStart).count();
    if (secs < 10) return;
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const double cpu = ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
    const int64_t render = g_render_cpu_ns.load(std::memory_order_relaxed), gx2 = thread_cpu_ns();
    const uint64_t bytes = g_submitted_bytes.load(std::memory_order_relaxed);
    const bool on = guest_copies_on();
    if (cpuStart && swaps > swapsStart) {
        const double frames = double(swaps - swapsStart);
        LOG("[copies-ab] %s: %.0f%% CPU, %.2f ms/frame, render thread %.2f ms/frame, GX2 thread %.2f ms/frame, "
            "submitted uniforms %.0f KB/frame (%.1f s)",
            on ? "copies on " : "copies off", 100 * (cpu - cpuStart) / secs, 1000 * secs / frames, (render - renderStart) / 1e6 / frames,
            (gx2 - gx2Start) / 1e6 / frames, (bytes - bytesStart) / 1024.0 / frames, secs);
    }
    g_guest_copies.store(!on, std::memory_order_relaxed);
    windowStart = now;
    cpuStart = cpu;
    renderStart = render;
    gx2Start = gx2;
    swapsStart = swaps;
    bytesStart = bytes;
}

// ---------------------------------------------------------------- submitted uniform blocks
// (submitted_uniforms.h) WWHD_GX2_SUBMITTED_UNIFORMS=0 turns it off: draws in display lists then read
// uniform blocks from guest memory whenever the render thread executes them (the old behaviour).
static bool submitted_uniforms_on() {
    static const bool on = [] {
        const char* e = getenv("WWHD_GX2_SUBMITTED_UNIFORMS");
        // only the Metal renderer reads the snapshots
        const bool v = (!e || atoi(e) != 0) && guest_copies_mode() != 0 && !render::vulkan();
        LOG("[gx2] display-list uniform blocks: %s", v ? "as submitted (default)" : "read when executed");
        return v;
    }();
    return on;
}
static submitted::DisplayListIndex g_dl_index;
// the blocks bound and lists called while this thread records a display list (t_rec)
static thread_local std::vector<submitted::Range> t_rec_blocks;
static thread_local std::vector<uint32> t_rec_calls;
static std::mutex g_snap_mutex;
static std::vector<submitted::Snapshot*> g_snap_free;
static std::atomic<uint64_t> g_snap_ids{0};  // submitting threads
static submitted::Snapshot* g_snap_active = nullptr;  // render thread

void note_uniform_block(uint32 addr, uint32 size) {
    if (t_rec.start && submitted_uniforms_on()) t_rec_blocks.push_back({addr, size});
}
const uint8_t* submitted_uniform_block(uint32 addr, uint32 size) {
    return g_snap_active ? g_snap_active->find(addr, size) : nullptr;
}
uint64_t submitted_uniforms_id() { return g_snap_active ? g_snap_active->id() : 0; }
static bool snapshot_readable(uint32 addr, uint32 size) {
    const uint64_t end = uint64_t(addr) + size + submitted::Snapshot::kTail;
    auto in = [&](uint64_t lo, uint64_t hi) { return addr >= lo && end <= hi; };
    return in(mem::kMem2Start, mem::kMem2End) || in(mem::kRuntimeStart, mem::kRuntimeEnd) ||
           in(mem::kMem1, uint64_t(mem::kMem1) + mem::kMem1Size) || in(mem::kFgBucket, uint64_t(mem::kFgBucket) + mem::kFgBucketSize);
}
// a display-list call from the command stream: its blocks as they are now go with it
static submitted::Snapshot* submit_snapshot(uint32 list, uint32 size) {
    if (!submitted_uniforms_on() || !guest_copies_on()) return nullptr;
    static thread_local std::vector<submitted::Range> ranges;
    ranges.clear();
    if (!g_dl_index.blocks(list, size, ranges) || ranges.empty()) return nullptr;
    submitted::Snapshot* s = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_snap_mutex);
        if (!g_snap_free.empty()) { s = g_snap_free.back(); g_snap_free.pop_back(); }
    }
    if (!s) s = new submitted::Snapshot;
    const uint64_t id = g_snap_ids.fetch_add(1, std::memory_order_relaxed) + 1;
    const size_t bytes = s->build(id, ranges, [](uint32 a) { return (const uint8_t*)mem::ptr(a); }, snapshot_readable);
    if (guest_copies_mode() == 2) g_submitted_bytes.fetch_add(bytes, std::memory_order_relaxed);
    // debug: WWHD_GX2_SUBMITTED_STATS=1 logs the copies made, every 1000 submissions
    static const bool stats = getenv("WWHD_GX2_SUBMITTED_STATS") != nullptr;
    if (stats) {
        static std::mutex mu;
        static uint64_t n = 0, blocks = 0, total = 0;
        std::lock_guard<std::mutex> lk(mu);
        blocks += s->blocks();
        total += bytes;
        if (++n % 1000 == 0) {
            LOG("[gx2] submitted uniforms: %llu submissions, last 1000: %.1f blocks and %.1f KB each; %zu lists indexed",
                (unsigned long long)n, blocks / 1000.0, total / 1000.0 / 1024, g_dl_index.size());
            blocks = total = 0;
        }
    }
    return s;
}
static void set_active_snapshot(submitted::Snapshot* s) {  // render thread
    if (g_snap_active) {
        std::lock_guard<std::mutex> lk(g_snap_mutex);
        g_snap_free.push_back(g_snap_active);
    }
    g_snap_active = s;
}

// ---------------------------------------------------------------- render thread
// Like the real GPU, command execution runs asynchronously to the game: GX2 calls append to a
// queue that a render thread turns into Metal work. WWHD_NO_RENDER_THREAD=1 executes inline.
static const bool g_render_thread = getenv("WWHD_NO_RENDER_THREAD") == nullptr;
static std::mutex g_q_mutex;
static std::condition_variable g_q_cv;
static std::vector<uint32> g_q_pending, g_q_work;
static bool g_q_waiting = false;

static uint64_t g_fence_issued = 0;
// fences the render thread has reached: the game thread waits on it with atomic wait (a __ulock futex); a
// condition variable's broadcast cost the render thread ~0.3 ms in the kernel at every DrawDone (8% of it)
static std::atomic<uint64_t> g_fence_done{0};

// debug: WWHD_FRAME_TRACE=1 logs, for every frame whose flip came over 40 ms after the previous one
// (and every 60th frame, for comparison), where its time went, in ms from the previous flip: the
// game's swap call, its GX2DrawDone waits, the render thread's GPU-fence wait, render-thread idle
// (waiting for the game's commands) after that fence, the end of encoding, GPU completion, the flip
extern "C" int64_t wwhd_frame_done_ns(uint64_t n);
extern "C" int64_t wwhd_frame_gpu_start_ns(uint64_t n);  // gfx: when frame n's GPU work completed (0: not yet)
namespace ftrace {
static const bool on = getenv("WWHD_FRAME_TRACE") != nullptr;
struct Rec { int64_t guestSwap, fenceA, fenceB, encoded, firstOp; double drawDoneMs, idleMs, idleAfterFenceMs; };
static Rec ring[256];
static uint64_t rframe = 0;  // render thread: swaps executed
static int64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static Rec& rcur() { return ring[(rframe + 1) & 255]; }
}

static void render_thread_main() {
    host::set_thread_name("GX2 render");
    host::boost_thread_priority();
    perf_hint::add_current_thread();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_q_mutex);
            g_q_waiting = true;
            if (g_q_pending.empty()) {
                const uint64_t idle = rprof::enabled() ? rprof::now_ns() : 0;
                const int64_t ft0 = ftrace::on ? ftrace::now() : 0;
                g_q_cv.wait(lk, [] { return !g_q_pending.empty(); });
                if (idle) rprof::add_idle(rprof::now_ns() - idle);
                if (ft0) {
                    double ms = (ftrace::now() - ft0) / 1e6;
                    ftrace::rcur().idleMs += ms;
                    if (ftrace::rcur().fenceB) ftrace::rcur().idleAfterFenceMs += ms;
                }
            }
            g_q_waiting = false;
            g_q_work.swap(g_q_pending);
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
            render::with_autorelease_pool([] { execute(g_q_work.data(), (uint32)g_q_work.size()); });
        }
        g_q_work.clear();
    }
}

static void enqueue(Op op, const uint32* payload, uint32 n) {
    static std::once_flag once;
    std::call_once(once, [] { std::thread([] { exception_report::boundary("GX2 render thread", render_thread_main); }).detach(); });
    bool wake;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        g_q_pending.push_back(op | (n << 8));
        g_q_pending.insert(g_q_pending.end(), payload, payload + n);
        wake = g_q_waiting;
    }
    if (wake) g_q_cv.notify_one();  // after unlocking: the woken render thread would block on the mutex at once
}

// block the game thread until the render thread has executed everything queued so far
// debug: WWHD_SYNC_STATS=1 logs, every 5 s, how often each caller waited for the render thread to
// catch up (render_sync) and for how long
enum SyncSite { kSyncShutdown, kSyncFlip, kSyncDrawDone, kSyncVsyncUncapped, kSyncVsyncFlip, kSyncSaveState, kSyncCopySurface, kSyncVsyncFrame, kSyncSites };
static void sync_stat(int site, std::chrono::steady_clock::duration waited) {
    static const bool on = getenv("WWHD_SYNC_STATS") != nullptr;
    if (!on) return;
    static std::mutex mu;
    static uint64_t count[kSyncSites], ns[kSyncSites];
    static auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu);
    count[site]++;
    ns[site] += std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count();
    const auto now = std::chrono::steady_clock::now();
    if (now - t0 < std::chrono::seconds(5)) return;
    const double secs = std::chrono::duration<double>(now - t0).count();
    static const char* names[kSyncSites] = {"shutdown", "flip", "DrawDone", "vsync-uncapped", "vsync-flip", "savestate", "CopySurface", "vsync-frame (GPU)"};
    char buf[400];
    int k = snprintf(buf, sizeof buf, "[gx2] render_sync per second:");
    for (int i = 0; i < kSyncSites; i++)
        if (count[i]) k += snprintf(buf + k, sizeof buf - k, " %s %.1f x %.2f ms", names[i], count[i] / secs, ns[i] / 1e6 / count[i]);
    LOG("%s", buf);
    memset(count, 0, sizeof count); memset(ns, 0, sizeof ns);
    t0 = now;
}
static void render_sync(int site = kSyncShutdown) {
    if (!g_render_thread) return;
    const auto started = std::chrono::steady_clock::now();
    struct Done {
        int site;
        std::chrono::steady_clock::time_point t;
        ~Done() {
            const auto waited = std::chrono::steady_clock::now() - t;
            sync_stat(site, waited);
            if (rprof::enabled())
                rprof::add_sync(site == kSyncDrawDone ? rprof::kSyncDrawDone : site == kSyncCopySurface ? rprof::kSyncCopySurface
                                : site == kSyncFlip || site == kSyncVsyncFlip ? rprof::kSyncFlip : rprof::kSyncOther,
                                (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count());
        }
    } done{site, started};
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        id = ++g_fence_issued;
    }
    uint32 w = (uint32)id;
    enqueue(OP_FENCE, &w, 1);
    for (uint64_t done; (done = g_fence_done.load(std::memory_order_acquire)) < id;) g_fence_done.wait(done, std::memory_order_acquire);
}

static std::atomic<uint64_t> g_draws_executed{0};  // render thread; the frame-rate code tells loading screens apart
void emit(Op op, const uint32* payload, uint32 n) {
    if (t_rec.start) {
        uint32 bytes = 4 * (n + 1);
        if (t_rec.pos + bytes > t_rec.end) {
            LOG("[gx2] display list overflow at %08X", t_rec.start);
            return;
        }
        uint32* w = (uint32*)mem::ptr(t_rec.pos);
        w[0] = op | (n << 8);
        memcpy(w + 1, payload, n * 4);
        t_rec.pos += bytes;
        return;
    }
    if (g_render_thread) {
        enqueue(op, payload, n);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(op, payload, n); });
}

// host-only commands never go into display lists
static void emit_host(Op op, std::initializer_list<uint32> payload) {
    if (g_render_thread) {
        enqueue(op, payload.begin(), (uint32)payload.size());
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(op, payload.begin(), (uint32)payload.size()); });
}

#ifdef WWHD_HAS_VULKAN
void checkpoint_vulkan_caches() {
    // Wait for queued work, then exclude further renderer mutations while the
    // SDL thread writes the final cache checkpoint during orderly shutdown.
    render_sync();
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([] {
        gfxvk::wait_idle();
        gfxvk::save_renderer_caches();
    });
}
#endif

void set_regs(uint32 first, const uint32* values, uint32 count) {
    if (!count) return;
    static thread_local std::vector<uint32> buf;
    buf.resize(count + 1);
    buf[0] = first;
    memcpy(&buf[1], values, count * 4);
    emit(OP_SET_REGS, buf.data(), count + 1);
}
void set_reg(uint32 reg, uint32 value) { emit(OP_SET_REGS, {reg, value}); }

void execute(const uint32* words, uint32 count) {
    uint32 i = 0;
    while (i < count) {
        uint32 hdr = words[i];
        Op op = (Op)(hdr & 0xFF);
        uint32 n = hdr >> 8;
        if (op >= OP_COUNT || i + 1 + n > count) {
            LOG("[gx2] corrupt display list command %08X", hdr);
            return;
        }
        execute_one(op, &words[i + 1], n);
        i += 1 + n;
    }
}

static void set_context(uint32 ctx) {
    if (!ctx) {
        g_shadow = nullptr;
        return;
    }
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) {
        g_shadow = nullptr;
        return;
    }
    g_shadow = it->second.data();
    // Draw writes primitive type without updating the context shadow.
    g_register_blocks.touch(uint32(REGADDR::VGT_PRIMITIVE_TYPE), 1);
    if (g_register_blocks.restore(g_regs, g_shadow, [](uint32 reg, uint32 old, uint32 value) {
        if (shader_irrelevant(reg)) return false;
        if (!render::vulkan() && !metal_key_changes(reg, old, value)) return false;
#ifdef WWHD_HAS_VULKAN
        static const bool keyDirty = [] {
            const char* e = getenv("WWHD_VK_SHADER_KEY_DIRTY");
            return render::vulkan() && e && !strcmp(e, "1");
        }();
        uint32 mask;
        if (keyDirty && vulkan_shader_key_mask(reg, mask)) return ((old ^ value) & mask) != 0;
#endif
        return true;
    })) ++g_shader_state_gen;
    rprof::g_reg_dirty |= 2;  // draw classifier: a context load counts as a full state change
}

constexpr uint32 kColorBufferWords = 0x9C / 4, kDepthBufferWords = 0xAC / 4, kSurfaceWords = 0x74 / 4;
// struct copies carried in a command, placed back in guest memory for the renderer (commands run
// one at a time, so a couple of fixed slots suffice)
static uint32 unpack_struct(const uint32* words, uint32 count, int slot) {
    static uint32 scratch = 0;
    if (!scratch) scratch = mem::host_alloc(2 * 0x100, 0x40);
    uint32 addr = scratch + slot * 0x100;
    memcpy(mem::ptr(addr), words, count * 4);
    return addr;
}

// GX2DrawDone queues the work instead of waiting for an idle GPU. WWHD kept the full wait on Metal
// (its renderer reads large vertex buffers straight from guest memory); in BotW that wait was 55% of
// the main thread's time. Flips still wait for the GPU frame (frames_completed), so the CPU stays at
// most a frame ahead. WWHD_LAZY_DRAW_DONE=0 (Vulkan: WWHD_VK_LAZY_DRAW_DONE=0) restores the full wait.
static bool lazy_draw_done() {
    static const bool on = [] {
        const char* e = getenv(render::vulkan() ? "WWHD_VK_LAZY_DRAW_DONE" : "WWHD_LAZY_DRAW_DONE");
        return !e || atoi(e) != 0;
    }();
    return on;
}

static void execute_op(Op op, const uint32* p, uint32 n);
// every op is counted and (sampled) timed for the render-thread profiler (render_prof.h); display-list
// calls are not timed themselves: their ops are
static void execute_one(Op op, const uint32* p, uint32 n) {
    rprof::Op kind;
    switch (op) {
    case OP_SUBMITTED_UNIFORMS:
        set_active_snapshot(n >= 2 ? (submitted::Snapshot*)(uintptr_t(p[0]) | (uint64_t(p[1]) << 32)) : nullptr);
        return;
    case OP_CALL: execute_op(op, p, n); return;
    case OP_SET_REGS: case OP_SET_PROJ_REGS: kind = rprof::kOpRegs; break;
    case OP_DRAW: case OP_DRAW_INDEXED: kind = rprof::kOpDraw; rprof::classify_draw(); break;
    case OP_CLEAR_COLOR: case OP_CLEAR_DEPTH: case OP_CLEAR_BUFFERS: kind = rprof::kOpClear; break;
    case OP_COPY_SURFACE: kind = rprof::kOpCopy; break;
    case OP_COPY_TO_SCAN: kind = rprof::kOpScan; break;
    case OP_INVALIDATE: kind = rprof::kOpInvalidate; break;
    case OP_PEEK_Z: kind = rprof::kOpCopy; break;
    case OP_FLUSH: kind = rprof::kOpFlush; break;
    case OP_DRAW_DONE: kind = rprof::kOpDrawDone; break;
    case OP_SWAP: kind = rprof::kOpSwap; break;
    default: kind = rprof::kOpOther; break;
    }
    if (ftrace::on && !ftrace::rcur().firstOp) ftrace::rcur().firstOp = ftrace::now();
    const uint64_t started = rprof::op_begin(kind);
    execute_op(op, p, n);
    rprof::op_end(kind, started);
    if (op == OP_SWAP) {
        rprof::frame_end(n >= 2 && p[1]);
        if (guest_copies_mode() == 2) g_render_cpu_ns.store(thread_cpu_ns(), std::memory_order_relaxed);
    }
}

static void execute_op(Op op, const uint32* p, uint32 n) {
    switch (op) {
    case OP_NOP: break;
    case OP_SET_REGS: apply_regs(p[0], p + 1, n - 1); break;
    case OP_DRAW: g_draws_executed.fetch_add(1, std::memory_order_relaxed); render::draw(g_regs, p[0], p[1], 0, 0, p[2], p[3]); break;
    case OP_DRAW_INDEXED:
        g_draws_executed.fetch_add(1, std::memory_order_relaxed);
        render::g_base_instance = n > 6 ? p[6] : 0;
        render::draw(g_regs, p[0], p[1], p[2], p[3], p[4], p[5]);
        render::g_base_instance = 0;
        break;
    case OP_CLEAR_COLOR: {
        const uint32* q = p + kColorBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        render::clear_color(g_regs, unpack_struct(p, kColorBufferWords, 0), rgba);
        break;
    }
    case OP_CLEAR_DEPTH: {
        const uint32* q = p + kDepthBufferWords;
        render::clear_depth_stencil(g_regs, unpack_struct(p, kDepthBufferWords, 0), bitsf(q[0]), q[1], q[2]);
        break;
    }
    case OP_CLEAR_BUFFERS: {
        uint32 cb = unpack_struct(p, kColorBufferWords, 0), db = unpack_struct(p + kColorBufferWords, kDepthBufferWords, 1);
        const uint32* q = p + kColorBufferWords + kDepthBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        render::clear_color(g_regs, cb, rgba);
        render::clear_depth_stencil(g_regs, db, bitsf(q[4]), q[5], q[6]);
        break;
    }
    case OP_COPY_SURFACE: {
        // debug: WWHD_GX2_DELAY_COPY=ms stalls the render thread before each surface copy (a slow
        // or busy render thread; reproduced the agl boot crash every time before GX2CopySurface waited)
        static const int delay = getenv("WWHD_GX2_DELAY_COPY") ? atoi(getenv("WWHD_GX2_DELAY_COPY")) : 0;
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        uint32 src = unpack_struct(p, kSurfaceWords, 0);
        const uint32* q = p + kSurfaceWords;
        uint32 dst = unpack_struct(q + 2, kSurfaceWords, 1);
        const uint32* r = q + 2 + kSurfaceWords;
        render::copy_surface(src, q[0], q[1], dst, r[0], r[1]);
        break;
    }
    case OP_COPY_TO_SCAN: render::copy_to_scan(unpack_struct(p, kColorBufferWords, 0), p[kColorBufferWords]); break;
    case OP_CALL: execute((const uint32*)mem::ptr(p[0]), p[1] / 4); break;
    case OP_SET_CONTEXT: set_context(p[0]); break;
    case OP_INVALIDATE: render::invalidate(p[0], p[1], p[2]); break;
    case OP_EXPAND_COLOR: case OP_EXPAND_DEPTH: break;  // MSAA/HiZ decompression: nothing to do on the host
    case OP_PEEK_Z: render::peek_z(p, n); break;
    case OP_FLUSH: render::guest_flush(); break;  // Vulkan: asynchronous submission
    case OP_DRAW_DONE:
        // The Vulkan renderer writes GPU results back to guest memory only for linear surfaces (guest data is copied
        // into fenced upload slices when work is recorded), so GX2DrawDone needs this op executed
        // (render_sync in the HLE), not an idle GPU. Lazy DrawDone (lazy_draw_done(), the default)
        // queues the work instead of waiting for the whole device every frame. A payload word of 1
        // (save states) always waits for the idle GPU.
        if (lazy_draw_done() && !(n && p[0])) render::guest_flush();
        else render::wait_idle();
        // except what the CPU reads back: linear render targets (the Picto Box picture, issue #53)
        render::write_back();
        break;
    case OP_SWAP:
        if (ftrace::on) {
            ftrace::rcur().encoded = ftrace::now();
            ftrace::rframe++;
            ftrace::Rec& next = ftrace::rcur();
            next.fenceA = next.fenceB = next.encoded = next.firstOp = 0;
            next.idleMs = next.idleAfterFenceMs = 0;
        }
        if (n) render::set_frame_aspect(gx2::bitsf(p[0]));  // aspect ratio from the next frame on (aspect.cpp)
        if (n >= 3 && p[2]) render::request_capture();
        render::swap();
        break;
    case OP_SET_PROJ_REGS: {
        uint32 v[16];
        memcpy(v, p + 1, sizeof v);
        float kx, ky;
        if (n == 17 && render::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky))
            for (int i = 0; i < 4; i++) {  // rows x and y: the 16:9 layout space centred in the wider picture
                v[i] = fbits(bitsf(v[i]) / kx);
                v[4 + i] = fbits(bitsf(v[4 + i]) / ky);
            }
        apply_regs(p[0], v, std::min<uint32>(n - 1, 16));
        break;
    }
    case OP_WAIT_MEM: {
        if (gx2::fps_pacing()) break;  // the game's 30 fps fence (BotW frame rate above 30)
        // the command processor waits until guest memory meets the condition (BotW: its vsync counter,
        // kept by the vsync callback, >= 2: the game's 30 fps cap; without it menus ran at 60 fps
        // and their animations at double speed). Work recorded so far is submitted first; 100 ms at
        // most, so a counter nobody updates can't hang the GPU.
        render::guest_flush();
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        auto met = [&] {
            const uint32 v = ld32(p[0]) & p[1], ref = p[3];
            switch (p[2] & 7) {
            case 0: return false;
            case 1: return v < ref;
            case 2: return v == ref;
            case 3: return v <= ref;
            case 4: return v > ref;
            case 5: return v != ref;
            case 6: return v >= ref;
            default: return true;
            }
        };
        const int64_t ft0 = ftrace::on ? ftrace::now() : 0;
        while (!met() && std::chrono::steady_clock::now() < give_up) std::this_thread::sleep_for(std::chrono::microseconds(250));
        if (ft0) {
            ftrace::rcur().fenceA = ft0;
            ftrace::rcur().fenceB = ftrace::now();
        }
        break;
    }
    case OP_QUERY_BEGIN:
        if (n >= 1) render::query_begin(p[0]);
        break;
    case OP_QUERY_END:
        if (n >= 2) render::query_end(p[0], p[1]);
        break;
    case OP_STREAMOUT_UPDATE: {  // the buffer's write offset (bytes), in command order like the GPU's
        uint32 reg = mmVGT_STRMOUT_BUFFER_OFFSET_0 + 4 * (p[0] & 3);
        if (p[1] == 0) g_regs[reg] = 0;
        else if (p[1] == 1) g_regs[reg] = p[2] ? ld32(p[2]) : 0;
        else if (p[1] == 3 && p[2]) st32(p[2], g_regs[reg]);
        break;
    }
    case OP_LAYOUT_CONTENT:
        aspect::set_content_clip(n && p[0]);
        break;
    case OP_LAYOUT_ROOT: {
        float kx, ky;
        aspect::layout_root_target(p[0], render::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky));
        break;
    }
    case OP_SETUP_CONTEXT:
        g_contexts[p[0]].assign(kNumRegs, 0);
        g_shadow = g_contexts[p[0]].data();
        break;
    case OP_FENCE: {
        uint64_t done = g_fence_done.load(std::memory_order_relaxed);
        while (done < p[0] && !g_fence_done.compare_exchange_weak(done, p[0], std::memory_order_release)) {}
        g_fence_done.notify_all();
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------- default state
static void set_default_state() {
    // GX2SetShaderModeEx(UNIFORM_REGISTER, ...)
    LATTE_SQ_CONFIG sq;
    sq.set_DX9_CONSTS(true).set_ALU_INST_PREFER_VECTOR(true).set_PS_PRIO(3).set_VS_PRIO(2).set_GS_PRIO(1).set_ES_PRIO(0);
    set_reg(REGADDR::SQ_CONFIG, sq.getRawValue());
    set_reg(REGADDR::VGT_GS_MODE, 0);
    LATTE_PA_CL_VTE_CNTL vte{};
    vte.set_VPORT_X_OFFSET_ENA(true).set_VPORT_X_SCALE_ENA(true).set_VPORT_Y_OFFSET_ENA(true).set_VPORT_Y_SCALE_ENA(true);
    vte.set_VPORT_Z_OFFSET_ENA(true).set_VPORT_Z_SCALE_ENA(true).set_VTX_W0_FMT(true);
    set_reg(REGADDR::PA_CL_VTE_CNTL, vte.getRawValue());
    set_reg(REGADDR::DB_DEPTH_CONTROL, (1 << 1) | (1 << 2) | (1 << 4));  // z test + write, LESS
    LATTE_SX_ALPHA_TEST_CONTROL at;
    at.set_ALPHA_FUNC(LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC::LESS).set_ALPHA_TEST_ENABLE(false);
    set_reg(REGADDR::SX_ALPHA_TEST_CONTROL, at.getRawValue());
    set_reg(REGADDR::SX_ALPHA_REF, 0);
    LATTE_PA_SU_SC_MODE_CNTL pm{};
    pm.set_FRONT_FACE(LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW);
    pm.set_FRONT_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES).set_BACK_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES);
    set_reg(REGADDR::PA_SU_SC_MODE_CNTL, pm.getRawValue());
    set_reg(REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFFFFFF);
    set_reg(REGADDR::CB_TARGET_MASK, 0xFFFFFFFF);
    for (int i = 0; i < 4; i++) set_reg(REGADDR::CB_BLEND_RED + i, 0);
    set_reg(REGADDR::PA_SU_POINT_SIZE, LATTE_PA_SU_POINT_SIZE().set_WIDTH(8).set_HEIGHT(8).getRawValue());
    LATTE_CB_COLOR_CONTROL cc;
    cc.set_SPECIAL_OP(LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL).set_ROP(LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY);
    set_reg(REGADDR::CB_COLOR_CONTROL, cc.getRawValue());
    LATTE_PA_CL_CLIP_CNTL clip{};
    clip.set_DX_LINEAR_ATTR_CLIP_ENA(true);
    set_reg(REGADDR::PA_CL_CLIP_CNTL, clip.getRawValue());
    set_reg(mmDB_DEPTH_CLEAR, fbits(1.0f));
}

}  // namespace gx2

using namespace gx2;

// ---------------------------------------------------------------- init / timing
// Display timing, modelled on the hardware: vsync ticks at 60 Hz on its own clock, and a requested
// flip executes on the first vsync that is at least `swap interval` vsyncs after the previous flip.
// Games pace themselves by waiting for vsync until their flips have executed.
// 120/240 fps frame interpolation needs 4/8 flips per logic step: the virtual vsync then ticks 2/4
// times per 59.94 Hz vsync (interp::vsync_rate()). The clock counts quarter vsyncs ("ticks"); a
// vsync at the current rate is a granule of 4/rate ticks, and every vsync count below is in ticks,
// rounded to granules, so at 30/60 fps (granule 4) the timing is the 59.94 Hz one exactly.
static uint64_t g_swap_count = 0, g_flip_count = 0;
namespace gx2 { uint64_t flips_presented() { return __atomic_load_n(&g_flip_count, __ATOMIC_RELAXED); } }  // live fps in the title
namespace gx2 { uint64_t game_frames() { return __atomic_load_n(&g_swap_count, __ATOMIC_RELAXED); } }  // frames the game submitted
static uint32 g_swap_interval = 1;  // as set by the game (frame interpolation halves it)
namespace interp { uint32_t effective_swap_interval(uint32_t game); uint64_t logic_steps(); int vsync_rate(); }
static std::mutex g_flip_mutex;
static const auto g_vsync_epoch = std::chrono::steady_clock::now();
static constexpr std::chrono::nanoseconds kVsyncPeriod(16683333);  // 59.94 Hz
static constexpr uint64_t kTicksPerVsync = 4;
// a flip also waits for the GPU to finish that frame, as on hardware: the game reuses a frame's
// buffers once its flip has executed
struct PendingFlip { uint64_t swap; std::chrono::steady_clock::time_point at; };  // (at: when it was swapped)
static std::deque<PendingFlip> g_pending_flips;
static uint64_t g_last_flip_vsync = 0;  // tick (a granule boundary)
static uint64_t g_last_flip_time = 0;  // timebase
static int64_t g_count_offset = 0;     // guest-visible swap/flip counts minus ours (set by a loaded save state)

// The clock is advanced (g_vsync_shift) when a late flip moves the grid (vsync_pacing.h, upstream e8207ca).
static constexpr vsync_pacing::Clock kVsyncClock{kVsyncPeriod.count(), kTicksPerVsync};
static std::atomic<int64_t> g_vsync_shift{0};  // ns; written with g_flip_mutex held
static int64_t vsync_clock_now() {  // ns since the epoch on the vsync's clock (the game's clock when it runs faster)
    if (!game_clock::shifted()) return (std::chrono::steady_clock::now() - g_vsync_epoch).count();
    const auto epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(g_vsync_epoch.time_since_epoch()).count();
    return game_clock::sample().guest - epoch;
}
static uint64_t vsync_index() {  // in ticks
    return kVsyncClock.index(vsync_clock_now(), g_vsync_shift.load(std::memory_order_relaxed));
}
static uint64_t vsync_granule() {  // ticks per vsync at the current rate: 4 (30/60 fps), 2 (120), 1 (240)
    const int rate = interp::vsync_rate();
    return rate >= 4 ? 1 : rate >= 2 ? 2 : kTicksPerVsync;
}
static std::chrono::steady_clock::time_point tick_time(uint64_t tick) {
    const auto at = std::chrono::nanoseconds(kVsyncClock.time(tick, g_vsync_shift.load(std::memory_order_relaxed)));
    if (!game_clock::shifted())
        return g_vsync_epoch + at;
    const auto sample = game_clock::sample();
    const auto target = g_vsync_epoch + at;
    const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(target.time_since_epoch()).count() - sample.guest;
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(sample.host + std::max<int64_t>(0, (left + sample.rate - 1) / sample.rate)));
}
// the first tick at which this flip may execute: `swap interval` vsyncs after the previous flip.
// (On hardware also the vsync after its swap, which is the same for a swap made before that tick; a
// later swap, a frame that missed its vsync, flips as soon as it is ready: vsync_pacing.h.)
static uint64_t flip_due(const PendingFlip&) {
    const uint64_t g = vsync_granule();
    const uint32_t interval = gx2::fps_pacing() ? 1 : interp::effective_swap_interval(g_swap_interval);
    return g_last_flip_vsync + interval * g;
}
// Uncapped (gx2.h): debug, to see how fast the renderer can go; GPU completion ordering is kept
static std::atomic<int> g_uncapped{-1};  // -1: not read from the environment yet
bool gx2::uncapped() {
    int v = g_uncapped.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* any = getenv("WWHD_UNCAPPED");
        const char* vk = getenv("WWHD_VK_UNCAPPED");
        v = (any && !strcmp(any, "1")) || (vk && !strcmp(vk, "1") && render::vulkan()) ? 1 : 0;
        int expected = -1;
        g_uncapped.compare_exchange_strong(expected, v);
        v = g_uncapped.load(std::memory_order_relaxed);
    }
    return v == 1;
}
void gx2::set_uncapped(bool on) {
    if (uncapped() == on) return;
    g_uncapped = on ? 1 : 0;
    LOG("[gx2] uncapped %s", on ? "on (debug: no frame limit, no vsync; the game runs faster than real time)" : "off");
}
// above 30 fps flips don't wait for the 60 Hz virtual vsync (a frame that misses one would wait for the
// next: 30 fps); fps_limit() paces instead
static bool uncapped_benchmark() { return gx2::uncapped() || gx2::fps_pacing(); }

// ---------------------------------------------------------------- BotW frame rate
// The game advances one 30 fps step per frame (frame-locked: faster than real time above 30 fps,
// slow motion below). Its engine already scales most per-frame work by ksys::VFR's delta, which is
// the speed multiplier times an "interval ratio" (present interval / base interval; on Wii U always
// 1). Above 30 fps the ratio follows the measured frame time (30 fps = 1, 60 fps = 0.5), averaged
// over a few frames and clamped (a long hitch must not make one giant step).
namespace {
std::atomic<int> g_fps_target{-1};
std::atomic<bool> g_fps_gameplay{false};  // above 30 fps, and in gameplay (not a loading screen or the title)
constexpr uint32_t kVfrVtable = 0x10319D4Cu;  // v208: ksys::VFR
// VFR (Wii U layout, from its vtable pointer): +0x0C pending interval ratio; +0x18/+0x1C interval ratio A/B; +0x44 two storages of 4x3 floats (raw delta
// frames, delta frames, raw delta times, delta times per core); +0xD4 base interval (2), +0xD8 frame
// rate (30), +0xDC frame time (1/30)
bool vfr_valid(uint32_t a) {
    return a && ld32(a) == kVfrVtable && ld32(a + 0xD4) == 2 && ld32(a + 0xD8) == 30 && ld32(a + 0xDC) == 0x3D088889u;
}
uint32_t vfr() {  // the VFR singleton's instance pointer (v208: 1047C258); the layout above is from its vtable pointer
    const uint32_t p = ld32(0x1047C258u);
    return p && vfr_valid(p + 0x10) ? p + 0x10 : 0;
}
// UI animation speed: the game sets it each frame from the VFR interval (an integer, 2) times this
// constant, and its layouts read it too; scaled by the frame ratio so menus keep their speed
constexpr uint32_t kUiSpeedConst = 0x102C1DD4u;
// gyro aiming (bow, runes): the controller's turn rate (per second) is divided by this 30.0 to give the
// turn per frame; it is that code's only reader. Scaled with the frame time, or gyro aim turns 2x at 60 fps
constexpr uint32_t kGyroPerFrameConst = 0x101B2D34u;
void set_f32(uint32_t a, float v) { uint32_t u; memcpy(&u, &v, 4); st32(a, u); }
float get_f32(uint32_t a) { uint32_t u = ld32(a); float v; memcpy(&v, &u, 4); return v; }
// game thread, once per frame (GX2SwapScanBuffers)
void fps_frame() {
    static auto last = std::chrono::steady_clock::now();
    static double hist[4] = {1 / 30.0, 1 / 30.0, 1 / 30.0, 1 / 30.0};
    static int hi = 0;
    static bool wasActive = false;
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    const bool active = gx2::fps_target() != 30;
    const uint32_t a = vfr();
    static uint32_t uiSpeed = 0, gyroRate = 0;  // the constants' original bits
    if (!uiSpeed) uiSpeed = ld32(kUiSpeedConst);
    if (!gyroRate) gyroRate = ld32(kGyroPerFrameConst);
    float gyroOrig;
    memcpy(&gyroOrig, &gyroRate, 4);
    float uiOrig;
    memcpy(&uiOrig, &uiSpeed, 4);
    if (!active) {
        if (wasActive) {  // back to the game's own fixed step
            if (a) {
                set_f32(a + 0x0C, 1.0f);
                set_f32(a + 0x18, 1.0f);
                set_f32(a + 0x1C, 1.0f);
            }
            st32(kUiSpeedConst, uiSpeed);
            st32(kGyroPerFrameConst, gyroRate);
        }
        wasActive = false;
        g_fps_gameplay = false;
        return;
    }
    wasActive = true;
    hist[hi++ & 3] = std::clamp(dt, 1 / 1000.0, 0.25);
    if (!a) { g_fps_gameplay = false; return; }
    // WWHD_FPS_LOAD_GUARD=1: loading screens and the title (~120 draws a frame, gameplay thousands) keep
    // the game's own step and 30 fps pacing, scaling resumes after a second of gameplay-sized frames.
    // Off by default: a load that never finished at 60 fps was the limiter holding a guest core while it
    // slept (starving the loader threads), not the fractional step; menus would drop to 30 fps with it.
    static uint64_t lastDraws = 0;
    static int busyFrames = 0;
    const uint64_t draws = g_draws_executed.load(std::memory_order_relaxed);
    const uint64_t frameDraws = draws - lastDraws;
    lastDraws = draws;
    static const bool loadGuard = getenv("WWHD_FPS_LOAD_GUARD") && atoi(getenv("WWHD_FPS_LOAD_GUARD"));  // off: not needed once the limiter released its core
    busyFrames = frameDraws < 300 && loadGuard ? 0 : std::min(busyFrames + 1, 1000);
    const bool gameplay = busyFrames >= 60;
    g_fps_gameplay = gameplay;
    const double avg = (hist[0] + hist[1] + hist[2] + hist[3]) / 4;
    static int dbg = 0;
    static const bool fpsDebug = getenv("WWHD_FPS_DEBUG") != nullptr;
    if (fpsDebug && (++dbg % 120) == 0)
        LOG("[fps] engine at frame end: pending %.3f live %.3f delta frames %.3f; %llu draws, gameplay %d", get_f32(a + 0x0C),
            get_f32(a + 0x18), get_f32(a + 0x50), (unsigned long long)frameDraws, gameplay);
    static const double maxRatio = getenv("WWHD_FPS_MAX_STEP") ? atof(getenv("WWHD_FPS_MAX_STEP")) : 1.5;  // 20 fps
    const float ratio = gameplay ? (float)std::clamp(avg * 30.0, 0.125, maxRatio) : 1.0f;
    // the engine copies its pending ratio (+0x0C) into the live one at the start of every frame
    // (updateInterval); setting only the live one was undone before anything used it
    set_f32(a + 0x0C, ratio);
    set_f32(kUiSpeedConst, uiOrig * ratio);
    set_f32(kGyroPerFrameConst, gyroOrig / ratio);
    set_f32(a + 0x18, ratio);
    set_f32(a + 0x1C, ratio);
    const float frameTime = get_f32(a + 0xDC);
    for (uint32_t st = a + 0x44; st < a + 0x44 + 2 * 0x30; st += 0x30)
        for (uint32_t core = 0; core < 3; core++) {
            const float df = get_f32(st + core * 4) * ratio;
            set_f32(st + 0x0C + core * 4, df);
            set_f32(st + 0x24 + core * 4, df * frameTime);
        }
}
// the frame limit on the game thread: 60, or with no cap WWHD_FPS_LIMIT (default 0 = none)
void fps_limit() {
    static const int envLimit = getenv("WWHD_FPS_LIMIT") ? atoi(getenv("WWHD_FPS_LIMIT")) : 0;
    const int target = gx2::fps_target();
    const int limit = !gx2::fps_pacing() ? 0 : target == 60 ? 60 : envLimit;
    if (limit <= 0) return;
    static auto next = std::chrono::steady_clock::now();
    const auto period = std::chrono::nanoseconds(1000000000 / limit);
    next += period;
    const auto now = std::chrono::steady_clock::now();
    if (next < now - period) {
        next = now;
    } else {
        BlockingScope b;  // the other guest threads keep this core meanwhile
        std::this_thread::sleep_until(next);
    }
}
}  // namespace
int gx2::fps_target() {
    int v = g_fps_target.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = getenv("WWHD_FPS");
        v = e ? atoi(e) : 30;
        if (v != 0 && v != 60) v = 30;
        int expected = -1;
        g_fps_target.compare_exchange_strong(expected, v);
        v = g_fps_target.load();
        if (v != 30) LOG("[fps] frame rate %s", v ? "60" : "uncapped");
    }
    return v;
}
void gx2::set_fps_target(int fps) {
    if (fps != 0 && fps != 60) fps = 30;
    fps_target();
    if (g_fps_target.exchange(fps) != fps) LOG("[fps] frame rate %s", fps == 30 ? "30 (the game's own)" : fps ? "60" : "uncapped");
}
bool gx2::present_unsynced() { return gx2::uncapped() || (gx2::fps_target() == 0 && fps_pacing()); }
bool gx2::fps_pacing() { return gx2::fps_target() != 30 && g_fps_gameplay.load(std::memory_order_relaxed); }

static void update_flips() {  // g_flip_mutex held
    uint64_t now = vsync_index();
    while (!g_pending_flips.empty()) {
        const uint64_t due = flip_due(g_pending_flips.front());
        if ((!uncapped_benchmark() && due > now) || render::frames_completed() < g_pending_flips.front().swap) break;
        uint64_t at = now / vsync_granule() * vsync_granule();
        if (!uncapped_benchmark()) {  // on its vsync, or late: then the grid may move (vsync_pacing.h)
            int64_t shift = g_vsync_shift.load(std::memory_order_relaxed);
            at = vsync_pacing::flip_executed(kVsyncClock, shift, due, vsync_clock_now(), vsync_granule());
            g_vsync_shift.store(shift, std::memory_order_relaxed);
            now = std::max(now, at);
        }
        const uint64_t flipped = g_pending_flips.front().swap;
        g_pending_flips.pop_front();
        g_last_flip_vsync = at;
        g_last_flip_time = timebase::simulation_now();
        g_flip_count++;
        if (ftrace::on) {
            static int64_t lastFlip = 0;
            const int64_t t = ftrace::now();
            const ftrace::Rec& r = ftrace::ring[flipped & 255];
            const double since = lastFlip ? (t - lastFlip) / 1e6 : 0;
            auto rel = [&](int64_t x) { return x && lastFlip ? (x - lastFlip) / 1e6 : -1.0; };
            if (lastFlip && (since > 40 || flipped % 60 == 0))
                LOG("[frame] %llu: flip after %.1f ms | game swap %.1f, DrawDone waits %.1f | fence %.1f..%.1f, render idle %.1f "
                    "(%.1f after fence), first command %.1f, encoded %.1f, GPU start %.1f, GPU done %.1f",
                    (unsigned long long)flipped, since, rel(r.guestSwap), r.drawDoneMs, rel(r.fenceA), rel(r.fenceB), r.idleMs,
                    r.idleAfterFenceMs, rel(r.firstOp), rel(r.encoded), rel(wwhd_frame_gpu_start_ns(flipped)), rel(wwhd_frame_done_ns(flipped)));
            ftrace::ring[(flipped + 128) & 255].drawDoneMs = 0;  // cleared well before frame flipped+128 starts
            lastFlip = t;
        }
    }
}
// A flip whose vsync has come but whose frame is not ready yet (the game swapped late, or the
// renderer has not finished it): GX2WaitForVsync waits for that frame in short steps instead of
// sleeping to the next vsync, and returns when the flip has executed (vsync_pacing.h).
static bool late_flip_pending() {  // g_flip_mutex held
    if (g_pending_flips.empty()) return false;
    const auto& front = g_pending_flips.front();
    return flip_due(front) <= vsync_index() && render::frames_completed() < front.swap;
}
static bool wait_for_late_flip(std::chrono::steady_clock::time_point deadline) {
    uint64_t flips;
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        flips = g_flip_count;
        update_flips();
        if (g_flip_count != flips) return true;
        if (!late_flip_pending()) return false;
    }
    constexpr auto kStep = std::chrono::microseconds(250);
    for (auto now = std::chrono::steady_clock::now(); now < deadline; now = std::chrono::steady_clock::now()) {
        threads::park_sleep_until(std::min(deadline, now + kStep));
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        if (g_flip_count != flips) return true;
        if (!late_flip_pending()) return false;
    }
    return false;
}
#ifdef WWHD_HAS_VULKAN
static void ready_flip_before_resume() {
    bool needsSync;
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        if(g_pending_flips.empty()) return;
        const auto& front = g_pending_flips.front();
        const uint64_t at = flip_due(front);
        if(at > vsync_index()) return;
        needsSync = render::frames_completed() < front.swap;
    }
    if(needsSync) render_sync(kSyncFlip); // Core already released; no flip lock held.
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
}
#endif

HLE(gx2, GX2Init) {
    set_default_state();
    LOG("[gx2] initialized (native GX2 -> %s)", render::api_name(render::active()));
    if(uncapped_benchmark())
        LOG("[gx2 benchmark] uncapped guest flips; GPU completion ordering retained; frame-based simulation accelerates while timebase/audio clocks remain real-time");
}

HLE(gx2, GX2SetupContextStateEx) {
    uint32 ctx = arg(c, 0);
    emit_host(OP_SETUP_CONTEXT, {ctx});
    set_default_state();
    // the context's "restore" display list lives inside the (0xA100 byte) context structure
    uint32 dl = ctx + 0x9800;
    uint32* w = (uint32*)mem::ptr(dl);
    w[0] = OP_SET_CONTEXT | (1u << 8);
    w[1] = ctx;
}
HLE(gx2, GX2SetContextState) { emit(OP_SET_CONTEXT, {arg(c, 0)}); }
HLE(gx2, GX2GetContextStateDisplayList) {
    if (arg(c, 1)) st32(arg(c, 1), arg(c, 0) + 0x9800);
    if (arg(c, 2)) st32(arg(c, 2), 8);
}

// ---------------------------------------------------------------- display lists
HLE(gx2, GX2BeginDisplayListEx) {
    t_rec.start = t_rec.pos = arg(c, 0);
    t_rec.end = arg(c, 0) + arg(c, 1);
    t_rec_blocks.clear();
    t_rec_calls.clear();
}
HLE(gx2, GX2EndDisplayList) {
    uint32 size = t_rec.pos - t_rec.start;
    if (submitted_uniforms_on() && t_rec.start) g_dl_index.publish(t_rec.start, size, t_rec_blocks, t_rec_calls);
    t_rec_blocks.clear();
    t_rec_calls.clear();
    t_rec = Recording{};
    ret(c, size);
}
HLE(gx2, GX2GetCurrentDisplayList) {
    if (arg(c, 0)) st32(arg(c, 0), t_rec.start);
    if (arg(c, 1)) st32(arg(c, 1), t_rec.start ? t_rec.end - t_rec.start : 0);
    ret(c, t_rec.start != 0);
}
// A list called while another is recorded runs with that one (its blocks are noted for it); a call
// from the command stream takes the blocks of the list as they are now with it (submitted_uniforms.h).
static void call_display_list(uint32 list, uint32 size) {
    if (t_rec.start) {
        if (submitted_uniforms_on()) t_rec_calls.push_back(list);
        emit(OP_CALL, {list, size});
        return;
    }
    submitted::Snapshot* s = submit_snapshot(list, size);
    if (s) emit_host(OP_SUBMITTED_UNIFORMS, {uint32(uintptr_t(s)), uint32(uint64_t(uintptr_t(s)) >> 32)});
    emit(OP_CALL, {list, size});
    if (s) emit_host(OP_SUBMITTED_UNIFORMS, {});
}
HLE(gx2, GX2CallDisplayList) { call_display_list(arg(c, 0), arg(c, 1)); }
// a copy into the command stream: run where it stands (our display lists are op lists, not PM4)
HLE(gx2, GX2CopyDisplayList) { call_display_list(arg(c, 0), arg(c, 1)); }
HLE(gx2, GX2SetDefaultState) { set_default_state(); }
HLE(gx2, GX2DirectCallDisplayList) { call_display_list(arg(c, 0), arg(c, 1)); }

// ---------------------------------------------------------------- draws
HLE(gx2, GX2DrawEx) { emit(OP_DRAW, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)}); }
HLE(gx2, GX2DrawIndexedEx) { emit(OP_DRAW_INDEXED, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)}); }
// Stream out (transform feedback, BotW). Registers as Cemu's GX2_Streamout.cpp; the strides come
// from GX2SetVertexShader. GX2StreamOutBuffer: +0 size, +4 dataPtr, +8 vertexStride, +0xC GX2RBuffer
// (+0x10 elementSize, +0x14 elementCount, +0x18 ptr; used when dataPtr is null), +0x1C ctxPtr.
HLE(gx2, GX2SetStreamOutEnable) { set_reg(mmVGT_STRMOUT_EN, arg(c, 0) & 1); }
HLE(gx2, GX2SetStreamOutBuffer) {
    uint32 i = arg(c, 0), b = arg(c, 1);
    if (i > 3 || !b) return;
    uint32 addr = ld32(b + 4), size = ld32(b);
    if (!addr) { addr = ld32(b + 0x18); size = ld32(b + 0x10) * ld32(b + 0x14); }
    set_reg(mmVGT_STRMOUT_BUFFER_SIZE_0 + i * 4, size >> 2);
    set_reg(mmVGT_STRMOUT_BUFFER_BASE_0 + i * 4, addr >> 8);
}
// (index, buffer, reset): reset != 0 starts writing at 0, otherwise continues from the saved context
HLE(gx2, GX2SetStreamOutContext) {
    uint32 i = arg(c, 0), b = arg(c, 1);
    if (i > 3) return;
    emit(OP_STREAMOUT_UPDATE, {i, arg(c, 2) ? 0u : 1u, b ? ld32(b + 0x1C) : 0u});
}
HLE(gx2, GX2SaveStreamOutContext) {
    uint32 i = arg(c, 0), b = arg(c, 1);
    if (i > 3 || !b) return;
    emit(OP_STREAMOUT_UPDATE, {i, 3u, ld32(b + 0x1C)});
}

// Occlusion queries. BotW reads one result on the CPU (GX2QueryGetOcclusionResult: the sun's lens
// flare, dimmed by how much of the sun is visible) and uses the rest for conditional rendering.
// The renderer counts the samples that pass between begin and end (Metal visibility results) and
// reports them once the GPU finished; until then the query is "not ready" and the game keeps its
// previous value, as on hardware. A query whose draws were skipped (shader still compiling) keeps
// the last count. Conditional rendering always draws (Metal has no predication; nothing is culled
// wrongly, some hidden things are drawn). WWHD_NO_QUERIES=1: every query "fully visible" (before).
static std::atomic<uint64_t> g_queries{0}, g_cond_renders{0};
extern "C" uint64_t wwhd_query_counts(uint64_t* conditional) {
    *conditional = g_cond_renders.exchange(0);
    return g_queries.exchange(0);
}
namespace {
struct QueryState {
    uint32 issued = 0, done = 0;  // generations: begun by the game, reported by the renderer
    uint64_t samples = 0x100000;  // until a real result: fully visible
    uint64_t issuedFrame = 0;     // a result that never comes (recorded in a display list never run):
};                                // ready with the last count after a second
std::mutex g_query_mu;
std::unordered_map<uint32, QueryState> g_query_state;
bool queries_off() {
    static const bool off = getenv("WWHD_NO_QUERIES") && atoi(getenv("WWHD_NO_QUERIES"));
    return off;
}
}  // namespace
extern "C" void wwhd_query_result(uint32_t addr, uint32_t gen, uint64_t samples, bool ok) {
    std::lock_guard<std::mutex> l(g_query_mu);
    QueryState& q = g_query_state[addr];
    if ((int32_t)(gen - q.done) <= 0) return;
    static const bool log = getenv("WWHD_QUERY_LOG") && atoi(getenv("WWHD_QUERY_LOG"));
    if (log) LOG("[query] %08X gen %u: %llu samples%s", addr, gen, (unsigned long long)samples, ok ? "" : " (lost, kept)");
    q.done = gen;
    if (ok) q.samples = samples;
}
HLE(gx2, GX2QueryBegin) {  // (type, GX2Query*)
    memset(mem::ptr(arg(c, 1)), 0, 0x40);
    g_queries++;
    if (queries_off()) return;
    emit(OP_QUERY_BEGIN, {arg(c, 1)});
}
HLE(gx2, GX2QueryEnd) {  // (type, GX2Query*)
    if (queries_off()) return;
    uint32 gen;
    {
        std::lock_guard<std::mutex> l(g_query_mu);
        QueryState& q = g_query_state[arg(c, 1)];
        gen = ++q.issued;
        q.issuedFrame = render::frame_count();
    }
    emit(OP_QUERY_END, {arg(c, 1), gen});
}
HLE(gx2, GX2QueryGetOcclusionResult) {  // (GX2Query*, u64* result) -> ready
    uint64_t samples = 0x100000;
    bool ready = true;
    if (!queries_off()) {
        std::lock_guard<std::mutex> l(g_query_mu);
        auto it = g_query_state.find(arg(c, 0));
        if (it != g_query_state.end()) {
            ready = it->second.done == it->second.issued || render::frame_count() > it->second.issuedFrame + 60;
            samples = it->second.samples;
        }
    }
    static const bool log = getenv("WWHD_QUERY_LOG") && atoi(getenv("WWHD_QUERY_LOG"));
    if (log) LOG("[query] read %08X: %s %llu", arg(c, 0), ready ? "ready" : "not ready", (unsigned long long)samples);
    if (!ready) { ret(c, 0); return; }
    if (arg(c, 1)) { st32(arg(c, 1), (uint32)(samples >> 32)); st32(arg(c, 1) + 4, (uint32)samples); }
    ret(c, 1);
}
HLE(gx2, GX2QueryBeginConditionalRender) { g_cond_renders++; }
HLE(gx2, GX2QueryEndConditionalRender) {}

HLE(gx2, GX2DrawIndexedEx2) {  // (prim, count, indexType, indices, baseVertex, instances, baseInstance)
    emit(OP_DRAW_INDEXED, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6)});
}

// ---------------------------------------------------------------- clears and copies
static float farg(Cpu* c, int i) { return (float)c->f[1 + i].ps0; }
// Struct parameters are copied into the command (as GX2 encodes them into the command buffer):
// games reuse one GX2ColorBuffer/GX2DepthBuffer and change its view between calls, also while
// recording display lists that run later.
static void put_struct(std::vector<uint32>& p, uint32 addr, uint32 words) {
    size_t at = p.size();
    p.resize(at + words);
    if (addr) memcpy(&p[at], mem::ptr(addr), words * 4);
}
HLE(gx2, GX2ClearColor) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    for (int i = 0; i < 4; i++) p.push_back(fbits(farg(c, i)));
    emit(OP_CLEAR_COLOR, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearDepthStencilEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), arg(c, 1) & 0xFF, arg(c, 2)});
    emit(OP_CLEAR_DEPTH, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearBuffersEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    put_struct(p, arg(c, 1), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), fbits(farg(c, 1)), fbits(farg(c, 2)), fbits(farg(c, 3)), fbits(farg(c, 4)),
                       arg(c, 2) & 0xFF, arg(c, 3)});
    emit(OP_CLEAR_BUFFERS, p.data(), (uint32)p.size());
}
HLE(gx2, GX2SetClearDepthStencil) {
    auto* db = (GX2::GX2DepthBuffer*)mem::ptr(arg(c, 0));
    db->clearDepth = farg(c, 0);
    db->clearStencil = arg(c, 1) & 0xFF;
}
HLE(gx2, GX2CopySurface) {
    // debug: WWHD_COPYDBG=1 logs each copy as issued (thread, caller, source and destination images)
    static const bool dbg = getenv("WWHD_COPYDBG") != nullptr;
    if (dbg)
        LOG("[copydbg] issue t=%.3f thread %08X lr %08X src %08X img %08X dst %08X img %08X size %X", timebase::now() / (double)timebase::kTicksPerSec,
            threads::current_thread(), c->lr, arg(c, 0), ld32(arg(c, 0) + 0x24), arg(c, 3), ld32(arg(c, 3) + 0x24), ld32(arg(c, 3) + 0x20));
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kSurfaceWords);
    p.insert(p.end(), {arg(c, 1), arg(c, 2)});
    put_struct(p, arg(c, 3), kSurfaceWords);
    p.insert(p.end(), {arg(c, 4), arg(c, 5)});
    emit(OP_COPY_SURFACE, p.data(), (uint32)p.size());
    // The copy is complete when GX2CopySurface returns: the game uses the result (and frees the
    // surfaces) right away. agl's tile-mode conversion (027B5EEC) copies into a temporary surface,
    // OSBlockMoves it back and frees it at once; executed later on the render thread, the copy
    // wrote into the freed memory after the heap had reused it (boot crash: agl shader program
    // array 21EFE28C, program 0's +0x7c zeroed). Not for display lists (they run when called).
    if (!t_rec.start) {
        BlockingScope b;
        render_sync(kSyncCopySurface);
    }
}
HLE(gx2, GX2CopyColorBufferToScanBuffer) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    p.push_back(arg(c, 1));
    emit(OP_COPY_TO_SCAN, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ExpandAAColorBuffer) { emit(OP_EXPAND_COLOR, {arg(c, 0)}); }
HLE(gx2, GX2ExpandDepthBuffer) { emit(OP_EXPAND_DEPTH, {arg(c, 0)}); }
HLE(gx2, GX2Invalidate) { emit(OP_INVALIDATE, {arg(c, 0), arg(c, 1), arg(c, 2)}); }

// ---------------------------------------------------------------- submission and presentation
HLE(gx2, GX2Flush) { emit_host(OP_FLUSH, {}); }
HLE(gx2, GX2DrawDone) {
    BlockingScope b;
    emit_host(OP_DRAW_DONE, {});
    const int64_t ft0 = ftrace::on ? ftrace::now() : 0;
    render_sync(kSyncDrawDone);
    if (ft0) ftrace::ring[(g_swap_count + 1) & 255].drawDoneMs += (ftrace::now() - ft0) / 1e6;
    ret(c, 1);
}
// Freeze watchdog: when the game presents no frame for 1.5 s (outside the first minute's loading),
// every guest thread's state is logged once per freeze ([freeze]), so what blocked can be found.
static std::atomic<int64_t> g_last_swap_ns{0};
static void freeze_watchdog() {
    bool reported = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t gap = now - g_last_swap_ns.load(std::memory_order_relaxed);
        if (gap < 1500000000ll) { reported = false; continue; }
        if (reported) continue;
        reported = true;
        char why[96];
        snprintf(why, sizeof why, "no frame for %.1f s (after swap %llu)", gap / 1e9, (unsigned long long)g_swap_count);
        threads::dump_threads(why);
    }
}
HLE(gx2, GX2SwapScanBuffers) {
    {
        g_last_swap_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        static const bool watch = !getenv("WWHD_NO_FREEZE_WATCH");
        static std::once_flag started;
        if (watch && g_swap_count > 3600) std::call_once(started, [] { std::thread(freeze_watchdog).detach(); });
    }
    perf::game_frame();  // the overlay's Game CPU: this thread's CPU time per submitted frame
    guest_copies_ab();
    // save states are taken and loaded here: the thread that submits frames calls this once a frame from
    // the same place (Wind Waker's per-frame hook, savestate.cpp's anchor, never runs in BotW)
    ss::service(c);
    fps_frame();
    fps_limit();
    // debug: WWHD_TRACE_SWAP=n logs the guest call chain of the first n swaps
    static int trace = getenv("WWHD_TRACE_SWAP") ? atoi(getenv("WWHD_TRACE_SWAP")) : 0;
    if (trace > 0) {
        trace--;
        char buf[256];
        int n = snprintf(buf, sizeof buf, "[gx2] swap from lr=%08X", c->lr);
        uint32_t sp = c->r[1];
        for (int i = 0; i < 8 && sp; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp) break;
            n += snprintf(buf + n, sizeof buf - n, " <- %08X", ld32(prev + 4));
            sp = prev;
        }
        LOG("%s", buf);
    }
    float a = aspect::on_swap();  // aspect ratio of the next frame (game projections, render targets)
    uint32 ab;
    memcpy(&ab, &a, 4);
    // the frame drew no new logic step (an interpolation hold pass): for the render-thread profiler
    static uint64_t lastSteps = ~0ull;
    const uint64_t steps = interp::logic_steps();
    const bool hold = steps == lastSteps;
    lastSteps = steps;
    // Opt-in correctness capture: select the guest frame before it enters
    // the asynchronous render queue. Renderer-frame parity is not a game clock.
    static const char* captureCounter = getenv("WWHD_TEST_CAPTURE_LOAD_COUNTER");
    static const char* captureStep = getenv("WWHD_TEST_CAPTURE_LOAD_STEP");
    bool capture = false;
    if ((captureCounter || captureStep) && ss::last_load_frame()) {
        static uint64_t capturedLoad = 0;
        const auto loaded = ss::last_load_frame();
        const uint32_t counter = ld32(GD(0x101FF560));
        const uint64_t offset = captureStep ? steps - ss::last_load_step()
                                            : uint32_t(counter - ss::last_load_counter());
        const uint64_t target = strtoull(captureStep ? captureStep : captureCounter, nullptr, 10);
        if (capturedLoad != loaded && !hold && offset == target) {
            capturedLoad = loaded;
            capture = true;
            LOG("[test] capture at loaded %s +%llu (game counter %08X), full pass",
                captureStep ? "logic step" : "game counter",(unsigned long long)offset,counter);
        }
    }
    if (capture) emit_host(OP_SWAP, {ab, hold ? 1u : 0u, 1u});
    else emit_host(OP_SWAP, {ab, hold ? 1u : 0u});
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        g_swap_count++;
        g_pending_flips.push_back({g_swap_count, std::chrono::steady_clock::now()});
        if (ftrace::on) {
            ftrace::Rec& r = ftrace::ring[g_swap_count & 255];
            r.guestSwap = ftrace::now();
        }
    }
    // debug: WWHD_LOG_SLOW_SWAP=ms logs swaps that came more than ms after the previous one
    static const double slow_ms = getenv("WWHD_LOG_SLOW_SWAP") ? atof(getenv("WWHD_LOG_SLOW_SWAP")) : 0;
    if (slow_ms > 0) {
        static auto prev = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - prev).count();
        prev = now;
        if (ms > slow_ms) LOG("[gx2] slow swap %llu: %.1f ms", (unsigned long long)g_swap_count, ms);
    }
    if (g_swap_count % 300 == 1) {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - last).count();
        last = now;
        LOG("[gx2] frame %llu, %.1f swaps/s, swap interval %u", (unsigned long long)g_swap_count, 300 / s, g_swap_interval);
        // debug: WWHD_PEEK=<hex addr>:<bytes> logs that guest memory with every frame report
        if (const char* pk = getenv("WWHD_PEEK")) {
            unsigned a = 0, n = 16;
            if (sscanf(pk, "%x:%u", &a, &n) >= 1) {
                char hex[256] = {};
                for (unsigned i = 0; i < n && i < 64; i++) snprintf(hex + i * 3, sizeof hex - i * 3, "%02X ", ld8(a + i));
                LOG("[peek] %08X: %s", a, hex);
            }
        }
        if (getenv("WWHD_SCHED_STATS")) threads::report_sched();
    }
}
HLE(gx2, GX2GetSwapStatus) {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    if (arg(c, 0)) st32(arg(c, 0), (uint32)(g_swap_count + g_count_offset));
    if (arg(c, 1)) st32(arg(c, 1), (uint32)(g_flip_count + g_count_offset));
    if (arg(c, 2)) st64(arg(c, 2), timebase::to_guest(g_last_flip_time));
    if (arg(c, 3)) st64(arg(c, 3), timebase::guest_now());
}
HLE(gx2, GX2SetSwapInterval) { g_swap_interval = std::max<uint32>(arg(c, 0), 1); }
// GX2 event callbacks (GX2SetEventCallback): BotW registers vsync (2) and flip (3) handlers; its
// vsync handler counts vsyncs (1046D420, also its GX2SetGPUFence value) and the flip handler records
// that count per frame. A host thread calls them like Cemu's event callback thread: vsync at
// 59.94 Hz, then one flip callback per flip executed since. callback(eventType, userData).
static std::atomic<uint32_t> g_event_cb[5], g_event_ud[5];
static void event_callback_thread() {
    Cpu* c = threads::make_service_cpu("GX2 event", 0x8000);
    host::set_thread_name("GX2 event");
    uint64_t seenFlips;
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        seenFlips = g_flip_count;
    }
    for (;;) {
        // wake just after the next vsync of the clock flips execute on (vsync_index), so a flip that
        // executes at that vsync is reported at once, like the hardware's interrupt: the game's fence
        // target is "count at the last flip + swap interval"
        // on the vsync grid flips execute on, which a late flip moves (vsync_pacing.h)
        std::this_thread::sleep_until(tick_time((vsync_index() / kTicksPerVsync + 1) * kTicksPerVsync) + std::chrono::microseconds(200));
        uint64_t flips;
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips();
            flips = g_flip_count;
        }
        const uint32_t vcb = g_event_cb[2].load(), fcb = g_event_cb[3].load();
        if (!vcb && !fcb) { seenFlips = flips; continue; }
        threads::service_begin();
        threads::set_service_core(1);
        bool took = threads::ensure_core();
        // flip before vsync: the flip records the count of the vsync it executed on before that
        // vsync is counted (the game's fence target, last flip + 2, then means 2 vsyncs per frame;
        // the other order gave 3, vsync callbacks unaligned with flips gave 1-2)
        if (fcb)
            for (uint64_t n = std::min<uint64_t>(flips - seenFlips, 2); n > 0; n--) guest_call(c, fcb, {3u, g_event_ud[3].load()});
        if (vcb) guest_call(c, vcb, {2u, g_event_ud[2].load()});
        seenFlips = flips;
        if (took) threads::release_core();
        threads::service_end();
    }
}
// (address, mask, compare op, value): the GPU waits there until the condition holds (see OP_WAIT_MEM)
HLE(gx2, GX2SetGPUFence) {
    static const bool off = getenv("WWHD_NO_GPU_FENCE") && atoi(getenv("WWHD_NO_GPU_FENCE"));
    if (!off && arg(c, 0)) emit(OP_WAIT_MEM, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)});
}
HLE(gx2, GX2SetEventCallback) {
    const uint32_t type = arg(c, 0);
    if (type >= 5) return;
    g_event_cb[type] = arg(c, 1);
    g_event_ud[type] = arg(c, 2);
    LOG("[gx2] event callback %u: %08X (user data %08X)", type, arg(c, 1), arg(c, 2));
    static std::once_flag started;
    std::call_once(started, [] { std::thread(event_callback_thread).detach(); });
}
HLE(gx2, GX2GetEventCallback) {
    const uint32_t type = arg(c, 0);
    if (type >= 5) return;
    if (arg(c, 1)) st32(arg(c, 1), g_event_cb[type]);
    if (arg(c, 2)) st32(arg(c, 2), g_event_ud[type]);
}
// debug: WWHD_VSYNC_FENCE_POLL=1 makes the uncapped GX2WaitForVsync poll with a queue fence on every
// call, as before; =2 alternates every 10 s and logs the process CPU and frame time of each window
// (an A/B in one run: a fanless Mac's heat drifts too much between runs)
static bool vsync_fence_poll() {
    static const int mode = getenv("WWHD_VSYNC_FENCE_POLL") ? atoi(getenv("WWHD_VSYNC_FENCE_POLL")) : 0;
    if (mode != 2) return mode == 1;
    static std::mutex mu;
    static auto windowStart = std::chrono::steady_clock::now();
    static double cpuStart = 0;
    static uint64_t flipsStart = 0;
    static bool poll = false;
    auto cpuSeconds = [] {
        rusage ru{};
        getrusage(RUSAGE_SELF, &ru);
        return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
    };
    std::lock_guard<std::mutex> lk(mu);
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - windowStart).count();
    if (secs < 10) return poll;
    const double cpu = cpuSeconds();
    uint64_t flips;
    {
        std::lock_guard<std::mutex> fl(g_flip_mutex);
        flips = g_flip_count;
    }
    if (cpuStart)
        LOG("[vsync-ab] %s: %.0f%% CPU, %.2f ms/frame (%.1f s)", poll ? "fence poll" : "frame wait",
            100 * (cpu - cpuStart) / secs, flips > flipsStart ? 1000 * secs / (flips - flipsStart) : 0.0, secs);
    poll = !poll;
    windowStart = now;
    cpuStart = cpu;
    flipsStart = flips;
    return poll;
}
HLE(gx2, GX2WaitForVsync) {
    if(uncapped_benchmark()) {
        BlockingScope b;
        // Here a flip executes as soon as its frame's GPU work is done (update_flips), and BotW polls
        // this 450-650 times a frame for it. So sleep until frames_completed() reaches the oldest
        // pending swap (or one vsync passed): that implies the render thread executed the swap, so
        // no queue fence is needed. With nothing pending, or a renderer that cannot wait for a
        // frame, the fence: the earlier swaps' presentation path waits for GPU completion. Never
        // wait while holding the flip mutex.
        const bool fencePoll = vsync_fence_poll();
        uint64_t frame = 0;
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            const uint64_t flips = g_flip_count;
            update_flips();
            if (g_flip_count != flips && !fencePoll) return;
            if (!g_pending_flips.empty()) frame = g_pending_flips.front().swap;
        }
        if (frame && !fencePoll && render::can_wait_frames()) {
            const auto t0 = std::chrono::steady_clock::now();
            render::wait_frames_completed(frame, 1000000000 / 60);
            const auto waited = std::chrono::steady_clock::now() - t0;
            sync_stat(kSyncVsyncFrame, waited);
            if (rprof::enabled())
                rprof::add_sync(rprof::kSyncFlip, (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count());
        } else {
            render_sync(kSyncVsyncUncapped);
        }
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        return;
    }
#ifdef WWHD_HAS_VULKAN
    static const bool readyFlipWait = [] {
        const char* value = getenv("WWHD_VK_READY_FLIP_WAIT");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    if(readyFlipWait) {
        bool eligible = false, needsSync = false;
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            if(!g_pending_flips.empty()) {
                const auto& front = g_pending_flips.front();
                const uint64_t at = flip_due(front);
                eligible = at <= vsync_index();
                if(eligible) needsSync = render::frames_completed() < front.swap;
            }
        }
        if(eligible) {
            if(needsSync) {
                BlockingScope b;
                render_sync(kSyncVsyncFlip); // Queued swap completion; never hold flip mutex.
            }
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips(); // Retains minimum interval and FIFO GPU guards.
            return;
        }
    }
#endif
    static const bool preciseSleep = [] {
#ifdef WWHD_HAS_VULKAN
        // Vulkan renderer's pacing (docs/vulkan.md); the Metal renderer keeps plain sleeping
        if (!render::vulkan()) return false;
        const char* value = getenv("WWHD_VSYNC_PRECISE");
#ifdef __APPLE__
        // Avoid the measured macOS sleep overshoot; explicit zero opts out.
        return !value || atoi(value) != 0;
#else
        return value && atoi(value) != 0;
#endif
#else
        return false;
#endif
    }();
    const uint64_t granule = vsync_granule();
    const auto deadline = tick_time((vsync_index() / granule + 1) * granule);  // the next (virtual) vsync
    if (wait_for_late_flip(deadline)) return;
#ifdef WWHD_HAS_VULKAN
    static const bool readyFlipPark = [] {
        const char* value = getenv("WWHD_VK_READY_FLIP_PARK");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    threads::park_sleep_until(deadline, preciseSleep,
        readyFlipPark ? ready_flip_before_resume : nullptr);
#else
    threads::park_sleep_until(deadline, preciseSleep);
#endif
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    static uint64_t calls = 0;
    static const bool logVsync = getenv("WWHD_LOG_VSYNC") != nullptr;
    if (logVsync && ++calls % 60 == 0)
        LOG("[gx2] vsync %llu: swaps %llu flips %llu pending %zu", (unsigned long long)vsync_index(),
            (unsigned long long)g_swap_count, (unsigned long long)g_flip_count, g_pending_flips.size());
}

// scan buffers: the game renders into its own color buffers and copies to "scan buffers";
// the renderer presents whatever was copied to the TV target.
// (buffer, size, mode, surfaceFormat, bufferingMode): an sRGB format means scan-out applies the encoding
HLE(gx2, GX2SetTVBuffer) {
    LOG("[gx2] TV buffer format %X", arg(c, 3));
    render::set_tv_format(arg(c, 3), true);
}
HLE(gx2, GX2SetDRCBuffer) { render::set_tv_format(arg(c, 3), false); }
HLE(gx2, GX2SetTVScale) {}
HLE(gx2, GX2SetDRCScale) {}
HLE(gx2, GX2SetTVEnable) {}
HLE(gx2, GX2SetDRCEnable) {}
HLE(gx2, GX2CalcTVSize) {
    // (mode, format, bufferingMode, uint32* size, bool* scaleNeeded)
    uint32 mode = arg(c, 0), buffers = std::max<uint32>(arg(c, 2), 1);
    uint32 w = mode >= 5 ? 1920 : mode <= 2 ? 854 : 1280, h = mode >= 5 ? 1080 : mode <= 2 ? 480 : 720;
    st32(arg(c, 3), w * h * 4 * buffers);
    st32(arg(c, 4), 0);
}
HLE(gx2, GX2CalcDRCSize) {
    st32(arg(c, 3), 854 * 480 * 4 * std::max<uint32>(arg(c, 2), 1));
    st32(arg(c, 4), 0);
}

// ---------------------------------------------------------------- misc queries
HLE(gx2, GX2TempGetGPUVersion) { ret(c, 2); }
HLE(gx2, GX2CalcGeometryShaderInputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcGeometryShaderOutputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcFetchShaderSizeEx) {
    uint32 n = arg(c, 0);
    uint32 cf = ((((n + 15) / 16) + 1) * 8 + 0xF) & ~0xFu;
    ret(c, std::max<uint32>(cf + n * 16, 16 + n * 16));
}
HLE(gx2, GX2GPUTimeToCPUTime) { ret64(c, arg64(c, 3)); }
HLE(gx2, GX2SampleTopGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }
HLE(gx2, GX2SampleBottomGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }

// ---------------------------------------------------------------- save states

// the game is frozen between frames: finish all queued GPU work and let pending flips execute, so no
// command reads guest memory while it is replaced and the swap/flip counts agree
void gx2_ss_drain() {
    emit_host(OP_DRAW_DONE, {1});  // full GPU wait, also with lazy DrawDone
    render_sync(kSyncSaveState);
    for (int i = 0; i < 300; i++) {
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips();
            if (g_pending_flips.empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LOG("[savestate] flips still pending after 300 ms");
}

void gx2_ss_save(ss::Writer& w) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    w.u32(kNumRegs);
    w.bytes(g_regs, sizeof g_regs);
    uint32 active = 0;
    std::vector<uint32> keys;
    for (auto& [k, v] : g_contexts) {
        keys.push_back(k);
        if (g_shadow == v.data()) active = k;
    }
    std::sort(keys.begin(), keys.end());
    w.u32((uint32)keys.size());
    for (uint32 k : keys) {
        w.u32(k);
        w.u32((uint32)g_contexts[k].size());
        w.bytes(g_contexts[k].data(), g_contexts[k].size() * 4);
    }
    w.u32(active);
    w.u32(g_swap_interval);
    std::lock_guard<std::mutex> fl(g_flip_mutex);
    w.u64(g_swap_count + g_count_offset);
}

bool gx2_ss_check(ss::Reader r, std::string& why) {
    if (r.u32() != kNumRegs) { why = "GX2 register file size differs"; return false; }
    return r.ok;
}

void gx2_ss_load(ss::Reader& r) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    r.u32();
    r.bytes(g_regs, sizeof g_regs);
    g_register_blocks = {};
    g_register_blocks.include(g_regs);
    g_contexts.clear();
    uint32 n = r.u32();
    for (uint32 i = 0; i < n && r.ok; i++) {
        uint32 k = r.u32(), words = r.u32();
        auto& v = g_contexts[k];
        v.resize(words);
        r.bytes(v.data(), (size_t)words * 4);
        if (words == kNumRegs) g_register_blocks.include(v.data());
    }
    uint32 active = r.u32();
    auto it = g_contexts.find(active);
    g_shadow = active && it != g_contexts.end() ? it->second.data() : nullptr;
    g_shader_state_gen++;
    g_swap_interval = std::max<uint32>(r.u32(), 1);
    uint64_t guest_swaps = r.u64();
    {
        std::lock_guard<std::mutex> fl(g_flip_mutex);
        g_count_offset = (int64_t)guest_swaps - (int64_t)g_swap_count;
    }
    render::ss_reset();  // the renderer forgets surface contents and shader memos (Vulkan)
    g_dl_index.clear();  // lists recorded before the load say nothing about the loaded ones
    set_active_snapshot(nullptr);
}
