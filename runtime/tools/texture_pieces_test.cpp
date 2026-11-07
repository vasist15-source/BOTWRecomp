// Partial texture updates (runtime/src/gfx/texture_pieces.h and texture_pieces_layout.h) with Cemu's
// address library and real page write tracking (runtime/src/write_watch.cpp):
// 1. layout: for real texture descriptions (BotW's terrain array, cube maps, small and odd sizes, swizzled
//    bases and mip chains, every thin and thick tile mode, 1- to 16-byte elements, 3D), every block the
//    upload's decode reads for slice z of a level (metal_surfaces.mm decode_level, its per-block reference
//    path) lies inside that slice's piece; the pieces of a level lie in it without overlapping, and the
//    chunks cover each piece in 64 KiB-aligned blocks;
// 2. a texture cache as metal_surfaces.mm's check_texture: a check after writes to the armed pages hashes
//    only the chunks written and uploads only the pieces that changed, over the game's write patterns (a
//    slice streamed over three frames, bytes rewritten unchanged, gaps between levels, pages shared with
//    other data, host writes, random pokes, forced checks, a texture rewritten every frame that the churn
//    rule leaves unarmed, partial updates switched on and off as the in-run A/B does). After every check
//    the uploaded copy equals guest memory byte for byte, as a full upload would leave it, and a partial
//    check uploads exactly the pieces whose bytes changed;
// 3. a writer thread (and host writes) racing the checks: once it stops, the next checks leave every
//    piece's upload equal to guest memory. (A write undone while the upload decodes after the hash can leave
//    a stale texture with the full path as with the partial one; uploads are modelled by the hashes here.)
#include "gfx/texture_pieces_layout.h"

#include <sys/mman.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#define XXH_INLINE_ALL
#include "../third_party/xxhash/xxhash.h"

static uint8_t* g_mem;
constexpr uint64_t kSize = 512ull << 20;
constexpr uint32_t kPage = 16384;  // the M4's page size

static uint64_t xxh(const void* p, size_t n) { return XXH3_64bits(p, n); }

namespace E {
constexpr uint32_t D2 = (uint32_t)Latte::E_DIM::DIM_2D, D3 = (uint32_t)Latte::E_DIM::DIM_3D, Cube = (uint32_t)Latte::E_DIM::DIM_CUBEMAP,
                   Arr = (uint32_t)Latte::E_DIM::DIM_2D_ARRAY;
}

// GX2 surface formats and the bytes per element (texel or 4x4 block) the decode reads (formats.h)
struct Fmt {
    uint32_t gx2, bytes;
    bool compressed;
};
constexpr Fmt kR8{0x001, 1, false}, kRG8{0x007, 2, false}, kR565{0x008, 2, false}, kRGBA8{0x01A, 4, false}, kRGBA16F{0x81F, 8, false},
    kRGBA32F{0x823, 16, false}, kBC1{0x031, 8, true}, kBC1S{0x431, 8, true}, kBC3{0x033, 16, true}, kBC5{0x035, 16, true};

static texpieces::Desc desc(uint32_t addr, const Fmt& f, uint32_t w, uint32_t h, uint32_t slices, uint32_t dim, uint32_t tm, uint32_t mips,
                            uint32_t swizzle = 0, uint32_t mipSwizzle = 0) {
    texpieces::Desc d;
    d.addr = addr;
    d.format = f.gx2;
    d.width = w;
    d.height = h;
    d.slices = slices;
    d.dim = dim;
    d.tileMode = tm;
    d.swizzle = swizzle;
    d.mips = mips;
    if (mips > 1) {  // the mip chain after level 0, 4 KiB aligned, its swizzle in bits 8-10
        LatteAddrLib::AddrSurfaceInfo_OUT info{};
        LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)f.gx2, w, h, slices, (Latte::E_DIM)dim, Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)tm),
                                              0, 0, &info);
        d.mipAddr = (uint32_t)((addr + info.surfSize + 0xFFF) & ~0xFFFull) | mipSwizzle;
    }
    return d;
}

static texpieces::Table table_of(const texpieces::Desc& d, const Fmt& f) {
    texpieces::Table t;
    texpieces::build(t, texpieces::levels_of(d, texpieces::level_ranges_of(d), f.bytes * 8));
    return t;
}

// 1. every block the decode reads for slice z of a level is in that slice's piece. split: whether some
// level is cut into slices (-1: either)
static void layout(const char* name, const texpieces::Desc& d, const Fmt& f, int split) {
    const auto ranges = texpieces::level_ranges_of(d);
    const texpieces::Table t = table_of(d, f);
    assert(t.levels.size() == ranges.size() && t.levelEnd.size() == ranges.size());
    bool cut = false;
    uint64_t blocks = 0;
    for (uint32_t l = 0; l < (uint32_t)ranges.size(); l++) {
        const bool is3D = d.dim == E::D3;
        const uint32_t w = std::max(d.width >> l, 1u), h = std::max(d.height >> l, 1u);
        const uint32_t slices = is3D ? std::max(d.slices >> l, 1u) : d.slices;
        const uint32_t bw = f.compressed ? (w + 3) / 4 : w, bh = f.compressed ? (h + 3) / 4 : h;
        LatteAddrLib::AddrSurfaceInfo_OUT info{};
        LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)d.format, d.width, d.height, d.slices, (Latte::E_DIM)d.dim,
                                              Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)d.tileMode), 0, l, &info);
        const auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
        const bool macro = Latte::TM_IsMacroTiled(tm);
        const uint32_t swz = l > 0 && d.mipAddr ? (d.mipAddr & 0x700) : d.swizzle;
        const uint32_t base = ranges[l].first, bpp = f.bytes * 8;
        const uint32_t p0 = l ? t.levelEnd[l - 1] : 0, p1 = t.levelEnd[l];
        cut |= p1 - p0 > 1;
        if (LatteAddrLib::TM_IsThick(tm) || is3D) assert(p1 - p0 == 1);
        for (uint32_t p = p0; p < p1; p++) {
            const texpieces::Piece& pc = t.pieces[p];
            assert(pc.level == l && pc.z0 < pc.z1 && pc.z1 <= std::max(slices, 1u));
            assert(pc.addr >= ranges[l].first && (uint64_t)pc.addr + pc.size <= (uint64_t)ranges[l].first + ranges[l].second);
            if (p > p0) assert(pc.addr >= t.pieces[p - 1].addr + t.pieces[p - 1].size && pc.z0 == t.pieces[p - 1].z1);
            // chunks: in order, each inside one 64 KiB block, covering the piece
            uint64_t at = pc.addr;
            for (uint32_t i = pc.chunk0; i < pc.chunk0 + pc.chunks; i++) {
                const texpieces::Chunk& c = t.chunks[i];
                assert(c.addr == at && c.size > 0 && c.addr / texpieces::kChunk == (c.addr + c.size - 1) / texpieces::kChunk);
                at += c.size;
            }
            assert(at == (uint64_t)pc.addr + pc.size);
        }
        assert(t.pieces[p0].z0 == 0 && t.pieces[p1 - 1].z1 == std::max(slices, 1u));
        for (uint32_t z = 0; z < slices; z++) {
            const texpieces::Piece* pc = nullptr;
            for (uint32_t p = p0; p < p1; p++)
                if (t.pieces[p].z0 <= z && z < t.pieces[p].z1) pc = &t.pieces[p];
            assert(pc);
            LatteAddrLib::CachedSurfaceAddrInfo ci;
            if (macro) LatteAddrLib::SetupCachedSurfaceAddrInfo(&ci, z, 0, bpp, info.pitch, info.height, slices, 1, tm, false, (swz >> 8) & 1, (swz >> 9) & 3);
            uint64_t lo = ~0ull, hi = 0;
            for (uint32_t y = 0; y < bh; y++)
                for (uint32_t x = 0; x < bw; x++) {
                    uint32_t off;
                    if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
                        off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, z, 0, bpp, info.pitch, info.height, slices);
                    else if (!macro)
                        off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, z, bpp, info.pitch, info.height, tm, false);
                    else
                        off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, &ci);
                    lo = std::min<uint64_t>(lo, (uint64_t)base + off);
                    hi = std::max<uint64_t>(hi, (uint64_t)base + off + f.bytes);
                }
            blocks += (uint64_t)bw * bh;
            if (lo < pc->addr || hi > (uint64_t)pc->addr + pc->size) {
                printf("%s: level %u slice %u (tile mode %u) reads %llX..%llX outside its piece %X+%X\n", name, l, z, (unsigned)tm,
                       (unsigned long long)lo, (unsigned long long)hi, pc->addr, pc->size);
                assert(false);
            }
        }
    }
    if (split >= 0) assert(cut == (split != 0));
    printf("  %-44s %2zu levels, %4zu pieces, %5zu chunks, %9.1f KiB, %9llu blocks checked\n", name, ranges.size(), t.pieces.size(),
           t.chunks.size(), t.bytes / 1024.0, (unsigned long long)blocks);
}

static void layouts() {
    printf("layouts:\n");
    const uint32_t a = 0xAC455000;  // the terrain array's address
    layout("terrain array 1024x1024 x83 BC1 sRGB tm4", desc(a, kBC1S, 1024, 1024, 83, E::Arr, 4, 11), kBC1S, 1);
    layout("... base swizzle 700, mip chain swizzle 300", desc(a, kBC1S, 1024, 1024, 83, E::Arr, 4, 11, 0x700, 0x300), kBC1S, 1);
    layout("cube map 256x256 RGBA8 tm4", desc(0x30001000, kRGBA8, 256, 256, 6, E::Cube, 4, 9, 0x200, 0x500), kRGBA8, 1);
    layout("array 128x64 x7 R8 tm4", desc(0x30801000, kR8, 128, 64, 7, E::Arr, 4, 8, 0x100, 0x600), kR8, 1);
    layout("array 64x64 x5 R8 tm4 (narrower than a macro tile)", desc(0x30901000, kR8, 64, 64, 5, E::Arr, 4, 7), kR8, 1);
    layout("array 96x40 x3 RG8 tm4", desc(0x30A01000, kRG8, 96, 40, 3, E::Arr, 4, 7, 0x400, 0x100), kRG8, 1);
    layout("array 300x200 x5 RGBA16F tm4", desc(0x30B01000, kRGBA16F, 300, 200, 5, E::Arr, 4, 9, 0x300, 0x700), kRGBA16F, 1);
    layout("array 64x64 x4 RGBA32F tm4", desc(0x31001000, kRGBA32F, 64, 64, 4, E::Arr, 4, 7, 0x500, 0x200), kRGBA32F, 1);
    layout("array 512x512 x4 BC3 tm4", desc(0x31101000, kBC3, 512, 512, 4, E::Arr, 4, 10, 0x600, 0x400), kBC3, 1);
    layout("array 256x128 x3 BC5 tm8 (bank swapped)", desc(0x31601000, kBC5, 256, 128, 3, E::Arr, 8, 9, 0x200, 0x200), kBC5, 1);
    for (uint32_t tm : {5u, 6u, 9u, 10u, 12u, 14u}) {
        char n[64];
        snprintf(n, sizeof n, "array 128x128 x6 RGBA8 tm%u", tm);
        layout(n, desc(0x31801000, kRGBA8, 128, 128, 6, E::Arr, tm, 8, 0x300, 0x100), kRGBA8, 1);
    }
    layout("array 100x60 x3 RGBA8 linear aligned", desc(0x32001000, kRGBA8, 100, 60, 3, E::Arr, 1, 1), kRGBA8, 1);
    layout("array 80x80 x9 R5G6B5 tm2 (1D tiled)", desc(0x32101000, kR565, 80, 80, 9, E::Arr, 2, 7), kR565, 1);
    layout("array 32x32 x32 RGBA8 tm7 (2D thick)", desc(0x32201000, kRGBA8, 32, 32, 32, E::Arr, 7, 6), kRGBA8, -1);
    layout("array 64x64 x8 RG8 tm3 (1D thick)", desc(0x32401000, kRG8, 64, 64, 8, E::Arr, 3, 7), kRG8, -1);
    layout("3D 64x64x16 RGBA8 tm4", desc(0x32601000, kRGBA8, 64, 64, 16, E::D3, 4, 7), kRGBA8, 0);
    layout("3D 32x32x8 RGBA8 tm7 (thick)", desc(0x32801000, kRGBA8, 32, 32, 8, E::D3, 7, 6), kRGBA8, 0);
    layout("2D 512x256 RGBA8 tm4 (levels only)", desc(0x32A01000, kRGBA8, 512, 256, 1, E::D2, 4, 10, 0x100, 0x700), kRGBA8, 0);
    layout("2D 2048x2048 BC1 tm4", desc(0x33001000, kBC1, 2048, 2048, 1, E::D2, 4, 12, 0x500, 0x500), kBC1, 0);
    layout("2D 1280x720 R8 tm4, no mip address", desc(0x34001000, kR8, 1280, 720, 1, E::D2, 4, 1), kR8, 0);
}

// 2. a texture cache over guest memory (check_texture with write tracking)
struct Tex {
    const char* name;
    texpieces::Desc d;
    Fmt f;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    texpieces::Table t;
    texchurn::Watch w;
    bool watched = false, uploaded = false;
    uint64_t contentHash = 0;
    std::vector<uint8_t> copy;     // what the uploads put in the texture: each piece's guest bytes at its last upload
    std::vector<size_t> at;        // each piece's place in copy
    std::vector<uint64_t> upHash;  // the race's uploads: each piece's chunk hashes, hashed, at its last upload
    uint64_t checks = 0, partial = 0, full = 0, fullUploads = 0, pieceUploads = 0, hashed = 0, unarmed = 0;
};

static void setup(Tex& x) {
    x.ranges = texpieces::level_ranges_of(x.d);
    x.t = table_of(x.d, x.f);
    x.at.clear();
    size_t n = 0;
    for (auto& p : x.t.pieces) {
        x.at.push_back(n);
        n += p.size;
    }
    x.copy.assign(n, 0);
    x.upHash.assign(x.t.pieces.size(), 0);
}

static uint64_t piece_hash(const texpieces::Table& t, uint32_t p) {
    const texpieces::Piece& pc = t.pieces[p];
    return XXH3_64bits(t.hashes.data() + pc.chunk0, pc.chunks * sizeof(uint64_t));
}

static bool same(const Tex& x, uint32_t p) {
    const texpieces::Piece& pc = x.t.pieces[p];
    return !memcmp(x.copy.data() + x.at[p], g_mem + pc.addr, pc.size);
}

static void upload(Tex& x, uint32_t p, bool exact) {
    const texpieces::Piece& pc = x.t.pieces[p];
    if (exact) memcpy(x.copy.data() + x.at[p], g_mem + pc.addr, pc.size);
    else x.upHash[p] = piece_hash(x.t, p);
}

// one check; exact: the writes are done (no other thread), so the uploaded copy must equal guest memory
// (compared after every check, or after hashing ones only)
static void check(Tex& x, uint64_t frame, bool force, bool churnOn, bool partialOn, bool exact, bool verifyClean = true) {
    x.checks++;
    const texchurn::Check c = texchurn::check(x.w, x.ranges, frame, force || !x.watched, churnOn);
    x.unarmed += c.unarmed;
    if (c.hash) {
        x.watched = true;
        if (partialOn && x.uploaded && texpieces::partial_ok(x.t, c)) {
            static std::vector<uint32_t> changed;
            changed.clear();
            x.hashed += texpieces::rehash_written(x.t, g_mem, c.since, changed, xxh);
            x.contentHash = texpieces::content(x.t, xxh);
            texpieces::hashed(x.t, x.w);
            if (exact)  // exactly the pieces whose bytes changed
                for (uint32_t p = 0, i = 0; p < x.t.pieces.size(); p++) {
                    const bool listed = i < changed.size() && changed[i] == p;
                    i += listed;
                    if (listed == same(x, p)) {
                        printf("%s frame %llu: piece %u (level %u, slices %u-%u) %s\n", x.name, (unsigned long long)frame, p, x.t.pieces[p].level,
                               x.t.pieces[p].z0, x.t.pieces[p].z1, listed ? "uploaded unchanged" : "changed, not uploaded");
                        assert(false);
                    }
                }
            for (uint32_t p : changed) upload(x, p, exact);
            x.partial++;
            x.pieceUploads += changed.size();
        } else {
            texpieces::hash_chunks(x.t, g_mem, 0, x.t.chunks.size(), xxh);
            const uint64_t h = texpieces::content(x.t, xxh);
            texpieces::hashed(x.t, x.w);
            x.hashed += x.t.bytes;
            x.full++;
            if (h != x.contentHash || !x.uploaded) {
                for (uint32_t p = 0; p < x.t.pieces.size(); p++) upload(x, p, exact);
                x.contentHash = h;
                x.uploaded = true;
                x.fullUploads++;
            }
        }
    }
    if (exact && (c.hash || verifyClean))
        for (uint32_t p = 0; p < x.t.pieces.size(); p++)
            if (!same(x, p)) {
                printf("%s frame %llu: piece %u (level %u, slices %u-%u) stale after a %s check\n", x.name, (unsigned long long)frame, p,
                       x.t.pieces[p].level, x.t.pieces[p].z0, x.t.pieces[p].z1, !c.hash ? "clean" : "hashing");
                assert(false);
            }
}

// the uploads match guest memory now (race: by hashes)
static bool fresh_by_hash(Tex& x) {
    texpieces::Table now = x.t;
    texpieces::hash_chunks(now, g_mem, 0, now.chunks.size(), xxh);
    for (uint32_t p = 0; p < now.pieces.size(); p++)
        if (piece_hash(now, p) != x.upHash[p]) return false;
    return true;
}

static std::mt19937 g_rng(7);
static void fill(uint32_t addr, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) g_mem[addr + i] = (uint8_t)g_rng();
}
static void poke(uint32_t addr) { g_mem[addr] = (uint8_t)(g_mem[addr] + 1 + g_rng() % 255); }

// the pieces of slice z (all of them for levels not cut into slices)
static std::vector<uint32_t> slice_pieces(const Tex& x, uint32_t z) {
    std::vector<uint32_t> out;
    for (uint32_t p = 0; p < x.t.pieces.size(); p++)
        if (x.t.pieces[p].z0 <= z && z < x.t.pieces[p].z1) out.push_back(p);
    return out;
}

// the game's writes to texture x in frame f
static void stream(Tex& x, uint64_t f, uint32_t every) {  // a slice rewritten over three frames, every `every` frames
    const uint64_t k = f / every, phase = f % every;
    if (phase > 2) return;
    const uint32_t z = (uint32_t)((k * 37) % std::max(x.d.slices, 1u));
    const auto ps = slice_pieces(x, z);
    const texpieces::Piece& top = x.t.pieces[ps[0]];
    if (phase == 0) fill(top.addr, top.size / 2);
    if (phase == 1) fill(top.addr + top.size / 2, top.size - top.size / 2);
    if (phase == 2)
        for (size_t i = 1; i < ps.size(); i++) fill(x.t.pieces[ps[i]].addr, x.t.pieces[ps[i]].size);
}
static void unchanged(Tex& x) {  // a piece's bytes written again as they are: its pages fault, nothing changes
    const texpieces::Piece& pc = x.t.pieces[g_rng() % x.t.pieces.size()];
    for (uint32_t o = 0; o < pc.size; o += 4096) ((volatile uint8_t*)g_mem)[pc.addr + o] = g_mem[pc.addr + o];
}
static void around(Tex& x) {  // bytes next to the texture and between its levels (pages it shares)
    const uint32_t first = x.ranges.front().first;
    poke(first - 1 - g_rng() % 64);
    uint64_t end = 0;
    for (auto& [a, n] : x.ranges) end = std::max<uint64_t>(end, (uint64_t)a + n);
    poke((uint32_t)end + g_rng() % 64);
    for (size_t l = 0; l + 1 < x.ranges.size(); l++) {
        const uint64_t e = (uint64_t)x.ranges[l].first + x.ranges[l].second;
        if (e < x.ranges[l + 1].first) poke((uint32_t)(e + g_rng() % (x.ranges[l + 1].first - e)));
    }
    // what follows a level's last slice (padding the upload does not read)
    for (size_t l = 0; l < x.t.levels.size(); l++) {
        const texpieces::Piece& last = x.t.pieces[x.t.levelEnd[l] - 1];
        const uint64_t e = (uint64_t)last.addr + last.size, le = (uint64_t)x.ranges[l].first + x.ranges[l].second;
        if (e < le) poke((uint32_t)(e + g_rng() % (le - e)));
    }
}
static void pokes(Tex& x, int n) {
    for (int i = 0; i < n; i++) {
        const texpieces::Piece& pc = x.t.pieces[g_rng() % x.t.pieces.size()];
        poke(pc.addr + g_rng() % pc.size);
    }
}
static void host_write(Tex& x) {  // a file read into a piece (fs.cpp's FSReadFile brackets it so)
    const texpieces::Piece& pc = x.t.pieces[g_rng() % x.t.pieces.size()];
    const uint32_t n = std::min<uint32_t>(pc.size, 1 + g_rng() % 40000), o = (uint32_t)(g_rng() % (pc.size - n + 1));
    wwatch::HostWrite hw(pc.addr + o, n);
    fill(pc.addr + o, n);
}

struct Pattern {
    Tex tex;
    std::function<void(Tex&, uint64_t)> write;
    bool churn = false;  // the churn rule on for it (texture_churn.h), whatever the run says
};

static void place(Tex& x) {
    setup(x);
    for (auto& [a, n] : x.ranges) fill(a, n);
}

static void cache() {
    printf("texture cache (partial updates, real page protection):\n");
    // the patterns, one after another from base (each at 4 KiB past a MiB: slices straddle pages and chunks)
    auto make = [](uint32_t base) {
        std::vector<Pattern> v;
        uint32_t at = base;
        auto add = [&](const char* name, uint32_t off, const Fmt& f, uint32_t w, uint32_t h, uint32_t slices, uint32_t dim, uint32_t tm,
                       uint32_t mips, uint32_t swz, uint32_t mipSwz, std::function<void(Tex&, uint64_t)> write, bool churn = false) {
            Pattern p;
            p.tex.name = name;
            p.tex.d = desc(at + off, f, w, h, slices, dim, tm, mips, swz, mipSwz);
            p.tex.f = f;
            p.write = std::move(write);
            p.churn = churn;
            place(p.tex);
            uint64_t end = 0;
            for (auto& [a, n] : p.tex.ranges) end = std::max<uint64_t>(end, (uint64_t)a + n);
            at = (uint32_t)((end + (64u << 10) + 0xFFFFF) & ~0xFFFFFull);
            assert(at < kSize);
            v.push_back(std::move(p));
        };
        add("streamed BC1 array 512x512 x24", 0x1000, kBC1S, 512, 512, 24, E::Arr, 4, 10, 0, 0, [](Tex& x, uint64_t f) { stream(x, f, 7); });
        add("streamed + around + unchanged", 0x3000, kBC3, 256, 256, 9, E::Arr, 4, 9, 0x200, 0x600, [](Tex& x, uint64_t f) {
            stream(x, f, 5);
            if (f % 3 == 0) around(x);
            if (f % 4 == 1) unchanged(x);
        });
        add("cube map, random pokes", 0x1000, kRGBA8, 128, 128, 6, E::Cube, 4, 8, 0x300, 0x100, [](Tex& x, uint64_t f) {
            if (f % 2 == 0) pokes(x, 1 + (int)(g_rng() % 6));
        });
        add("2D with mips, host writes", 0x2000, kRGBA8, 256, 256, 1, E::D2, 4, 9, 0, 0, [](Tex& x, uint64_t f) {
            if (f % 6 == 0) host_write(x);
            if (f % 9 == 4) around(x);
        });
        add("small levels sharing pages", 0x1000, kR8, 128, 64, 16, E::Arr, 4, 8, 0x100, 0x500, [](Tex& x, uint64_t f) {
            if (f % 3 == 0) pokes(x, 2);
            if (f % 5 == 0) stream(x, f / 5 * 7, 7);
        });
        add("3D, one piece per level", 0x1000, kRGBA8, 64, 64, 16, E::D3, 4, 7, 0, 0, [](Tex& x, uint64_t f) {
            if (f % 4 == 0) pokes(x, 3);
            if (f % 10 == 5) around(x);
        });
        add("thick array", 0x1000, kRGBA8, 32, 32, 32, E::Arr, 7, 6, 0, 0, [](Tex& x, uint64_t f) {
            if (f % 3 == 1) pokes(x, 2);
        });
        add("rewritten every frame (churn rule)", 0x1000, kRGBA8, 64, 64, 1, E::D2, 4, 1, 0, 0,
            [](Tex& x, uint64_t) { fill(x.ranges[0].first, x.ranges[0].second); }, true);
        return v;
    };
    constexpr uint64_t kFrames = 700;
    // churnAll: the churn rule on for every pattern (its unarmed spells and re-arming between partial checks)
    auto play = [](std::vector<Pattern>& v, const std::function<bool(uint64_t)>& partialOn, bool churnAll) {
        for (uint64_t f = 1; f < kFrames; f++) {
            for (auto& p : v) p.write(p.tex, f);
            for (auto& p : v) check(p.tex, f, f % 37 == 0, churnAll || p.churn, partialOn(f), true);
        }
    };
    std::vector<Pattern> on = make(0x0100000), off = make(0x2000000), ab = make(0x4000000);
    play(on, [](uint64_t) { return true; }, false);
    play(off, [](uint64_t) { return false; }, false);
    play(ab, [](uint64_t f) { return (f / 97) % 2 == 0; }, true);  // the in-run A/B: partial and full in turns
    printf("  %-36s %28s %32s\n", "", "full: checks/uploads/MiB hashed", "partial: checks/pieces/MiB hashed");
    for (size_t i = 0; i < on.size(); i++) {
        const Tex &a = on[i].tex, &b = off[i].tex;
        printf("  %-36s %9llu/%llu/%8.1f %13llu/%llu/%8.1f (%llu full)\n", a.name, (unsigned long long)(b.full + b.partial),
               (unsigned long long)b.fullUploads, b.hashed / 1048576.0, (unsigned long long)a.partial, (unsigned long long)a.pieceUploads,
               a.hashed / 1048576.0, (unsigned long long)a.full);
        assert(b.partial == 0);
    }
    auto by = [](std::vector<Pattern>& v, const char* name) -> Tex& {
        for (auto& p : v)
            if (!strcmp(p.tex.name, name)) return p.tex;
        assert(false);
        return v[0].tex;
    };
    // the streamed arrays: their checks after writes are partial, hash a fraction of what full checks do,
    // and upload pieces instead of the whole texture
    for (const char* n : {"streamed BC1 array 512x512 x24", "streamed + around + unchanged", "small levels sharing pages"}) {
        Tex &a = by(on, n), &b = by(off, n);
        assert(a.partial > 50 && a.pieceUploads > 0 && a.hashed * 4 < b.hashed);
    }
    assert(by(on, "cube map, random pokes").partial > 50 && by(on, "2D with mips, host writes").partial > 10);
    // the 3D and thick textures: one piece per level, still partial
    assert(by(on, "3D, one piece per level").partial > 20 && by(on, "thick array").partial > 20);
    // the texture rewritten every frame: the churn rule leaves it unarmed, so those checks hash it in full
    {
        Tex& a = by(on, "rewritten every frame (churn rule)");
        assert(a.unarmed > 100 && a.full > a.unarmed);
    }
    uint64_t abPartial = 0, abUnarmed = 0;
    for (auto& p : ab) abPartial += p.tex.partial, abUnarmed += p.tex.unarmed;
    assert(abPartial > 100 && abUnarmed > 100);
    printf("  switched every 97 frames, churn rule on: %llu partial checks, %llu unarmed; every check matched guest memory\n",
           (unsigned long long)abPartial, (unsigned long long)abUnarmed);
}

// BotW's terrain array at its real size: a slice streamed over three frames, every 20 frames
static void terrain() {
    Tex on, off;
    on.name = off.name = "terrain array";
    on.f = off.f = kBC1S;
    on.d = desc(0x6001000, kBC1S, 1024, 1024, 83, E::Arr, 4, 11);
    off.d = desc(0xA001000, kBC1S, 1024, 1024, 83, E::Arr, 4, 11);
    place(on);
    place(off);
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t f = 1; f < 200; f++) {
        stream(on, f, 20);
        stream(off, f, 20);
        check(on, f, false, true, true, true, false);
        check(off, f, false, true, false, true, false);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("terrain array (%zu pieces, %.1f MiB): full %llu checks hashing %.0f MiB, %llu uploads of it all; partial %llu checks hashing "
           "%.1f MiB, %llu pieces uploaded (%.0f ms)\n",
           on.t.pieces.size(), on.t.bytes / 1048576.0, (unsigned long long)(off.full + off.partial), off.hashed / 1048576.0,
           (unsigned long long)off.fullUploads, (unsigned long long)on.partial, on.hashed / 1048576.0, (unsigned long long)on.pieceUploads,
           ms);
    assert(on.partial >= 25 && (on.hashed - on.t.bytes) * 50 < off.hashed - off.t.bytes && on.fullUploads == 1);
    // each streamed slice: its pieces only (11 levels), the first half of level 0 and the rest a frame later
    assert(on.pieceUploads <= on.partial * 11);
}

// 3. a writer thread racing the checks, and host writes from it
static void race() {
    Tex x;
    x.name = "racing writer";
    x.f = kBC1;
    x.d = desc(0xE001000, kBC1, 256, 256, 16, E::Arr, 4, 9, 0x200, 0x400);
    place(x);
    std::mt19937 rng(11);
    uint64_t frame = 1, partial = 0;
    for (int round = 0; round < 300; round++) {
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> writes{0};
        std::thread writer([&, seed = (unsigned)rng()] {
            std::mt19937 r(seed);
            while (!stop.load(std::memory_order_relaxed)) {
                const texpieces::Piece& pc = x.t.pieces[r() % x.t.pieces.size()];
                if (r() % 64 == 0) {
                    const uint32_t n = std::min<uint32_t>(pc.size, 64), o = r() % (pc.size - n + 1);
                    wwatch::HostWrite hw(pc.addr + o, n);
                    for (uint32_t i = 0; i < n; i++) g_mem[pc.addr + o + i] = (uint8_t)r();
                } else {
                    g_mem[pc.addr + r() % pc.size] = (uint8_t)r();
                }
                writes.fetch_add(1, std::memory_order_relaxed);
            }
        });
        const bool churn = round % 3 != 2;
        const int checks = 1 + (int)(rng() % 40);
        for (int i = 0; i < checks; i++) {
            const uint64_t until = writes.load(std::memory_order_relaxed) + 1 + rng() % 200;
            while (writes.load(std::memory_order_relaxed) < until) std::this_thread::yield();
            const uint64_t before = x.partial, f = frame++;
            check(x, f, f % 11 == 0, churn, true, false);
            partial += x.partial - before;
        }
        stop = true;
        writer.join();
        check(x, frame++, false, churn, true, false);  // the writer's last writes: faulted after the arm, or left unarmed
        assert(fresh_by_hash(x));
        check(x, frame++, false, churn, true, false);
        assert(fresh_by_hash(x));
    }
    assert(partial > 100);
    printf("racing writer: 300 rounds, %llu partial checks (%llu pieces uploaded), %llu full; uploads matched guest memory after each\n",
           (unsigned long long)x.partial, (unsigned long long)x.pieceUploads, (unsigned long long)x.full);
}

static void runs() {
    texpieces::Table t;
    std::vector<texpieces::Level> lv = {{0x10000, 4 * 4096, 4, 4096}, {0x20000, 4 * 1024, 4, 1024}, {0x30000, 512, 4, 0}};
    texpieces::build(t, lv);
    assert(t.pieces.size() == 9 && t.levelEnd == (std::vector<uint32_t>{4, 8, 9}));
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> got;
    texpieces::for_runs(t, {0, 1, 3, 4, 5, 6, 7, 8}, [&](uint32_t l, uint32_t z0, uint32_t z1) { got.push_back({l, z0, z1}); });
    assert((got == std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>{{0, 0, 2}, {0, 3, 4}, {1, 0, 4}, {2, 0, 4}}));
    // a level whose slices would run past its range is not cut
    texpieces::build(t, {{0x10000, 3 * 4096, 4, 4096}});
    assert(t.pieces.size() == 1 && t.pieces[0].z1 == 4);
}

int main() {
    g_mem = (uint8_t*)mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(g_mem != (uint8_t*)MAP_FAILED);
    assert(wwatch::init(g_mem, kSize) && wwatch::active());
    runs();
    layouts();
    cache();
    terrain();
    race();
    printf("texture_pieces_test: layouts, cache patterns (partial, full, switched), terrain array and racing writer passed\n");
    return 0;
}
