// Register class tables (runtime/src/gx2/register_class.h) and thread identities (thread_identity.h):
// for every register, the tables give exactly what the functions they are built from say (shader
// relevance with and without the texture-address memo, the profiler's buffer/constant class, and
// whether a change can change a Metal shader key, for every single-bit change and a few wider ones,
// in each key mode) and, for the Vulkan shader key mask, the same answer as vulkan_shader_key_mask.
#include "register_class.h"
#include "render_prof.h"
#include "shader_key_dirty.h"
#include "thread_identity.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <set>
#include <thread>
#include <vector>

using namespace gx2;
using Latte::REGADDR;

static int failures = 0;
static void fail(const char* what, uint32_t reg) {
    if (++failures <= 20) std::printf("register %04X: %s\n", reg, what);
}
static bool prof_fast(uint32_t reg) { return rprof::fast_class_reg(reg); }

int main() {
    // a register file where a few texture units sample bound colour buffers (framebuffer fetch)
    std::vector<uint32_t> regs(kNumRegs);
    for (uint32_t r = 0; r < kNumRegs; ++r) regs[r] = r * 0x9E3779B9u;
    for (uint32_t i = 0; i < 8; i++) regs[mmCB_COLOR0_BASE + i] = 0x30000000u + i * 0x100000u;
    for (uint32_t base : {uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS), uint32_t(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS)})
        for (uint32_t u = 0; u < 18; u++) regs[base + u * 7 + 2] = u % 3 == 0 ? (0x30000000u + (u % 8) * 0x100000u) >> 8 : 0x400000u + u;

    MetalKeyOptions modes[4];
    modes[1].wideKeys = true;
    modes[2].allTex = true;
    modes[3].allRegs = true;
    for (bool memo : {false, true})
        for (const MetalKeyOptions& o : modes) {
            auto classes = std::make_unique<RegisterClasses>();
            classes->build(memo, o, prof_fast);
            for (uint32_t r = 0; r < kNumRegs; ++r) {
                const uint8_t c = classes->cls[r];
                const bool irrelevant = shader_irrelevant_reg(r, memo);
                if (bool(c & kRcShaderIrrelevant) != irrelevant) fail("shader relevance differs", r);
                if (bool(c & kRcProfFast) != prof_fast(r)) fail("profiler class differs", r);
                if (irrelevant) continue;
                auto check = [&](uint32_t old, uint32_t val) {
                    if (old == val) return;
                    if (classes->metal_key_changes(r, old, val, regs.data()) != metal_key_changes_ref(r, old, val, regs.data(), o))
                        fail("Metal key change differs", r);
                };
                for (int b = 0; b < 32; ++b) check(regs[r], regs[r] ^ (1u << b));
                for (uint32_t delta : {0x3u, 0x80000001u, 0xFFFFFFFFu, 0x00F000F0u}) check(regs[r], regs[r] ^ delta);
                check(0x300000u, 0x300000u);
                check(0x300000u, 0x301000u);  // one colour buffer's address to another's
                check(0x301000u, 0x400000u);  // to none
            }
        }

    // spot checks of the Metal key rules themselves
    {
        const MetalKeyOptions narrow, wide{true};
        const uint32_t ps = REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, sampler = REGADDR::SQ_TEX_SAMPLER_WORD0_0;
        const uint32_t* R = regs.data();
        if (!metal_key_changes_ref(ps + 1 * 7 + 0, 1, 2, R, narrow)) fail("texture dimension does not count", ps + 7);
        if (metal_key_changes_ref(ps + 1 * 7 + 1, 0, 0x04000000u, R, narrow)) fail("texture format counts without fb fetch", ps + 8);
        if (!metal_key_changes_ref(ps + 0 * 7 + 1, 0, 0x04000000u, R, narrow)) fail("fb-fetched texture format does not count", ps + 1);
        if (!metal_key_changes_ref(ps + 1 * 7 + 1, 0, 0x00100000u, R, wide)) fail("wide key texture word 1 bits do not count", ps + 8);
        if (metal_key_changes_ref(sampler, 0, 0x08000000u, R, narrow)) fail("sampler depth compare counts for the narrow key", sampler);
        if (!metal_key_changes_ref(sampler, 0, 0x08000000u, R, wide)) fail("sampler depth compare does not count for the wide key", sampler);
        if (!metal_key_changes_ref(REGADDR::VGT_GS_MODE, 0, 1, R, narrow)) fail("VGT_GS_MODE does not count", REGADDR::VGT_GS_MODE);
        if (!metal_key_changes_ref(mmSPI_INTERP_CONTROL_0, 0, 2, R, narrow)) fail("point sprite does not count", mmSPI_INTERP_CONTROL_0);
        if (metal_key_changes_ref(REGADDR::PA_SC_GENERIC_SCISSOR_BR, 0, 1, R, narrow)) fail("scissor counts", REGADDR::PA_SC_GENERIC_SCISSOR_BR);
        if (!shader_irrelevant_reg(mmSQ_ALU_CONSTANT0_0 + 5, false)) fail("ALU constant counted as shader-relevant", mmSQ_ALU_CONSTANT0_0 + 5);
        if (shader_irrelevant_reg(mmSQ_VTX_ATTRIBUTE_BLOCK_START + 2, false)) fail("vertex stride counted as shader-irrelevant", mmSQ_VTX_ATTRIBUTE_BLOCK_START + 2);
    }

    // the Vulkan shader key mask by table
    ShaderKeyBits bits;
    bits.build(vulkan_shader_key_mask);
    for (uint32_t r = 0; r < kNumRegs; ++r) {
        auto byMask = [&](uint32_t old, uint32_t value) {
            uint32_t mask;
            return !vulkan_shader_key_mask(r, mask) || ((old ^ value) & mask) != 0;
        };
        for (int b = 0; b < 32; ++b)
            if (bits.changed(r, 0x5A5A5A5Au, 0x5A5A5A5Au ^ (1u << b)) != byMask(0x5A5A5A5Au, 0x5A5A5A5Au ^ (1u << b))) fail("Vulkan key bit differs", r);
    }

    // thread identities (display-list recording slots): distinct for threads alive together
    {
        constexpr int kThreads = 8;
        uintptr_t ids[kThreads + 1] = {};
        std::vector<std::thread> threads;
        std::atomic<int> arrived{0};
        for (int i = 0; i < kThreads; ++i)
            threads.emplace_back([&, i] {
                ids[i] = thread_identity();
                ++arrived;
                while (arrived < kThreads) std::this_thread::yield();  // all alive at once
                if (thread_identity() != ids[i]) fail("thread identity not stable", uint32_t(i));
            });
        for (auto& t : threads) t.join();
        ids[kThreads] = thread_identity();
        if (std::set<uintptr_t>(ids, ids + kThreads + 1).size() != kThreads + 1) fail("thread identities collide", 0);
    }
    if (failures) {
        std::printf("register_class_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("register_class_test: ok\n");
    return 0;
}
