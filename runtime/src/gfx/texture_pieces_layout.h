// Where a texture's levels and slices are in guest memory, as the upload reads them (metal_surfaces.mm
// level_ranges and decode_level, with Cemu's address library), for partial updates (texture_pieces.h).
// Test: runtime/tools/texture_pieces_test.cpp checks every block the upload reads against its piece.
#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "texture_pieces.h"

namespace texpieces {

// what a surface's guest memory layout depends on (metal.h Surface)
struct Desc {
    uint32_t addr = 0, mipAddr = 0, format = 0, width = 1, height = 1, slices = 1, dim = 1, tileMode = 0, swizzle = 0, mips = 1;
};

// The guest range of every level the upload reads, base first: level 0 at the address, the others at
// their offsets from the mip chain's start (none without a mip address). A macro-tiled mip chain keeps
// its swizzle in the mip pointer's bits 8-10, which the address library strips (taken raw, level 1 was
// read up to 0x700 bytes off: zeros or neighbouring data, the black cards on Kakariko's roofs). Sizes are
// clamped to the 4 GiB guest space, so a bogus descriptor cannot make a hash read past it.
inline std::vector<std::pair<uint32_t, uint32_t>> level_ranges_of(const Desc& s) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (uint32_t level = 0; level < s.mips; level++) {
        uint32_t base;
        if (level == 0) base = s.addr;
        else if (!s.mipAddr) break;
        else {
            uint32_t sliceOffset = 0, sliceSize = 0;
            sint32 sub = 0;
            LatteAddrLib::CalculateMipAndSliceAddr(s.addr, s.mipAddr, (Latte::E_GX2SURFFMT)s.format, s.width, s.height, s.slices,
                                                   (Latte::E_DIM)s.dim, (Latte::E_HWTILEMODE)s.tileMode, s.swizzle, 0, level, 0,
                                                   &sliceOffset, &sliceSize, &sub);
            base = sliceOffset;
        }
        LatteAddrLib::AddrSurfaceInfo_OUT info{};
        LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)s.format, s.width, s.height, s.slices, (Latte::E_DIM)s.dim,
                                              Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)s.tileMode), 0, level, &info);
        out.push_back({base, (uint32_t)std::min<uint64_t>(info.surfSize, 0x100000000ull - base)});
    }
    return out;
}

// The levels' slices: slice z of a level is the z-th slice size of its range (CalculateMipAndSliceAddr),
// where the upload reads only that range for it: not a 3D texture (one upload per level), not a thick
// tile mode (4 slices interleaved), a slice size the address library computes the decode's way (its bpp
// is the decode's, bitsPerBlock), and, macro-tiled, a multiple of the 2 KiB the pipe and bank bits
// interleave (always so: a macro tile row of a level is at least 2 KiB, or the level is micro-tiled).
inline std::vector<Level> levels_of(const Desc& s, const std::vector<std::pair<uint32_t, uint32_t>>& ranges, uint32_t bitsPerBlock) {
    std::vector<Level> out;
    const bool is3D = s.dim == (uint32_t)Latte::E_DIM::DIM_3D;
    constexpr uint32_t kInterleave = LatteAddrLib::m_pipeInterleaveBytes * LatteAddrLib::m_pipes * LatteAddrLib::m_banks;
    for (uint32_t level = 0; level < (uint32_t)ranges.size(); level++) {
        LatteAddrLib::AddrSurfaceInfo_OUT info{};
        LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)s.format, s.width, s.height, s.slices, (Latte::E_DIM)s.dim,
                                              Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)s.tileMode), 0, level, &info);
        const auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
        const uint64_t sliceBytes = (uint64_t)info.pitch * info.height * info.bpp / 8;
        const bool separate = !is3D && !LatteAddrLib::TM_IsThick(tm) && info.bpp == bitsPerBlock && sliceBytes && sliceBytes < 0x100000000ull &&
                              (!Latte::TM_IsMacroTiled(tm) || sliceBytes % kInterleave == 0);
        Level L;
        L.addr = ranges[level].first;
        L.size = ranges[level].second;
        L.slices = is3D ? std::max(s.slices >> level, 1u) : std::max(s.slices, 1u);
        L.sliceBytes = separate ? (uint32_t)sliceBytes : 0;
        out.push_back(L);
    }
    return out;
}

}  // namespace texpieces
