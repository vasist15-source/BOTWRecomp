// Exercise the production HLE and decoder with synthetic ADPCM only.
#include "runtime.h"
#undef HLE
#define HLE(lib, name) static void hle_impl_##lib##_##name(Cpu* c)
#include "../src/hle/ax.cpp"
#include <cassert>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace timebase { uint64_t now() { return 0; } }
namespace interp { const char* phase_name() { return "test"; } }
void log_msg(const char*, ...) {}
// Save-state entry points in ax.cpp are linked but not exercised here.
namespace audio { int channels() { return 2; } void flush() {} }

static void map(uint32_t address) {
    void* wanted = mem::ptr(address);
#ifdef _WIN32
    void* actual = VirtualAlloc(wanted, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* actual = mmap(wanted, 0x10000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
#endif
    assert(actual == wanted);
}

static void seek(unsigned index, uint32_t offset) {
    Cpu cpu{};
    cpu.r[3] = g_voices[index].vpb;
    cpu.r[4] = offset;
    hle_impl_snd_core_AXSetVoiceCurrentOffset(&cpu);
}

static void rebased_resume(uint32_t samples) {
    Voice& v = g_voices[0];
    v = Voice{};
    v.vpb = g_vpb_base;
    v.acquired = true;
    v.format = ADPCM;
    v.samples = samples;
    v.cur_abs = base_units(ADPCM, samples) + 10;
    v.end_abs = base_units(ADPCM, samples) + 15;
    const uint64_t position = v.cur_abs;

    // An Ex setter changes the base used for relative offsets without moving
    // the current absolute position. A later pause/resume must round-trip it,
    // even while the cursor still precedes the new sample base.
    Cpu c{};
    c.r[3] = v.vpb;
    c.r[4] = 2;
    c.r[5] = samples + 0x100;
    hle_impl_snd_core_AXSetVoiceLoopOffsetEx(&c);
    c.r[3] = v.vpb;
    c.r[4] = 0x60008000;
    hle_impl_snd_core_AXGetVoiceOffsets(&c);
    const uint32_t saved = ld32(0x6000800C);
    assert(saved == uint32_t(-502));
    seek(0, saved);
    if (v.cur_abs != position) {
        fprintf(stderr, "rebased resume: expected %llx, got %llx (offset %08x)\n",
                (unsigned long long)position, (unsigned long long)v.cur_abs, saved);
        abort();
    }
    // The batch setter must have the same interpretation as the current-only
    // setter, including a negative end offset in an earlier segment.
    c.r[3] = v.vpb;
    c.r[4] = 0x60008000;
    hle_impl_snd_core_AXSetVoiceOffsets(&c);
    assert(v.cur_abs == position);
    assert(v.end_abs == base_units(ADPCM, samples) + 15);
    // Decode across the earlier segment's end into the later loop segment.
    // This tests the resulting sound and loop transition, not only addresses.
    for (uint32_t data : {samples, samples + 0x100}) {
        mem::ptr(data)[0] = 0;
        memset(mem::ptr(data) + 1, 0x11, 7);
    }
    v.state = 1;
    v.loop = 1;
    for (int i = 0; i < 6; ++i) assert(next_sample(v) == 1);
    assert(v.cur_abs == base_units(ADPCM, samples + 0x100) + 2);
    assert(v.loop_count == 1);
    // Ex setters must accept the same wrapped negative displacements.
    c.r[3] = v.vpb;
    c.r[4] = uint32_t(-497);
    c.r[5] = samples + 0x100;
    hle_impl_snd_core_AXSetVoiceEndOffsetEx(&c);
    assert(v.end_abs == base_units(ADPCM, samples) + 15);
    c.r[4] = uint32_t(-510);
    hle_impl_snd_core_AXSetVoiceLoopOffsetEx(&c);
    assert(v.loop_abs == base_units(ADPCM, samples) + 2);
    for (uint32_t offset : {0u, 1u, 2u, 15u, 0x7fffffffu,
                            0x80000000u, 0xfffffe0au, 0xfffffff0u, UINT32_MAX}) {
        seek(0, offset);
        // Independent byte-address oracle used by Cemu's ADPCM decoder.
        const int64_t displacement = offset < 0x80000000u
            ? int64_t(offset) : int64_t(offset) - 0x100000000ll;
        const int64_t byte_displacement = displacement >= 0
            ? displacement / 2 : (displacement - 1) / 2;
        assert(uint32_t(v.cur_abs >> 1) == uint32_t(int64_t(v.samples) + byte_displacement));
    }
}

int main() {
    map(0x60000000);
    map(0x10000000);
    map(0xA5F30000);
    g_vpb_base = 0x60001000;
    for (uint32_t samples : {0x100002B0u, 0xA5F302B0u}) rebased_resume(samples);
    for (uint32_t samples : {0x100002B0u, 0xA5F302B0u}) {
        for (unsigned i = 0; i < 2; ++i) {
            Voice& v = g_voices[i];
            v = Voice{};
            v.vpb = g_vpb_base + i * kVpbSize;
            v.acquired = true;
            v.state = 1;
            v.format = ADPCM;
            v.samples = samples + i * 0x100;
            v.loop = 1;
            v.loop_abs = base_units(ADPCM, v.samples) + 2;
            v.end_abs = base_units(ADPCM, v.samples) + 15;
            mem::ptr(v.samples)[0] = 0;  // predictor 0, scale 0
            memset(mem::ptr(v.samples) + 1, 0x11, 7);  // fourteen +1 samples
            seek(i, 2);
            assert(v.cur_abs == v.loop_abs);
        }
        const uint64_t other = g_voices[1].cur_abs;
        // NW4F 03C2376C..03C23784 resumes a paused ADPCM voice by rewinding
        // offsets 0 or 1 to the previous frame's last nibble (u32 -1).
        for (uint32_t saved : {0u, 1u}) {
            const uint32_t offset = saved - ((saved & 15) + 1);
            assert(offset == UINT32_MAX);
            seek(0, offset);
            Voice& v = g_voices[0];
            assert(v.cur_abs == base_units(ADPCM, v.samples) - 1);
            assert(ld32(v.vpb + kVpbOffsets + 0xC) == UINT32_MAX);
            assert(g_voices[1].cur_abs == other);
            assert(next_sample(v) == 0);  // last nibble before the first header
            for (int n = 0; n < 14 * 20; ++n) {
                assert(next_sample(v) == 1);
                assert(v.cur_abs >= v.loop_abs && v.cur_abs <= v.end_abs);
            }
            assert(v.loop_count >= 20);
        }
        // Ordinary seeking and absolute addresses above 32-bit nibble space
        // must retain their high bit; truncating the whole address is wrong.
        seek(0, 8);
        assert(g_voices[0].cur_abs == base_units(ADPCM, samples) + 8);
        const uint64_t first = g_voices[0].cur_abs;
        seek(1, UINT32_MAX);
        assert(g_voices[1].cur_abs == base_units(ADPCM, g_voices[1].samples) - 1);
        assert(g_voices[0].cur_abs == first);
        // ADPCM's relative nibble offset is signed even though the API carries
        // its two's-complement bits in u32. PCM retains its unsigned semantics.
        seek(0, 0x80000000u);
        assert(g_voices[0].cur_abs == uint64_t(int64_t(base_units(ADPCM, samples)) - 0x80000000ll));
        for (uint16_t format : {uint16_t(PCM16), uint16_t(PCM8)}) {
            g_voices[0].format = format;
            for (uint32_t offset : {8u, 0x80000000u, UINT32_MAX}) {
                seek(0, offset);
                assert(g_voices[0].cur_abs == base_units(format, samples) + offset);
            }
        }
    }
    puts("AX resume: rebased signed offsets, batch/Ex setters, low/high buffers, decode/loop, independent voices and PCM passed");
}
