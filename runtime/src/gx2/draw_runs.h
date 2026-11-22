// Index runs (on by default, WWHD_INDEX_RUNS=0 turns them off; WWHD_INDEX_RUNS_AB=1 alternates on and off every 10 s,
// [index-ab] lines). Town A/B 2026-11-23: 4,230 -> 3,730 draws/frame, render thread -0.18 ms, GPU busy -0.25 ms,
// CPU % and mJ/frame unchanged at the 60 fps cap; picture checked. Included by gx2_core.cpp only, inside namespace gx2, before execute_one.
// Ported from ZeldaWWHDRecomp 546255ba (runtime/src/gx2/draw_runs.h, its WWHD_VK_INDEX_RUNS; the run logic follows
// VR-WakerHD's draw batching by @mkooner, MPL-2.0). Only the index runs; upstream's same-state instanced runs join
// ~1 draw a frame in BotW (draw-run census, PLAN.md 2026-11-23) and are not ported.
//
// Consecutive draws on the render thread with no register value changed and no other command between them become one
// draw when they are
//   - indexed triangle lists or strips, one instance, with the same primitive, index type, base vertex (and base
//     instance): a list whose pieces are one contiguous range is drawn as that range; otherwise the pieces' indices
//     (their own index type, values unchanged) are copied into a host scratch buffer that the renderer reads instead
//     of guest memory (render::g_host_indices). Strips are joined by zero-area triangles that start every piece on an
//     even index, so each triangle keeps its own winding and provoking vertex. A restart index in a strip piece:
//     drawn piece by piece.
//   - non-indexed lists (points, lines, triangles, rects, quads) whose vertices follow on: one larger count.
// Upstream's ring of guest memory (for its Vulkan buffer cache) is not needed: Metal copies every draw's indices
// into its upload pool at the draw. Breakers: any register write that changes a value, and every other command
// (invalidates, queries, fences, clears, copies, context loads, ...; OP_CALL's own commands are looked at one by one).
// A geometry shader or stream out on (the primitive count or ID shows): no runs. What is held is drawn before the
// render thread waits for more work, so nothing is left undrawn while the game runs on.
#pragma once

namespace draw_runs {

inline bool g_index_on = [] {
    const char* e = getenv("WWHD_INDEX_RUNS");
    const char* ab = getenv("WWHD_INDEX_RUNS_AB");
    const bool isAb = ab && !strcmp(ab, "1");
    const bool on = isAb || !e || strcmp(e, "0") != 0;
    LOG("[gx2] index draw runs %s", isAb ? "on, A/B every 10 s (WWHD_INDEX_RUNS_AB=1)"
                                   : on ? "on (WWHD_INDEX_RUNS=0 turns them off)" : "off (WWHD_INDEX_RUNS=0)");
    return on;
}();
inline bool index_on() { return g_index_on && g_render_thread && !render::vulkan(); }

// totals since start (render thread; read for the A/B lines)
struct Stats { uint64_t runs = 0, pieces = 0, contiguous = 0, copied = 0, refused = 0; };
inline Stats g_stats;

constexpr uint32 kPrimTriangles = 4, kPrimTriangleStrip = 6;
constexpr uint32 kIndexRunMax = 256;     // pieces in a run
constexpr uint32 kPieceMax = 1u << 20;   // elements in a piece
constexpr uint32 kRunMax = 1u << 21;     // elements in a run (the copy's size; seams included)
struct IndexRun {
    bool on = false;
    Op op = OP_DRAW_INDEXED;
    uint32 n = 6;
    uint32 args[7] = {};  // the first piece's
    uint32 pieces = 0, total = 0;
    uint32 counts[kIndexRunMax] = {}, addrs[kIndexRunMax] = {};  // addrs: indices, or the first vertex
    bool contiguous = true;
};
inline IndexRun g_irun;
inline bool g_inside = false;  // (a run's draw executing: not intercepted again)

inline uint32 index_size(uint32 type) { return type == 1 || type == 9 ? 4 : 2; }  // GX2_INDEX_TYPE_U32(_LE)
inline bool index_le(uint32 type) { return type == 0 || type == 1; }             // GX2_INDEX_TYPE_U16_LE, U32_LE
inline bool index_type_known(uint32 type) { return type == 0 || type == 1 || type == 4 || type == 9; }
inline uint32 list_vertices(uint32 prim) {
    switch (prim) {
    case 1: return 1;             // points
    case 2: return 2;             // lines
    case 4: case 0x11: return 3;  // triangles, rects
    case 0x13: return 4;          // quads
    default: return 0;
    }
}
inline bool run_shape(Op op, const uint32* p, uint32 n) {
    if (op == OP_DRAW_INDEXED)
        return (n == 6 || n == 7) && p[5] == 1 && p[1] >= 3 && p[1] < kPieceMax && p[3] && index_type_known(p[2]) &&
               (p[0] == kPrimTriangleStrip || (p[0] == kPrimTriangles && p[1] % 3 == 0));
    const uint32 v = op == OP_DRAW && n == 4 ? list_vertices(p[0]) : 0;
    return v && p[3] == 1 && p[1] && p[1] < kPieceMax && p[1] % v == 0;
}
inline bool index_candidate(Op op, const uint32* p, uint32 n) {
    return index_on() && run_shape(op, p, n) && !(g_regs[Latte::REGADDR::VGT_GS_MODE] & 3) && !g_regs[mmVGT_STRMOUT_EN];
}
inline bool index_continues(Op op, const uint32* p, uint32 n) {
    const IndexRun& r = g_irun;
    if (op != r.op || n != r.n || p[0] != r.args[0] || r.pieces >= kIndexRunMax || !run_shape(op, p, n) ||
        r.total + p[1] + 3 * r.pieces > kRunMax)
        return false;
    if (op == OP_DRAW_INDEXED)  // same index type, base vertex and base instance
        return p[2] == r.args[2] && p[4] == r.args[4] && (n < 7 || p[6] == r.args[6]);
    return p[2] == r.addrs[r.pieces - 1] + r.counts[r.pieces - 1];  // (non-indexed: the vertices follow on)
}
inline void index_add(Op op, const uint32* p) {
    IndexRun& r = g_irun;
    const uint32 k = r.pieces++;
    r.counts[k] = p[1];
    r.total += p[1];
    if (op == OP_DRAW_INDEXED) {
        r.addrs[k] = p[3];
        if (k) r.contiguous &= p[3] == r.addrs[k - 1] + r.counts[k - 1] * index_size(p[2]);
    } else {
        r.addrs[k] = p[2];
    }
}

// the run's indices (their own index type, unchanged values) copied into host scratch; nullptr when they cannot be
inline const uint8_t* gather_indices(uint32& outCount) {
    static std::vector<uint8_t> scratch;  // render thread only; read by the renderer inside the draw
    const IndexRun& r = g_irun;
    const uint32 type = r.args[2], size = index_size(type);
    const bool le = index_le(type), strip = r.args[0] == kPrimTriangleStrip;
    const size_t bytes = (size_t(r.total) + (strip ? 3 * size_t(r.pieces) : 0)) * size;
    if (scratch.size() < bytes) scratch.resize(bytes);
    uint8_t* out = scratch.data();
    size_t len = 0;
    if (!strip) {  // lists: the pieces one after another
        for (uint32 k = 0; k < r.pieces; ++k) {
            memcpy(out + len * size, mem::ptr(r.addrs[k]), size_t(r.counts[k]) * size);
            len += r.counts[k];
        }
        outCount = uint32(len);
        return out;
    }
    const uint32 restart = size == 4 ? 0xFFFFFFFFu : 0xFFFFu;
    auto get = [&](const uint8_t* e) -> uint32 {
        if (size == 2) return le ? uint32(e[0] | e[1] << 8) : uint32(e[0] << 8 | e[1]);
        return le ? uint32(e[0] | e[1] << 8 | e[2] << 16 | uint32(e[3]) << 24)
                  : uint32(uint32(e[0]) << 24 | e[1] << 16 | e[2] << 8 | e[3]);
    };
    auto put = [&](const uint8_t* e) { memcpy(out + size * len++, e, size); };
    for (uint32 k = 0; k < r.pieces; ++k) {
        const uint8_t* in = (const uint8_t*)mem::ptr(r.addrs[k]);
        for (uint32 i = 0; i < r.counts[k]; ++i)
            if (get(in + size_t(size) * i) == restart) return nullptr;  // (a restart: a new strip on the seam's index)
        if (k) {  // zero-area triangles from the last index to this piece's first, which lands on an even index
            const uint8_t* prev = out + size * (len - 1);
            uint8_t last[4];
            memcpy(last, prev, size);
            if (len & 1) put(last);
            put(last);
            put(in);
        }
        memcpy(out + size * len, in, size_t(r.counts[k]) * size);
        len += r.counts[k];
    }
    outCount = uint32(len);
    return out;
}

inline void exec_draw(Op op, const uint32* a, uint32 n) {
    g_inside = true;
    execute_one(op, a, n);
    g_inside = false;
}

// the run drawn: one draw, or (one piece, or refused) as the draws it is
inline void index_flush() {
    if (!g_irun.on) return;
    IndexRun& r = g_irun;
    r.on = false;
    uint32 a[7];
    memcpy(a, r.args, sizeof a);
    if (r.pieces == 1) { exec_draw(r.op, a, r.n); return; }
    if (r.op == OP_DRAW || (r.contiguous && r.args[0] == kPrimTriangles)) {  // (strips need their seams)
        a[1] = r.total;
        ++g_stats.contiguous;
        exec_draw(r.op, a, r.n);
    } else {
        uint32 count = 0;
        const uint8_t* host = gather_indices(count);
        if (!host) {  // (a restart index in a strip: piece by piece, as before)
            ++g_stats.refused;
            for (uint32 k = 0; k < r.pieces; ++k) {
                a[1] = r.counts[k], a[3] = r.addrs[k];
                exec_draw(r.op, a, r.n);
            }
            return;
        }
        ++g_stats.copied;
        a[1] = count;  // (a[3]: the first piece's address, for logs; the indices come from host)
        render::g_host_indices = host;
        exec_draw(r.op, a, r.n);
        render::g_host_indices = nullptr;
    }
    ++g_stats.runs;
    g_stats.pieces += r.pieces;
}

inline bool write_changes(const uint32* p, uint32 n) {
    if (!n) return false;
    const uint32 first = p[0], count = n - 1;
    if (first >= kNumRegs || count > kNumRegs - first) return true;
    return memcmp(&g_regs[first], p + 1, count * 4) != 0;
}

// The hook (top of execute_one): true when the command was taken into a run. Every other command first draws what
// is held unless it continues it.
inline bool before_op(Op op, const uint32* p, uint32 n) {
    if (g_inside) return false;
    switch (op) {
    case OP_NOP: case OP_CALL: return false;
    case OP_SET_REGS:
        if (g_irun.on && write_changes(p, n)) index_flush();
        return false;
    case OP_DRAW: case OP_DRAW_INDEXED:
        if (g_irun.on) {
            if (index_continues(op, p, n)) {
                index_add(op, p);
                return true;
            }
            index_flush();
        }
        if (index_candidate(op, p, n)) {
            IndexRun& r = g_irun;
            r.on = true, r.op = op, r.n = n;
            memset(r.args, 0, sizeof r.args);
            memcpy(r.args, p, n * 4);
            r.pieces = r.total = 0;
            r.contiguous = true;
            index_add(op, p);
            return true;
        }
        return false;
    default:
        index_flush();
        return false;
    }
}

}  // namespace draw_runs
