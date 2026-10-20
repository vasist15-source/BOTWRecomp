// zlib125: the zlib 1.2.5 the Wii U ships, mapped to the host's zlib. The game's z_stream (0x38 bytes,
// big-endian, 32-bit pointers; Cemu's z_stream_ppc2) is translated in and out on every call; zlib's own
// state stays on the host, keyed by the z_stream's address (the game never reads it), so the guest's
// zalloc/zfree are not used. The stream's state field holds a nonzero token while one is open.
#include <zlib.h>

#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "../runtime.h"

namespace {

// z_stream_ppc2: +0 next_in, +4 avail_in, +8 total_in, +C next_out, +10 avail_out, +14 total_out,
// +18 msg, +1C state, +20 zalloc, +24 zfree, +28 opaque, +2C data_type, +30 adler, +34 reserved
struct Stream {
    z_stream z{};
    bool deflating = false;
};
std::mutex g_mu;
std::map<uint32_t, std::unique_ptr<Stream>> g_streams;

Stream* find(uint32_t gs) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_streams.find(gs);
    return it == g_streams.end() ? nullptr : it->second.get();
}
void load(uint32_t gs, z_stream& z) {
    z.next_in = ld32(gs + 0x0) ? mem::ptr(ld32(gs + 0x0)) : nullptr;
    z.avail_in = ld32(gs + 0x4);
    z.total_in = ld32(gs + 0x8);
    z.next_out = ld32(gs + 0xC) ? mem::ptr(ld32(gs + 0xC)) : nullptr;
    z.avail_out = ld32(gs + 0x10);
    z.total_out = ld32(gs + 0x14);
}
void store(uint32_t gs, const z_stream& z, bool open) {
    const uint32_t in = ld32(gs + 0x0), out = ld32(gs + 0xC);
    if (in && z.next_in) st32(gs + 0x0, in + (uint32_t)(z.next_in - mem::ptr(in)));
    st32(gs + 0x4, z.avail_in);
    st32(gs + 0x8, (uint32_t)z.total_in);
    if (out && z.next_out) st32(gs + 0xC, out + (uint32_t)(z.next_out - mem::ptr(out)));
    st32(gs + 0x10, z.avail_out);
    st32(gs + 0x14, (uint32_t)z.total_out);
    st32(gs + 0x18, 0);              // msg: no guest copy of zlib's message
    st32(gs + 0x1C, open ? gs : 0);  // state: a token
    st32(gs + 0x2C, (uint32_t)z.data_type);
    st32(gs + 0x30, (uint32_t)z.adler);
}
// writes into guest memory go to next_out: texture pages watched for writes stay correct
int run(uint32_t gs, bool deflating, int (*fn)(z_streamp, int), int flush) {
    Stream* s = find(gs);
    if (!s || s->deflating != deflating) return Z_STREAM_ERROR;
    load(gs, s->z);
    const int r = fn(&s->z, flush);
    store(gs, s->z, true);
    return r;
}
int end(uint32_t gs, bool deflating) {
    std::unique_ptr<Stream> s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_streams.find(gs);
        if (it == g_streams.end() || it->second->deflating != deflating) return Z_STREAM_ERROR;
        s = std::move(it->second);
        g_streams.erase(it);
    }
    load(gs, s->z);
    const int r = deflating ? deflateEnd(&s->z) : inflateEnd(&s->z);
    store(gs, s->z, false);
    return r;
}
template <class Init>
int open(uint32_t gs, bool deflating, Init init) {
    auto s = std::make_unique<Stream>();
    s->deflating = deflating;
    load(gs, s->z);
    const int r = init(&s->z);
    store(gs, s->z, r == Z_OK);
    if (r == Z_OK) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_streams[gs] = std::move(s);
    }
    return r;
}

}  // namespace

// (z_stream*, windowBits, version, stream size)
HLE(zlib125, inflateInit2_) {
    const int bits = (int)arg(c, 1);
    ret(c, (uint32_t)open(arg(c, 0), false, [&](z_streamp z) { return inflateInit2(z, bits); }));
}
HLE(zlib125, inflateInit_) { ret(c, (uint32_t)open(arg(c, 0), false, [](z_streamp z) { return inflateInit(z); })); }
HLE(zlib125, inflate) { ret(c, (uint32_t)run(arg(c, 0), false, ::inflate, (int)arg(c, 1))); }
HLE(zlib125, inflateEnd) { ret(c, (uint32_t)end(arg(c, 0), false)); }
// (z_stream*, level, method, windowBits, memLevel, strategy, version, stream size)
HLE(zlib125, deflateInit2_) {
    const int level = (int)arg(c, 1), method = (int)arg(c, 2), bits = (int)arg(c, 3), memLevel = (int)arg(c, 4), strategy = (int)arg(c, 5);
    ret(c, (uint32_t)open(arg(c, 0), true, [&](z_streamp z) { return deflateInit2(z, level, method, bits, memLevel, strategy); }));
}
HLE(zlib125, deflateInit_) {
    const int level = (int)arg(c, 1);
    ret(c, (uint32_t)open(arg(c, 0), true, [&](z_streamp z) { return deflateInit(z, level); }));
}
HLE(zlib125, deflate) { ret(c, (uint32_t)run(arg(c, 0), true, ::deflate, (int)arg(c, 1))); }
HLE(zlib125, deflateEnd) { ret(c, (uint32_t)end(arg(c, 0), true)); }
