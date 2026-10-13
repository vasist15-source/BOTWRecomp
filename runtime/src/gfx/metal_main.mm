#include <map>
#include <chrono>
#include "perf_metrics.h"
#include "gfx/capture_schedule.h"
#include "savestate.h"
#include "interp.h"
#include "gfx/depth_peek.h"
// Metal renderer: device, window, presentation, clears and copies.
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>

#include "gx2/gx2.h"
#include "gx2_texture_regs.h"
#include "metal.h"
#include "write_watch.h"
extern "C" uint64_t wwhd_query_counts(uint64_t* conditional);  // gx2_core.cpp
#include "runtime.h"
#include "input.h"

// gameplay mods (runtime/src/mods): HUD drawn into the TV image
namespace mods { void draw_overlay(id<MTLCommandBuffer> cmd, id<MTLTexture> tex); }

// texture invalidation statistics (see invalidate, note_surface_range)
static uint64_t g_inval_texture = 0, g_inval_passed = 0, g_pages_marked = 0;
namespace gfx {
Renderer R;
// GPU depth peeks for the game's sun visibility test (fork 73b54e1, Metal adaptation).
void peek_z(const uint32_t* cells, uint32_t n) {
    Surface* depth = nullptr;
    auto range = R.surfaces.equal_range(R.mainDepthAddr);
    for (auto it = range.first; it != range.second; ++it) {
        auto* s = it->second.get();
        if (s->isDepth && s->width == 1280 && s->height == 720 && s->tex &&
            (!depth || s->writeSeq > depth->writeSeq)) depth = s;
    }
    uint32_t points = std::min(n / 3,64u);
    if (!depth || !points) return;
    auto format = depth->tex.pixelFormat;
    if (format != MTLPixelFormatDepth16Unorm && format != MTLPixelFormatDepth32Float &&
        format != MTLPixelFormatDepth32Float_Stencil8) return;
    id<MTLBuffer> staging = [R.device newBufferWithLength:points*256 options:MTLResourceStorageModeShared];
    if (!staging) return;
    std::vector<uint32_t> destinations(points);
    end_encoder();
    id<MTLBlitCommandEncoder> b = TIMED_BLIT();
    for (uint32_t i=0; i<points; i++) {
        int x = depth_peek::pixel(cells[i*3],(uint32_t)depth->tex.width,640);
        int y = depth_peek::pixel(cells[i*3+1],(uint32_t)depth->tex.height,480);
        [b copyFromTexture:depth->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x,y,0)
            sourceSize:MTLSizeMake(1,1,1) toBuffer:staging destinationOffset:i*256 destinationBytesPerRow:256
            destinationBytesPerImage:256 options:format == MTLPixelFormatDepth32Float_Stencil8 ?
                MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
        destinations[i] = cells[i*3+2];
    }
    [b endEncoding];
    uint64_t ticket = depth_peek::next_ticket();
    [command_buffer() addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        if (cb.status != MTLCommandBufferStatusCompleted) return;
        const uint8_t* raw = static_cast<const uint8_t*>(staging.contents);
        std::vector<uint32_t> depths(points);
        for (uint32_t i=0; i<points; i++) {
            if (format == MTLPixelFormatDepth16Unorm) {
                uint16_t v; memcpy(&v,raw+i*256,2);
                depths[i] = v == 0xFFFF ? 0xFFFFFF : uint32_t(v) << 8;
            } else {
                float v; memcpy(&v,raw+i*256,4); depths[i] = depth_peek::from_float(v);
            }
        }
        depth_peek::publish(ticket,destinations,depths);
    }];
}


bool log_this_frame();

// windows, full screen, the present shader and the composition of the screens: display.mm
void display_init();
void present_screens();
void request_present_dump(const std::string& path);

// enhancement, toggled in game (Graphics menu or 8; WWHD_FXAA=1 starts with it on)
static std::atomic<bool> g_fxaa{[] { const char* e = getenv("WWHD_FXAA"); return e && atoi(e) != 0; }()};
bool fxaa_enabled() { return g_fxaa.load(std::memory_order_relaxed); }
void set_fxaa(bool v) { g_fxaa = v; LOG("[gfx] edge smoothing (FXAA) %s", v ? "on" : "off"); }

// ---------------------------------------------------------------- guest memory
static void map_guest_range(uint32_t base, uint32_t size) {
    id<MTLBuffer> b = [R.device newBufferWithBytesNoCopy:mem::ptr(base)
                                                  length:size
                                                 options:MTLResourceStorageModeShared
                                             deallocator:nil];
    if (!b) fatal("cannot map guest memory %08X+%X for the GPU", base, size);
    R.guest.push_back({base, size, b});
}

id<MTLBuffer> guest_buffer(uint32_t addr, uint32_t* offset) {
    for (auto& g : R.guest) {
        if (addr - g.base < g.size) {
            *offset = addr - g.base;
            return g.buf;
        }
    }
    return nil;
}

// ---------------------------------------------------------------- command buffers
static uint64_t commandFrame = 0, commandToken = 0;
static int64_t g_cmd_created_ns = 0;  // when R.cmd was started (time-based commits)
static std::atomic<int64_t> g_frame_gpu_start[256];  // WWHD_FRAME_TRACE: first GPU start of each swap's work
struct OverlayGpuTime {
    id<MTLCommandBuffer> buffer = nil; // ARC retains it until a later frame can read the result.
    uint64_t frame = 0, token = 0;
};
static std::array<OverlayGpuTime, 256> overlayGpuTimes;
static bool overlayGpuPending = false;
static void read_overlay_gpu_times() {
    if (!perf::enabled()) {
        if (overlayGpuPending) { overlayGpuTimes = {}; overlayGpuPending = false; }
        return;
    }
    static uint64_t lastPoll = UINT64_MAX;
    if (lastPoll == R.frame) return;
    lastPoll = R.frame;
    for (auto& q : overlayGpuTimes) {
        if (!q.buffer || q.frame >= R.frame) continue;
        auto status = q.buffer.status;
        if (status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError) continue;
        perf::timestamp_read(R.frame - q.frame);
        double ns = status == MTLCommandBufferStatusCompleted
            ? (q.buffer.GPUEndTime - q.buffer.GPUStartTime) * 1e9 : -1;
        perf::gpu_complete(q.frame, q.token, ns);
        q = {};
    }
}
static id<MTLCommandBuffer> raw_command_buffer() {
    if (!R.cmd) {
        read_overlay_gpu_times();
        R.cmd = [R.queue commandBuffer];
        g_cmd_created_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        commandFrame = R.frame;
        static const bool ftrace = getenv("WWHD_FRAME_TRACE") != nullptr;
        if (ftrace) {
            const uint64_t swapNo = R.frame + 1;  // the swap this command buffer's work belongs to
            [R.cmd addCompletedHandler:^(id<MTLCommandBuffer> b) {
                                // GPUStartTime counts mach_absolute_time (no sleep), steady_clock counts sleep too: shift by now's difference
                const int64_t off = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() -
                                    (int64_t)(CACurrentMediaTime() * 1e9);
                const int64_t t = (int64_t)(b.GPUStartTime * 1e9) + off;
                std::atomic<int64_t>& slot = g_frame_gpu_start[swapNo & 255];
                int64_t cur = slot.load();
                if (t > 0 && (cur == 0 || t < cur || cur < t - 1000000000)) slot = t;
            }];
        }
        commandToken = perf::gpu_begin(commandFrame);
    }
    return R.cmd;
}

// ---------------------------------------------------------------- deferred clears
// A guest clear is held until the next render pass: if that pass attaches the cleared slice, the clear
// becomes its load action (no separate pass storing the whole target and the next one loading it back).
// Any other GPU work (blits, other passes, commits) encodes the held clears first, in order.
struct PendingClear {
    id<MTLTexture> tex;
    uint32_t slice;
    bool isDepth, hasStencil, clearDepth, clearStencil;
    float rgba[4];
    float depth;
    uint32_t stencil;
};
static std::vector<PendingClear> g_pending_clears;
std::atomic<uint64_t> g_clears_folded{0}, g_clears_flushed{0};

static void encode_clear(const PendingClear& c) {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.slice = rp.stencilAttachment.slice = c.slice;
    if (c.tex.textureType == MTLTextureType3D) rp.colorAttachments[0].depthPlane = c.slice;
    else rp.colorAttachments[0].slice = c.slice;
    if (c.isDepth) {
        rp.depthAttachment.texture = c.tex;
        rp.depthAttachment.loadAction = c.clearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = c.depth;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (c.hasStencil) {
            rp.stencilAttachment.texture = c.tex;
            rp.stencilAttachment.loadAction = c.clearStencil ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = c.stencil;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    } else {
        rp.colorAttachments[0].texture = c.tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3]);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    time_render_pass(rp, __FILE_NAME__, __LINE__);
    [[raw_command_buffer() renderCommandEncoderWithDescriptor:rp] endEncoding];
}

void flush_pending_clears() {
    if (g_pending_clears.empty()) return;
    std::vector<PendingClear> v;
    v.swap(g_pending_clears);
    for (auto& c : v) encode_clear(c);
    g_clears_flushed += v.size();
}

// a pass about to attach tex/slice: turn a held clear of it into the attachment's load action
void take_pending_clear(MTLRenderPassDescriptor* rp, int colorIndex, id<MTLTexture> tex, uint32_t slice) {
    for (size_t i = 0; i < g_pending_clears.size(); i++) {
        PendingClear& c = g_pending_clears[i];
        if (c.tex != tex || c.slice != slice) continue;
        if (colorIndex >= 0) {
            if (c.isDepth) return;
            rp.colorAttachments[colorIndex].loadAction = MTLLoadActionClear;
            rp.colorAttachments[colorIndex].clearColor = MTLClearColorMake(c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3]);
        } else {
            if (!c.isDepth) return;
            if (c.clearDepth) {
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = c.depth;
            }
            if (c.hasStencil && c.clearStencil && rp.stencilAttachment.texture) {
                rp.stencilAttachment.loadAction = MTLLoadActionClear;
                rp.stencilAttachment.clearStencil = c.stencil;
            }
        }
        g_pending_clears.erase(g_pending_clears.begin() + i);
        g_clears_folded++;
        return;
    }
}

id<MTLCommandBuffer> command_buffer() {
    flush_pending_clears();
    return raw_command_buffer();
}

uint32_t g_draws_since_commit = 0;
std::atomic<uint64_t> g_gpu_ns{0};  // GPU busy time, for the periodic report

static void track_gpu_time(id<MTLCommandBuffer> cb) {
    const uint64_t frame = commandFrame;
    const uint64_t token = commandToken;
    // Keep explicit profiler diagnostics, but add no timing completion handler while hidden.
    static const bool diagnostic = [] {
        const char* p = getenv("WWHD_PROFILE"); return p && !strcmp(p, "1");
    }();
    if (token) {
        bool stored = false;
        for (auto& q : overlayGpuTimes) if (!q.buffer) {
            q = {cb, frame, token}; overlayGpuPending = true; stored = true; break;
        }
        if (!stored) perf::gpu_complete(frame, token, -1); // bounded storage; omit partial frames
    }
    if (!diagnostic) return;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        double t = b.GPUEndTime - b.GPUStartTime;
        if (t > 0) g_gpu_ns += (uint64_t)(t * 1e9);
        // per-frame GPU busy time and span; frames over 30 ms are logged (to line up with [hitch])
        static uint64_t curFrame = ~0ull;
        static double busy = 0, first = 0, last = 0;
        if (frame != curFrame) {
            if (curFrame != ~0ull && (busy > 0.030 || last - first > 0.040))
                LOG("[gpuslow] frame %llu: GPU busy %.1f ms, first start to last end %.1f ms", (unsigned long long)curFrame,
                    busy * 1e3, (last - first) * 1e3);
            curFrame = frame;
            busy = 0;
            first = b.GPUStartTime;
        }
        if (t > 0) busy += t;
        last = b.GPUEndTime;
    }];
}

void end_encoder() {
    if (R.enc) {
        [R.enc endEncoding];
        R.enc = nil;
        // submit work in chunks so the GPU starts while the frame is still being built (like the
        // hardware command processor), instead of all at once on swap
        // WWHD_COMMIT_DRAWS (default 16) and WWHD_COMMIT_US (default 500) set how much is held: committing
        // early lets the GPU start on a frame while the rest is still encoded (the GPU idled ~5 ms a frame)
        static const bool chunked = getenv("WWHD_NO_CHUNK") == nullptr;
        static const uint32_t commitDraws = getenv("WWHD_COMMIT_DRAWS") ? (uint32_t)atoi(getenv("WWHD_COMMIT_DRAWS")) : 16;
        static const int64_t commitNs = getenv("WWHD_COMMIT_US") ? atoll(getenv("WWHD_COMMIT_US")) * 1000 : 500000;
        const int64_t nowNs = commitNs ? std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
        if (chunked && R.cmd && g_draws_since_commit > 0 &&
            (g_draws_since_commit >= commitDraws || (commitNs && nowNs - g_cmd_created_ns >= commitNs))) {
            void pool_retire(id<MTLCommandBuffer> cmd);
            pool_retire(R.cmd);
            track_gpu_time(R.cmd);
            [R.cmd commit];
            R.cmd = nil;
            g_draws_since_commit = 0;
        }
    }
    for (auto& c : R.passColor) c = nullptr;
    R.passDepth = nullptr;
}

void pool_retire(id<MTLCommandBuffer> cmd);

void flush() {
    end_encoder();
    flush_pending_clears();
    if (R.cmd) {
        pool_retire(R.cmd);
        track_gpu_time(R.cmd);
        [R.cmd commit];
        R.cmd = nil;
    }
}

void wait_idle() {
    end_encoder();
    flush_pending_clears();
    if (R.cmd) {
        id<MTLCommandBuffer> c = R.cmd;
        pool_retire(c);
        [c commit];
        R.cmd = nil;
        [c waitUntilCompleted];
    }
}

// ---------------------------------------------------------------- init
void init() {
    R.device = MTLCreateSystemDefaultDevice();
    if (!R.device) fatal("no Metal device");
    R.queue = [R.device newCommandQueue];
    display_init();
    // GPU-visible guest memory: MEM2 (code data + heaps), runtime objects, foreground bucket, MEM1
    map_guest_range(0x10000000, 0x40000000);
    map_guest_range(0x60000000, 0x10000000);
    map_guest_range(0xA0000000, 0x1C000000);  // overlay arena (OSEnableOverlayArena, BotW)
    map_guest_range(0xE0000000, 0x04000000);
    map_guest_range(0xF4000000, 0x02000000);
    LOG("[gfx] Metal device: %s", R.device.name.UTF8String);
}

void run_main_loop() { [NSApp run]; }

// the MTLDevice's name, for reports (renderer.h Backend::device); "" before init
std::string device_name() { return R.device ? std::string(R.device.name.UTF8String) : std::string(); }

// ---------------------------------------------------------------- clears
static void clear_surface(Surface* s, const float* rgba, bool clearDepth, float depth, bool clearStencil, uint32_t stencil,
                          uint32_t firstSlice = 0, uint32_t numSlices = 1) {
    if (!s || !s->tex) return;
    end_encoder();
    bool pass_dump_on();
    if (pass_dump_on())
        LOG("[pass] clear %08X %lux%lu f%X%s%s%s", s->addr, (unsigned long)s->tex.width, (unsigned long)s->tex.height, s->format,
            s->isDepth ? " depth" : " color", clearDepth ? " d" : "", clearStencil ? " s" : "");
    static const bool fold = getenv("WWHD_NO_CLEAR_FOLD") == nullptr;
    for (uint32_t slice = firstSlice; slice < firstSlice + numSlices; slice++) {
        PendingClear c{};
        c.tex = s->tex;
        c.slice = slice;
        c.isDepth = s->isDepth;
        c.hasStencil = s->isDepth && s->fmt.stencil;
        c.clearDepth = clearDepth;
        c.clearStencil = clearStencil;
        if (rgba) for (int i = 0; i < 4; i++) c.rgba[i] = rgba[i];
        c.depth = depth;
        c.stencil = stencil;
        // a second clear of a slice already held: encode the first (rare; keeps partial depth/stencil clears exact)
        for (auto& p : g_pending_clears)
            if (p.tex == c.tex && p.slice == c.slice) { flush_pending_clears(); break; }
        if (fold) g_pending_clears.push_back(c);
        else encode_clear(c);
    }
    mark_gpu_written(s);
}

void clear_color(const uint32_t* regs, uint32_t cb, const float rgba[4]) {
    uint32_t first = 0, num = 1;
    Surface* s = surface_from_color_buffer(cb, &first, &num);
    if (log_this_frame() && s)
        LOG("[clear] color %08X %ux%u fmt %X -> %.3f %.3f %.3f %.3f", s->addr, s->width, s->height, s->format, rgba[0], rgba[1], rgba[2], rgba[3]);
    clear_surface(s, rgba, false, 0, false, 0, first, num);
}

void clear_depth_stencil(const uint32_t* regs, uint32_t db, float depth, uint32_t stencil, uint32_t flags) {
    // flags: 1 = depth, 2 = stencil
    uint32_t first = 0, num = 1;
    Surface* s = surface_from_depth_buffer(db, &first, &num);
    if (log_this_frame() && s)
        LOG("[clear] depth %08X %ux%ux%u fmt %X pixel %lu -> depth %.4f stencil %u flags %u slices %u+%u", s->addr, s->width, s->height,
            s->slices, s->format, (unsigned long)s->fmt.pixel, depth, stencil, flags, first, num);
    clear_surface(s, nullptr, flags & 1, depth, (flags & 2) != 0, stencil, first, num);
}

// ---------------------------------------------------------------- copies
void copy_surface(uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip, uint32_t dstSlice) {
    void copy_surface_impl(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    copy_surface_impl(src, srcMip, srcSlice, dst, dstMip, dstSlice);
}

void copy_to_scan(uint32_t cb, uint32_t target) {
    // target: 1 = TV, 4/8 = GamePad
    if (log_this_frame()) LOG("[scan] copy %08X to %s", cb, (target & 1) ? "TV" : "DRC");
    Screen& scr = (target & 1) ? R.tv : R.drc;
    Surface* s = surface_from_color_buffer(cb);
    if (!s || !s->tex) return;
    end_encoder();
    // the image as rendered (internal resolution); the present pass scales it to the window
    NSUInteger w = s->tex.width, h = s->tex.height;
    if (!scr.tex || scr.tex.width != w || scr.tex.height != h || scr.tex.pixelFormat != s->tex.pixelFormat) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s->tex.pixelFormat
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;  // render target: mod overlays
        d.storageMode = MTLStorageModePrivate;
        scr.tex = [R.device newTextureWithDescriptor:d];
    }
    id<MTLBlitCommandEncoder> b = TIMED_BLIT();
    [b copyFromTexture:s->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(w, h, 1) toTexture:scr.tex destinationSlice:0 destinationLevel:0
     destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
    if (target & 1) ::mods::draw_overlay(command_buffer(), scr.tex);  // HUD of gameplay mods (stamina wheel)
}

// debug: WWHD_DUMP_FRAMES=100,300 writes the TV image of those frames to frame_<n>.png
static const auto g_dump_frames = capture_schedule::parse(getenv("WWHD_DUMP_FRAMES"));
static const auto g_dump_load_frames = capture_schedule::parse(getenv("WWHD_DUMP_LOAD_FRAMES"));

// async: write the file when the GPU gets there instead of stalling (keeps frame timing intact)
void set_tv_format(uint32_t gx2Format, bool tv) {
    (tv ? R.tv : R.drc).srgb = (gx2Format & 0x400) != 0;
}

// depth buffers: grey image, contrast-stretched to the range of values present
static void dump_depth(id<MTLTexture> src, const char* name) {
    // all array slices side by side
    uint32_t w = (uint32_t)src.width, h = (uint32_t)src.height, n = (uint32_t)std::max<NSUInteger>(src.arrayLength, 1);
    MTLPixelFormat pf = src.pixelFormat;
    uint32_t bpp = pf == MTLPixelFormatDepth16Unorm ? 2 : 4;
    id<MTLBuffer> buf = [R.device newBufferWithLength:(NSUInteger)w * h * bpp * n options:MTLResourceStorageModeShared];
    end_encoder();
    id<MTLBlitCommandEncoder> b = TIMED_BLIT();
    for (uint32_t z = 0; z < n; z++)
        [b copyFromTexture:src sourceSlice:z sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                  toBuffer:buf destinationOffset:(NSUInteger)z * w * h * bpp destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp
                   options:pf == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
    [b endEncoding];
    wait_idle();
    uint32_t W = w * n;
    std::vector<uint8_t> px((size_t)W * h * 4);
    std::string ranges;
    for (uint32_t z = 0; z < n; z++) {
        std::vector<float> v((size_t)w * h);
        for (size_t i = 0; i < v.size(); i++) {
            size_t k = (size_t)z * w * h + i;
            v[i] = bpp == 2 ? ((const uint16_t*)buf.contents)[k] / 65535.0f : ((const float*)buf.contents)[k];
        }
        float lo = 1, hi = 0;
        for (float x : v) if (x < 1.0f) { lo = std::min(lo, x); hi = std::max(hi, x); }
        char r[64];
        snprintf(r, sizeof r, " [%u] %.4f..%.4f", z, lo, hi);
        ranges += r;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                float f = v[(size_t)y * w + x];
                uint8_t g = f >= 1.0f ? 255 : (uint8_t)std::clamp((f - lo) / std::max(hi - lo, 1e-6f) * 230.0f, 0.0f, 230.0f);
                uint8_t* o = &px[((size_t)y * W + z * w + x) * 4];
                o[0] = o[1] = o[2] = g;
                o[3] = 255;
            }
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), W, h, 8, W * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:[NSString stringWithUTF8String:name]], (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    CGImageDestinationAddImage(dst, img, nullptr);
    CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    LOG("[gfx] wrote %s (%ux%u depth x%u, ranges%s)", name, w, h, n, ranges.c_str());
}

void dump_texture(id<MTLTexture> src, const char* name, bool async, bool srgbEncode) {
    if (!src) return;
    uint32_t w = (uint32_t)src.width, h = (uint32_t)src.height;
    MTLPixelFormat pf = src.pixelFormat;
    if (pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth32Float_Stencil8) {
        dump_depth(src, name);
        return;
    }
    bool rgb10 = pf == MTLPixelFormatRGB10A2Unorm;
    bool rgba8 = pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatRGBA8Unorm_sRGB;
    bool f16 = pf == MTLPixelFormatRGBA16Float;
    bool r8 = pf == MTLPixelFormatR8Unorm;
    bool rg11 = pf == MTLPixelFormatRG11B10Float;
    // single-channel float/16-bit: grey, stretched between the values' min and max (logged)
    bool r16f = pf == MTLPixelFormatR16Float, r32f = pf == MTLPixelFormatR32Float, r16 = pf == MTLPixelFormatR16Unorm;
    bool scalar = r16f || r32f || r16;
    if (!rgb10 && !rgba8 && !f16 && !r8 && !rg11 && !scalar) { LOG("[gfx] dump: unsupported format %lu", (unsigned long)pf); return; }
    uint32_t bpp = f16 ? 8 : r8 ? 1 : (r16f || r16) ? 2 : 4;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [R.device newTextureWithDescriptor:d];
    end_encoder();
    id<MTLBlitCommandEncoder> b = TIMED_BLIT();
    [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
             toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
    std::string file = name;
    auto write = [=]() {
    std::vector<uint8_t> raw((size_t)w * h * bpp), px((size_t)w * h * 4);
    [t getBytes:raw.data() bytesPerRow:w * bpp fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    std::vector<float> vals;
    float lo = INFINITY, hi = -INFINITY;
    if (scalar) {
        vals.resize((size_t)w * h);
        for (size_t i = 0; i < vals.size(); i++) {
            float v;
            if (r16f) { __fp16 hv; memcpy(&hv, &raw[i * 2], 2); v = hv; }
            else if (r16) { uint16_t u; memcpy(&u, &raw[i * 2], 2); v = u / 65535.0f; }
            else memcpy(&v, &raw[i * 4], 4);
            vals[i] = v;
            if (std::isfinite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
        }
        LOG("[gfx] dump %s: values %g .. %g (centre %g)", file.c_str(), lo, hi, vals[(size_t)(h / 2) * w + w / 2]);
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t* o = &px[i * 4];
        if (rgb10) {
            uint32_t v; memcpy(&v, &raw[i * 4], 4);
            o[0] = (v & 0x3FF) >> 2; o[1] = ((v >> 10) & 0x3FF) >> 2; o[2] = ((v >> 20) & 0x3FF) >> 2;
        } else if (rgba8) {
            memcpy(o, &raw[i * 4], 3);
        } else if (r8) {
            o[0] = o[1] = o[2] = raw[i];
        } else if (scalar) {
            float v = std::isfinite(vals[i]) && hi > lo ? (vals[i] - lo) / (hi - lo) : 0.0f;
            o[0] = o[1] = o[2] = (uint8_t)(v * 255.0f);
        } else if (rg11) {
            uint32_t v; memcpy(&v, &raw[i * 4], 4);
            auto f11 = [](uint32_t m, uint32_t e) { float f = e ? ldexpf(1.0f + m / 64.0f, (int)e - 15) : ldexpf(m / 64.0f, -14); return f; };
            float r = f11(v & 0x3F, (v >> 6) & 0x1F), g = f11((v >> 11) & 0x3F, (v >> 17) & 0x1F), bl = ldexpf(1.0f + ((v >> 22) & 0x1F) / 32.0f, (int)((v >> 27) & 0x1F) - 15);
            o[0] = (uint8_t)std::min(255.0f, r * 255); o[1] = (uint8_t)std::min(255.0f, g * 255); o[2] = (uint8_t)std::min(255.0f, bl * 255);
        } else {
            for (int c = 0; c < 3; c++) {
                __fp16 hv; memcpy(&hv, &raw[i * 8 + c * 2], 2);
                o[c] = (uint8_t)std::clamp((float)hv * 255.0f, 0.0f, 255.0f);
            }
        }
        o[3] = 255;
        if (srgbEncode)
            for (int c = 0; c < 3; c++) {
                float v = o[c] / 255.0f;
                v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
                o[c] = (uint8_t)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
            }
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    NSString* path = [NSString stringWithUTF8String:file.c_str()];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],
                                                                (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    CGImageDestinationAddImage(dst, img, nullptr);
    CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    LOG("[gfx] wrote %s (%ux%u)", file.c_str(), w, h);
    };
    if (async) {
        [command_buffer() addCompletedHandler:^(id<MTLCommandBuffer>) { write(); }];
    } else {
        wait_idle();
        write();
    }
}

// save states: thumbnails and test dumps of the TV image a number of frames from now
static std::mutex g_tv_dump_mu;
static std::vector<std::pair<uint64_t, std::string>> g_tv_dumps;
uint64_t frame_count() { return __atomic_load_n(&R.frame, __ATOMIC_RELAXED); }
void request_tv_dump(const std::string& path, int frames_ahead) {
    std::lock_guard<std::mutex> lk(g_tv_dump_mu);
    g_tv_dumps.push_back({frame_count() + frames_ahead, path});
}
static void service_tv_dumps() {
    std::lock_guard<std::mutex> lk(g_tv_dump_mu);
    for (auto it = g_tv_dumps.begin(); it != g_tv_dumps.end();)
        if (it->first <= R.frame && R.tv.tex) {
            dump_texture(R.tv.tex, it->second.c_str(), true, R.tv.srgb);
            it = g_tv_dumps.erase(it);
        } else {
            ++it;
        }
}

static void dump_tv(const std::string& stem) {
    dump_texture(R.tv.tex, (stem + ".png").c_str(), true, R.tv.srgb);
    if (R.drc.tex)
        dump_texture(R.drc.tex, (stem + "_drc.png").c_str(), true, R.drc.srgb);
    static const bool present = getenv("WWHD_DUMP_PRESENT") != nullptr;
    if (present) request_present_dump(stem + "_present.png");
}

void with_autorelease_pool(void (*fn)()) {
    @autoreleasepool {
        fn();
    }
}

static std::atomic<uint64_t> g_frames_completed{0};
static std::atomic<int64_t> g_frame_done_ns[256];
uint64_t frames_completed() { return g_frames_completed; }

void cache_warm_step();

void pass_timing_arm(double lastFrameMs);
void swap() {
    {  // debug: values a patched shader wrote (WWHD_MSL_PATCH, buffer 30)
        extern id<MTLBuffer> g_debug_values;
        static uint64_t n = 0;
        if (g_debug_values && ++n % 30 == 0) {
            const float* v = (const float*)g_debug_values.contents;
            if (v[63] == 12345.0f) {
                std::string line;
                char b[32];
                for (int i = 0; i < 56; i++) { snprintf(b, sizeof b, " [%d]%g", i, v[i]); line += b; }
                LOG("[dbgvals]%s", line.c_str());
            }
        }
    }
    {
        static double prev = 0;
        const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (prev) pass_timing_arm(t - prev);
        prev = t;
    }
    cache_warm_step();
    static bool sync_gpu = getenv("WWHD_SYNC_GPU") != nullptr;  // debug: no CPU/GPU overlap
    if (sync_gpu) wait_idle();
    end_encoder();
    if (log_this_frame()) LOG("[frame] end %llu", (unsigned long long)R.frame);
    void pass_timing_end_frame(id<MTLCommandBuffer>);
    pass_timing_end_frame(command_buffer());
    R.frame++;
    latch_res_scale();
    if (capture_schedule::contains(g_dump_frames, R.frame))
        dump_tv(capture_schedule::stem(R.frame, false));
    const uint64_t loaded = g_dump_load_frames.empty() ? 0 : ss::last_load_frame();
    if (capture_schedule::relative_due(g_dump_load_frames, R.frame, loaded)) {
        dump_tv(capture_schedule::stem(R.frame - loaded, true));
        LOG("[gfx] load-relative capture: load frame %llu, frame %llu, offset %llu, load step %llu, step %llu",
            (unsigned long long)loaded, (unsigned long long)R.frame,
            (unsigned long long)(R.frame - loaded), (unsigned long long)ss::last_load_step(),
            (unsigned long long)interp::logic_steps());
    }
    service_tv_dumps();
    const char* capture_begin_frame();
    static std::string pendingCapture;  // the TV image is dumped once the captured frame has been drawn
    if (!pendingCapture.empty()) {
        dump_texture(R.tv.tex, (pendingCapture + "/tv.png").c_str(), true, R.tv.srgb);
        if (R.drc.tex) dump_texture(R.drc.tex, (pendingCapture + "/gamepad.png").c_str(), true, R.drc.srgb);
        request_present_dump(pendingCapture + "/present.png");  // the TV window as shown (overlay, bars)
        LOG("[gfx] capture written to %s", pendingCapture.c_str());
        pendingCapture.clear();
    }
    if (const char* dir = capture_begin_frame()) pendingCapture = dir;
    @autoreleasepool {
        present_screens();
        [command_buffer() addCompletedHandler:^(id<MTLCommandBuffer>) {
            uint64_t n = ++g_frames_completed;
            g_frame_done_ns[n & 255] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch()).count();
        }];
        flush();
    }
    perf::gpu_advance(R.frame);
    {
        // Hitch log: a frame over 45 ms of wall time gets one line saying what the render thread did
        // in it (texture work, new shaders/pipelines, passes split), so stutters can be attributed.
        // WWHD_HITCH_MS changes the threshold; 0 turns it off.
        static const double threshold = getenv("WWHD_HITCH_MS") ? atof(getenv("WWHD_HITCH_MS")) : 45.0;
        extern double g_stat_hash_ms, g_stat_decode_ms;
        extern uint64_t g_stat_pass_splits, g_stat_uploads;
        void compile_counters(size_t*, size_t*, double*, double*);
        struct Snap { double wall, cpu, hash, decode, decompile, msl; uint64_t uploads, splits; size_t shaders, pipelines; };
        static Snap last{};
        extern std::atomic<double> g_compile_wait_ms;
        static double lastWait = 0;
        Snap now{};
        now.wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        now.cpu = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
        now.hash = g_stat_hash_ms;
        now.decode = g_stat_decode_ms;
        now.uploads = g_stat_uploads;
        now.splits = g_stat_pass_splits;
        compile_counters(&now.shaders, &now.pipelines, &now.decompile, &now.msl);
        // the 300-frame report resets hash/decode/splits: never let a reset look negative
        auto d = [](double a, double b) { return a >= b ? a - b : a; };
        auto du = [](uint64_t a, uint64_t b) { return a >= b ? a - b : a; };
        if (threshold > 0 && last.wall > 0 && now.wall - last.wall > threshold)
            LOG("[hitch] frame %llu: %.0f ms (render thread CPU %.0f ms) | textures: %llu uploads, decode %.0f ms, hash %.0f ms | "
                "shaders +%zu (%.0f ms translate, %.0f ms compile), pipelines +%zu, waited for compiles %.0f ms | passes split %llu",
                (unsigned long long)R.frame, now.wall - last.wall, now.cpu - last.cpu, (unsigned long long)du(now.uploads, last.uploads),
                d(now.decode, last.decode), d(now.hash, last.hash), now.shaders - last.shaders, now.decompile - last.decompile,
                now.msl - last.msl, now.pipelines - last.pipelines, g_compile_wait_ms.load() - lastWait, (unsigned long long)du(now.splits, last.splits));
        last = now;
        lastWait = g_compile_wait_ms.load();
    }
    if (R.frame % 300 == 1) {
        // render thread CPU time (excludes sleeping and GPU waits), for performance comparisons
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        static uint64_t lastCpu = 0;
        uint64_t cpu = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        extern std::atomic<uint64_t> g_pass_stats[4], g_pass_stats_merged;
        LOG("[gfx] last 300 frames: %.0f draws/frame kept in a merged pass", g_pass_stats_merged.exchange(0) / 300.0);
        LOG("[gfx] last 300 frames: %.0f render passes/frame (%.0f MiB loaded+stored), %.0f of them reopen the previous pass's targets (%.0f MiB)",
            g_pass_stats[0].exchange(0) / 300.0, g_pass_stats[1].exchange(0) / 300.0 / 1048576.0, g_pass_stats[2].exchange(0) / 300.0,
            g_pass_stats[3].exchange(0) / 300.0 / 1048576.0);
        // addresses with more than one surface (one guest buffer, several textures): which one a draw renders
        // to and which one a later pass samples can differ. Logged every 1800 frames.
        if (R.frame % 1800 == 1) {
            std::map<uint32_t, std::vector<Surface*>> multi;
            for (auto& [a, sp] : R.surfaces) multi[a].push_back(sp.get());
            int lines = 0;
            for (auto& [a, v] : multi) {
                if (v.size() < 2 || lines >= 60) continue;
                bool gpu = false;
                for (auto* x : v) gpu = gpu || x->gpuWritten;
                if (!gpu) continue;
                std::string line;
                char b[160];
                for (auto* x : v) {
                    snprintf(b, sizeof b, " [%ux%ux%u f%X%s mips %u %s seq %llu tex %lux%lu]", x->width, x->height, x->slices, x->format,
                             x->isDepth ? " depth" : "", x->mips, x->gpuWritten ? "gpu" : "cpu", (unsigned long long)x->writeSeq,
                             x->tex ? (unsigned long)x->tex.width : 0, x->tex ? (unsigned long)x->tex.height : 0);
                    line += b;
                }
                LOG("[alias] %08X:%s", a, line.c_str());
                lines++;
            }
        }
        extern std::atomic<uint64_t> g_pass_why[7];
        LOG("[gfx] last 300 frames: passes/frame ended by: other work %.1f, slice change %.1f, more targets %.1f, other depth %.1f, "
            "sampling an attached target %.1f, other targets %.1f; %.1f passes/frame had at most one draw",
            g_pass_why[0].exchange(0) / 300.0, g_pass_why[1].exchange(0) / 300.0, g_pass_why[2].exchange(0) / 300.0,
            g_pass_why[3].exchange(0) / 300.0, g_pass_why[4].exchange(0) / 300.0, g_pass_why[5].exchange(0) / 300.0,
            g_pass_why[6].exchange(0) / 300.0);
        extern std::atomic<uint64_t> g_copy_cpu, g_copy_cpu_bytes, g_copy_gpu, g_write_backs, g_write_back_bytes;
        uint64_t cond = 0, queries = wwhd_query_counts(&cond);
        extern uint64_t g_alpha_to_mask_draws;
        LOG("[gfx] last 300 frames: %.1f draws/frame with alpha-to-coverage on", g_alpha_to_mask_draws / 300.0);
        g_alpha_to_mask_draws = 0;
        LOG("[gfx] last 300 frames: surface copies %.1f/frame on the GPU, %.1f on the CPU (%.2f MiB/frame); linear write-backs %.1f/frame "
            "(%.2f MiB/frame); occlusion queries %.1f/frame, conditional renders %.1f/frame",
            g_copy_gpu.exchange(0) / 300.0, g_copy_cpu.exchange(0) / 300.0, g_copy_cpu_bytes.exchange(0) / 300.0 / 1048576.0,
            g_write_backs.exchange(0) / 300.0, g_write_back_bytes.exchange(0) / 300.0 / 1048576.0, queries / 300.0, cond / 300.0);
        LOG("[gfx] last 300 frames: %.1f clears/frame folded into the next pass, %.1f encoded on their own",
            g_clears_folded.exchange(0) / 300.0, g_clears_flushed.exchange(0) / 300.0);
        LOG("[gfx] frame %llu, %llu draws so far, GPU %.1f ms/frame, render thread CPU %.2f ms/frame", (unsigned long long)R.frame,
            (unsigned long long)R.drawCount, g_gpu_ns.exchange(0) / 1e6 / 300.0, lastCpu ? (cpu - lastCpu) / 1e6 / 300.0 : 0.0);
        lastCpu = cpu;
        LOG("[inval] texture invalidates %llu, with candidates %llu, surfaces examined %llu",
            (unsigned long long)g_inval_texture, (unsigned long long)g_inval_passed, (unsigned long long)g_pages_marked);
        void report_skips();
        report_skips();
    }
}

// Index for invalidate(): page (4 KiB) -> surfaces whose checked range (invalidate's size estimate or
// its level ranges) touches it. BotW has thousands of surfaces and invalidates ~460 times a frame,
// nearly always over texture memory but overlapping no surface; walking the whole table each time
// was the largest single cost of the render thread. A range over kBigPages pages goes in a short list
// that every invalidation checks instead of filling thousands of buckets.
static constexpr uint64_t kBigPages = 256;
static std::unordered_map<uint32_t, std::vector<Surface*>> g_page_surfaces;
static std::vector<Surface*> g_big_surfaces;
void note_surface_range(Surface* s, uint32_t addr, uint32_t size) {
    if (!size) return;
    uint64_t first = addr >> 12, last = ((uint64_t)addr + size - 1) >> 12;
    if (last - first >= kBigPages) {
        static int big = 0;
        if (big++ < 12) LOG("[inval] large surface range %08X+%X", addr, size);
        if (std::find(g_big_surfaces.begin(), g_big_surfaces.end(), s) == g_big_surfaces.end()) g_big_surfaces.push_back(s);
        return;
    }
    for (uint64_t pg = first; pg <= last; pg++) {
        auto& v = g_page_surfaces[(uint32_t)pg];
        if (v.empty() || v.back() != s) v.push_back(s);
    }
}

extern uint64_t g_stat_invalidates, g_stat_invalidated_surfaces;
void invalidate(uint32_t flags, uint32_t addr, uint32_t size) {
    g_stat_invalidates++;
    static int logged = 0;
    static const bool logInval = getenv("WWHD_LOG_INVALIDATE") != nullptr;
    if (logInval && (flags & 0x2) && logged++ < 400)
        LOG("[inval] frame %llu flags %X addr %08X size %X", (unsigned long long)R.frame, flags, addr, size);
    // GX2_INVALIDATE_MODE_TEXTURE (0x2): the CPU wrote texture data; force a full check of surfaces in
    // range on next use. Uniform/attribute/shader invalidations need nothing here.
    if (!(flags & 0x2)) return;
    // "invalidate everything" (sent several times per frame) carries no information about CPU writes;
    // changed textures are caught by the write tracking (write_watch.h, metal_surfaces.mm check_texture)
    if (size >= 0x10000000) return;
    g_inval_texture++;
    if (!size) return;
    // with page-protection write tracking every CPU write to a texture's pages is seen anyway (check_texture);
    // BotW sends ~460 texture invalidations a frame and this scan (~50 surfaces each) never marked one.
    // WWHD_SCAN_INVALIDATES=1 scans anyway.
    static const bool scan = getenv("WWHD_SCAN_INVALIDATES") != nullptr;
    if (!scan && wwatch::active()) return;
    uint64_t end = (uint64_t)addr + size;
    auto hits = [&](Surface* s) {
        // every level once known (after the first check), else the base level estimate
        if (s->levelRanges.empty()) return addr < (uint64_t)s->addr + std::max<uint32_t>(s->dataSize, s->pitch * s->height * 4) && s->addr < end;
        for (auto& [b, n] : s->levelRanges)
            if (addr < (uint64_t)b + n && b < end) return true;
        return false;
    };
    auto mark = [&](Surface* s) {
        uint32_t a = s->addr;
        // MEM1 holds render targets; CPU-side surfaces there are views of GPU data, not CPU uploads
        if (!s->gpuWritten && !(a >= 0xF4000000 && a < 0xF6000000) && hits(s)) {
            s->lastCheckedFrame = ~0ull;
            s->dirty = true;
            g_surface_gen++;
            g_stat_invalidated_surfaces++;
            static int lg = 0;
            if (logInval && lg++ < 300)
                LOG("[inval]   marks %08X %ux%u fmt %X size %X (range %08X+%X)", a, s->width, s->height, s->format, s->dataSize, addr, size);
        }
    };
    uint64_t firstPage = addr >> 12, lastPage = (end - 1) >> 12;
    if (lastPage - firstPage > 4 * kBigPages) {  // a huge invalidation: look at everything
        for (auto& [a, s] : R.surfaces) mark(s.get());
        return;
    }
    static thread_local std::vector<Surface*> cand;  // render thread only
    cand.assign(g_big_surfaces.begin(), g_big_surfaces.end());
    for (uint64_t pg = firstPage; pg <= lastPage; pg++)
        if (auto it = g_page_surfaces.find((uint32_t)pg); it != g_page_surfaces.end())
            cand.insert(cand.end(), it->second.begin(), it->second.end());
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
    g_inval_passed += !cand.empty();
    g_pages_marked += cand.size();  // reported as "surfaces examined"
    for (Surface* c : cand) mark(c);
}

}  // namespace gfx

// frame n's GPU completion time (steady clock ns; 0 when not completed in the last 256 frames)
extern "C" int64_t wwhd_frame_done_ns(uint64_t n) {
    return gfx::frames_completed() >= n && gfx::frames_completed() < n + 256 ? gfx::g_frame_done_ns[n & 255].load() : 0;
}

extern "C" int64_t wwhd_frame_gpu_start_ns(uint64_t n) { return gfx::g_frame_gpu_start[n & 255].load(); }
