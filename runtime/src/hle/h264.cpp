// h264: the Wii U's H.264 decoder library (H264DEC*), which BotW uses for its pre-rendered movies
// (content/Movie/Demo*.mp4: most recovered memories and parts of the ending). The game demuxes the MP4
// itself and hands us one access unit at a time (Annex B: start codes, SPS/PPS in front of IDR frames);
// we decode it with VideoToolbox (hardware) and write the picture into the game's buffer as NV12, the
// layout the console's decoder produces: luma rows padded to 256 bytes, then the interleaved chroma.
//
// API semantics follow Cemu (Cafe/OS/libs/h264_avc/H264Dec.cpp):
//  - H264DECMemoryRequirement asks for 256 bytes of work memory (Cemuhook's BotW rule; our state is host side)
//  - buffered mode (the default, OUTPUT_PER_FRAME 0): H264DECExecute returns a frame through the game's
//    output callback once more than 5 are in flight; H264DECEnd returns the rest
//  - after a second H264DECBegin (a seek), inputs are refused until the next IDR frame
//  - a 1920x1088 stream is reported as 1080 rows (BotW with Switch movies)
// The movies are High profile with B-frames, and VideoToolbox returns pictures in decode order: they
// are handed to the game in display order (picture order count from the slice headers).
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "../runtime.h"
#include "../write_watch.h"

namespace {

enum : uint32_t { H264_OK = 0, H264_BAD_STREAM = 0x1000000, H264_INVALID_PARAM = 0x1010000 };
constexpr uint32_t kParamOutputFn = 0x1, kParamOutputPerFrame = 0x20000002, kParamUserMemory = 0x70000001;
constexpr uint32_t kBuffered = 5;  // frames kept in flight in buffered mode (Cemu's numFramesToBuffer)

// ---------------------------------------------------------------- bitstream parsing (just what we need)
// RBSP: the NAL payload with emulation prevention bytes (00 00 03) removed
std::vector<uint8_t> to_rbsp(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    for (size_t i = 0, zeros = 0; i < n; i++) {
        if (zeros >= 2 && p[i] == 3) { zeros = 0; continue; }
        zeros = p[i] == 0 ? zeros + 1 : 0;
        out.push_back(p[i]);
    }
    return out;
}
struct Bits {
    const uint8_t* d;
    size_t n, pos = 0;  // pos in bits
    bool ok = true;
    uint32_t bit() {
        if (pos >= n * 8) { ok = false; return 0; }
        uint32_t b = (d[pos >> 3] >> (7 - (pos & 7))) & 1;
        pos++;
        return b;
    }
    uint32_t u(int k) { uint32_t v = 0; while (k--) v = v << 1 | bit(); return v; }
    uint32_t ue() {
        int z = 0;
        while (!bit() && ok && z < 32) z++;
        return z ? ((1u << z) - 1 + u(z)) : 0;
    }
    int32_t se() { uint32_t v = ue(); return (v & 1) ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2); }
};
struct Sps {
    bool valid = false;
    uint32_t width = 0, height = 0;  // coded size in luma samples
    uint32_t log2MaxFrameNum = 4, pocType = 0, log2MaxPocLsb = 4;
    bool frameMbsOnly = true, separateColourPlane = false;
};
bool parse_sps(const uint8_t* nal, size_t n, Sps& s) {  // nal: after the 1-byte NAL header
    auto r = to_rbsp(nal, n);
    Bits b{r.data(), r.size()};
    const uint32_t profile = b.u(8);
    b.u(16);  // constraint flags, level
    b.ue();   // sps id
    s.separateColourPlane = false;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 ||
        profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        const uint32_t chroma = b.ue();
        if (chroma == 3) s.separateColourPlane = b.bit();
        b.ue();   // bit depth luma
        b.ue();   // bit depth chroma
        b.bit();  // qpprime y zero transform bypass
        if (b.bit()) {  // scaling matrices: skipped
            for (int i = 0; i < (chroma != 3 ? 8 : 12); i++) {
                if (!b.bit()) continue;
                int last = 8, next = 8;
                for (int j = 0; j < (i < 6 ? 16 : 64); j++) {
                    if (next) next = (last + b.se() + 256) % 256;
                    last = next ? next : last;
                }
            }
        }
    }
    s.log2MaxFrameNum = b.ue() + 4;
    s.pocType = b.ue();
    if (s.pocType == 0) {
        s.log2MaxPocLsb = b.ue() + 4;
    } else if (s.pocType == 1) {
        b.bit();
        b.se();
        b.se();
        for (uint32_t i = 0, k = b.ue(); i < k && b.ok; i++) b.se();
    }
    b.ue();   // max num ref frames
    b.bit();  // gaps in frame num allowed
    const uint32_t wMbs = b.ue() + 1, hMap = b.ue() + 1;
    s.frameMbsOnly = b.bit();
    s.width = wMbs * 16;
    s.height = hMap * 16 * (s.frameMbsOnly ? 1 : 2);
    s.valid = b.ok && s.width && s.height;
    return s.valid;
}
// the slice header up to pic_order_cnt_lsb (nal: after the 1-byte NAL header)
struct SliceInfo { uint32_t firstMb = 0, frameNum = 0, pocLsb = 0; bool ok = false; };
SliceInfo parse_slice(const uint8_t* nal, size_t n, uint32_t nalType, const Sps& s) {
    auto r = to_rbsp(nal, std::min<size_t>(n, 64));  // the fields we read are in the first bytes
    Bits b{r.data(), r.size()};
    SliceInfo si;
    si.firstMb = b.ue();
    b.ue();  // slice type
    b.ue();  // pps id
    if (s.separateColourPlane) b.u(2);
    si.frameNum = b.u((int)s.log2MaxFrameNum);
    if (!s.frameMbsOnly && b.bit()) b.bit();  // field_pic_flag, bottom_field_flag
    if (nalType == 5) b.ue();                 // idr_pic_id
    if (s.pocType == 0) si.pocLsb = b.u((int)s.log2MaxPocLsb);
    si.ok = b.ok;
    return si;
}
// NAL units of an Annex B buffer: (start, length) of each payload including its header byte
std::vector<std::pair<const uint8_t*, size_t>> split_nals(const uint8_t* p, size_t n) {
    std::vector<std::pair<const uint8_t*, size_t>> nals;
    size_t i = 0, start = SIZE_MAX;
    while (i + 2 < n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && p[end - 1] == 0) end--;  // trailing zero of a 4-byte start code
                nals.push_back({p + start, end - start});
            }
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    if (start != SIZE_MAX && start < n) nals.push_back({p + start, n - start});
    return nals;
}

// ---------------------------------------------------------------- decoder sessions
struct Frame {
    uint32_t image = 0;        // guest buffer the picture goes to (passed with the input that held it)
    double timestamp = 0;
    uint64_t order = 0;        // display order: IDR epoch, then picture order count
    uint32_t width = 0, height = 0, pitch = 0;
    bool decoded = false;
};
struct Session {
    // parameters (H264DECSetParam)
    uint32_t outputFn = 0, userMemory = 0;
    bool outputPerFrame = false;
    // bitstream (H264DECSetBitstream)
    uint32_t bsPtr = 0, bsLen = 0;
    double bsTimestamp = 0;
    // decoder
    bool begun = false, recovering = false;
    std::vector<uint8_t> spsBytes, ppsBytes;
    Sps sps;
    CMVideoFormatDescriptionRef format = nullptr;
    VTDecompressionSessionRef vt = nullptr;
    // picture order count (type 0) and the display order key
    int32_t prevPocMsb = 0, prevPocLsb = 0;
    uint64_t epoch = 0, decodeIndex = 0;
    std::vector<std::unique_ptr<Frame>> pending;  // decoded, not yet handed to the game
    uint32_t scratch = 0;                         // guest memory for the output callback's structs
    uint64_t framesOut = 0;
    ~Session() { reset_decoder(); }
    void reset_decoder() {
        if (vt) {
            VTDecompressionSessionInvalidate(vt);
            CFRelease(vt);
            vt = nullptr;
        }
        if (format) {
            CFRelease(format);
            format = nullptr;
        }
    }
};
std::mutex g_mu;
std::map<uint32_t, std::unique_ptr<Session>> g_sessions;  // by work memory address

// once a second while a movie plays: what the decoder did (to tell a slow decoder from a starved thread)
struct Stats {
    double windowStart = 0, decodeMs = 0, maxGapMs = 0, lastExec = 0;
    uint32_t executes = 0, decodes = 0, outputs = 0, changed = 0;
    uint64_t lastSum = 0;
} g_stats;
double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void stats_tick() {
    const double t = now_ms();
    if (!g_stats.windowStart) g_stats.windowStart = t;
    if (t - g_stats.windowStart < 1000) return;
    LOG("[h264] last %.0f ms: %u executes, %u decoded (%.1f ms each), %u frames out (%u with new pixels), longest gap between executes %.0f ms",
        t - g_stats.windowStart, g_stats.executes, g_stats.decodes, g_stats.decodes ? g_stats.decodeMs / g_stats.decodes : 0.0,
        g_stats.outputs, g_stats.changed, g_stats.maxGapMs);
    const uint64_t keepSum = g_stats.lastSum;
    const double keepLast = g_stats.lastExec;
    g_stats = {};
    g_stats.windowStart = t;
    g_stats.lastSum = keepSum;
    g_stats.lastExec = keepLast;
}

Session* session(uint32_t work) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sessions.find(work);
    return it == g_sessions.end() ? nullptr : it->second.get();
}

// VideoToolbox output (called before VTDecompressionSessionDecodeFrame returns: decoding is synchronous):
// the picture into the guest buffer as NV12 with 256-byte aligned rows
void vt_output(void*, void* ref, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef img, CMTime, CMTime) {
    Frame* f = (Frame*)ref;
    if (status != noErr || !img || !f->image) {
        static int logged = 0;
        if (logged++ < 10) LOG("[h264] decode failed (status %d)", (int)status);
        return;
    }
    CVPixelBufferRef pb = (CVPixelBufferRef)img;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint32_t w = (uint32_t)CVPixelBufferGetWidth(pb);
    uint32_t h = (uint32_t)CVPixelBufferGetHeight(pb);
    if (w == 1920 && h == 1088) h = 1080;  // BotW plays 1080p (Switch) movies cropped
    const uint32_t pitch = (w + 0xFF) & ~0xFFu;
    uint8_t* out = mem::ptr(f->image);
    const uint8_t* y = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    const uint8_t* uv = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    if (y && uv) {
        wwatch::HostWrite hw(f->image, pitch * h * 3 / 2);  // the texture cache sees the new picture
        for (uint32_t r = 0; r < h; r++) memcpy(out + (size_t)r * pitch, y + r * ys, w);
        uint8_t* uvOut = out + (size_t)pitch * h;
        for (uint32_t r = 0; r < h / 2; r++) memcpy(uvOut + (size_t)r * pitch, uv + r * uvs, w);
        f->decoded = true;
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    f->width = w;
    f->height = h;
    f->pitch = pitch;
}

bool make_decoder(Session& s) {
    s.reset_decoder();
    const uint8_t* sets[2] = {s.spsBytes.data(), s.ppsBytes.data()};
    const size_t sizes[2] = {s.spsBytes.size(), s.ppsBytes.size()};
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, sets, sizes, 4, &s.format);
    if (st != noErr) {
        LOG("[h264] format description from SPS/PPS failed (%d)", (int)st);
        return false;
    }
    const int32_t pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pf = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFormat);
    const void* keys[] = {kCVPixelBufferPixelFormatTypeKey};
    const void* vals[] = {pf};
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord cb{vt_output, nullptr};
    st = VTDecompressionSessionCreate(kCFAllocatorDefault, s.format, nullptr, attrs, &cb, &s.vt);
    CFRelease(attrs);
    CFRelease(pf);
    if (st != noErr) {
        LOG("[h264] VideoToolbox session failed (%d)", (int)st);
        s.reset_decoder();
        return false;
    }
    LOG("[h264] decoder: %ux%u (VideoToolbox)", s.sps.width, s.sps.height);
    return true;
}

// hands one decoded frame to the game's output callback (H264DECFrameOutput, Cemu's layout)
void output_frame(Cpu* c, Session& s, const Frame& f) {
    if (!s.outputFn) return;
    if (!s.scratch) s.scratch = mem::host_alloc(0x100, 0x40);
    const uint32_t cbs = s.scratch, arr = s.scratch + 0x10, fo = s.scratch + 0x20;
    memset(mem::ptr(s.scratch), 0, 0x100);
    st32(fo + 0x00, 100);  // result
    stf64(fo + 0x08, f.timestamp);
    st32(fo + 0x10, f.width);
    st32(fo + 0x14, f.height);
    st32(fo + 0x18, f.pitch);
    st32(fo + 0x44, f.image);
    st32(arr, fo);
    st32(cbs + 0x0, 1);  // frame count
    st32(cbs + 0x4, arr);
    st32(cbs + 0x8, s.userMemory);
    s.framesOut++;
    g_stats.outputs++;
    if (f.image && f.pitch) {
        uint64_t sum = 0;
        const uint8_t* p = mem::ptr(f.image);
        for (uint32_t r = 0; r < f.height; r += 37) for (uint32_t x = 0; x < f.width; x += 61) sum = sum * 31 + p[(size_t)r * f.pitch + x];
        if (sum != g_stats.lastSum) g_stats.changed++;
        g_stats.lastSum = sum;
    }
    guest_call(c, s.outputFn, {cbs});
}
// hands the earliest pending frame (display order) to the game
void output_next(Cpu* c, Session& s) {
    if (s.pending.empty()) return;
    auto it = std::min_element(s.pending.begin(), s.pending.end(), [](auto& a, auto& b) { return a->order < b->order; });
    std::unique_ptr<Frame> f = std::move(*it);
    s.pending.erase(it);
    if (!f->decoded && f->image && f->width == 0) {  // nothing decoded: a black picture instead of stale memory
        f->width = s.sps.width;
        f->height = s.sps.height == 1088 ? 1080 : s.sps.height;
        f->pitch = (f->width + 0xFF) & ~0xFFu;
        memset(mem::ptr(f->image), 16, (size_t)f->pitch * f->height);
        memset(mem::ptr(f->image) + (size_t)f->pitch * f->height, 128, (size_t)f->pitch * f->height / 2);
    }
    output_frame(c, s, *f);
}

// decodes the access unit set with H264DECSetBitstream; true if it held a picture
bool decode_unit(Session& s, uint32_t image) {
    if (!s.bsPtr || !s.bsLen) return false;
    const auto nals = split_nals(mem::ptr(s.bsPtr), s.bsLen);
    std::vector<uint8_t> avcc;  // the slices, length-prefixed
    bool idr = false, haveSlice = false, paramsChanged = false;
    uint32_t sliceType = 0, refIdc = 0;
    SliceInfo first;
    for (auto [p, n] : nals) {
        if (!n) continue;
        const uint32_t type = p[0] & 0x1F;
        if (type == 7) {
            if (s.spsBytes.size() != n || memcmp(s.spsBytes.data(), p, n)) {
                s.spsBytes.assign(p, p + n);
                paramsChanged = true;
                parse_sps(p + 1, n - 1, s.sps);
            }
        } else if (type == 8) {
            if (s.ppsBytes.size() != n || memcmp(s.ppsBytes.data(), p, n)) {
                s.ppsBytes.assign(p, p + n);
                paramsChanged = true;
            }
        } else if (type >= 1 && type <= 5) {
            if (!haveSlice) {
                first = parse_slice(p + 1, n - 1, type, s.sps);
                sliceType = type;
                refIdc = (p[0] >> 5) & 3;
            }
            haveSlice = true;
            idr = idr || type == 5;
            const uint32_t len = (uint32_t)n;
            const uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
            avcc.insert(avcc.end(), be, be + 4);
            avcc.insert(avcc.end(), p, p + n);
        }
    }
    if (!haveSlice) return false;
    if (s.recovering) {
        if (!idr) return false;
        s.recovering = false;
    }
    if ((paramsChanged || !s.vt) && !s.spsBytes.empty() && !s.ppsBytes.empty()) {
        if (s.vt) VTDecompressionSessionWaitForAsynchronousFrames(s.vt);
        make_decoder(s);
    }
    // display order: picture order count (type 0) within an IDR epoch; other types decode in display order
    if (idr) {
        s.epoch++;
        s.prevPocMsb = s.prevPocLsb = 0;
    }
    int32_t poc;
    if (s.sps.pocType == 0 && first.ok) {
        const int32_t maxLsb = 1 << s.sps.log2MaxPocLsb, lsb = (int32_t)first.pocLsb;
        int32_t msb = s.prevPocMsb;
        if (lsb < s.prevPocLsb && s.prevPocLsb - lsb >= maxLsb / 2) msb += maxLsb;
        else if (lsb > s.prevPocLsb && lsb - s.prevPocLsb > maxLsb / 2) msb -= maxLsb;
        poc = msb + lsb;
        if (refIdc) {
            s.prevPocMsb = msb;
            s.prevPocLsb = lsb;
        }
    } else {
        poc = (int32_t)(s.decodeIndex & 0xFFFFFF);
    }
    (void)sliceType;
    s.decodeIndex++;
    auto f = std::make_unique<Frame>();
    f->image = image;
    f->timestamp = s.bsTimestamp;
    f->order = (s.epoch << 32) | (uint32_t)(poc + 0x40000000);
    if (s.vt) {
        CMBlockBufferRef block = nullptr;
        CMSampleBufferRef sample = nullptr;
        OSStatus st = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, avcc.size(), kCFAllocatorDefault, nullptr, 0,
                                                         avcc.size(), kCMBlockBufferAssureMemoryNowFlag, &block);
        if (st == noErr) st = CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size());
        const size_t sampleSize = avcc.size();
        if (st == noErr) st = CMSampleBufferCreateReady(kCFAllocatorDefault, block, s.format, 1, 0, nullptr, 1, &sampleSize, &sample);
        if (st == noErr) {
            BlockingScope blocking;  // other threads on this core run meanwhile
            VTDecodeInfoFlags info = 0;
            const double t0 = now_ms();
            st = VTDecompressionSessionDecodeFrame(s.vt, sample, 0, f.get(), &info);
            g_stats.decodeMs += now_ms() - t0;
            g_stats.decodes++;
            if (st != noErr) {
                static int logged = 0;
                if (logged++ < 10) LOG("[h264] VTDecompressionSessionDecodeFrame: %d", (int)st);
            }
        }
        if (sample) CFRelease(sample);
        if (block) CFRelease(block);
    }
    s.pending.push_back(std::move(f));
    return true;
}

}  // namespace

// (profile, level, width, height, u32* size)
HLE(h264, H264DECMemoryRequirement) {
    if (arg(c, 4)) st32(arg(c, 4), 256);
    ret(c, arg(c, 4) ? H264_OK : H264_INVALID_PARAM);
}
HLE(h264, H264DECCheckMemSegmentation) { ret(c, 0); }  // any range is fine
// (stream, size, offset, u32* width, u32* height): the size in the first SPS from offset on
HLE(h264, H264DECGetImageSize) {
    const uint32_t stream = arg(c, 0), wOut = arg(c, 3), hOut = arg(c, 4);
    const int32_t size = (int32_t)arg(c, 1), off = (int32_t)arg(c, 2);
    if (!stream || size < 4 || off < 0 || off + 4 >= size || !wOut || !hOut) { ret(c, H264_INVALID_PARAM); return; }
    for (auto [p, n] : split_nals(mem::ptr(stream + off), (size_t)(size - off))) {
        if (!n || (p[0] & 0x1F) != 7) continue;
        Sps s;
        if (!parse_sps(p + 1, n - 1, s)) { ret(c, H264_BAD_STREAM); return; }
        st32(wOut, s.width);
        st32(hOut, s.width == 1920 && s.height == 1088 ? 1080 : s.height);
        ret(c, H264_OK);
        return;
    }
    ret(c, H264_BAD_STREAM);
}
// (size, work memory)
HLE(h264, H264DECInitParam) {
    const uint32_t work = arg(c, 1);
    if (work) memset(mem::ptr(work), 0, std::min<uint32_t>(arg(c, 0), 256));
    std::lock_guard<std::mutex> lk(g_mu);
    g_sessions[work] = std::make_unique<Session>();
    ret(c, 0);
}
HLE(h264, H264DECOpen) {
    const uint32_t work = arg(c, 0);
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_sessions.count(work)) g_sessions[work] = std::make_unique<Session>();
    LOG("[h264] session opened (work memory %08X)", work);
    ret(c, 0);
}
HLE(h264, H264DECClose) {
    const uint32_t work = arg(c, 0);
    std::lock_guard<std::mutex> lk(g_mu);
    if (auto it = g_sessions.find(work); it != g_sessions.end()) {
        LOG("[h264] session closed: %llu frames", (unsigned long long)it->second->framesOut);
        g_sessions.erase(it);
    }
    ret(c, 0);
}
HLE(h264, H264DECBegin) {
    if (Session* s = session(arg(c, 0))) {
        if (s->begun) s->recovering = true;  // playback resumes elsewhere: wait for an IDR frame
        s->begun = true;
    }
    ret(c, 0);
}
// flush: every frame still held, in display order
HLE(h264, H264DECEnd) {
    if (Session* s = session(arg(c, 0))) {
        if (s->vt) VTDecompressionSessionWaitForAsynchronousFrames(s->vt);
        while (!s->pending.empty()) output_next(c, *s);
    }
    ret(c, H264_OK);
}
// (work memory, param id, value)
HLE(h264, H264DECSetParam) {
    Session* s = session(arg(c, 0));
    const uint32_t id = arg(c, 1), v = arg(c, 2);
    if (!s) { ret(c, H264_INVALID_PARAM); return; }
    if (id == kParamOutputFn) s->outputFn = v;
    else if (id == kParamUserMemory) s->userMemory = v;
    else if (id == kParamOutputPerFrame) s->outputPerFrame = v && *mem::ptr(v) != 0;
    else LOG("[h264] H264DECSetParam: parameter %08X ignored", id);
    ret(c, H264_OK);
}
HLE(h264, H264DECSetParam_FPTR_OUTPUT) {
    if (Session* s = session(arg(c, 0))) s->outputFn = arg(c, 1);
    ret(c, H264_OK);
}
HLE(h264, H264DECSetParam_OUTPUT_PER_FRAME) {
    if (Session* s = session(arg(c, 0))) s->outputPerFrame = arg(c, 1) != 0;
    ret(c, H264_OK);
}
HLE(h264, H264DECSetParam_USER_MEMORY) {  // (work memory, void** value)
    if (Session* s = session(arg(c, 0))) s->userMemory = arg(c, 1) ? ld32(arg(c, 1)) : 0;
    ret(c, H264_OK);
}
// (work memory, data, length, double timestamp in f1)
HLE(h264, H264DECSetBitstream) {
    if (Session* s = session(arg(c, 0))) {
        s->bsPtr = arg(c, 1);
        s->bsLen = arg(c, 2);
        s->bsTimestamp = c->f[1].ps0;
    }
    ret(c, 0);
}
// (work memory, image output buffer): decodes the unit; returns frames to the game in display order
HLE(h264, H264DECExecute) {
    Session* s = session(arg(c, 0));
    if (!s) { ret(c, 0); return; }
    {
        const double t = now_ms();
        if (g_stats.lastExec) g_stats.maxGapMs = std::max(g_stats.maxGapMs, t - g_stats.lastExec);
        g_stats.lastExec = t;
        g_stats.executes++;
        stats_tick();
    }
    const bool wasRecovering = s->recovering;
    decode_unit(*s, arg(c, 1));
    if (wasRecovering && s->recovering) { ret(c, 0x400); return; }  // still waiting for an IDR frame
    const size_t keep = s->outputPerFrame ? 0 : kBuffered;
    while (s->pending.size() > keep) output_next(c, *s);
    ret(c, 0x80 | 100);
}
