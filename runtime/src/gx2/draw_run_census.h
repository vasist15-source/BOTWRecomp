// Draw-run census (diagnostics only, WWHD_DRAW_RUN_CENSUS=1; nothing is drawn differently): for every draw executed on
// the render thread, what changed since the previous draw, and which join rule could have merged it with that draw.
// Logged every 300 frames as "[run census]" lines. Included by gx2_core.cpp only (execute_one).
// Ported from ZeldaWWHDRecomp 546255ba (runtime/src/gx2/draw_run_census.h, the census for its draw-run batching, which
// is based on VR-WakerHD's draw batching by @mkooner, MPL-2.0). Changes here: OP_SET_PROJ_REGS counted as a register
// write, 7-word indexed draws (base instance) compared in full, run-length buckets per rule, which vertex-shader block
// slots change inside same-state runs, and how many of those joins have a geometry shader on.
//
// Rules, each tried in order on a draw with the previous draw's op and arguments (one instance):
//   identical         nothing changed at all
//   VS-block-addr     only vertex-shader uniform block addresses changed (the upstream instanced batching)
//   VS+PS-block-addr  vertex and pixel uniform block addresses
//   +ALU              ... and uniform registers (ALU constants)
//   VB/fetch/blocks   ... block addresses, vertex buffer address/size, fetch shader address
//   index-continue    no register changed, same primitive/index type/base vertex, indices follow on (one range)
//   index-any         no register changed, same primitive/index type, indices anywhere (upstream copies them)
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace run_census {

enum Change : uint32_t {
    kVsAddr = 1, kVsOther = 2, kPsAddr = 4, kPsOther = 8, kAlu = 16, kVb = 32, kFetchAddr = 64, kOtherReg = 128,
    kOtherOp = 256,
};
enum Cat { kSame0, kVsOnly, kVsPs, kVsPsAlu, kWeather, kIdxCont, kIdxAny, kNone, kCats };
inline const char* cat_name(int c) {
    static const char* n[kCats] = {"identical", "VS-block-addr", "VS+PS-block-addr", "+ALU", "VB/fetch/blocks",
                                   "index-continue", "index-any", "no rule"};
    return n[c];
}
// run lengths (draws in the run, the first one included): 2, 3-4, 5-8, 9-16, 17-64, 65+
constexpr int kLenBuckets = 6;
inline int len_bucket(uint32_t len) {
    return len <= 2 ? 0 : len <= 4 ? 1 : len <= 8 ? 2 : len <= 16 ? 3 : len <= 64 ? 4 : 5;
}

struct State {
    bool on = false, init = false, havePrev = false;
    uint32_t changed = 0, vsSlots = 0;
    uint32_t prevOp = 0, prevN = 0, prevArgs[8] = {};
    uint64_t frames = 0, draws = 0, inst = 0;
    uint64_t cat[kCats] = {};
    uint64_t runs[kCats] = {};  // runs of 2+ (a join after a draw that was not a join of that rule)
    uint64_t runDraws[kCats] = {};  // draws in those runs, the first draw of each included
    uint64_t lens[kCats][kLenBuckets] = {};  // runs by length
    uint64_t longest[kCats] = {};
    uint64_t slotJoins[16] = {};  // VS-block-addr joins in which vertex block slot i changed
    uint64_t gsJoins = 0;         // VS-block-addr joins with a geometry shader on
    int prevCat = kNone;
    uint32_t runLen = 0;
    std::unordered_map<uint64_t, uint64_t> byVs[kCats];  // VS address -> joins
    std::unordered_map<uint32_t, uint64_t> breakers;     // first non-VS-addr register changed (same args, VS rule broken)
    std::unordered_map<uint32_t, uint64_t> breakOps;     // op that came between two same-args draws
    uint32_t firstBreak = 0, firstOp = 0;
};
inline State& st() { static State s; return s; }
inline bool on() {
    State& s = st();
    if (!s.init) {
        s.init = true;
        const char* e = std::getenv("WWHD_DRAW_RUN_CENSUS");
        s.on = e && *e == '1';
        if (s.on) LOG("[run census] on (WWHD_DRAW_RUN_CENSUS=1): logged every 300 frames, nothing drawn differently");
    }
    return s.on;
}

inline uint32_t reg_change(uint32_t r) {
    if (r >= mmSQ_ALU_CONSTANT0_0 && r < mmSQ_ALU_CONSTANT0_0 + 0x1000) return kAlu;
    if (r >= mmSQ_VTX_UNIFORM_BLOCK_START && r < mmSQ_VTX_UNIFORM_BLOCK_START + 7 * 16)
        return (r - mmSQ_VTX_UNIFORM_BLOCK_START) % 7 == 0 ? kVsAddr : kVsOther;
    if (r >= mmSQ_PS_UNIFORM_BLOCK_START && r < mmSQ_PS_UNIFORM_BLOCK_START + 7 * 16)
        return (r - mmSQ_PS_UNIFORM_BLOCK_START) % 7 == 0 ? kPsAddr : kPsOther;
    if (r >= mmSQ_VTX_ATTRIBUTE_BLOCK_START && r < mmSQ_VTX_ATTRIBUTE_BLOCK_START + 7 * 16)
        return (r - mmSQ_VTX_ATTRIBUTE_BLOCK_START) % 7 < 2 ? kVb : kOtherReg;
    if (r == mmSQ_PGM_START_FS || r == mmSQ_PGM_START_FS + 1) return kFetchAddr;
    return kOtherReg;
}

// a register write about to be applied (regs: the values before it)
inline void regs(const uint32_t* regs, uint32_t first, const uint32_t* v, uint32_t n) {
    State& s = st();
    for (uint32_t i = 0; i < n; ++i)
        if (regs[first + i] != v[i]) {
            const uint32_t r = first + i, c = reg_change(r);
            if (c != kVsAddr && !(s.changed & ~kVsAddr) && !s.firstBreak) s.firstBreak = r;
            if (c == kVsAddr) s.vsSlots |= 1u << ((r - mmSQ_VTX_UNIFORM_BLOCK_START) / 7);
            s.changed |= c;
        }
}
inline void other_op(uint32_t op) {
    State& s = st();
    if (!(s.changed & kOtherOp)) s.firstOp = op;
    s.changed |= kOtherOp;
}

inline uint32_t index_bytes(uint32_t type) { return type == 1 || type == 9 ? 4 : 2; }

inline void run_end(State& s) {
    if (s.prevCat != kNone && s.runLen >= 2) {
        ++s.lens[s.prevCat][len_bucket(s.runLen)];
        s.runDraws[s.prevCat] += s.runLen;
        s.longest[s.prevCat] = std::max<uint64_t>(s.longest[s.prevCat], s.runLen);
    }
}

inline void draw(const uint32_t* regs, uint32_t op, const uint32_t* p, uint32_t n, bool indexed) {
    State& s = st();
    ++s.draws;
    const uint32_t instances = indexed ? (n > 5 ? p[5] : 1) : (n > 3 ? p[3] : 1);
    if (instances != 1) ++s.inst;
    int c = kNone;
    const bool cmp = s.havePrev && op == s.prevOp && n == s.prevN && n <= 8 && instances == 1;
    const bool same = cmp && !std::memcmp(p, s.prevArgs, n * 4);
    if (cmp) {
        const uint32_t ch = s.changed;
        if (same && !ch) c = kSame0;
        else if (same && !(ch & ~kVsAddr)) c = kVsOnly;
        else if (same && !(ch & ~(kVsAddr | kPsAddr))) c = kVsPs;
        else if (same && !(ch & ~(kVsAddr | kPsAddr | kAlu))) c = kVsPsAlu;
        else if (same && !(ch & ~(kVsAddr | kPsAddr | kVb | kFetchAddr))) c = kWeather;
        else if (!ch && n >= 3 && p[0] == s.prevArgs[0]) {
            if (indexed && n >= 5) {
                const bool listOrStrip = p[0] == 4 || p[0] == 6;
                if (listOrStrip && p[2] == s.prevArgs[2] && p[4] == s.prevArgs[4] &&
                    p[3] == s.prevArgs[3] + s.prevArgs[1] * index_bytes(p[2]))
                    c = kIdxCont;
                else if (listOrStrip && p[2] == s.prevArgs[2] && p[4] == s.prevArgs[4]) c = kIdxAny;
            } else if (!indexed && p[2] == s.prevArgs[2] + s.prevArgs[1]) c = kIdxCont;
        }
        if (same && c == kNone) {
            if (s.changed & kOtherOp) ++s.breakOps[s.firstOp];
            else if (s.firstBreak) ++s.breakers[s.firstBreak];
        }
    }
    if (c == kVsOnly) {
        for (uint32_t i = 0; i < 16; ++i)
            if (s.vsSlots & (1u << i)) ++s.slotJoins[i];
        if (regs[Latte::REGADDR::VGT_GS_MODE] & 3) ++s.gsJoins;
    }
    if (c != kNone) {
        ++s.cat[c];
        if (s.prevCat != c) {
            run_end(s);
            ++s.runs[c];
            s.runLen = 2;
        } else {
            ++s.runLen;
        }
        ++s.byVs[c][regs[mmSQ_PGM_START_VS]];
    } else {
        run_end(s);
        s.runLen = 0;
    }
    s.prevCat = c;
    s.havePrev = true;
    s.prevOp = op;
    s.prevN = n;
    std::memcpy(s.prevArgs, p, std::min<uint32_t>(n, 8) * 4);
    s.changed = 0;
    s.vsSlots = 0;
    s.firstBreak = 0;
    s.firstOp = 0;
}

inline void frame_end() {
    State& s = st();
    if (++s.frames < 300) return;
    const double f = double(s.frames);
    LOG("[run census] per frame over %llu frames: %.1f draws (%.1f instanced)", (unsigned long long)s.frames,
        s.draws / f, s.inst / f);
    for (int c = 0; c < kNone; ++c) {
        std::vector<std::pair<uint64_t, uint64_t>> v;
        for (auto& [k, x] : s.byVs[c]) v.push_back({x, k});
        std::sort(v.rbegin(), v.rend());
        char buf[256];
        int k = 0;
        buf[0] = 0;
        for (size_t i = 0; i < v.size() && i < 6 && k < int(sizeof buf) - 40; ++i)
            k += std::snprintf(buf + k, sizeof buf - k, " %08llX:%.1f", (unsigned long long)v[i].second, v[i].first / f);
        const double runs = s.runs[c] / f;
        LOG("[run census]   %-16s joins %.1f in %.1f runs (%.1f draws in runs, avg %.1f, longest %llu; lengths "
            "2:%.1f 3-4:%.1f 5-8:%.1f 9-16:%.1f 17-64:%.1f 65+:%.1f); by VS:%s",
            cat_name(c), s.cat[c] / f, runs, s.runDraws[c] / f, s.runs[c] ? double(s.runDraws[c]) / s.runs[c] : 0.0,
            (unsigned long long)s.longest[c], s.lens[c][0] / f, s.lens[c][1] / f, s.lens[c][2] / f, s.lens[c][3] / f,
            s.lens[c][4] / f, s.lens[c][5] / f, buf);
    }
    {
        char buf[256];
        int k = 0;
        buf[0] = 0;
        for (int i = 0; i < 16 && k < int(sizeof buf) - 20; ++i)
            if (s.slotJoins[i]) k += std::snprintf(buf + k, sizeof buf - k, " b%d:%.1f", i, s.slotJoins[i] / f);
        LOG("[run census]   VS-block-addr joins by changed vertex block:%s; with a geometry shader %.1f", buf,
            s.gsJoins / f);
    }
    {
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto& [k, x] : s.breakers) v.push_back({x, k});
        std::sort(v.rbegin(), v.rend());
        char buf[512];
        int k = 0;
        buf[0] = 0;
        for (size_t i = 0; i < v.size() && i < 10 && k < int(sizeof buf) - 40; ++i)
            k += std::snprintf(buf + k, sizeof buf - k, " %04X:%.1f", v[i].second, v[i].first / f);
        std::vector<std::pair<uint64_t, uint32_t>> o;
        for (auto& [op, x] : s.breakOps) o.push_back({x, op});
        std::sort(o.rbegin(), o.rend());
        char ops[256];
        int j = 0;
        ops[0] = 0;
        for (size_t i = 0; i < o.size() && i < 8 && j < int(sizeof ops) - 24; ++i)
            j += std::snprintf(ops + j, sizeof ops - j, " op%u:%.1f", o[i].second, o[i].first / f);
        LOG("[run census]   same-args draws not joined: first register breaking the VS rule:%s; an op between:%s", buf,
            ops);
    }
    run_end(s);
    s.runLen = 0;
    s.prevCat = kNone;
    s.frames = s.draws = s.inst = s.gsJoins = 0;
    for (int c = 0; c < kCats; ++c) {
        s.cat[c] = s.runs[c] = s.runDraws[c] = s.longest[c] = 0;
        for (auto& x : s.lens[c]) x = 0;
        s.byVs[c].clear();
    }
    for (auto& x : s.slotJoins) x = 0;
    s.breakers.clear();
    s.breakOps.clear();
}

}  // namespace run_census
