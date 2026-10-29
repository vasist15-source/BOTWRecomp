#pragma once
// Register classes: what a register write has to do, looked up per register in tables built once
// (gx2_core.cpp, on the first register command) instead of walking range lists for every written
// word. Per register: whether its changes leave shader translation alone (shader_irrelevant_reg),
// whether the render-thread profiler's draw classifier counts it as a buffer/constant word
// (rprof::fast_class_reg), and the bits of it that can change a Metal shader key
// (metal_key_changes_ref). register_class_test checks every register against these functions.
// Ported from ZeldaWWHDRecomp bde7fb1 (idea from the GreenNaugahyde Android fork v0.9.5); the Metal
// key table is this port's.
#include <cstdint>
#include <vector>

#include "gx2_regs.h"

namespace gx2 {

// Register writes that can change how shaders are translated bump g_shader_state_gen; these don't:
// uniforms, uniform/vertex buffer addresses (the vertex stride does), and with addressMemo (Vulkan,
// WWHD_VK_SHADER_ADDRESS_MEMO=1) texture address words, which bind_stage resolves freshly.
inline bool shader_irrelevant_reg(uint32_t reg, bool addressMemo) {
    using Latte::REGADDR;
    if (addressMemo)
        for (uint32_t base : {uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS), uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS),
                              uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS)})
            if (reg >= base && reg < base + 7 * 18) {
                uint32_t word = (reg - base) % 7;
                if (word == 2 || word == 3) return true;
            }
    if (reg >= mmSQ_ALU_CONSTANT0_0 && reg < mmSQ_ALU_CONSTANT0_0 + 0x1000) return true;
    for (uint32_t base : {uint32_t(mmSQ_VTX_UNIFORM_BLOCK_START), uint32_t(mmSQ_PS_UNIFORM_BLOCK_START),
                          uint32_t(mmSQ_GS_UNIFORM_BLOCK_START)})
        if (reg >= base && reg < base + 7 * 16) return true;
    if (reg >= mmSQ_VTX_ATTRIBUTE_BLOCK_START && reg < mmSQ_VTX_ATTRIBUTE_BLOCK_START + 7 * 16) {
        uint32_t w = (reg - mmSQ_VTX_ATTRIBUTE_BLOCK_START) % 7;
        return w != 2;  // word 2 holds the stride
    }
    return false;
}

// Metal shader keys (metal_draw.mm) read, of the vertex and pixel stages' texture units 0-17: the
// dimension and integer-format bits always; the tile mode, data format, format bits and last mip level
// only for framebuffer fetch (a unit whose address is a bound colour buffer's); the address only as
// which colour buffers it is. These are the framebuffer-fetch bits of words 0-6.
constexpr uint32_t kTexFbBits[7] = {0x78, 0xFC000000u, 0, 0, 0x803, 0xF, 0};
// the colour buffers (bit per target) whose base is a texture address word's address
inline uint32_t fb_targets(const uint32_t* regs, uint32_t word2) {
    const uint32_t b = word2 << 8;
    if (!b) return 0;
    uint32_t m = 0;
    for (uint32_t i = 0; i < 8; i++)
        if (b == (regs[mmCB_COLOR0_BASE + i] & ~0xFFu)) m |= 1u << i;
    return m;
}

struct MetalKeyOptions {
    bool wideKeys = false;  // WWHD_MTL_WIDE_SHADER_KEYS=1: the wide key's texture word 1 bits too
    bool allTex = false;    // WWHD_SHADER_KEY_ALL_TEX=1: every shader-relevant register counts
    bool allRegs = false;   // WWHD_SHADER_KEY_ALL_REGS=1: every register outside the texture units counts
};

// The registers outside the texture units and samplers that a Metal shader key reads (metal_draw.mm:
// linkage_key and variant_key, and stage_state_hash for the wide key). Viewport, scissor, blend,
// stencil and the like changed on most draws and made every draw recompute both keys. The scissor and
// colour buffer sizes are read for framebuffer fetch (GetActiveColorBufferMask) but not tracked: they
// change often, and the wide key never had them either.
inline bool metal_key_reg(uint32_t reg) {
    using Latte::REGADDR;
    auto in = [&](uint32_t first, uint32_t count) { return reg >= first && reg < first + count; };
    if (in(mmSQ_PGM_START_VS, 2) || in(mmSQ_PGM_START_PS, 2) || in(mmSQ_PGM_START_FS, 2) || reg == mmVGT_STRMOUT_EN) return true;
    for (uint32_t b = 0; b < 4; b++)
        if (reg == mmVGT_STRMOUT_VTX_STRIDE_0 + b * 4) return true;
    return in(mmSQ_VTX_SEMANTIC_0, 32) || in(mmSPI_VS_OUT_ID_0, 10) || reg == mmSPI_VS_OUT_CONFIG || reg == mmPA_CL_VS_OUT_CNTL ||
           in(mmSPI_PS_IN_CONTROL_0, 2) || in(mmSPI_PS_INPUT_CNTL_0, 32) || reg == REGADDR::SQ_CONFIG || reg == mmCB_SHADER_MASK ||
           reg == mmCB_SHADER_CONTROL || reg == mmDB_SHADER_CONTROL || reg == mmSPI_INPUT_Z || reg == REGADDR::SX_ALPHA_TEST_CONTROL ||
           reg == REGADDR::PA_CL_VTE_CNTL || reg == REGADDR::PA_CL_CLIP_CNTL ||
           reg == REGADDR::PA_SU_SC_MODE_CNTL ||  // both faces culled: rasterization off
           reg == REGADDR::CB_COLOR_CONTROL || reg == REGADDR::CB_TARGET_MASK || in(mmCB_COLOR0_INFO, 8) || in(mmCB_COLOR0_BASE, 8) ||
           reg == REGADDR::DB_DEPTH_CONTROL || reg == REGADDR::VGT_GS_MODE || reg == mmSPI_INTERP_CONTROL_0;
}

// Whether a change of a register (old != val) can change a Metal shader key; regs is the register file
// before the write (for the colour buffer bases and a texture unit's address). Shader-irrelevant
// registers are left to the caller. The tables below give the same answer (register_class_test).
inline bool metal_key_changes_ref(uint32_t reg, uint32_t old, uint32_t val, const uint32_t* regs, const MetalKeyOptions& o) {
    using Latte::REGADDR;
    if (o.allTex) return true;
    for (uint32_t base : {uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS), uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS)}) {
        if (reg < base || reg >= base + 7 * 160) continue;
        const uint32_t unit = (reg - base) / 7, word = (reg - base) % 7;
        if (unit >= 18) return false;
        const uint32_t d = old ^ val;
        switch (word) {
        case 0: if (d & 7) return true; break;
        case 1: if (o.wideKeys && (d & 0x3F00000)) return true; break;
        case 2: return fb_targets(regs, old) != fb_targets(regs, val);
        case 4: if (d & 0x300) return true; break;
        default: break;
        }
        return (d & kTexFbBits[word]) && fb_targets(regs, regs[base + unit * 7 + 2]);
    }
    if (reg >= uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS) && reg < uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS) + 7 * 160)
        return false;  // geometry shaders aren't translated
    if (reg >= uint32_t(REGADDR::SQ_TEX_SAMPLER_WORD0_0) && reg < uint32_t(REGADDR::SQ_TEX_SAMPLER_WORD0_0) + 3 * 18 * 3) {
        // depth-compare bits: the wide key's (the translation takes depth compare from the instruction)
        const uint32_t word = (reg - uint32_t(REGADDR::SQ_TEX_SAMPLER_WORD0_0)) % 3;
        return o.wideKeys && word == 0 && ((old ^ val) & 0xF8000000) != 0;
    }
    return o.allRegs || metal_key_reg(reg);
}

enum : uint8_t {
    kRcShaderIrrelevant = 0x01,  // shader_irrelevant_reg()
    kRcProfFast = 0x02,          // rprof::fast_class_reg()
    kRcTexAddress = 0x04,        // a texture unit's address word: counts as the colour buffers it is (fb_targets)
    kRcTexFbWord = 0x08,         // a texture unit word with framebuffer-fetch bits (kTexFbBits)
};

struct RegisterClasses {
    uint8_t cls[kNumRegs];
    uint32_t keyBits[kNumRegs];  // Metal shader key: bits whose change always counts (0 for shader-irrelevant registers)

    template <class ProfFast>
    void build(bool addressMemo, const MetalKeyOptions& o, ProfFast profFast) {
        using Latte::REGADDR;
        for (uint32_t r = 0; r < kNumRegs; ++r) {
            const bool irrelevant = shader_irrelevant_reg(r, addressMemo);
            cls[r] = (irrelevant ? kRcShaderIrrelevant : 0) | (profFast(r) ? kRcProfFast : 0);
            keyBits[r] = 0;
            if (irrelevant) continue;
            if (o.allTex) {
                keyBits[r] = ~0u;
                continue;
            }
            bool tex = false;
            for (uint32_t base : {uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS), uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS)}) {
                if (r < base || r >= base + 7 * 160) continue;
                tex = true;
                const uint32_t unit = (r - base) / 7, word = (r - base) % 7;
                if (unit >= 18) break;
                if (word == 2) {
                    cls[r] |= kRcTexAddress;
                    break;
                }
                keyBits[r] = word == 0 ? 7 : word == 1 && o.wideKeys ? 0x3F00000 : word == 4 ? 0x300 : 0;
                if (kTexFbBits[word]) cls[r] |= kRcTexFbWord;
            }
            if (tex) continue;
            if (r >= uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS) && r < uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS) + 7 * 160) continue;
            if (r >= uint32_t(REGADDR::SQ_TEX_SAMPLER_WORD0_0) && r < uint32_t(REGADDR::SQ_TEX_SAMPLER_WORD0_0) + 3 * 18 * 3) {
                if (o.wideKeys && (r - uint32_t(REGADDR::SQ_TEX_SAMPLER_WORD0_0)) % 3 == 0) keyBits[r] = 0xF8000000;
                continue;
            }
            if (o.allRegs || metal_key_reg(r)) keyBits[r] = ~0u;
        }
    }

    // metal_key_changes_ref by table (old != val; regs as there)
    bool metal_key_changes(uint32_t reg, uint32_t old, uint32_t val, const uint32_t* regs) const {
        const uint32_t d = old ^ val;
        if (d & keyBits[reg]) return true;
        const uint8_t c = cls[reg];
        if (!(c & (kRcTexAddress | kRcTexFbWord))) return false;
        // texture words: the vertex stage's units follow the pixel stage's 160, so the word index is
        // the same counted from either base
        const uint32_t word = (reg - uint32_t(Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS)) % 7;
        if (c & kRcTexAddress) return fb_targets(regs, old) != fb_targets(regs, val);
        return (d & kTexFbBits[word]) && fb_targets(regs, regs[reg - word + 2]);
    }
};
static_assert((uint32_t(Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS) - uint32_t(Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS)) % 7 == 0,
              "texture word index from the pixel stage's base");

// Shader key bits per register for a key mask (Vulkan's vulkan_shader_key_mask by table): the mask's
// bits, or all bits where the mask does not cover the register (then any change counts).
struct ShaderKeyBits {
    std::vector<uint32_t> bits;
    void build(bool (*keyMask)(uint32_t reg, uint32_t& mask)) {
        bits.assign(kNumRegs, ~0u);
        for (uint32_t r = 0; r < kNumRegs; ++r) {
            uint32_t m;
            if (keyMask(r, m)) bits[r] = m;
        }
    }
    // a change of a register (old != value) changes the shader key
    bool changed(uint32_t reg, uint32_t old, uint32_t value) const { return ((old ^ value) & bits[reg]) != 0; }
};

}  // namespace gx2
