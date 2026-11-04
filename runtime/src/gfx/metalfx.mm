// MetalFX on the TV picture (options and settings: metalfx_opts.h).
//
// present_screens (display.mm) asks metalfx::frame() for this frame's TV picture before composing the
// window. What it does, in this frame's command buffer, before the composition pass:
//
//   Upscaling (MetalFX spatial scaler): the scan image (R.tv.tex, at the internal resolution, smaller
//   while dynamic resolution has lowered it) is scaled to the size of the picture's box in the window
//   whenever that box is larger. The composition then draws it 1:1, so the Picture scaling filter does
//   not apply while it upscales. The scaler is made again when either size changes (window resize, a
//   dynamic resolution step).
//
//   Frame generation: each frame's picture (upscaled, or a copy of the scan image) is kept; with the
//   previous one a picture halfway between them is made. display.mm presents that one first and this
//   frame's own picture half a game frame later (presentDrawable:afterMinimumDuration:), so 30 fps shows
//   as 60 and 60 as 120 on a 120 Hz screen, with half a game frame of extra latency. The game gives the
//   presentation no motion vectors or depth, so motion is estimated from the two pictures (luma at 1/4
//   of the picture's size, a 4-level pyramid down to 1/32, block matching coarse to fine with sub-pixel
//   refinement, a 3x3 median, then scaled to the picture's size). The halfway picture samples both
//   pictures half a motion vector away (mfx_interp_fs) and blends them, leaning to the newer one where
//   they disagree (occlusions). The HUD is part of the picture; its still parts estimate as still.
//   It runs only while two presents per game frame fit the display (game frame time >= 1.8 display
//   refreshes, vsync on); otherwise frames pass through and the history is dropped. A gap in the frames
//   (load, stall, size change) starts the history again.
//
//   MetalFX's own frame interpolator (macOS 26+) can make the halfway picture instead
//   (WWHD_METALFX_INTERPOLATOR=1, experimental). With only estimated motion and no depth it does not
//   help here: on moving test pictures (macOS 27, constant depth of any value, reversed or not, any
//   motion scale, with or without camera matrices, own history or ours) its output was the current
//   picture, slightly warped, never the halfway one.
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../interp.h"
#include "../metalfx_opts.h"
#include "gx2/gx2.h"
#include "metal.h"
#include "metalfx.h"
#include "runtime.h"

namespace gfx {
namespace metalfx {

static bool g_log = [] { const char* e = getenv("WWHD_METALFX_LOG"); return e && atoi(e) != 0; }();

static id<MTLTexture> make_tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u, const char* label) {
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:std::max<NSUInteger>(w, 1)
                                                                                height:std::max<NSUInteger>(h, 1) mipmapped:NO];
    d.usage = u;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [R.device newTextureWithDescriptor:d];
    t.label = @(label);
    return t;
}

// ---------------------------------------------------------------- motion estimation kernels
static const char* kMotionShader = R"(
#include <metal_stdlib>
using namespace metal;
// luma at 1/4 of the picture (4 bilinear taps = a 4x4 box); linear pictures are made roughly perceptual
kernel void mfx_luma(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],
                     sampler s [[sampler(0)]], constant int& lin [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float2 inv = 1.0 / float2(dst.get_width(), dst.get_height());
    float2 uv = (float2(gid) + 0.5) * inv, q = 0.25 * inv;
    float3 c = src.sample(s, uv + float2(-q.x, -q.y)).rgb + src.sample(s, uv + float2(q.x, -q.y)).rgb +
               src.sample(s, uv + float2(-q.x, q.y)).rgb + src.sample(s, uv + float2(q.x, q.y)).rgb;
    float l = dot(saturate(c * 0.25), float3(0.299, 0.587, 0.114));
    if (lin) l = sqrt(l);
    dst.write(float4(l, 0, 0, 1), gid);
}
kernel void mfx_down(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],
                     uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    uint2 m = uint2(src.get_width() - 1, src.get_height() - 1), p = gid * 2;
    float v = src.read(min(p, m)).r + src.read(min(p + uint2(1, 0), m)).r + src.read(min(p + uint2(0, 1), m)).r +
              src.read(min(p + uint2(1, 1), m)).r;
    dst.write(float4(v * 0.25, 0, 0, 1), gid);
}
struct SearchParams { int radius; int hasCoarse; float lambda; };
// cost of matching the 5x5 neighbourhood (9 taps, step 2) around pos in cur with the one around pos + d in prev
static float sad(thread const float* c, texture2d<float> prev, int2 pos, int2 d, int2 hi) {
    float s = 0;
    int k = 0;
    for (int y = -2; y <= 2; y += 2)
        for (int x = -2; x <= 2; x += 2) s += abs(c[k++] - prev.read(uint2(clamp(pos + d + int2(x, y), int2(0), hi))).r);
    return s;
}
// flow at this level (pixels of this level): where each pixel of cur was in prev
kernel void mfx_search(texture2d<float> cur [[texture(0)]], texture2d<float> prev [[texture(1)]],
                       texture2d<float> coarse [[texture(2)]], texture2d<float, access::write> out [[texture(3)]],
                       constant SearchParams& p [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= out.get_width() || gid.y >= out.get_height()) return;
    int2 hi = int2(cur.get_width() - 1, cur.get_height() - 1), pos = int2(gid);
    float c[9];
    int k = 0;
    for (int y = -2; y <= 2; y += 2)
        for (int x = -2; x <= 2; x += 2) c[k++] = cur.read(uint2(clamp(pos + int2(x, y), int2(0), hi))).r;
    int2 base = int2(0);
    if (p.hasCoarse) {
        int2 chi = int2(coarse.get_width() - 1, coarse.get_height() - 1);
        base = int2(round(coarse.read(uint2(clamp(pos / 2, int2(0), chi))).rg * 2.0));
    }
    // zero motion first: still parts (the HUD) win ties
    int2 best = int2(0);
    float bestCost = sad(c, prev, pos, int2(0), hi) - p.lambda;
    for (int dy = -p.radius; dy <= p.radius; dy++)
        for (int dx = -p.radius; dx <= p.radius; dx++) {
            int2 d = base + int2(dx, dy);
            float cost = sad(c, prev, pos, d, hi) + p.lambda * 0.25 * float(abs(dx) + abs(dy));
            if (cost < bestCost) { bestCost = cost; best = d; }
        }
    // sub-pixel: parabola through the neighbours' costs, per axis
    float c0 = sad(c, prev, pos, best, hi);
    float xm = sad(c, prev, pos, best - int2(1, 0), hi), xp = sad(c, prev, pos, best + int2(1, 0), hi);
    float ym = sad(c, prev, pos, best - int2(0, 1), hi), yp = sad(c, prev, pos, best + int2(0, 1), hi);
    float dxs = xm - 2.0 * c0 + xp, dys = ym - 2.0 * c0 + yp;
    float2 sub = float2(dxs > 1e-5 ? clamp(0.5 * (xm - xp) / dxs, -0.5, 0.5) : 0.0,
                        dys > 1e-5 ? clamp(0.5 * (ym - yp) / dys, -0.5, 0.5) : 0.0);
    out.write(float4(float2(best) + sub, 0, 0), gid);
}
static float med9(thread float* v) {
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 9; j++) if (v[j] < v[i]) { float t = v[i]; v[i] = v[j]; v[j] = t; }
    return v[4];
}
kernel void mfx_median(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],
                       uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    int2 hi = int2(src.get_width() - 1, src.get_height() - 1);
    float xs[9], ys[9];
    int k = 0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            float2 f = src.read(uint2(clamp(int2(gid) + int2(x, y), int2(0), hi))).rg;
            xs[k] = f.x;
            ys[k++] = f.y;
        }
    dst.write(float4(med9(xs), med9(ys), 0, 0), gid);
}
// the picture-sized motion texture MetalFX reads (pixels of the picture, pointing into the previous one)
kernel void mfx_motion(texture2d<float> flow [[texture(0)]], texture2d<float, access::write> motion [[texture(1)]],
                       sampler s [[sampler(0)]], constant float2& scale [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= motion.get_width() || gid.y >= motion.get_height()) return;
    float2 uv = (float2(gid) + 0.5) / float2(motion.get_width(), motion.get_height());
    motion.write(float4(flow.sample(s, uv).rg * scale, 0, 0), gid);
}
// the halfway picture: both pictures sampled half a motion vector away, blended; where they disagree
// (something uncovered or covered) the newer picture wins
struct IOut { float4 pos [[position]]; float2 uv; };
vertex IOut mfx_interp_vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    IOut o;
    o.uv = p;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    return o;
}
fragment float4 mfx_interp_fs(IOut in [[stage_in]], texture2d<float> cur [[texture(0)]], texture2d<float> prev [[texture(1)]],
                              texture2d<float> motion [[texture(2)]], sampler s [[sampler(0)]]) {
    float2 size = float2(motion.get_width(), motion.get_height());
    float2 f = motion.read(uint2(min(in.pos.xy, size - 1.0))).rg;  // pixels: where this pixel was in prev
    float2 h = 0.5 * f / size;
    float4 a = cur.sample(s, in.uv - h), b = prev.sample(s, in.uv + h);
    float d = abs(dot(a.rgb - b.rgb, float3(0.299, 0.587, 0.114)));
    return mix(b, a, 0.5 + 0.5 * smoothstep(0.08, 0.25, d));
}
)";

static id<MTLComputePipelineState> g_k_luma, g_k_down, g_k_search, g_k_median, g_k_motion;
static id<MTLSamplerState> g_linear;
static id<MTLLibrary> g_motion_lib;
// the halfway picture's pipeline, per pixel format
static id<MTLRenderPipelineState> interp_pipeline(MTLPixelFormat fmt) {
    static MTLPixelFormat madeFor = MTLPixelFormatInvalid;
    static id<MTLRenderPipelineState> p = nil;
    if (p && madeFor == fmt) return p;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [g_motion_lib newFunctionWithName:@"mfx_interp_vs"];
    d.fragmentFunction = [g_motion_lib newFunctionWithName:@"mfx_interp_fs"];
    d.colorAttachments[0].pixelFormat = fmt;
    NSError* err = nil;
    p = [R.device newRenderPipelineStateWithDescriptor:d error:&err];
    madeFor = fmt;
    if (!p) LOG("[metalfx] halfway picture pipeline (format %lu): %s", (unsigned long)fmt, err.localizedDescription.UTF8String);
    return p;
}
// WWHD_METALFX_INTERPOLATOR=1: MetalFX's frame interpolator makes the halfway picture (see above)
static const bool g_use_fx_interpolator = [] { const char* e = getenv("WWHD_METALFX_INTERPOLATOR"); return e && atoi(e) != 0; }();
static bool build_kernels() {
    if (g_k_luma) return true;
    NSError* err = nil;
    id<MTLLibrary> lib = [R.device newLibraryWithSource:@(kMotionShader) options:nil error:&err];
    if (!lib) {
        LOG("[metalfx] motion shaders: %s", err.localizedDescription.UTF8String);
        return false;
    }
    g_motion_lib = lib;
    auto k = [&](const char* name) {
        NSError* e = nil;
        id<MTLComputePipelineState> p = [R.device newComputePipelineStateWithFunction:[lib newFunctionWithName:@(name)] error:&e];
        if (!p) LOG("[metalfx] %s: %s", name, e.localizedDescription.UTF8String);
        return p;
    };
    g_k_luma = k("mfx_luma");
    g_k_down = k("mfx_down");
    g_k_search = k("mfx_search");
    g_k_median = k("mfx_median");
    g_k_motion = k("mfx_motion");
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g_linear = [R.device newSamplerStateWithDescriptor:sd];
    if (!(g_k_luma && g_k_down && g_k_search && g_k_median && g_k_motion)) {
        g_k_luma = nil;
        return false;
    }
    return true;
}
static void dispatch(id<MTLComputeCommandEncoder> e, id<MTLComputePipelineState> p, id<MTLTexture> out) {
    [e setComputePipelineState:p];
    [e dispatchThreads:MTLSizeMake(out.width, out.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
}

// ---------------------------------------------------------------- state
struct Scaler {
    id<MTLFXSpatialScaler> fx = nil;
    NSUInteger inW = 0, inH = 0, outW = 0, outH = 0;
    MTLPixelFormat fmt = MTLPixelFormatInvalid;
    bool failed = false;  // this configuration could not be made: not retried until it changes
};
static Scaler g_sc;

static const int kLevels = 4;  // 1/4, 1/8, 1/16, 1/32 of the picture
struct Gen {
    id g_fi = nil;  // id<MTLFXFrameInterpolator> (macOS 26)
    NSUInteger w = 0, h = 0;
    MTLPixelFormat fmt = MTLPixelFormatInvalid;
    bool failed = false;
    id<MTLTexture> pic[2] = {nil, nil};  // this frame's and the previous frame's picture (ring)
    id<MTLTexture> mid = nil, motion = nil, depth = nil;
    id<MTLTexture> lum[2][kLevels] = {}, flow[kLevels] = {}, flowMed = nil;
    int cur = 0;
    bool havePrev = false;
    float prevDyn = 1.0f;
    uint64_t prevFrame = ~0ull;
    bool active = false;   // two presents per game frame fit (hysteresis)
    double lastT = 0, dtEma = 0;
    uint64_t made = 0, passed = 0;  // since the last log line
};
static Gen g_gen;

void init() {
    bool up = [MTLFXSpatialScalerDescriptor supportsDevice:R.device];
    bool fxi = false;
    if (@available(macOS 26.0, *)) fxi = [MTLFXFrameInterpolatorDescriptor supportsDevice:R.device];
    const bool fg = g_use_fx_interpolator ? fxi : true;  // the halfway picture of our own needs nothing more
    mfx::set_available(up, fg);
    LOG("[metalfx] spatial upscaling %s, frame generation %s (%s)", up ? "available" : "not available", fg ? "available" : "not available",
        g_use_fx_interpolator ? "MetalFX frame interpolator, experimental" : "motion-compensated halfway picture");
}

// the spatial scaler for this configuration (nil: unavailable)
static id<MTLFXSpatialScaler> scaler(id<MTLTexture> in, NSUInteger outW, NSUInteger outH, bool linear) {
    Scaler& s = g_sc;
    if (s.inW != in.width || s.inH != in.height || s.outW != outW || s.outH != outH || s.fmt != in.pixelFormat) {
        s = Scaler{};
        s.inW = in.width;
        s.inH = in.height;
        s.outW = outW;
        s.outH = outH;
        s.fmt = in.pixelFormat;
        MTLFXSpatialScalerDescriptor* d = [MTLFXSpatialScalerDescriptor new];
        d.inputWidth = in.width;
        d.inputHeight = in.height;
        d.outputWidth = outW;
        d.outputHeight = outH;
        d.colorTextureFormat = in.pixelFormat;
        d.outputTextureFormat = in.pixelFormat;
        d.colorProcessingMode = linear ? MTLFXSpatialScalerColorProcessingModeLinear : MTLFXSpatialScalerColorProcessingModePerceptual;
        s.fx = [d newSpatialScalerWithDevice:R.device];
        if (s.fx && (in.usage & s.fx.colorTextureUsage) != s.fx.colorTextureUsage) {
            LOG("[metalfx] the scan image lacks the scaler's texture usage %lx: no upscaling", (unsigned long)s.fx.colorTextureUsage);
            s.fx = nil;
        }
        s.failed = s.fx == nil;
        if (g_log || s.failed)
            LOG("[metalfx] spatial scaler %lux%lu -> %lux%lu (format %lu)%s", (unsigned long)in.width, (unsigned long)in.height,
                (unsigned long)outW, (unsigned long)outH, (unsigned long)in.pixelFormat, s.failed ? ": could not be made" : "");
    }
    return s.failed ? nil : s.fx;
}

static MTLTextureUsage kPicUsage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;

// the pictures and the interpolator for this size; false: frame generation cannot run at it
static bool gen_resources(NSUInteger w, NSUInteger h, MTLPixelFormat fmt) {
    Gen& g = g_gen;
    if (g.w == w && g.h == h && g.fmt == fmt) return !g.failed;
    const bool wasActive = g.active;
    const double lastT = g.lastT, dtEma = g.dtEma;
    g = Gen{};
    g.active = wasActive;
    g.lastT = lastT;
    g.dtEma = dtEma;
    g.w = w;
    g.h = h;
    g.fmt = fmt;
    if (!build_kernels() || !interp_pipeline(fmt)) { g.failed = true; return false; }
    MTLTextureUsage colorUse = 0, outUse = 0, motionUse = 0;
    if (!g_use_fx_interpolator) {
        // nothing more
    } else if (@available(macOS 26.0, *)) {
        MTLFXFrameInterpolatorDescriptor* d = [MTLFXFrameInterpolatorDescriptor new];
        d.colorTextureFormat = fmt;
        d.outputTextureFormat = fmt;
        d.depthTextureFormat = MTLPixelFormatDepth32Float;
        d.motionTextureFormat = MTLPixelFormatRG16Float;
        d.inputWidth = w;
        d.inputHeight = h;
        d.outputWidth = w;
        d.outputHeight = h;
        id<MTLFXFrameInterpolator> fi = [d newFrameInterpolatorWithDevice:R.device];
        if (!fi) {
            LOG("[metalfx] frame interpolator %lux%lu (format %lu) could not be made", (unsigned long)w, (unsigned long)h, (unsigned long)fmt);
            g.failed = true;
            return false;
        }
        g.g_fi = fi;
        colorUse = fi.colorTextureUsage;
        outUse = fi.outputTextureUsage;
        motionUse = fi.motionTextureUsage;
        g.depth = make_tex(MTLPixelFormatDepth32Float, w, h, MTLTextureUsageRenderTarget | fi.depthTextureUsage, "metalfx depth");
        // one constant depth (the game's is not part of the scan image); 0 is the far plane (depthReversed)
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = g.depth;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 0.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        [[command_buffer() renderCommandEncoderWithDescriptor:rp] endEncoding];
    } else {
        g.failed = true;
        return false;
    }
    for (auto& p : g.pic) p = make_tex(fmt, w, h, kPicUsage | colorUse, "metalfx picture");
    g.mid = make_tex(fmt, w, h, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | outUse, "metalfx in-between");
    g.motion = make_tex(MTLPixelFormatRG16Float, w, h, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | motionUse, "metalfx motion");
    NSUInteger lw = (w + 3) / 4, lh = (h + 3) / 4;
    for (int l = 0; l < kLevels; l++) {
        for (int i = 0; i < 2; i++)
            g.lum[i][l] = make_tex(MTLPixelFormatR16Float, lw, lh, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, "metalfx luma");
        g.flow[l] = make_tex(MTLPixelFormatRG16Float, lw, lh, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, "metalfx flow");
        if (l == 0) g.flowMed = make_tex(MTLPixelFormatRG16Float, lw, lh, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, "metalfx flow");
        lw = std::max<NSUInteger>((lw + 1) / 2, 1);
        lh = std::max<NSUInteger>((lh + 1) / 2, 1);
    }
    if (g_log) LOG("[metalfx] frame generation at %lux%lu (format %lu)", (unsigned long)w, (unsigned long)h, (unsigned long)fmt);
    return true;
}

// luma pyramid of pictures[i] into lum[i]
static void encode_pyramid(id<MTLComputeCommandEncoder> e, int i, bool linear) {
    Gen& g = g_gen;
    int lin = linear ? 1 : 0;
    [e setTexture:g.pic[i] atIndex:0];
    [e setTexture:g.lum[i][0] atIndex:1];
    [e setSamplerState:g_linear atIndex:0];
    [e setBytes:&lin length:sizeof lin atIndex:0];
    dispatch(e, g_k_luma, g.lum[i][0]);
    for (int l = 1; l < kLevels; l++) {
        [e setTexture:g.lum[i][l - 1] atIndex:0];
        [e setTexture:g.lum[i][l] atIndex:1];
        dispatch(e, g_k_down, g.lum[i][l]);
    }
}

static void encode_motion(bool linear) {
    Gen& g = g_gen;
    const int c = g.cur, p = g.cur ^ 1;
    id<MTLComputeCommandEncoder> e = [command_buffer() computeCommandEncoder];
    e.label = @"metalfx motion";
    encode_pyramid(e, c, linear);  // the previous picture's pyramid is from the last frame
    struct { int radius, hasCoarse; float lambda; } sp;
    static const int radii[kLevels] = {1, 2, 2, 4};
    for (int l = kLevels - 1; l >= 0; l--) {
        sp.radius = radii[l];
        sp.hasCoarse = l < kLevels - 1;
        sp.lambda = 0.02f;
        [e setTexture:g.lum[c][l] atIndex:0];
        [e setTexture:g.lum[p][l] atIndex:1];
        [e setTexture:sp.hasCoarse ? g.flow[l + 1] : g.flowMed atIndex:2];  // (unread at the coarsest level)
        [e setTexture:g.flow[l] atIndex:3];
        [e setBytes:&sp length:sizeof sp atIndex:0];
        dispatch(e, g_k_search, g.flow[l]);
    }
    [e setTexture:g.flow[0] atIndex:0];
    [e setTexture:g.flowMed atIndex:1];
    dispatch(e, g_k_median, g.flowMed);
    float scale[2] = {float(g.w) / float(g.flowMed.width), float(g.h) / float(g.flowMed.height)};
    [e setTexture:g.flowMed atIndex:0];
    [e setTexture:g.motion atIndex:1];
    [e setSamplerState:g_linear atIndex:0];
    [e setBytes:scale length:sizeof scale atIndex:0];
    dispatch(e, g_k_motion, g.motion);
    [e endEncoding];
}

static double g_second_delay = 0;
double second_present_delay() { return g_second_delay; }

static void publish_status(const char* upscaleLine, const std::string& genLine) {
    static uint64_t last = 0;
    if (R.frame - last < 30 && last) return;
    last = R.frame;
    std::string s = upscaleLine;
    if (!genLine.empty()) s += (s.empty() ? "" : "\n") + genLine;
    mfx::set_status(s);
}

bool frame(float boxW, float boxH, Picture* real, Picture* mid) {
    *real = Picture{R.tv.tex, R.tv.srgb.load(), R.tv.dyn};
    *mid = Picture{};
    g_second_delay = 0;
    id<MTLTexture> in = R.tv.tex;
    const bool wantUp = mfx::upscale(), wantGen = mfx::framegen();
    if (!in || (!wantUp && !wantGen)) {
        if (g_gen.w) g_gen = Gen{};  // frees the pictures
        if (!wantUp) g_sc = Scaler{};
        if (!mfx::status().empty()) mfx::set_status("");
        return false;
    }
    end_encoder();
    const bool linear = R.tv.srgb;
    const NSUInteger outW = (NSUInteger)std::lround(std::max(boxW, 1.0f)), outH = (NSUInteger)std::lround(std::max(boxH, 1.0f));
    char upLine[160] = "";
    // ---- frame timing (frame generation's gate)
    Gen& g = g_gen;
    const double t = CACurrentMediaTime();
    const double dt = g.lastT > 0 ? t - g.lastT : 0;
    g.lastT = t;
    const bool gap = dt <= 0 || dt > 0.25;  // first frame, load or stall
    if (!gap) g.dtEma = g.dtEma > 0 ? g.dtEma * 0.9 + dt * 0.1 : dt;
    const int hz = interp::display_hz() > 0 ? interp::display_hz() : 60;
    const double refresh = 1.0 / hz;
    const bool vsync = !gx2::present_unsynced();
    if (!wantGen || !vsync) g.active = false;
    else if (!g.active && g.dtEma >= 1.8 * refresh) g.active = true;
    else if (g.active && g.dtEma < 1.6 * refresh) g.active = false;

    // ---- upscaling
    const bool upNeeded = wantUp && (outW > in.width + 1 || outH > in.height + 1);
    id<MTLFXSpatialScaler> sc = upNeeded ? scaler(in, outW, outH, linear) : nil;
    if (wantUp) {
        if (sc) snprintf(upLine, sizeof upLine, "Upscaling %lux%lu to %lux%lu (MetalFX spatial)", (unsigned long)in.width,
                         (unsigned long)in.height, (unsigned long)outW, (unsigned long)outH);
        else if (!upNeeded) snprintf(upLine, sizeof upLine, "Upscaling: not needed (the window is not larger than the picture)");
        else snprintf(upLine, sizeof upLine, "Upscaling: MetalFX could not scale this picture (see the log)");
    } else {
        g_sc = Scaler{};
    }
    const NSUInteger picW = sc ? outW : in.width, picH = sc ? outH : in.height;
    const float picDyn = sc ? 1.0f : R.tv.dyn;

    // ---- frame generation off (or not fitting): upscale into one picture and present it
    const bool genOk = g.active && gen_resources(picW, picH, in.pixelFormat);
    std::string genLine;
    if (wantGen) {
        char b[200];
        if (!vsync) snprintf(b, sizeof b, "Frame generation: off while presenting without vsync (uncapped)");
        else if (g.failed) snprintf(b, sizeof b, "Frame generation: MetalFX could not interpolate this picture (see the log)");
        else if (!g.active)
            snprintf(b, sizeof b, "Frame generation: waiting (the game runs at %.0f fps, more than half of the display's %d Hz)",
                     g.dtEma > 0 ? 1.0 / g.dtEma : 0.0, hz);
        else snprintf(b, sizeof b, "Frame generation: %.0f -> %.0f fps (MetalFX)", 1.0 / g.dtEma, 2.0 / g.dtEma);
        genLine = b;
    }
    publish_status(upLine, genLine);
    if (!genOk) {
        if (g.havePrev) g.havePrev = false;
        if (!wantGen && g.w) g = Gen{};
        if (!sc) return false;
        // a picture of its own for the upscaled image (the ring's first texture when frame generation has one)
        static id<MTLTexture> out = nil;
        if (!out || out.width != outW || out.height != outH || out.pixelFormat != in.pixelFormat)
            out = make_tex(in.pixelFormat, outW, outH, kPicUsage | sc.outputTextureUsage, "metalfx upscaled");
        sc.colorTexture = in;
        sc.outputTexture = out;
        sc.inputContentWidth = in.width;
        sc.inputContentHeight = in.height;
        [sc encodeToCommandBuffer:command_buffer()];
        *real = Picture{out, linear, 1.0f};
        return false;
    }

    // ---- frame generation: this frame's picture into the ring
    g.cur ^= 1;
    id<MTLTexture> pic = g.pic[g.cur];
    if (sc) {
        sc.colorTexture = in;
        sc.outputTexture = pic;
        sc.inputContentWidth = in.width;
        sc.inputContentHeight = in.height;
        [sc encodeToCommandBuffer:command_buffer()];
    } else {
        id<MTLBlitCommandEncoder> b = TIMED_BLIT();
        [b copyFromTexture:in sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(picW, picH, 1)
                 toTexture:pic destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
    }
    *real = Picture{pic, linear, picDyn};
    const bool consecutive = g.havePrev && !gap && g.prevFrame + 1 == R.frame && g.prevDyn == picDyn;
    g.prevFrame = R.frame;
    g.prevDyn = picDyn;
    if (!consecutive) {
        // history starts here: the pyramid of this picture for the next frame
        id<MTLComputeCommandEncoder> e = [command_buffer() computeCommandEncoder];
        encode_pyramid(e, g.cur, linear);
        [e endEncoding];
        g.havePrev = true;
        g.passed++;
        return false;
    }
    encode_motion(linear);
    if (!g.g_fi) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = g.mid;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        time_render_pass(rp, __FILE_NAME__, __LINE__);
        id<MTLRenderCommandEncoder> e = [command_buffer() renderCommandEncoderWithDescriptor:rp];
        e.label = @"metalfx halfway picture";
        [e setRenderPipelineState:interp_pipeline(g.fmt)];
        [e setFragmentTexture:pic atIndex:0];
        [e setFragmentTexture:g.pic[g.cur ^ 1] atIndex:1];
        [e setFragmentTexture:g.motion atIndex:2];
        [e setFragmentSamplerState:g_linear atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
    } else if (@available(macOS 26.0, *)) {
        id<MTLFXFrameInterpolator> fi = g.g_fi;
        fi.colorTexture = pic;
        fi.prevColorTexture = g.pic[g.cur ^ 1];
        fi.motionTexture = g.motion;
        fi.depthTexture = g.depth;
        fi.outputTexture = g.mid;
        fi.motionVectorScaleX = 1.0f;
        fi.motionVectorScaleY = 1.0f;
        fi.deltaTime = (float)g.dtEma;
        fi.nearPlane = 0.1f;
        fi.farPlane = 10000.0f;
        fi.fieldOfView = 50.0f;
        fi.aspectRatio = float(picW) / float(picH);
        fi.jitterOffsetX = 0.0f;
        fi.jitterOffsetY = 0.0f;
        fi.shouldResetHistory = NO;
        // macOS 27: the region to interpolate (left at 0 it is empty, and the output is a copy of the colour)
        if (@available(macOS 27.0, *)) {
            fi.contentWidth = picW;
            fi.contentHeight = picH;
        }
        [fi encodeToCommandBuffer:command_buffer()];
    }
    *mid = Picture{g.mid, linear, picDyn};
    // the real picture half a game frame after the in-between one (a quarter refresh early: the next vsync)
    g_second_delay = std::max(refresh * 0.5, g.dtEma * 0.5 - refresh * 0.25);
    g.made++;
    if (g_log && (g.made + g.passed) % 300 == 0)
        LOG("[metalfx] frame generation: %llu in-between frames, %llu passed through; game %.1f ms/frame, display %d Hz",
            (unsigned long long)g.made, (unsigned long long)g.passed, g.dtEma * 1e3, hz);
    return true;
}

}  // namespace metalfx
}  // namespace gfx
