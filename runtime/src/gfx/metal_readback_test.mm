// Game-free GPU regression: DrawDone must publish picking results before the CPU resumes.
// Run wwhd --metal-readback-test with WWHD_SYNC_WRITE_BACK unset.
#include "metal.h"
#include "runtime.h"
#include <array>
#include <cstring>
#include <stdexcept>

namespace gfx {
void write_back_linear_targets();
void drain_write_backs(bool);
void wait_idle();

int readback_test() {
    try {
        if (const char* sync = getenv("WWHD_SYNC_WRITE_BACK"); sync && atoi(sync))
            throw std::runtime_error("unset WWHD_SYNC_WRITE_BACK to test the default policy");
        mem::init();
        R.device = MTLCreateSystemDefaultDevice();
        R.queue = [R.device newCommandQueue];
        if (!R.queue) throw std::runtime_error("no Metal command queue");
        std::array<Surface, 5> targets;
        for (size_t i = 0; i < targets.size(); ++i) {
            auto& s = targets[i];
            s.width = s.height = i == 4 ? 3 : 4;
            s.pitch = 8; // exercise guest row padding as well as texel visibility
            s.format = i < 2 ? 0x1a : 0x823;
            s.fmt = format_info(s.format, false);
            s.addr = mem::host_alloc(4096, 256);
            memset(mem::ptr(s.addr), 0xa5, 4096);
            auto* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s.fmt.pixel
                        width:s.width height:s.height mipmapped:NO];
            td.storageMode = MTLStorageModePrivate;
            td.usage = MTLTextureUsageRenderTarget;
            s.tex = [R.device newTextureWithDescriptor:td];
            if (!s.tex) throw std::runtime_error("could not allocate test target");
            R.linearTargets.push_back(&s);
        }
        auto draw = [](Surface& s, bool occupied) {
            auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = s.tex;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = occupied ? MTLClearColorMake(1, 0, 1, 1) : MTLClearColorMake(0, 0, 0, 0);
            auto encoder = [command_buffer() renderCommandEncoderWithDescriptor:pass];
            [encoder endEncoding];
            s.gpuWritten = true;
            ++s.writeSeq;
        };
        auto check = [](const Surface& s, bool occupied) {
            const uint8_t rgba8[4] = {255, 0, 255, 255};
            const float rgba32[4] = {1, 0, 1, 1};
            const uint8_t zero[16] = {};
            const void* expected = !occupied ? zero : s.format == 0x1a ? (const void*)rgba8 : (const void*)rgba32;
            const uint32_t b = s.fmt.bytesPerBlock;
            for (uint32_t y = 0; y < s.height; ++y) {
                const auto* row = mem::ptr(s.addr + y * s.pitch * b);
                for (uint32_t x = 0; x < s.width; ++x)
                    if (memcmp(row + x * b, expected, b))
                        throw std::runtime_error("GPU picking result is stale at DrawDone return");
                for (uint32_t x = s.width * b; x < s.pitch * b; ++x)
                    if (row[x] != 0xa5) throw std::runtime_error("readback overwrote guest row padding");
            }
        };
        // Alternate the game's two slots, including occupied -> empty transitions in each slot.
        for (unsigned frame = 0; frame < 8; ++frame) {
            const bool occupied = (frame / 2) % 2 == 0;
            auto& id = targets[frame % 2];
            auto& position = targets[2 + frame % 2];
            draw(id, occupied);
            draw(position, occupied);
            write_back_linear_targets();
            check(id, occupied);
            check(position, occupied);
            drain_write_backs(false);
            check(id, occupied); // an older pending copy must not restore a destroyed target
        }
        // The exposure chain retains asynchronous guest publication.
        draw(targets[4], false);
        write_back_linear_targets();
        if (*mem::ptr(targets[4].addr) != 0xa5)
            throw std::runtime_error("exposure readback unexpectedly became synchronous");
        wait_idle();
        drain_write_backs(true);
        check(targets[4], false);
        R.linearTargets.clear();
        LOG("[Metal readback test] PASS: alternating picking ID/position, destroyed targets, padding and async exposure");
        return 0;
    } catch (const std::exception& e) {
        wait_idle();
        drain_write_backs(true);
        R.linearTargets.clear();
        LOG("[Metal readback test] FAIL: %s", e.what());
        return 1;
    }
}
} // namespace gfx
