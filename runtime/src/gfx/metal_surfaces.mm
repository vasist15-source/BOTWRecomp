#include <set>
#include <chrono>
#include "mods/cemu_pack.h"
#include <tuple>
#include <atomic>
#include <cmath>
#include <vector>
// Guest surfaces <-> Metal textures: render targets, depth buffers, sampled textures.
// Tiled layouts are decoded with the vendored LatteAddrLib.
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "gx2/gx2.h"
#include "render_prof.h"
#include "gx2_texture_regs.h"
#include "metal.h"

#include "runtime.h"
#include "write_watch.h"
#define XXH_INLINE_ALL
#include "../../third_party/xxhash/xxhash.h"

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N&, const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N&);

namespace gfx {
MTLTextureUsage format_view_usage();
static const char* g_resample_why = nullptr;  // resample's caller, for WWHD_GPU_PASS_TIMES


static MTLTextureType texture_type(uint32_t dim, uint32_t slices) {
    switch ((Latte::E_DIM)dim) {
    case Latte::E_DIM::DIM_1D: return MTLTextureType1D;
    case Latte::E_DIM::DIM_3D: return MTLTextureType3D;
    case Latte::E_DIM::DIM_CUBEMAP: return MTLTextureTypeCube;
    case Latte::E_DIM::DIM_1D_ARRAY: return MTLTextureType1DArray;
    case Latte::E_DIM::DIM_2D_ARRAY: case Latte::E_DIM::DIM_2D_ARRAY_MSAA: return MTLTextureType2DArray;
    default: return slices > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    }
}

uint64_t next_write_seq() {
    static uint64_t seq = 0;
    return ++seq;
}

// ---------------------------------------------------------------- internal resolution
// Render targets are allocated at res_scale() x their guest size. Everything that talks to the game
// (lookups, aliasing, guest memory) uses the guest size; draws scale their viewport and scissor, and
// shaders sample with normalized coordinates, so they see the same picture at more pixels.
static float parse_scale(const char* e) {
    float f = e ? (float)atof(e) : 1.0f;
    return std::clamp(f > 0 ? f : 1.0f, 1.0f, 4.0f);
}
static std::atomic<float> g_res_requested{parse_scale(getenv("WWHD_RES_SCALE"))};
static float g_res_frame = g_res_requested.load();
static void latch_aspect();  // render thread: the factor for this frame
static void rescale_all_surfaces();
static void clear_surface_to_zero(Surface* t);
static float g_aspect_kx_latched();
static float g_aspect_ky_latched();
float res_scale() { return g_res_frame; }
void set_res_scale(float f) {
    g_res_requested = std::clamp(f, 1.0f, 4.0f);
    LOG("[gfx] internal resolution %gx", g_res_requested.load());
}
void latch_res_scale() {
    // test aid: WWHD_RES_SCALE_AT=frame:factor,... switches the factor at those frames
    static std::vector<std::pair<uint64_t, float>> at = [] {
        std::vector<std::pair<uint64_t, float>> v;
        if (const char* e = getenv("WWHD_RES_SCALE_AT"))
            for (char* p = (char*)e; *p;) {
                uint64_t f = strtoull(p, &p, 10);
                if (*p++ != ':') break;
                v.push_back({f, (float)strtod(p, &p)});
                while (*p == ',') p++;
            }
        return v;
    }();
    for (auto& [f, v] : at)
        if (R.frame == f) set_res_scale(v);
    const float before = g_res_frame, kx = g_aspect_kx_latched(), ky = g_aspect_ky_latched();
    g_res_frame = g_res_requested.load(std::memory_order_relaxed);
    latch_aspect();
    // every surface made at the old factor follows at once: one left at the old factor (sampled, not
    // drawn to since) can no longer be synced with its related surfaces and keeps stale contents
    if (g_res_frame != before || kx != g_aspect_kx_latched() || ky != g_aspect_ky_latched()) rescale_all_surfaces();
}

// ---------------------------------------------------------------- aspect ratio
// The game renders a 16:9 screen (1280x720 guest pixels). At another aspect ratio (aspect.cpp) the
// game's projections are widened (or made taller) and every screen-shaped render target is allocated
// that much wider (taller) than its guest size: draws keep their guest viewports, which cover the
// whole texture, so the picture comes out at the new shape without the game knowing. kx/ky: texture
// pixels per guest pixel on top of the internal resolution, (A / (16/9), 1) for wider screens,
// (1, (16/9) / A) for narrower ones. Latched at the frame boundary like the resolution.
static std::atomic<float> g_aspect_requested{16.0f / 9.0f};
static float g_aspect_kx = 1.0f, g_aspect_ky = 1.0f;  // render thread: factors for this frame
void set_frame_aspect(float a) { g_aspect_requested.store(a, std::memory_order_relaxed); }
static void latch_aspect() {
    float a = g_aspect_requested.load(std::memory_order_relaxed), base = 16.0f / 9.0f;
    float kx = a >= base ? a / base : 1.0f, ky = a >= base ? 1.0f : base / a;
    if (kx != g_aspect_kx || ky != g_aspect_ky) LOG("[gfx] aspect %.4f: screen targets x%.4f wide, x%.4f tall", a, kx, ky);
    g_aspect_kx = kx;
    g_aspect_ky = ky;
}
static float g_aspect_kx_latched() { return g_aspect_kx; }
static float g_aspect_ky_latched() { return g_aspect_ky; }
// the game's screen-sized buffers and their reductions (1920x1080 ... 60x33); not the GamePad's
// (854x480, shown in its own window), not shadow maps, mip chains or textures
static bool screen_shaped(const Surface* s) {
    if (s->fmt.compressed || s->mips > 1 || s->slices > 1 || s->width < 32) return false;
    for (uint32_t w = 854, h = 480; w >= 32; w >>= 1, h >>= 1)
        if ((s->width == w || s->width == w + 1) && s->height == h) return false;
    float r = (float)s->width * 9.0f / ((float)s->height * 16.0f);
    return r > 0.97f && r < 1.03f;
}

// the factor a render target gets. Shadow maps (depth arrays: the game's cascades) scale with the
// internal resolution by default. WWHD_SHADOW_SCALE=n gives them their own factor; =1 keeps the
// console's 1024x1024, which uses far less GPU memory at 2x/3x. Issue #67: the hard, crawling
// shadow edges at 2x in v0.2.6-v0.2.8 came mainly from the missing mip chains the game's
// shadow-mask softening samples (restored in v0.2.9); since then both sizes give practically the
// same soft edges, the larger maps only a hair crisper.
static float target_scale_rules(const Surface* s);
static float target_scale(const Surface* s) { return std::max(target_scale_rules(s), s->minScale); }
static float target_scale_rules(const Surface* s) {
    uint32_t width,height;
    if(!s->fmt.compressed&&s->mips==1&&mods::cemu::texture_extent(s->width,s->height,s->format,s->slices,s->tileMode,width,height))return 1.0f;
    if (s->fmt.compressed) return 1.0f;
    static const float shadow = getenv("WWHD_SHADOW_SCALE") ? parse_scale(getenv("WWHD_SHADOW_SCALE")) : 0.0f;
    static const bool shadow_logged = [] {  // issue #67: show that the switch was picked up
        if (shadow) LOG("[gfx] WWHD_SHADOW_SCALE=%g: shadow maps at %gx the console's 1024x1024", shadow, shadow);
        return true;
    }();
    (void)shadow_logged;
    if (s->isDepth && s->slices > 1) return shadow ? shadow : res_scale();
    // which targets follow the internal resolution: the sizes and formats of the BotW "Graphics" pack's
    // TextureRedefine rules (what Cemu scales; everything else keeps the console's size). Scaling every
    // target broke data targets the game reads by position (white water at 2x).
    // WWHD_SCALE_ALL_TARGETS=1: every target as before.
    struct Rule { uint16_t w, h; std::initializer_list<uint16_t> formats; bool notLinearAligned; };
    static const Rule kRules[] = {
        {1280, 720, {0x001,0x005,0x007,0x019,0x01a,0x41a,0x80e,0x806,0x816,0x820}, true},
        {864, 480, {0x019}, false},
        {854, 480, {0x019}, false},
        {640, 368, {0x001,0x005,0x007,0x019,0x01a,0x80e,0x806,0x816,0x820,0x41a}, false},
        {640, 360, {0x001,0x005,0x007,0x019,0x01a,0x80e,0x806,0x816,0x820,0x41a}, true},
        {384, 192, {0x001}, false},
        {320, 192, {0x001,0x005,0x007,0x019,0x01a,0x80e,0x816,0x806,0x820,0x41a}, false},
        {320, 180, {0x001,0x005,0x007,0x019,0x01a,0x80e,0x816,0x806,0x820,0x41a}, false},
        {192, 96, {0x007,0x806}, false},
        {160, 96, {0x001,0x005,0x007,0x806,0x80e,0x816}, false},
        {160, 90, {0x001,0x005,0x007,0x806,0x80e,0x816}, false},
        {128, 48, {0x806,0x816,0x005,0x007,0x820}, false},
        {96, 48, {0x816,0x80e}, false},
        {80, 45, {0x816,0x806,0x007}, false},
        {64, 64, {0x80e}, false},
        {128, 720, {0x019}, false},
        {110, 720, {0x019}, false},
        {1024, 512, {0x816}, false},
        {512, 256, {0x816,0x001}, false},
        {256, 128, {0x816,0x001}, false},
        {128, 64, {0x816}, false},
        {64, 32, {0x816}, false},
        {32, 16, {0x816}, false},
        {24, 16, {0x816}, false},
        {40, 22, {0x816}, false},
        {20, 11, {0x816}, false},
        {960, 540, {0x816,0x005,0x007,0x019,0x01a,0x41a,0x80e,0x806}, true},
        {960, 544, {0x816,0x005,0x007,0x019,0x01a,0x41a,0x80e,0x806}, false},
        {512, 272, {0x001,0x005}, false},
        {480, 272, {0x816,0x01a,0x820,0x001,0x80e}, false},
        {480, 270, {0x816,0x01a,0x820,0x001,0x80e}, false},
        {512, 512, {0x816}, false},
        {1504, 720, {0x019}, false},
        {1500, 720, {0x019}, false},
        {1280, 608, {0x019}, false},
        {1280, 400, {0x019}, false},
        {192, 192, {0x019}, false},
        {1920, 912, {0x001}, false},
        {512, 288, {0x01a,0x80e}, false},
        {1280, 288, {0x019}, false},
        {1280, 278, {0x019}, false},
        {293, 185, {0x01a}, false},
        {512, 96, {0x01a}, false},
        {500, 94, {0x01a}, false},
        {480, 32, {0x01a}, false},
        {464, 28, {0x01a}, false},
        {768, 720, {0x005}, false},
        {720, 720, {0x005}, false},
        {384, 368, {0x005}, false},
        {384, 360, {0x005}, false},
        {360, 360, {0x005}, false},
    };
    static const bool all = getenv("WWHD_SCALE_ALL_TARGETS") != nullptr;
    if (all) return res_scale();
    for (const Rule& r : kRules) {
        if (r.w != s->width || r.h != s->height) continue;
        if (r.notLinearAligned && s->tileMode == 1) continue;  // tileModesExcluded = 0x001 (video playback)
        for (uint16_t f : r.formats)
            if (f == s->format) return res_scale();
    }
    return 1.0f;
}
bool target_aspect_factors(uint32_t w, uint32_t h, float& kx, float& ky) {
    Surface s;
    s.width = w;
    s.height = h;
    bool on = screen_shaped(&s) && (g_aspect_kx != 1.0f || g_aspect_ky != 1.0f);
    kx = on ? g_aspect_kx : 1.0f;
    ky = on ? g_aspect_ky : 1.0f;
    return on;
}
// extra horizontal / vertical factor for the aspect ratio
static void target_aspect(const Surface* s, float& kx, float& ky) {
    uint32_t width,height;
    if(!s->fmt.compressed&&s->mips==1&&mods::cemu::texture_extent(s->width,s->height,s->format,s->slices,s->tileMode,width,height)){
        kx=float(width)/s->width;ky=float(height)/s->height;return;
    }
    bool on = screen_shaped(s);
    kx = on ? g_aspect_kx : 1.0f;
    ky = on ? g_aspect_ky : 1.0f;
}

static id<MTLTexture> make_texture(Surface* s, MTLTextureType type, float scale, float ax = 1.0f, float ay = 1.0f) {
    bool is1D = type == MTLTextureType1D || type == MTLTextureType1DArray;
    uint32_t pw = s->width, ph = s->height;
    // volumes keep the guest size (they are sampled as lookup tables, not shown)
    if ((scale != 1.0f || ax != 1.0f || ay != 1.0f) && !is1D && type != MTLTextureType3D && !s->fmt.compressed) {
        pw = (uint32_t)std::ceil(s->width * scale * ax - 0.01f);
        ph = (uint32_t)std::ceil(s->height * scale * ay - 0.01f);
    } else {
        scale = 1.0f;
        ax = ay = 1.0f;
    }
    if (type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray) ph = pw;
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType = type;
    td.pixelFormat = s->fmt.pixel;
    td.width = pw;
    td.height = is1D ? 1 : ph;
    td.depth = type == MTLTextureType3D ? s->slices : 1;
    td.arrayLength = (type == MTLTextureType2DArray || type == MTLTextureType1DArray) ? s->slices
                     : type == MTLTextureTypeCubeArray ? s->slices / 6 : 1;
    td.mipmapLevelCount = is1D ? 1 : s->mips;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | format_view_usage();
    td.storageMode = MTLStorageModePrivate;
    if (s->fmt.compressed || is1D) td.usage = MTLTextureUsageShaderRead | format_view_usage();
    id<MTLTexture> t = [R.device newTextureWithDescriptor:td];
    if (t) {
        if (is1D) s->mips = 1;
        s->scale = scale;
        s->ax = ax;
        s->ay = ay;
        s->sx = (float)pw / s->width;
        s->sy = is1D ? 1.0f : (float)ph / s->height;
    }
    return t;
}

// a render target made at another factor (the setting changed, or a CPU texture now rendered to):
// reallocate it at the current one, keeping its contents (filtered)
static Surface* rescale(Surface* s) {
    float want = target_scale(s), ax, ay;
    target_aspect(s, ax, ay);
    if ((s->scale == want && s->ax == ax && s->ay == ay) || !s->tex || s->tex.textureType == MTLTextureType3D) return s;
    id<MTLTexture> old = s->tex;
    float osx = s->sx, osy = s->sy, oscale = s->scale, oax = s->ax, oay = s->ay;
    id<MTLTexture> t = make_texture(s, old.textureType, want, ax, ay);
    if (!t) { s->sx = osx; s->sy = osy; s->scale = oscale; s->ax = oax; s->ay = oay; return s; }
    end_encoder();
    // every level: a new texture's memory is undefined (NaN in float formats), and later passes that
    // blend into a level keep a NaN forever (the bloom mip chain: black lower screen after a switch)
    s->tex = t;
    clear_surface_to_zero(s);
    g_resample_why = "rescale";
    const uint32_t layers = old.textureType == MTLTextureType3D ? 1 : (uint32_t)layer_count(old);
    const NSUInteger levels = std::min(old.mipmapLevelCount, t.mipmapLevelCount);
    if (levels <= 1 || s->fmt.compressed) {
        resample(old, t, s->fmt, layers);
    } else {
        const MTLTextureType vt = old.textureType == MTLTextureTypeCube || old.textureType == MTLTextureTypeCubeArray ? MTLTextureType2DArray
                                                                                                                 : old.textureType;
        for (NSUInteger m = 0; m < levels; m++) {
            id<MTLTexture> a = [old newTextureViewWithPixelFormat:old.pixelFormat textureType:vt levels:NSMakeRange(m, 1)
                                                          slices:NSMakeRange(0, layers)];
            id<MTLTexture> b = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:vt levels:NSMakeRange(m, 1)
                                                        slices:NSMakeRange(0, layers)];
            if (a && b) resample(a, b, s->fmt, layers);
        }
    }
    g_resample_why = nullptr;
    forget_texture_views();
    if (getenv("WWHD_LOG_RESCALE"))
        LOG("[gfx] rescaled %08X %ux%u fmt %X to %lux%lu", s->addr, s->width, s->height, s->format, (unsigned long)t.width,
            (unsigned long)t.height);
    return s;
}

// A depth buffer drawn together with a colour target that is scaled more than it (the pack's rules
// scale some colour formats at a size whose depth format they leave alone) would not fit the pass:
// Metal needs matching sizes, and the draw ran without depth (no depth test or write; BotW's water
// surface never reached the depth buffer at 2x: white water). The depth buffer follows the colour
// target's factor from then on. Only when its guest size covers the colour target's.
Surface* match_depth_scale(Surface* depth, const Surface* color) {
    if (!depth || !color || depth->width < color->width || depth->height < color->height) return depth;
    if (color->scale <= depth->scale || depth->minScale >= color->scale) return depth;
    depth->minScale = color->scale;
    static int logged = 0;
    if (logged++ < 20)
        LOG("[gfx] depth %08X %ux%u f%X scaled %gx to match colour %08X %ux%u f%X", depth->addr, depth->width, depth->height,
            depth->format, color->scale, color->addr, color->width, color->height, color->format);
    return rescale(depth);
}

// fullscreen-triangle copy with filtering; one pipeline per destination format
static const char* kResampleShader = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut rs_vs(uint vid [[vertex_id]], constant float2& uvMax [[buffer(0)]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv = p * uvMax;
    return o;
}
fragment float4 rs_float(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, in.uv);
}
fragment uint4 rs_uint(VOut in [[stage_in]], texture2d<uint> t [[texture(0)]]) {
    return t.read(uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1))));
}
fragment int4 rs_sint(VOut in [[stage_in]], texture2d<int> t [[texture(0)]]) {
    return t.read(uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1))));
}
struct DOut { float d [[depth(any)]]; };
fragment DOut rs_depth(VOut in [[stage_in]], depth2d<float> t [[texture(0)]]) {
    DOut o;
    o.d = t.read(uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1))));
    return o;
}
)";

// Textures are made without MTLTextureUsagePixelFormatView: on Apple GPUs that flag turns off lossless
// framebuffer compression, which at 2x roughly doubles render target memory traffic. Copies between
// formats of one texel size go through a buffer instead (WWHD_FORMAT_VIEWS=1: the old views).
MTLTextureUsage format_view_usage() {
    static const bool on = getenv("WWHD_FORMAT_VIEWS") != nullptr;
    return on ? MTLTextureUsagePixelFormatView : 0;
}

// the bits of src's level/slice region into dst (same texel size, any formats) through a scratch buffer
static void copy_bits_via_buffer(id<MTLBlitCommandEncoder> b, id<MTLTexture> src, NSUInteger srcSlice, NSUInteger srcLevel,
                                 id<MTLTexture> dst, NSUInteger dstSlice, NSUInteger dstLevel, MTLSize size, NSUInteger bytesPerPixel) {
    NSUInteger row = size.width * bytesPerPixel, image = row * size.height, len = image * size.depth;
    if (!len) return;
    id<MTLBuffer> tmp = [R.device newBufferWithLength:len options:MTLResourceStorageModePrivate];
    if (!tmp) return;
    [b copyFromTexture:src sourceSlice:srcSlice sourceLevel:srcLevel sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
              toBuffer:tmp destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:image];
    [b copyFromBuffer:tmp sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:image sourceSize:size
            toTexture:dst destinationSlice:dstSlice destinationLevel:dstLevel destinationOrigin:MTLOriginMake(0, 0, 0)];
}

void resample(id<MTLTexture> src, id<MTLTexture> dst, const FormatInfo& fmt, uint32_t slices, float uMax, float vMax, uint32_t dstW,
              uint32_t dstH) {
    static id<MTLLibrary> lib;
    static id<MTLSamplerState> linear;
    static std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipes;
    if (!lib) {
        NSError* err = nil;
        lib = [R.device newLibraryWithSource:[NSString stringWithUTF8String:kResampleShader] options:nil error:&err];
        if (!lib) { LOG("[gfx] resample shader: %s", err.localizedDescription.UTF8String); return; }
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        linear = [R.device newSamplerStateWithDescriptor:sd];
    }
    if (fmt.compressed) return;
    uint64_t key = (uint64_t)dst.pixelFormat;
    auto it = pipes.find(key);
    if (it == pipes.end()) {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"rs_vs"];
        NSString* fs = fmt.depth ? @"rs_depth" : fmt.kind == FormatInfo::UINT ? @"rs_uint" : fmt.kind == FormatInfo::SINT ? @"rs_sint" : @"rs_float";
        d.fragmentFunction = [lib newFunctionWithName:fs];
        if (fmt.depth) {
            d.depthAttachmentPixelFormat = dst.pixelFormat;
            if (fmt.stencil) d.stencilAttachmentPixelFormat = dst.pixelFormat;
        } else {
            d.colorAttachments[0].pixelFormat = dst.pixelFormat;
        }
        NSError* err = nil;
        id<MTLRenderPipelineState> p = [R.device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!p) LOG("[gfx] resample pipeline (pixel %lu): %s", (unsigned long)dst.pixelFormat, err.localizedDescription.UTF8String);
        it = pipes.emplace(key, p).first;
    }
    if (!it->second) return;
    static id<MTLDepthStencilState> writeDepth;
    if (!writeDepth) {
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = YES;
        writeDepth = [R.device newDepthStencilStateWithDescriptor:dd];
    }
    end_encoder();
    for (uint32_t z = 0; z < slices; z++) {
        id<MTLTexture> view = [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureType2D
                                                          levels:NSMakeRange(0, 1) slices:NSMakeRange(z, 1)];
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        bool whole = !dstW || (dstW >= dst.width && dstH >= dst.height);
        if (fmt.depth) {
            rp.depthAttachment.texture = dst;
            rp.depthAttachment.slice = z;
            rp.depthAttachment.loadAction = whole ? MTLLoadActionDontCare : MTLLoadActionLoad;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            if (fmt.stencil) {
                rp.stencilAttachment.texture = dst;
                rp.stencilAttachment.slice = z;
                rp.stencilAttachment.loadAction = whole ? MTLLoadActionClear : MTLLoadActionLoad;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
            }
        } else {
            rp.colorAttachments[0].texture = dst;
            rp.colorAttachments[0].slice = z;
            rp.colorAttachments[0].loadAction = whole ? MTLLoadActionDontCare : MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        }
        {
            char why[96];
            snprintf(why, sizeof why, "resample(%s) %lux%lu->%lux%lu px%lu", g_resample_why ? g_resample_why : "?",
                     (unsigned long)src.width, (unsigned long)src.height, (unsigned long)dst.width, (unsigned long)dst.height,
                     (unsigned long)dst.pixelFormat);
            time_render_pass(rp, why, 0);
        }
        id<MTLRenderCommandEncoder> e = [command_buffer() renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:it->second];
        if (fmt.depth) [e setDepthStencilState:writeDepth];
        if (dstW) [e setViewport:MTLViewport{0, 0, (double)dstW, (double)dstH, 0, 1}];
        float uv[2] = {uMax, vMax};
        [e setVertexBytes:uv length:sizeof uv atIndex:0];
        [e setFragmentTexture:view atIndex:0];
        [e setFragmentSamplerState:linear atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
}

// ================================================================ texture cache
// After Cemu's texture cache (Cafe/HW/Latte/Core/LatteTexture.cpp, LatteTextureCache.cpp,
// LatteTextureView.cpp, LatteRenderTarget.cpp, LatteSurfaceCopy.cpp; MPL-2.0), adapted to Metal and to
// this renderer's internal-resolution scaling:
//  - one Surface per guest surface definition, holding all of its mips and slices; samplers and render
//    targets use views into it (a mip/slice range). A lookup with a larger mip or slice range than an
//    existing surface has recreates it bigger (GPU contents copied over)
//  - surfaces sharing memory at the same layout (same address, pitch, tile mode, texel size) are related:
//    a GPU write to one marks the others, which copy the newer slices before their next use
//  - a lookup that a bigger surface can serve as a view (a render target at the address of another
//    surface's mip or slice) becomes that view, and smaller surfaces it absorbs are dropped
//  - surfaces whose memory was overwritten by other GPU writes, or unused CPU textures, are dropped
// Lookups by exact view description: address, size, pitch, mip/slice range, format, dimension.
static uint64_t sparse_hash(Surface* s);
static void check_texture(Surface* s);
uint64_t g_stat_full_checks, g_stat_uploads, g_stat_invalidates, g_stat_invalidated_surfaces;
bool g_repeat_texture_init = false;

namespace {
// ---- view lookup (LatteTextureViewLookupCache)
struct ViewDesc {
    uint32_t addr, width, height, pitch, firstMip, numMip, firstSlice, numSlice, format, dim;
    bool isDepth;
    TexView* view;
};
constexpr uint32_t kViewBuckets = 1061;
std::vector<ViewDesc> g_viewBucket[kViewBuckets], g_viewBucketNoRes[kViewBuckets];
inline uint32_t view_key(uint32_t a, uint32_t w, uint32_t h, uint32_t p) {
    return (uint32_t)(((uint64_t)a + (uint64_t)w * 7 + (uint64_t)h * 11 + (uint64_t)p * 13) % kViewBuckets);
}
inline uint32_t view_key_nores(uint32_t a, uint32_t p) { return (uint32_t)(((uint64_t)a + (uint64_t)p * 13) % kViewBuckets); }

// ---- memory occupancy: which slices of which surfaces cover each 2 MiB of guest memory
struct Occ {
    uint32_t start, end;
    SliceMip* sm;
};
constexpr uint64_t kOccSize = 0x200000;
constexpr uint32_t kOccBuckets = (uint32_t)(0x100000000ull / kOccSize);
std::vector<Occ> g_occ[kOccBuckets];
template <class F>
inline void for_occ_buckets(uint32_t start, uint32_t end, F f) {
    if (end <= start) end = start + 1;
    for (uint64_t b = start / kOccSize; b <= ((uint64_t)end - 1) / kOccSize && b < kOccBuckets; b++) f(g_occ[b]);
}

std::vector<Surface*> g_all;                          // every live surface (cleanup scan)
std::vector<std::unique_ptr<Surface>> g_graveyard;    // deleted this frame, freed at its end
}  // namespace

static void rescale_all_surfaces() {
    size_t n = 0;
    for (Surface* t : g_all) {
        if (t->deleted) continue;
        id<MTLTexture> before = t->tex;
        rescale(t);
        n += t->tex != before;
    }
    LOG("[gfx] %zu surfaces reallocated for internal resolution %gx", n, res_scale());
}

static void view_lookup_add(TexView* v, uint32_t baseMip = 0, uint32_t baseSlice = 0) {
    Surface* t = v->tex;
    ViewDesc d{t->addr, t->width, t->height, t->pitch, v->firstMip, v->numMip, v->firstSlice, v->numSlice, v->format, v->dim, t->isDepth, v};
    if (baseMip || baseSlice) {  // looked up by the address of one of its slices/mips (a sub-texture)
        SliceMip* sm = t->slice_mip(baseSlice, baseMip);
        d.addr = sm->addrStart;
        d.pitch = sm->pitch;
        d.width = std::max(t->width >> baseMip, 1u);
        d.height = std::max(t->height >> baseMip, 1u);
        d.firstMip -= baseMip;
        d.firstSlice -= baseSlice;
    }
    uint32_t k = view_key(d.addr, d.width, d.height, d.pitch);
    g_viewBucket[k].push_back(d);
    if (std::find(v->keys.begin(), v->keys.end(), k) == v->keys.end()) v->keys.push_back(k);
    k = view_key_nores(d.addr, d.pitch);
    g_viewBucketNoRes[k].push_back(d);
    if (std::find(v->keysNoRes.begin(), v->keysNoRes.end(), k) == v->keysNoRes.end()) v->keysNoRes.push_back(k);
}
static void view_lookup_remove(TexView* v) {
    auto drop = [v](std::vector<ViewDesc>& b) { b.erase(std::remove_if(b.begin(), b.end(), [v](const ViewDesc& d) { return d.view == v; }), b.end()); };
    for (uint32_t k : v->keys) drop(g_viewBucket[k]);
    for (uint32_t k : v->keysNoRes) drop(g_viewBucketNoRes[k]);
    v->keys.clear();
    v->keysNoRes.clear();
}
static TexView* view_lookup(uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint32_t firstMip, uint32_t numMip, uint32_t firstSlice,
                            uint32_t numSlice, uint32_t format, uint32_t dim, int isDepth = -1) {
    for (auto& d : g_viewBucket[view_key(addr, w, h, pitch)])
        if (d.format == format && d.dim == dim && d.width == w && d.height == h && d.pitch == pitch && d.addr == addr && d.firstMip == firstMip &&
            d.numMip == numMip && d.firstSlice == firstSlice && d.numSlice == numSlice && (isDepth < 0 || d.isDepth == (isDepth != 0)))
            return d.view;
    return nullptr;
}
// any mip/slice count; isDepth < 0: either kind
static TexView* view_lookup_slice(uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint32_t firstMip, uint32_t firstSlice, uint32_t format,
                                  int isDepth = -1) {
    for (auto& d : g_viewBucket[view_key(addr, w, h, pitch)])
        if (d.width == w && d.height == h && d.pitch == pitch && d.addr == addr && d.format == format && d.firstSlice == firstSlice &&
            d.firstMip == firstMip && (isDepth < 0 || d.isDepth == (isDepth != 0)))
            return d.view;
    return nullptr;
}
// any mip/slice count, at least this size
static TexView* view_lookup_slice_min(uint32_t addr, uint32_t minW, uint32_t minH, uint32_t pitch, uint32_t firstMip, uint32_t firstSlice,
                                      uint32_t format) {
    for (auto& d : g_viewBucketNoRes[view_key_nores(addr, pitch)])
        if (d.width >= minW && d.height >= minH && d.pitch == pitch && d.addr == addr && d.format == format && d.firstSlice == firstSlice &&
            d.firstMip == firstMip)
            return d.view;
    return nullptr;
}

static TexView* get_or_create_view(Surface* t, uint32_t dim, uint32_t format, uint32_t firstMip, uint32_t numMip, uint32_t firstSlice,
                                   uint32_t numSlice) {
    for (TexView* v : t->views)
        if (v->firstMip == firstMip && v->numMip == numMip && v->firstSlice == firstSlice && v->numSlice == numSlice && v->dim == dim &&
            v->format == format)
            return v;
    TexView* v = new TexView;
    v->tex = t;
    v->firstMip = firstMip;
    v->numMip = numMip;
    v->firstSlice = firstSlice;
    v->numSlice = numSlice;
    v->format = format;
    v->dim = dim;
    t->views.push_back(v);
    view_lookup_add(v);
    return v;
}

// ---- slice/mip layout and occupancy
static void init_slice_mips(Surface* t) {
    t->sm.assign((size_t)t->slices * t->mips, SliceMip{});
    for (uint32_t mip = 0; mip < t->mips; mip++) {
        LatteAddrLib::AddrSurfaceInfo_OUT info{};
        LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)t->format, t->width, t->height, t->slices, (Latte::E_DIM)t->dim,
                                              Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)t->tileMode), 0, mip, &info);
        for (uint32_t slice = 0; slice < t->mip_slices(mip); slice++) {
            uint32_t a = 0, size = 0;
            sint32 sub = 0;
            LatteAddrLib::CalculateMipAndSliceAddr(t->addr, t->mipAddr, (Latte::E_GX2SURFFMT)t->format, t->width, t->height, t->slices,
                                                   (Latte::E_DIM)t->dim, (Latte::E_HWTILEMODE)t->tileMode, t->swizzle, 0, mip, slice, &a, &size, &sub);
            SliceMip* sm = t->slice_mip(slice, mip);
            sm->addrStart = a;
            sm->addrEnd = a + size;
            sm->subIndex = (uint32_t)sub;
            sm->slice = slice;
            sm->mip = mip;
            sm->tex = t;
            sm->tileMode = (uint32_t)info.hwTileMode;
            sm->pitch = mip == 0 ? t->pitch : info.pitch;  // the base level: the pitch the hardware was given
        }
    }
}
static void register_occupancy(Surface* t) {
    for (uint32_t mip = 0; mip < t->mips; mip++)
        for (uint32_t slice = 0; slice < t->mip_slices(mip); slice++) {
            SliceMip* sm = t->slice_mip(slice, mip);
            for_occ_buckets(sm->addrStart, sm->addrEnd, [&](std::vector<Occ>& b) { b.push_back({sm->addrStart, sm->addrEnd, sm}); });
        }
}
static void unregister_occupancy(Surface* t) {
    for (uint32_t mip = 0; mip < t->mips; mip++)
        for (uint32_t slice = 0; slice < t->mip_slices(mip); slice++) {
            SliceMip* sm = t->slice_mip(slice, mip);
            for_occ_buckets(sm->addrStart, sm->addrEnd, [&](std::vector<Occ>& b) {
                b.erase(std::remove_if(b.begin(), b.end(), [t](const Occ& o) { return o.sm->tex == t; }), b.end());
            });
        }
}

// ---- format compatibility
// whether two formats' textures may share data (synchronised copies between them)
static bool format_view_compatible(uint32_t a, uint32_t b) {
    if (a == b) return true;
    for (int swap = 0; swap < 2; swap++) {
        const auto fa = (Latte::E_GX2SURFFMT)a, fb = (Latte::E_GX2SURFFMT)b;
        const auto ha = Latte::GetHWFormat(fa), hb = Latte::GetHWFormat(fb);
        if (ha == Latte::E_HWFMT::HWFMT_2_10_10_10 && fb == Latte::E_GX2SURFFMT::R11_G11_B10_FLOAT) return false;
        if (ha == Latte::E_HWFMT::HWFMT_2_10_10_10 && fb == Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM) return false;
        if (fa == Latte::E_GX2SURFFMT::A1_B5_G5_R5_UNORM || fb == Latte::E_GX2SURFFMT::A1_B5_G5_R5_UNORM) return false;
        if (fa == Latte::E_GX2SURFFMT::D24_S8_UNORM && fb == Latte::E_GX2SURFFMT::R10_G10_B10_A2_SNORM) return false;
        if (fa == Latte::E_GX2SURFFMT::R32_FLOAT && fb == Latte::E_GX2SURFFMT::R10_G10_B10_A2_SNORM) return false;
        (void)hb;
        std::swap(a, b);
    }
    return true;
}
static bool texel_size_compatible(uint32_t a, uint32_t b) {
    const auto fa = (Latte::E_GX2SURFFMT)a, fb = (Latte::E_GX2SURFFMT)b;
    if (fa == Latte::E_GX2SURFFMT::D24_S8_UNORM && fb == Latte::E_GX2SURFFMT::D32_FLOAT) return false;
    if (Latte::IsCompressedFormat(fa) && Latte::IsCompressedFormat(fb) && Latte::GetHWFormat(fa) != Latte::GetHWFormat(fb)) return false;
    return Latte::GetFormatBits(fa) == Latte::GetFormatBits(fb);
}
static bool blocked_relation(Surface* a, Surface* b) {
    for (int swap = 0; swap < 2; swap++) {
        // a depth->colour copy ends up in R only: not for RGBA8 (Smash)
        if (a->isDepth && !b->isDepth && (Latte::E_GX2SURFFMT)a->format == Latte::E_GX2SURFFMT::D32_FLOAT &&
            Latte::GetHWFormat((Latte::E_GX2SURFFMT)b->format) == Latte::E_HWFMT::HWFMT_8_8_8_8)
            return true;
        std::swap(a, b);
    }
    return false;
}

// ---- GPU copies between surfaces
static const char* kConvertShader = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut cv_vs(uint vid [[vertex_id]], constant float2& uvMax [[buffer(0)]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv = p * uvMax;
    return o;
}
fragment float4 cv_depth_to_color(VOut in [[stage_in]], depth2d<float> t [[texture(0)]]) {
    float d = t.read(uint2(in.uv * float2(t.get_width(), t.get_height())));
    return float4(d, d, d, d);
}
struct DOut { float d [[depth(any)]]; };
fragment DOut cv_color_to_depth(VOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {
    DOut o;
    o.d = t.read(uint2(in.uv * float2(t.get_width(), t.get_height()))).x;
    return o;
}
)";
// one level/slice of src into dst's level/slice, top-left w x h texture pixels, where one is a depth
// texture and the other is not (Cemu: surfaceCopy_copySurfaceWithFormatConversion; depth goes to R)
static void convert_copy(Surface* src, uint32_t srcSlice, uint32_t srcMip, Surface* dst, uint32_t dstSlice, uint32_t dstMip, uint32_t w, uint32_t h) {
    static id<MTLLibrary> lib;
    static std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipes;
    static id<MTLDepthStencilState> writeDepth;
    if (!lib) {
        NSError* err = nil;
        lib = [R.device newLibraryWithSource:[NSString stringWithUTF8String:kConvertShader] options:nil error:&err];
        if (!lib) { LOG("[gfx] format conversion shader: %s", err.localizedDescription.UTF8String); return; }
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = YES;
        writeDepth = [R.device newDepthStencilStateWithDescriptor:dd];
    }
    if (src->fmt.compressed || dst->fmt.compressed || src->fmt.kind != FormatInfo::FLOAT || dst->fmt.kind != FormatInfo::FLOAT) return;
    const bool toDepth = dst->fmt.depth;
    const uint64_t key = (uint64_t)dst->tex.pixelFormat | (toDepth ? 1ull << 40 : 0);
    auto it = pipes.find(key);
    if (it == pipes.end()) {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"cv_vs"];
        d.fragmentFunction = [lib newFunctionWithName:toDepth ? @"cv_color_to_depth" : @"cv_depth_to_color"];
        if (toDepth) {
            d.depthAttachmentPixelFormat = dst->tex.pixelFormat;
            if (dst->fmt.stencil) d.stencilAttachmentPixelFormat = dst->tex.pixelFormat;
        } else {
            d.colorAttachments[0].pixelFormat = dst->tex.pixelFormat;
        }
        NSError* err = nil;
        it = pipes.emplace(key, [R.device newRenderPipelineStateWithDescriptor:d error:&err]).first;
        if (!it->second) LOG("[gfx] format conversion pipeline (pixel %lu): %s", (unsigned long)dst->tex.pixelFormat, err.localizedDescription.UTF8String);
    }
    if (!it->second) return;
    end_encoder();
    const MTLTextureType srcType = src->tex.textureType == MTLTextureType3D ? MTLTextureType3D : MTLTextureType2D;
    if (srcType == MTLTextureType3D) return;  // volumes: not converted
    id<MTLTexture> sv = [src->tex newTextureViewWithPixelFormat:src->tex.pixelFormat textureType:MTLTextureType2D
                                                         levels:NSMakeRange(srcMip, 1) slices:NSMakeRange(srcSlice, 1)];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    const NSUInteger dw = std::max<NSUInteger>(dst->tex.width >> dstMip, 1), dh = std::max<NSUInteger>(dst->tex.height >> dstMip, 1);
    const bool whole = w >= dw && h >= dh;
    if (toDepth) {
        rp.depthAttachment.texture = dst->tex;
        rp.depthAttachment.slice = dstSlice;
        rp.depthAttachment.level = dstMip;
        rp.depthAttachment.loadAction = whole ? MTLLoadActionDontCare : MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (dst->fmt.stencil) {
            rp.stencilAttachment.texture = dst->tex;
            rp.stencilAttachment.slice = dstSlice;
            rp.stencilAttachment.level = dstMip;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    } else {
        rp.colorAttachments[0].texture = dst->tex;
        if (dst->tex.textureType == MTLTextureType3D) rp.colorAttachments[0].depthPlane = dstSlice;
        else rp.colorAttachments[0].slice = dstSlice;
        rp.colorAttachments[0].level = dstMip;
        rp.colorAttachments[0].loadAction = whole ? MTLLoadActionDontCare : MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    time_render_pass(rp, __FILE_NAME__, __LINE__);
    id<MTLRenderCommandEncoder> e = [command_buffer() renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:it->second];
    if (toDepth) [e setDepthStencilState:writeDepth];
    [e setViewport:MTLViewport{0, 0, (double)std::min<NSUInteger>(w, dw), (double)std::min<NSUInteger>(h, dh), 0, 1}];
    const NSUInteger sw = std::max<NSUInteger>(src->tex.width >> srcMip, 1), sh = std::max<NSUInteger>(src->tex.height >> srcMip, 1);
    float uv[2] = {std::min(1.0f, (float)w / sw), std::min(1.0f, (float)h / sh)};
    [e setVertexBytes:uv length:sizeof uv atIndex:0];
    [e setFragmentTexture:sv atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

// src (slice, mip) region at (sx, sy) into dst (slice, mip) at (dx, dy), w x h texture pixels, raw texels
// (Cemu: texture_copyImageSubData). Different host formats: same texel size through a buffer, else drawn.
uint32_t probe_addr();
bool probe_capturing();
void probe_surface(Surface* s, uint32_t slice, uint32_t mip, const char* what);
static void gpu_copy_impl(Surface* src, uint32_t srcSlice, uint32_t srcMip, uint32_t sx, uint32_t sy, Surface* dst, uint32_t dstSlice,
                          uint32_t dstMip, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);
// F12 capture: copies into the probed surface are recorded (colorprobe.log), like the draws that write it
static void gpu_copy(Surface* src, uint32_t srcSlice, uint32_t srcMip, uint32_t sx, uint32_t sy, Surface* dst, uint32_t dstSlice, uint32_t dstMip,
                     uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    gpu_copy_impl(src, srcSlice, srcMip, sx, sy, dst, dstSlice, dstMip, dx, dy, w, h);
    if (probe_capturing() && dst && dst->addr == probe_addr()) {
        char what[200];
        snprintf(what, sizeof what, "copy from %08X %ux%u f%X (slice %u mip %u) region %u,%u %ux%u", src->addr, src->width, src->height,
                 src->format, srcSlice, srcMip, dx, dy, w, h);
        probe_surface(dst, dstSlice, dstMip, what);
    }
}
static void gpu_copy_impl(Surface* src, uint32_t srcSlice, uint32_t srcMip, uint32_t sx, uint32_t sy, Surface* dst, uint32_t dstSlice,
                          uint32_t dstMip, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
    if (!src->tex || !dst->tex || !w || !h) return;
    if (src->fmt.depth != dst->fmt.depth) { convert_copy(src, srcSlice, srcMip, dst, dstSlice, dstMip, w, h); return; }
    const NSUInteger sw = std::max<NSUInteger>(src->tex.width >> srcMip, 1), shh = std::max<NSUInteger>(src->tex.height >> srcMip, 1);
    const NSUInteger dw = std::max<NSUInteger>(dst->tex.width >> dstMip, 1), dh = std::max<NSUInteger>(dst->tex.height >> dstMip, 1);
    if (sx >= sw || sy >= shh || dx >= dw || dy >= dh) return;
    w = (uint32_t)std::min<NSUInteger>({(NSUInteger)w, sw - sx, dw - dx});
    h = (uint32_t)std::min<NSUInteger>({(NSUInteger)h, shh - sy, dh - dy});
    const bool s3 = src->tex.textureType == MTLTextureType3D, d3 = dst->tex.textureType == MTLTextureType3D;
    if (srcMip >= src->tex.mipmapLevelCount || dstMip >= dst->tex.mipmapLevelCount) return;
    if ((!s3 && srcSlice >= layer_count(src->tex)) || (!d3 && dstSlice >= layer_count(dst->tex))) return;
    end_encoder();
    id<MTLBlitCommandEncoder> b = TIMED_BLIT();
    const MTLOrigin so = MTLOriginMake(sx, sy, s3 ? srcSlice : 0), dorig = MTLOriginMake(dx, dy, d3 ? dstSlice : 0);
    if (src->tex.pixelFormat == dst->tex.pixelFormat) {
        if (src->fmt.compressed) {  // block-aligned
            w = (w + 3) & ~3u;
            h = (h + 3) & ~3u;
            w = (uint32_t)std::min<NSUInteger>(w, std::min(sw - sx, dw - dx));
            h = (uint32_t)std::min<NSUInteger>(h, std::min(shh - sy, dh - dy));
        }
        [b copyFromTexture:src->tex sourceSlice:s3 ? 0 : srcSlice sourceLevel:srcMip sourceOrigin:so sourceSize:MTLSizeMake(w, h, 1)
                 toTexture:dst->tex destinationSlice:d3 ? 0 : dstSlice destinationLevel:dstMip destinationOrigin:dorig];
    } else if (!src->fmt.compressed && !dst->fmt.compressed && src->fmt.hostBytesPerBlock == dst->fmt.hostBytesPerBlock && !src->fmt.depth) {
        const NSUInteger bpp = dst->fmt.hostBytesPerBlock, row = (NSUInteger)w * bpp, image = row * h;
        id<MTLBuffer> tmp = [R.device newBufferWithLength:image options:MTLResourceStorageModePrivate];
        if (tmp) {
            [b copyFromTexture:src->tex sourceSlice:s3 ? 0 : srcSlice sourceLevel:srcMip sourceOrigin:so sourceSize:MTLSizeMake(w, h, 1)
                      toBuffer:tmp destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:image];
            [b copyFromBuffer:tmp sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:image sourceSize:MTLSizeMake(w, h, 1)
                    toTexture:dst->tex destinationSlice:d3 ? 0 : dstSlice destinationLevel:dstMip destinationOrigin:dorig];
        }
    } else if (src->fmt.compressed != dst->fmt.compressed && !src->fmt.depth && !dst->fmt.depth &&
               src->fmt.convert == Convert::NONE && dst->fmt.convert == Convert::NONE &&
               src->fmt.hostBytesPerBlock == dst->fmt.hostBytesPerBlock) {
        // one texel of the uncompressed surface is one 4x4 block of the compressed one (the game encodes
        // BC textures on the GPU into a render target of the block size, e.g. RGBA16 -> BC4): raw bytes
        // through a buffer (Cemu: texture_copyImageSubData between compatible formats)
        const NSUInteger bpb = dst->fmt.hostBytesPerBlock;
        NSUInteger bw, bh;  // blocks (= texels of the uncompressed side)
        MTLSize srcSize, dstSize;
        MTLOrigin so2 = so, do2 = dorig;
        if (dst->fmt.compressed) {
            bw = w, bh = h;
            srcSize = MTLSizeMake(bw, bh, 1);
            do2.x = dx * 4 < dw ? dx * 4 : dx, do2.y = dy * 4 < dh ? dy * 4 : dy;  // block or pixel coordinates
            if (do2.x >= dw || do2.y >= dh) { [b endEncoding]; return; }
            dstSize = MTLSizeMake(std::min<NSUInteger>(bw * 4, dw - do2.x), std::min<NSUInteger>(bh * 4, dh - do2.y), 1);
            bw = (dstSize.width + 3) / 4, bh = (dstSize.height + 3) / 4;
            srcSize = MTLSizeMake(std::min<NSUInteger>(bw, sw - sx), std::min<NSUInteger>(bh, shh - sy), 1);
        } else {
            so2.x = sx & ~3u, so2.y = sy & ~3u;
            if (do2.x >= dw || do2.y >= dh) { [b endEncoding]; return; }
            bw = std::min<NSUInteger>((std::min<NSUInteger>(w, sw - so2.x) + 3) / 4, dw - do2.x);
            bh = std::min<NSUInteger>((std::min<NSUInteger>(h, shh - so2.y) + 3) / 4, dh - do2.y);
            srcSize = MTLSizeMake(std::min<NSUInteger>(bw * 4, sw - so2.x), std::min<NSUInteger>(bh * 4, shh - so2.y), 1);
            dstSize = MTLSizeMake(bw, bh, 1);
        }
        const NSUInteger row = bw * bpb, image = row * bh;
        id<MTLBuffer> tmp = [R.device newBufferWithLength:image options:MTLResourceStorageModePrivate];
        if (tmp) {
            [b copyFromTexture:src->tex sourceSlice:s3 ? 0 : srcSlice sourceLevel:srcMip sourceOrigin:so2 sourceSize:srcSize
                      toBuffer:tmp destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:image];
            [b copyFromBuffer:tmp sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:image sourceSize:dstSize
                    toTexture:dst->tex destinationSlice:d3 ? 0 : dstSlice destinationLevel:dstMip destinationOrigin:do2];
        }
        static int logged = 0;
        if (logged++ < 5)
            LOG("[tc] block copy %08X f%X -> %08X f%X: %lux%lu blocks (slice %u mip %u)", src->addr, src->format, dst->addr, dst->format,
                (unsigned long)bw, (unsigned long)bh, dstSlice, dstMip);
    } else {
        static int logged = 0;
        if (logged++ < 10)
            LOG("[tc] no copy between %08X f%X (pixel %lu) and %08X f%X (pixel %lu)", src->addr, src->format, (unsigned long)src->tex.pixelFormat,
                dst->addr, dst->format, (unsigned long)dst->tex.pixelFormat);
    }
    [b endEncoding];
}

static bool same_scale(const Surface* a, const Surface* b) {
    const double d = (double)a->sx / (double)b->sx;
    return std::fabs(1.0 - d) <= 0.05 && std::fabs(1.0 - (double)a->sy / (double)b->sy) <= 0.05;
}
static uint32_t eff(uint32_t v, float s) { return (uint32_t)std::lround(v * s); }

// guest region (width x height at srcX/srcY -> dstX/dstY) of one slice/mip into another surface's
static void copy_slice(Surface* src, uint32_t srcSlice, uint32_t srcMip, Surface* dst, uint32_t dstSlice, uint32_t dstMip, uint32_t srcX, uint32_t srcY,
                       uint32_t dstX, uint32_t dstY, uint32_t width, uint32_t height) {
    if (!same_scale(src, dst)) {
        static int logged = 0;
        if (logged++ < 20)
            LOG("[tc] not synced (scale %.2f vs %.2f): %08X %ux%u f%X -> %08X %ux%u f%X", src->sx, dst->sx, src->addr, src->width, src->height,
                src->format, dst->addr, dst->width, dst->height, dst->format);
        return;
    }
    gpu_copy(src, srcSlice, srcMip, eff(srcX, src->sx), eff(srcY, src->sy), dst, dstSlice, dstMip, eff(dstX, dst->sx), eff(dstY, dst->sy),
             eff(width, src->sx), eff(height, src->sy));
}

// every slice of the first mipCount levels of src into dst (a recreated surface)
static void copy_data(Surface* src, Surface* dst, uint32_t mipCount, uint32_t sliceCount) {
    if (!same_scale(src, dst)) return;
    for (uint32_t mip = 0; mip < mipCount && mip < dst->mips; mip++) {
        const uint32_t w = std::max(src->width >> mip, 1u), h = std::max(src->height >> mip, 1u);
        const uint32_t n = src->dim == 2 ? std::max(sliceCount >> mip, 1u) : sliceCount;
        for (uint32_t slice = 0; slice < n && slice < dst->mip_slices(mip); slice++) {
            gpu_copy(src, slice, mip, 0, 0, dst, slice, mip, 0, 0, eff(w, src->sx), eff(h, src->sy));
            dst->slice_mip(slice, mip)->lastDynamicUpdate = src->slice_mip(slice, mip)->lastDynamicUpdate;
        }
    }
}

static void sync_slice(Surface* src, uint32_t srcSlice, uint32_t srcMip, Surface* dst, uint32_t dstSlice, uint32_t dstMip) {
    uint32_t srcW = src->width, srcH = src->height, dstW = dst->width, dstH = dst->height;
    // a linear 2D surface holding a stack of slices of a smaller array (Ninja Gaiden's 24x576 -> 24x24x24)
    if (srcMip == 0 && dstMip == 0 &&
        (src->tileMode == (uint32_t)Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED || src->tileMode == (uint32_t)Latte::E_HWTILEMODE::TM_1D_TILED_THIN1) &&
        src->height > dst->height && src->height % dst->height == 0) {
        bool match = src->tileMode == (uint32_t)Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED ||
                     (src->tileMode == (uint32_t)Latte::E_HWTILEMODE::TM_1D_TILED_THIN1 && src->width == 32);
        if (match && !src->fmt.compressed && !dst->fmt.compressed) {
            const uint32_t virt = src->height / dst->height;
            if (dst->slices == virt)
                for (uint32_t s = 0; s < virt; s++)
                    copy_slice(src, srcSlice, srcMip, dst, dstSlice + s, dstMip, 0, s * dst->height, 0, 0, std::min(srcW, dstW), std::min(srcH, dstH));
            return;
        }
    }
    if (src->fmt.compressed != dst->fmt.compressed) {  // in the source's units
        if (!src->fmt.compressed) { dstW >>= 2; dstH >>= 2; }
        else { dstW <<= 2; dstH <<= 2; }
    }
    srcW = std::max(srcW >> srcMip, 1u);
    srcH = std::max(srcH >> srcMip, 1u);
    dstW = std::max(dstW >> dstMip, 1u);
    dstH = std::max(dstH >> dstMip, 1u);
    copy_slice(src, srcSlice, srcMip, dst, dstSlice, dstMip, 0, 0, 0, 0, std::min(srcW, dstW), std::min(srcH, dstH));
}

// bring t up to date with the newer GPU writes of related surfaces (LatteTexture_UpdateTextureFromDynamicChanges)
static void update_from_dynamic(Surface* t) {
    for (size_t r = 0; r < t->relations.size(); r++) {
        TexRelation* rel = t->relations[r];
        Surface* base = rel->base;
        Surface* sub = rel->sub;
        for (uint32_t m = 0; m < rel->mipCount; m++) {
            uint32_t n = rel->sliceCount;
            if (base->dim == 2) n = std::max(n >> m, 1u);
            for (uint32_t s = 0; s < n; s++) {
                if (rel->baseSlice + s >= base->mip_slices(rel->baseMip + m) || s >= sub->mip_slices(m)) continue;
                SliceMip* bi = base->slice_mip(rel->baseSlice + s, rel->baseMip + m);
                SliceMip* si = sub->slice_mip(s, m);
                if (t == base) {
                    if (bi->lastDynamicUpdate < si->lastDynamicUpdate) {
                        sync_slice(sub, s, m, base, rel->baseSlice + s, rel->baseMip + m);
                        bi->lastDynamicUpdate = si->lastDynamicUpdate;
                        if (sub->gpuWritten) t->gpuWritten = true;
                    }
                } else if (si->lastDynamicUpdate < bi->lastDynamicUpdate) {
                    sync_slice(base, rel->baseSlice + s, rel->baseMip + m, sub, s, m);
                    si->lastDynamicUpdate = bi->lastDynamicUpdate;
                    if (base->gpuWritten) t->gpuWritten = true;
                }
            }
        }
    }
}

void mark_gpu_written(Surface* s, uint32_t slice, uint32_t mip) {
    const uint64_t ev = next_write_seq();
    s->gpuWritten = true;
    s->writeSeq = ev;
    if (!s->sm.empty() && mip < s->mips && slice < s->mip_slices(mip)) s->slice_mip(slice, mip)->lastDynamicUpdate = ev;
    for (TexRelation* rel : s->relations) {
        Surface* other = rel->base == s ? rel->sub : rel->base;
        other->reloadFromDynamic = true;
    }
}

// ---- relations
static bool find_sub_slice_mip(Surface* base, Surface* sub, uint32_t& baseSlice, uint32_t& baseMip) {
    SliceMip* si = sub->slice_mip(0, 0);
    for (uint32_t mip = 0; mip < base->mips; mip++)
        for (uint32_t slice = 0; slice < base->mip_slices(mip); slice++) {
            SliceMip* bi = base->slice_mip(slice, mip);
            if (bi->addrStart == si->addrStart && bi->subIndex == si->subIndex) {
                baseSlice = slice;
                baseMip = mip;
                return true;
            }
        }
    return false;
}
static void track_relation(Surface* a, Surface* b) {
    if (a->addr > b->addr) std::swap(a, b);  // b at a's level 0 or beyond
    for (TexRelation* r : a->relations)
        if (r->base == a && r->sub == b) return;
    if (blocked_relation(a, b)) return;
    uint32_t baseSlice = 0, baseMip = 0;
    if (a->addr != b->addr && !find_sub_slice_mip(a, b, baseSlice, baseMip)) return;
    SliceMip* ai = a->slice_mip(baseSlice, baseMip);
    SliceMip* bi = b->slice_mip(0, 0);
    const bool tileOk = (baseMip == 0) ? a->tileMode == b->tileMode : ai->tileMode == bi->tileMode;
    if (!tileOk || ai->pitch != bi->pitch) return;
    const uint32_t remaining = a->mip_slices(baseMip) - baseSlice;
    TexRelation* r = new TexRelation{a, b, baseSlice, baseMip, std::min(remaining, b->slices), 1};
    a->relations.push_back(r);
    b->relations.push_back(r);
}
static void track_overlap(SliceMip* sm, SliceMip* other) {
    if (!(sm->addrEnd > other->addrStart && sm->addrStart < other->addrEnd)) return;
    for (auto& o : sm->overlaps)
        if (o.second == other) return;
    sm->overlaps.push_back({other->tex, other});
    other->overlaps.push_back({sm->tex, sm});
}
static void gather_relations(Surface* t) {
    for (uint32_t mip = 0; mip < t->mips; mip++)
        for (uint32_t slice = 0; slice < t->mip_slices(mip); slice++) {
            SliceMip* sm = t->slice_mip(slice, mip);
            for_occ_buckets(sm->addrStart, sm->addrEnd, [&](std::vector<Occ>& b) {
                for (size_t i = 0; i < b.size(); i++) {
                    Occ o = b[i];
                    Surface* other = o.sm->tex;
                    if (other == t) continue;
                    if (!(sm->addrEnd >= o.start && sm->addrStart < o.end)) continue;
                    if (sm->addrStart == o.start && sm->subIndex == o.sm->subIndex) {
                        if (sm->pitch == o.sm->pitch && texel_size_compatible(t->format, other->format) && sm->tileMode == o.sm->tileMode &&
                            format_view_compatible(t->format, other->format))
                            track_relation(t, other);
                    } else {
                        track_overlap(sm, o.sm);
                    }
                }
            });
        }
}
static void delete_relations(Surface* t) {
    while (!t->relations.empty()) {
        TexRelation* r = t->relations.front();
        for (Surface* x : {r->base, r->sub}) x->relations.erase(std::find(x->relations.begin(), x->relations.end(), r));
        delete r;
    }
}
static void delete_overlaps(Surface* t) {
    for (auto& sm : t->sm) {
        for (auto& [other, osm] : sm.overlaps)
            osm->overlaps.erase(std::remove_if(osm->overlaps.begin(), osm->overlaps.end(), [t](auto& e) { return e.first == t; }), osm->overlaps.end());
        sm.overlaps.clear();
    }
}

// ---- creation and deletion
static void delete_surface(Surface* t) {
    if (t->deleted) return;
    // attached to the open pass: end it (the texture stays alive until the frame's end)
    bool attached = R.passDepth == t;
    for (Surface* c : R.passColor) attached = attached || c == t;
    if (attached) end_encoder();
    t->deleted = true;
    for (TexView* v : t->views) view_lookup_remove(v);
    delete_relations(t);
    delete_overlaps(t);
    unregister_occupancy(t);
    forget_surface_ranges(t);
    R.linearTargets.erase(std::remove(R.linearTargets.begin(), R.linearTargets.end(), t), R.linearTargets.end());
    g_all.erase(std::remove(g_all.begin(), g_all.end(), t), g_all.end());
    auto range = R.surfaces.equal_range(t->addr);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second.get() == t) {
            g_graveyard.push_back(std::move(it->second));
            R.surfaces.erase(it);
            break;
        }
    g_repeat_texture_init = true;
}

static MTLTextureType texture_type_for(const Surface* s) {
    switch ((Latte::E_DIM)s->dim) {
    case Latte::E_DIM::DIM_1D: return s->slices > 1 ? MTLTextureType1DArray : MTLTextureType1D;
    case Latte::E_DIM::DIM_1D_ARRAY: return MTLTextureType1DArray;
    case Latte::E_DIM::DIM_3D: return MTLTextureType3D;
    case Latte::E_DIM::DIM_CUBEMAP:
        if (s->slices % 6 == 0 && s->width == s->height) return s->slices == 6 ? MTLTextureTypeCube : MTLTextureTypeCubeArray;
        return MTLTextureType2DArray;
    default: return s->slices > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    }
}

static bool in_mem1(uint32_t a) { return a >= 0xF4000000 && a < 0xF6000000; }
// every level and slice of a new texture to 0 (colour) / 1 (depth): private textures start undefined
static void clear_surface_to_zero(Surface* t) {
    if (t->fmt.compressed || !(t->tex.usage & MTLTextureUsageRenderTarget)) return;
    end_encoder();
    for (uint32_t mip = 0; mip < t->mips && mip < t->tex.mipmapLevelCount; mip++)
        for (uint32_t slice = 0; slice < t->mip_slices(mip); slice++) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            if (t->fmt.depth) {
                rp.depthAttachment.texture = t->tex;
                rp.depthAttachment.level = mip;
                rp.depthAttachment.slice = slice;
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = 1.0;
                rp.depthAttachment.storeAction = MTLStoreActionStore;
                if (t->fmt.stencil) {
                    rp.stencilAttachment.texture = t->tex;
                    rp.stencilAttachment.level = mip;
                    rp.stencilAttachment.slice = slice;
                    rp.stencilAttachment.loadAction = MTLLoadActionClear;
                    rp.stencilAttachment.storeAction = MTLStoreActionStore;
                }
            } else {
                rp.colorAttachments[0].texture = t->tex;
                rp.colorAttachments[0].level = mip;
                if (t->tex.textureType == MTLTextureType3D) rp.colorAttachments[0].depthPlane = slice;
                else rp.colorAttachments[0].slice = slice;
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            }
            time_render_pass(rp, __FILE_NAME__, __LINE__);
            [[command_buffer() renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
}

static TexView* create_surface(uint32_t dim, uint32_t addr, uint32_t mipAddr, uint32_t format, uint32_t width, uint32_t height, uint32_t depth,
                               uint32_t pitch, uint32_t mips, uint32_t swizzle, uint32_t tileMode, bool isDepth, float minScale = 0.0f) {
    auto up = std::make_unique<Surface>();
    Surface* t = up.get();
    t->addr = addr;
    t->mipAddr = mipAddr;
    t->dim = dim;
    t->format = format;
    t->width = std::max(width, 1u);
    t->height = std::max(height, 1u);
    t->slices = std::max(depth, 1u);
    t->pitch = pitch;
    t->mips = std::max(mips, 1u);
    t->swizzle = swizzle;
    t->tileMode = tileMode;
    t->isDepth = isDepth;
    t->minScale = minScale;
    t->fmt = format_info(format, isDepth);
    if ((Latte::E_DIM)dim == Latte::E_DIM::DIM_3D) t->mips = std::min(t->mips, 1u + (uint32_t)std::log2((double)std::max({t->width, t->height, t->slices})));
    else t->mips = std::min(t->mips, 1u + (uint32_t)std::log2((double)std::max(t->width, t->height)));
    float ax, ay;
    target_aspect(t, ax, ay);
    t->tex = make_texture(t, texture_type_for(t), target_scale(t), ax, ay);
    if (!t->tex) {
        LOG("[gfx] cannot create %ux%ux%u texture (format %X, pixel %lu, mips %u)", t->width, t->height, t->slices, t->format,
            (unsigned long)t->fmt.pixel, t->mips);
        return nullptr;
    }
    init_slice_mips(t);
    register_occupancy(t);
    note_surface_range(t, addr, std::max<uint32_t>(t->dataSize, t->pitch * t->height * 4));  // as invalidate's estimate
    R.surfaces.emplace(addr, std::move(up));
    g_all.push_back(t);
    if (!isDepth && (Latte::E_HWTILEMODE)tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED) R.linearTargets.push_back(t);
    // the guest memory's current contents (Cemu loads every new texture). Not in MEM1: it holds GPU
    // results (render targets) whose guest-memory copy is stale; those start as zeros instead
    if (in_mem1(addr)) clear_surface_to_zero(t);
    else {
        t->lastCheckedFrame = R.frame;
        check_texture(t);
    }
    t->lastUseFrame = R.frame;
    t->baseView = get_or_create_view(t, dim, format, 0, t->mips, 0, t->slices);
    g_repeat_texture_init = true;
    return t->baseView;
}

enum ViewCompat { VIEW_COMPATIBLE, VIEW_BASE_TOO_SMALL, VIEW_NOT_COMPATIBLE };
static bool dim_view_compatible(uint32_t base, uint32_t view) {
    using D = Latte::E_DIM;
    const D b = (D)base, v = (D)view;
    return b == v || (b == D::DIM_CUBEMAP && v == D::DIM_2D) || (b == D::DIM_2D && v == D::DIM_2D_ARRAY) || (b == D::DIM_2D_ARRAY && v == D::DIM_2D) ||
           (b == D::DIM_CUBEMAP && v == D::DIM_2D_ARRAY) || (b == D::DIM_2D_ARRAY && v == D::DIM_CUBEMAP) || (b == D::DIM_3D && v == D::DIM_2D_ARRAY) ||
           (b == D::DIM_1D && v == D::DIM_2D) || (b == D::DIM_2D && v == D::DIM_1D);
}
static ViewCompat can_be_view(Surface* t, uint32_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint32_t dimView, uint32_t format, bool isDepth,
                              uint32_t firstMip, uint32_t numMip, uint32_t firstSlice, uint32_t numSlice, uint32_t& relMip, uint32_t& relSlice) {
    relMip = relSlice = 0;
    if (!format_view_compatible(t->format, format)) return VIEW_NOT_COMPATIBLE;
    if (t->addr == addr && t->pitch == pitch) {
        if (t->isDepth != isDepth) return VIEW_NOT_COMPATIBLE;
        if (!texel_size_compatible(t->format, format) || t->width != width || t->height != height) return VIEW_NOT_COMPATIBLE;
        // a volume's slices rendered one at a time: views of one slice (this renderer renders into depth planes)
        const bool volume = (Latte::E_DIM)dimView == Latte::E_DIM::DIM_3D && (Latte::E_DIM)t->dim == Latte::E_DIM::DIM_3D;
        if (!volume && !dim_view_compatible(t->dim, dimView)) return VIEW_NOT_COMPATIBLE;
        if (t->format != format) return VIEW_NOT_COMPATIBLE;  // one texture per format; related textures sync
        if (firstMip + numMip > t->mips || firstSlice + numSlice > t->slices) return VIEW_BASE_TOO_SMALL;
        return VIEW_COMPATIBLE;
    }
    // another address or pitch: one of its mips or slices?
    if (numMip > 1 || (Latte::E_DIM)t->dim == Latte::E_DIM::DIM_3D) return VIEW_NOT_COMPATIBLE;
    if (LatteAddrLib::TM_IsThickAndMacroTiled((Latte::E_HWTILEMODE)t->tileMode)) return VIEW_NOT_COMPATIBLE;
    for (uint32_t m = 0; m < t->mips; m++) {
        if (t->slice_mip(0, m)->pitch != pitch) continue;
        for (uint32_t s = 0; s < t->mip_slices(m); s++) {
            SliceMip* sm = t->slice_mip(s, m);
            if (sm->addrStart != addr || sm->pitch != pitch) continue;
            if (t->isDepth != isDepth) return VIEW_NOT_COMPATIBLE;
            if (std::max(t->width >> m, 1u) != width || std::max(t->height >> m, 1u) != height) return VIEW_NOT_COMPATIBLE;
            if (!texel_size_compatible(t->format, format) || !dim_view_compatible(t->dim, dimView) || t->format != format) return VIEW_NOT_COMPATIBLE;
            relMip = m;
            relSlice = s;
            if (m + firstMip + numMip > t->mips || s + firstSlice + numSlice > t->slices) return VIEW_BASE_TOO_SMALL;
            return VIEW_COMPATIBLE;
        }
    }
    return VIEW_NOT_COMPATIBLE;
}

// related surfaces that are now views of t: dropped (their data is in t)
static void delete_absorbed(Surface* t) {
    for (bool again = true; again;) {
        again = false;
        for (TexRelation* rel : t->relations) {
            Surface* o = rel->base != t ? rel->base : rel->sub;
            uint32_t rm, rs;
            if (can_be_view(t, o->addr, o->width, o->height, o->pitch, o->dim, o->format, o->isDepth, 0, o->mips, 0, o->slices, rm, rs) ==
                VIEW_COMPATIBLE) {
                delete_surface(o);
                again = true;
                break;
            }
        }
    }
}

static TexView* create_mapping(uint32_t addr, uint32_t mipAddr, uint32_t width, uint32_t height, uint32_t depth, uint32_t pitch, uint32_t tileMode,
                               uint32_t swizzle, uint32_t firstMip, uint32_t numMip, uint32_t firstSlice, uint32_t numSlice, uint32_t format,
                               uint32_t dimBase, uint32_t dimView, bool isDepth, bool allowCreate = true);

static bool recreate_bigger(Surface* t, uint32_t mipAddr, uint32_t newMips, uint32_t newDepth) {
    uint32_t dim = t->dim;
    if ((Latte::E_DIM)dim == Latte::E_DIM::DIM_2D && newDepth > 1) dim = (uint32_t)Latte::E_DIM::DIM_2D_ARRAY;
    else if ((Latte::E_DIM)dim == Latte::E_DIM::DIM_1D && newDepth > 1) dim = (uint32_t)Latte::E_DIM::DIM_1D_ARRAY;
    end_encoder();
    TexView* v = create_surface(dim, t->addr, mipAddr, t->format, t->width, t->height, newDepth, t->pitch, newMips, t->swizzle, t->tileMode, t->isDepth,
                                t->minScale);
    if (!v) return false;
    Surface* n = v->tex;
    if (n->mips < newMips || n->slices < newDepth) {  // could not grow (size limits): keep the old one
        delete_surface(n);
        return false;
    }
    if (t->gpuWritten) {
        copy_data(t, n, std::min(t->mips, n->mips), std::min(t->slices, n->slices));
        n->gpuWritten = true;
        n->writeSeq = t->writeSeq;
    }
    n->lastRenderTargetSwizzle = t->lastRenderTargetSwizzle;
    static int logged = 0;
    if (logged++ < 40)
        LOG("[tc] %08X %ux%u f%X%s: %u mips x %u slices -> %u x %u", t->addr, t->width, t->height, t->format, t->isDepth ? " depth" : "", t->mips,
            t->slices, n->mips, n->slices);
    delete_surface(t);
    gather_relations(n);
    update_from_dynamic(n);
    delete_absorbed(n);
    return true;
}

static TexView* create_mapping(uint32_t addr, uint32_t mipAddr, uint32_t width, uint32_t height, uint32_t depth, uint32_t pitch, uint32_t tileMode,
                               uint32_t swizzle, uint32_t firstMip, uint32_t numMip, uint32_t firstSlice, uint32_t numSlice, uint32_t format,
                               uint32_t dimBase, uint32_t dimView, bool isDepth, bool allowCreate) {
    if (!format || !width || !height) return nullptr;
    depth = std::max(depth, 1u);
    numMip = std::max(numMip, 1u);
    numSlice = std::max(numSlice, 1u);
    // surfaces starting at the address of one of the requested slices
    std::vector<Surface*> overlapping;
    const uint32_t sliceCount = std::min(firstSlice + numSlice, std::max(depth, firstSlice + numSlice));
    for (uint32_t slice = 0; slice < sliceCount && slice < 64; slice++) {
        uint32_t a = 0, size = 0;
        sint32 sub = 0;
        LatteAddrLib::CalculateMipAndSliceAddr(addr, mipAddr, (Latte::E_GX2SURFFMT)format, width, height, std::max(depth, sliceCount), (Latte::E_DIM)dimBase,
                                               (Latte::E_HWTILEMODE)tileMode, swizzle, 0, 0, slice, &a, &size, &sub);
        for_occ_buckets(a, a + size, [&](std::vector<Occ>& b) {
            for (auto& o : b)
                if (a + size >= o.start && a < o.end && a == o.start &&
                    std::find(overlapping.begin(), overlapping.end(), o.sm->tex) == overlapping.end())
                    overlapping.push_back(o.sm->tex);
        });
    }
    for (Surface* t : overlapping) {
        if (t->deleted) continue;
        uint32_t rm, rs;
        ViewCompat c = can_be_view(t, addr, width, height, pitch, dimView, format, isDepth, firstMip, numMip, firstSlice, numSlice, rm, rs);
        if (c == VIEW_NOT_COMPATIBLE) { allowCreate = true; continue; }
        if (c == VIEW_BASE_TOO_SMALL) {
            if (rm || rs) { allowCreate = true; continue; }
            const uint32_t newDepth = std::max(rs + firstSlice + numSlice, std::max(depth, t->slices));
            const uint32_t newMips = std::max(rm + firstMip + numMip, t->mips);
            if (!recreate_bigger(t, rm + firstMip + numMip > 1 ? mipAddr : t->mipAddr, newMips, newDepth)) { allowCreate = true; continue; }
            return create_mapping(addr, mipAddr, width, height, depth, pitch, tileMode, swizzle, firstMip, numMip, firstSlice, numSlice, format, dimBase,
                                  dimView, isDepth);
        }
        TexView* v = get_or_create_view(t, dimView, format, rm + firstMip, numMip, rs + firstSlice, numSlice);
        if (rm || rs) view_lookup_add(v, rm, rs);  // also found by this address
        return v;
    }
    if (!allowCreate) return nullptr;
    TexView* base = create_surface(dimBase, addr, mipAddr, format, width, height, std::max(depth, firstSlice + numSlice), pitch, firstMip + numMip,
                                   swizzle, tileMode, isDepth);
    if (!base) return nullptr;
    Surface* t = base->tex;
    gather_relations(t);
    update_from_dynamic(t);
    delete_absorbed(t);
    return get_or_create_view(t, dimView, format, firstMip, std::min(numMip, t->mips - std::min(firstMip, t->mips - 1)), firstSlice, numSlice);
}

// a surface written by other GPU writes over all of its first levels' memory (Cemu: LatteTC_IsTextureDataOverwritten)
static bool data_overwritten(Surface* t) {
    for (uint32_t mip = 0; mip < std::min(t->mips, 3u); mip++)
        for (uint32_t slice = 0; slice < t->mip_slices(mip); slice++) {
            SliceMip* sm = t->slice_mip(slice, mip);
            bool outdated = false;
            for (auto& [o, osm] : sm->overlaps)
                if (sm->lastDynamicUpdate < osm->lastDynamicUpdate) { outdated = true; break; }
            if (!outdated) return false;
        }
    return true;
}

// keep t's contents current before a use: CPU writes to its memory (once a frame), newer GPU writes to related surfaces
static const std::vector<std::pair<uint32_t, uint32_t>>& level_ranges(Surface* s);
static uint64_t content_hash(Surface* s);

// a GPU-written surface whose guest memory the CPU has since changed: reloaded from it, as Cemu does
// (LatteTC_HasTextureChanged hashes GPU-updated textures too). BotW streams its terrain and grass maps
// into memory that earlier frames drew into.
static void check_cpu_overwrite(Surface* t) {
    static const bool off = getenv("WWHD_NO_GPU_RELOAD") != nullptr;
    if (off || !wwatch::active() || !t->tex || t->fmt.depth) return;
    const auto& ranges = level_ranges(t);
    auto arm = [&] {
        uint64_t stamp = ~0ull;
        for (auto& [a, n] : ranges) stamp = std::min(stamp, wwatch::arm(a, n));
        t->watchStamp = stamp;
    };
    if (!t->gpuWatched) {
        arm();
        t->gpuRamHash = content_hash(t);
        t->gpuWatched = true;
        return;
    }
    bool hit = false;
    for (auto& [a, n] : ranges)
        if (wwatch::written_since(a, n, t->watchStamp)) { hit = true; break; }
    if (!hit) return;
    arm();
    const uint64_t h = content_hash(t);
    if (h == t->gpuRamHash) return;
    t->gpuRamHash = h;
    static int logged = 0;
    if (logged++ < 60)
        LOG("[tc] CPU wrote GPU surface %08X %ux%u x%u f%X mips %u: reloaded", t->addr, t->width, t->height, t->slices, t->format, t->mips);
    t->gpuWritten = false;
    t->contentHash = 0;
    t->watched = true;
    t->dirty = false;
    t->gpuWatched = false;
    upload_surface(t);
    // the newest data of every slice now: related surfaces sync from it
    for (auto& sm : t->sm) sm.lastDynamicUpdate = t->writeSeq;
    for (TexRelation* rel : t->relations) (rel->base == t ? rel->sub : rel->base)->reloadFromDynamic = true;
}

static void update_to_latest(Surface* t) {
    if (t->lastCheckedFrame != R.frame && !in_mem1(t->addr)) {
        t->lastCheckedFrame = R.frame;
        if (!t->gpuWritten) check_texture(t);
        else check_cpu_overwrite(t);
    }
    if (t->reloadFromDynamic) {
        t->reloadFromDynamic = false;
        update_from_dynamic(t);
    }
    t->lastUseFrame = R.frame;
}

void texture_cache_frame_end() {
    // Cemu's cleanup: a few surfaces a frame. CPU textures unused for a long time (reloaded from guest
    // memory when needed again) and surfaces all of whose memory other GPU writes have replaced.
    static size_t scan = 0;
    int deleted = 0;
    for (int c = 0; c < 25 && !g_all.empty() && deleted < 10; c++) {
        if (scan >= g_all.size()) scan = 0;
        Surface* t = g_all[scan++];
        const uint64_t idle = R.frame - t->lastUseFrame;
        if ((!t->gpuWritten && idle >= 2000) || (idle >= 6 && data_overwritten(t))) {
            delete_surface(t);
            deleted++;
            scan--;
        }
    }
    if (!g_graveyard.empty()) {
        end_encoder();
        for (auto& t : g_graveyard)
            for (TexView* v : t->views) delete v;
        g_graveyard.clear();
        forget_texture_views();
    }
    g_repeat_texture_init = false;
}

// ---- lookups
TexView* sampled_texture(const uint32_t* w, bool isDepthSampler) {
    Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N w0;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N w1;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD5_N w5;
    memcpy(&w0, &w[0], 4);
    memcpy(&w1, &w[1], 4);
    memcpy(&w4, &w[4], 4);
    memcpy(&w5, &w[5], 4);
    uint32_t addr = w[2] << 8, mipAddr = w[3] << 8;
    if (!addr) return nullptr;
    auto dim = w0.get_DIM();
    uint32_t pitch = (w0.get_PITCH() + 1) << 3;
    uint32_t width = w0.get_WIDTH() + 1;
    uint32_t height = w1.get_HEIGHT() + 1;
    uint32_t depth = w1.get_DEPTH();
    if (dim == Latte::E_DIM::DIM_2D_ARRAY || dim == Latte::E_DIM::DIM_3D || dim == Latte::E_DIM::DIM_2D_ARRAY_MSAA || dim == Latte::E_DIM::DIM_1D_ARRAY)
        depth += 1;
    else {
        if (dim == Latte::E_DIM::DIM_CUBEMAP) depth = 6 * (depth + 1);
        if (depth == 0) depth = 1;
    }
    if (dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY) height = 1;
    auto tileMode = w0.get_TILE_MODE();
    if (Latte::IsCompressedFormat(w1.get_DATA_FORMAT())) pitch /= 4;
    // the view: first slice / mip and how many (Cemu: word5 BASE_ARRAY..LAST_ARRAY, word4 BASE_LEVEL..word5 LAST_LEVEL)
    uint32_t firstSlice = w5.get_BASE_ARRAY(), lastSlice = w5.get_LAST_ARRAY();
    uint32_t numSlice = lastSlice >= firstSlice ? lastSlice + 1 - firstSlice : 1;
    uint32_t firstMip = w4.get_BASE_LEVEL(), lastMip = w5.get_LAST_LEVEL();
    uint32_t numMip = lastMip >= firstMip ? lastMip + 1 - firstMip : 1;
    if (dim == Latte::E_DIM::DIM_2D_MSAA || !mipAddr) { firstMip = 0; numMip = 1; }
    {  // levels the size has
        uint32_t m = std::max(width, height);
        if (dim == Latte::E_DIM::DIM_3D) m = std::max(m, depth);
        const uint32_t maxMips = 1 + (uint32_t)std::log2((double)m);
        firstMip = std::min(firstMip, maxMips - 1);
        numMip = std::max(1u, std::min(numMip, maxMips - firstMip));
    }
    if (firstSlice >= depth) { firstSlice = 0; numSlice = depth; }
    numSlice = std::min(numSlice, depth - firstSlice);
    if (dim == Latte::E_DIM::DIM_3D) { firstSlice = 0; numSlice = depth; }
    uint32_t swizzle = 0;
    if (Latte::TM_IsMacroTiled(tileMode)) {
        swizzle = addr & 0x700;
        addr &= ~0x700u;
    }
    const uint32_t format = (uint32_t)LatteTexture_ReconstructGX2Format(w1, w4);
    TexView* v = isDepthSampler ? view_lookup(addr, width, height, pitch, firstMip, numMip, firstSlice, numSlice, format, (uint32_t)dim, 1)
                                : view_lookup(addr, width, height, pitch, firstMip, numMip, firstSlice, numSlice, format, (uint32_t)dim);
    if (!v) {
        if (R.binding) return nullptr;  // looked up by the draw's preflight; nothing is created while binding
        v = create_mapping(addr, mipAddr, width, height, depth, pitch, (uint32_t)tileMode, swizzle, firstMip, numMip, firstSlice, numSlice, format,
                           (uint32_t)dim, (uint32_t)dim, isDepthSampler);
        if (!v) return nullptr;
    }
    Surface* t = v->tex;
    if (R.binding) return v;
    // a changed swizzle or mip address: the same memory holds something else now (unless the last render used that swizzle)
    bool reload = false;
    if (t->swizzle != swizzle) {
        if (swizzle == t->lastRenderTargetSwizzle) t->swizzle = swizzle;
        else reload = true;
    } else if (firstMip + numMip > 1 && t->mipAddr != mipAddr && mipAddr) {
        reload = true;
    }
    if (reload && t->gpuWritten) {  // GPU contents are kept (Cemu reloads from guest memory here)
        static int logged = 0;
        if (logged++ < 20)
            LOG("[tc] %08X %ux%u f%X sampled with swizzle %X / mip address %08X (was %X / %08X)", t->addr, t->width, t->height, t->format, swizzle,
                mipAddr, t->swizzle, t->mipAddr);
        reload = false;
    }
    if (reload && in_mem1(t->addr)) {  // GPU memory: nothing to load
        t->swizzle = swizzle;
        reload = false;
    }
    if (reload) {
        t->swizzle = swizzle;
        if (firstMip + numMip > 1 && mipAddr) t->mipAddr = mipAddr;
        {
            t->dirty = true;
            t->contentHash = 0;
            t->writeSeq = 0;
            t->gpuWritten = false;
            t->levelRanges.clear();
            t->lastCheckedFrame = R.frame;
            check_texture(t);
        }
    }
    {  // debug: WWHD_TRACE_TEX=hexaddr logs what a sampled lookup at that address resolved to (60 times)
        static const uint32_t trace = getenv("WWHD_TRACE_TEX") ? (uint32_t)strtoul(getenv("WWHD_TRACE_TEX"), nullptr, 16) : 0;
        static int traced = 0;
        if (trace && addr == trace && traced++ < 60)
            LOG("[tracetex] f%llu want %ux%u x%u f%X dim %u mips %u+%u slices %u+%u -> %08X %ux%u x%u f%X mips %u tex %lux%lu gpu %d seq %llu",
                (unsigned long long)R.frame, width, height, depth, format, (uint32_t)dim, firstMip, numMip, firstSlice, numSlice, t->addr, t->width,
                t->height, t->slices, t->format, t->mips, (unsigned long)t->tex.width, (unsigned long)t->tex.height, t->gpuWritten,
                (unsigned long long)t->writeSeq);
    }
    update_to_latest(t);
    return v;
}

// a render target: a view of one slice of one level (Cemu: LatteMRT::GetColorAttachmentTexture / UpdateCurrentFBO)
static TexView* target_view(uint32_t addr, uint32_t width, uint32_t height, uint32_t slices, uint32_t pitch, uint32_t tileMode, uint32_t format,
                            uint32_t slice, bool volume, bool isDepth) {
    uint32_t swizzle = 0;
    if (Latte::TM_IsMacroTiled((Latte::E_HWTILEMODE)tileMode)) {
        swizzle = addr & 0x700;
        addr &= ~0x700u;
    }
    TexView* v = view_lookup_slice(addr, width, height, pitch, 0, slice, format, isDepth ? 1 : 0);
    if (v && volume && (Latte::E_DIM)v->dim != Latte::E_DIM::DIM_3D) v = nullptr;
    if (!v) {
        const uint32_t dimBase = volume ? (uint32_t)Latte::E_DIM::DIM_3D : slices > 1 ? (uint32_t)Latte::E_DIM::DIM_2D_ARRAY : (uint32_t)Latte::E_DIM::DIM_2D;
        const uint32_t dimView = volume ? (uint32_t)Latte::E_DIM::DIM_3D : (uint32_t)Latte::E_DIM::DIM_2D;
        v = create_mapping(addr, 0, width, height, std::max(slices, slice + 1), pitch, tileMode, swizzle, 0, 1, slice, 1, format, dimBase, dimView, isDepth);
        if (!v) return nullptr;
    }
    Surface* t = v->tex;
    if (t->swizzle != swizzle) t->lastRenderTargetSwizzle = swizzle;
    update_to_latest(t);
    // the internal resolution (or aspect ratio) changed since it was made: reallocate it
    rescale(t);
    return v;
}

// CB_COLOR_BASE holds the full guest address; CB_COLOR_TILE/FRAG hold width/height (our convention).
TexView* color_target(const uint32_t* regs, int i) {
    uint32_t base = regs[mmCB_COLOR0_BASE + i];
    if (!base) return nullptr;
    uint32_t size = regs[mmCB_COLOR0_SIZE + i], info = regs[mmCB_COLOR0_INFO + i];
    uint32_t pitch = ((size & 0x3FF) + 1) * 8;
    uint32_t height = (((size >> 10) & 0xFFFFF) + 1) * 64 / pitch;
    // our convention (GX2SetColorBuffer, gx2.h kColorTarget3D): TILE = width | slices << 16 | volume flag, FRAG = height
    uint32_t tile = regs[mmCB_COLOR0_TILE + i];
    uint32_t w = tile & 0xFFFF, h = regs[mmCB_COLOR0_FRAG + i];
    uint32_t slices = gx2::color_target_slices(tile);
    bool volume = (tile & gx2::kColorTarget3D) != 0;
    uint32_t slice = slices > 1 ? std::min<uint32_t>(regs[mmCB_COLOR0_VIEW + i] & 0x7FF, slices - 1) : 0;
    static const uint32_t numberBits[8] = {0, 0x200, 0, 0, 0x100, 0x300, 0x400, 0x800};
    uint32_t format = ((info >> 2) & 0x3F) | numberBits[(info >> 12) & 7];
    return target_view(base, w ? w : pitch, h ? h : height, slices, pitch, (info >> 8) & 0xF, format, slice, volume, false);
}

TexView* depth_target(const uint32_t* regs) {
    uint32_t base = regs[mmDB_DEPTH_BASE];
    if (!base) return nullptr;
    uint32_t slices = std::max<uint32_t>(regs[gx2::kDepthSlicesReg], 1);
    uint32_t slice = slices > 1 ? std::min<uint32_t>(regs[mmDB_DEPTH_VIEW] & 0x7FF, slices - 1) : 0;
    uint32_t size = regs[mmDB_DEPTH_SIZE], info = regs[mmDB_DEPTH_INFO];
    uint32_t pitch = ((size & 0x3FF) + 1) * 8;
    uint32_t height = (((size >> 10) & 0xFFFFF) + 1) * 64 / pitch;
    uint32_t wh = regs[mmDB_HTILE_DATA_BASE];  // our convention: width << 16 | height
    static const uint32_t fmts[8] = {0, 0x005, 0, 0x011, 0, 0x811, 0x80E, 0x81C};
    return target_view(base, wh ? (wh >> 16) : pitch, wh ? (wh & 0xFFFF) : height, slices, pitch, (info >> 15) & 0xF, fmts[info & 7], slice, false, true);
}

TexView* surface_from_color_buffer(uint32_t addr, uint32_t* numSlices) {
    auto* cb = (GX2::GX2ColorBuffer*)mem::ptr(addr);
    const bool volume = cb->surface.dim.value() == Latte::E_DIM::DIM_3D;
    const uint32_t slices = cb->surface.dim.value() == Latte::E_DIM::DIM_2D_ARRAY ? std::max<uint32_t>(cb->surface.depth, 1)
                            : volume ? std::max<uint32_t>(cb->surface.depth >> cb->viewMip, 1) : 1;
    const uint32_t first = std::min<uint32_t>(cb->viewFirstSlice, slices - 1);
    if (numSlices) *numSlices = std::clamp<uint32_t>(cb->viewNumSlices, 1, slices - first);
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo(cb->surface.format, cb->surface.width, cb->surface.height, cb->surface.depth, cb->surface.dim,
                                          cb->surface.tileMode, cb->surface.aa, cb->viewMip, &info);
    return target_view(gx2::color_buffer_address(cb), std::max<uint32_t>(cb->surface.width >> cb->viewMip, 1),
                       std::max<uint32_t>(cb->surface.height >> cb->viewMip, 1), slices, cb->viewMip ? info.pitch : (uint32_t)cb->surface.pitch,
                       (uint32_t)info.hwTileMode, (uint32_t)cb->surface.format.value(), first, volume, false);
}

TexView* surface_from_depth_buffer(uint32_t addr, uint32_t* numSlices) {
    auto* db = (GX2::GX2DepthBuffer*)mem::ptr(addr);
    const uint32_t slices = db->surface.dim.value() == Latte::E_DIM::DIM_2D_ARRAY ? std::max<uint32_t>(db->surface.depth, 1) : 1;
    const uint32_t first = std::min<uint32_t>(db->viewFirstSlice, slices - 1);
    if (numSlices) *numSlices = std::clamp<uint32_t>(db->viewNumSlices, 1, slices - first);
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo(db->surface.format, db->surface.width, db->surface.height, db->surface.depth, db->surface.dim,
                                          db->surface.tileMode, db->surface.aa, 0, &info);
    return target_view(gx2::depth_buffer_address(db), db->surface.width, db->surface.height, slices, db->surface.pitch, (uint32_t)info.hwTileMode,
                       (uint32_t)db->surface.format.value(), first, false, true);
}

// Guest ranges of every level the upload reads (base first), computed once: a Surface's geometry
// never changes after creation.
static const std::vector<std::pair<uint32_t, uint32_t>>& level_ranges(Surface* s) {
    if (!s->levelRanges.empty()) return s->levelRanges;
    for (uint32_t level = 0; level < s->mips; level++) {
        uint32_t base;
        if (level == 0) base = s->addr;
        else if (!s->mipAddr) break;
        else if (level == 1) base = s->mipAddr;
        else {
            // mip offsets relative to the mip chain start
            uint32_t sliceOffset = 0, sliceSize = 0;
            sint32 sub = 0;
            LatteAddrLib::CalculateMipAndSliceAddr(s->addr, s->mipAddr, (Latte::E_GX2SURFFMT)s->format, s->width, s->height,
                                                   s->slices, (Latte::E_DIM)s->dim, (Latte::E_HWTILEMODE)s->tileMode,
                                                   s->swizzle, 0, level, 0, &sliceOffset, &sliceSize, &sub);
            base = sliceOffset;
        }
        LatteAddrLib::AddrSurfaceInfo_OUT info{};
        LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)s->format, s->width, s->height, s->slices, (Latte::E_DIM)s->dim,
                                              Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)s->tileMode), 0, level, &info);
        // clamp to the 4 GiB guest space (a bogus descriptor must not make the hash read past it)
        uint32_t size = (uint32_t)std::min<uint64_t>(info.surfSize, 0x100000000ull - base);
        s->levelRanges.push_back({base, size});
        note_surface_range(s, base, size);
    }
    s->dataSize = s->levelRanges[0].second;
    return s->levelRanges;
}

// Has the CPU changed this texture since its last check? Exact with write tracking (write_watch.h):
// its pages were write-protected at that check, so any write since has stamped them. Changes the
// game announces (GX2Invalidate on the range, GX2CopySurface into it, a loaded save state) set dirty.
// Then every byte of every level is hashed and the texture re-uploaded if the hash differs.
// Without write tracking (page protection unavailable on the host): the old sampled check, 256 words
// per level every frame plus a full check every 64 frames, which can show a changed texture late.
static void check_texture(Surface* s) {
    const auto& ranges = level_ranges(s);
    bool full = s->dirty || !s->watched;
    if (wwatch::active()) {
        if (!full)
            for (auto& [a, n] : ranges)
                if (wwatch::written_since(a, n, s->watchStamp)) {
                    full = true;
                    // debug: WWHD_LOG_TEXCHECK=1 logs textures re-checked because their pages were written
                    static const bool log = getenv("WWHD_LOG_TEXCHECK") != nullptr;
                    static int logged = 0;
                    if (log && logged++ < 400)
                        LOG("[texcheck] frame %llu %08X %ux%u fmt %X mips %u: write at %08X+%X", (unsigned long long)R.frame, s->addr,
                            s->width, s->height, s->format, s->mips, a, n);
                    break;
                }
        if (!full) return;
        // arm before reading: a write from now on faults and stamps the pages after this stamp
        uint64_t stamp = ~0ull;
        for (auto& [a, n] : ranges) stamp = std::min(stamp, wwatch::arm(a, n));
        s->watchStamp = stamp;
    } else {
        full = full || ((R.frame + (s->addr >> 12)) & 63) == 0;
        uint64_t h = sparse_hash(s);
        if (h != s->sparseHash) full = true;
        s->sparseHash = h;
        if (!full) return;
    }
    s->watched = true;
    g_stat_full_checks++;
    upload_surface(s);
    s->dirty = false;
}

// ---------------------------------------------------------------- upload (detile + convert)
// 2D thin macro tiles (most textures): Cemu's fast path (AddrLibFastDecode.h): one address per 8x8
// micro tile, the elements by the micro tile's pixel index table; a micro tile larger than the
// 256-byte pipe interleave has its group bits moved up past the pipe and bank bits. EB = bytes per
// element, so each copy is one load and store. Writes rows of `rowStride` bytes.
// Rows [y0, y1) (multiples of 8); dst points at row y0.
template <uint32_t EB>
static void fast_tiles(const uint8_t* src, LatteAddrLib::CachedSurfaceAddrInfo& ci, uint32_t fullX, uint32_t y0, uint32_t y1,
                       uint8_t* dst, size_t rowStride) {
    const uint16_t* table = ci.microTilePixelIndexTable + ((ci.slice & 7) << 6);
    constexpr bool split = EB * 64 > 256;
    for (uint32_t yt = y0; yt < y1; yt += 8)
        for (uint32_t xt = 0; xt < fullX; xt += 8) {
            const uint32_t tileBase = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached_tm04_sample1(xt, yt, &ci);
            for (uint32_t ry = 0; ry < 8; ry++) {
                uint8_t* to = dst + (size_t)(yt - y0 + ry) * rowStride + (size_t)xt * EB;
                const uint16_t* idx = table + (ry << 3);
                for (uint32_t rx = 0; rx < 8; rx++, to += EB) {
                    uint32_t e = idx[rx] * EB;
                    if (split) e = (e & 0xFF) | ((e & ~0xFFu) << 3);
                    memcpy(to, src + tileBase + e, EB);
                }
            }
        }
}

// the level's size on the host: blocks across and down, slices
static void level_dims(Surface* s, uint32_t level, uint32_t& w, uint32_t& h, uint32_t& slices, uint32_t& bw, uint32_t& bh) {
    const FormatInfo& f = s->fmt;
    w = std::max(s->width >> level, 1u);
    h = std::max(s->height >> level, 1u);
    slices = s->dim == (uint32_t)Latte::E_DIM::DIM_3D ? std::max(s->slices >> level, 1u) : s->slices;
    bw = f.compressed ? (w + 3) / 4 : w;
    bh = f.compressed ? (h + 3) / 4 : h;
}

// detile and convert one level into `out` (bw * bh * slices * hostBytesPerBlock bytes). Large levels
// are split into bands of rows decoded on all cores (a 2048x2048 texture took ~15 ms on one: a
// visible hitch whenever the game streamed one in); the result is the same as decoding serially.
static void decode_level(Surface* s, uint32_t level, uint32_t base, uint8_t* out) {
    const FormatInfo& f = s->fmt;
    uint32_t w, h, slices, bw, bh;
    level_dims(s, level, w, h, slices, bw, bh);

    // level geometry from the address library
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)s->format, s->width, s->height, s->slices,
                                          (Latte::E_DIM)s->dim, Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)s->tileMode),
                                          0, level, &info);
    const uint32_t pitch = info.pitch, height = info.height;
    const auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
    const uint32_t bpp = f.bytesPerBlock * 8;
    const uint32_t pipeSwizzle = (s->swizzle >> 8) & 1, bankSwizzle = (s->swizzle >> 9) & 3;
    // small mips of macro-tiled surfaces drop the swizzle
    const uint8_t* src = mem::ptr(base);
    const uint32_t eb = f.bytesPerBlock;
    // without a conversion the guest bytes are the host bytes: detile straight into the output
    const bool direct = f.convert == Convert::NONE && f.hostBytesPerBlock == eb;
    static const bool verify = getenv("WWHD_VERIFY_DETILE") != nullptr;
    const bool macro = Latte::TM_IsMacroTiled(tm);
    const bool fast = tm == Latte::E_HWTILEMODE::TM_2D_TILED_THIN1 && (eb == 1 || eb == 2 || eb == 4 || eb == 8 || eb == 16);
    const uint32_t fullX = fast ? bw & ~7u : 0, fullY = fast && fullX ? bh & ~7u : 0;
    const size_t rowStride = (size_t)bw * eb, outRow = (size_t)bw * f.hostBytesPerBlock;
    // bands of at least 256 KiB guest data, whole tile rows; one band per slice when verifying
    uint32_t band = verify ? bh : std::max<uint32_t>(8, (uint32_t)(((262144 / std::max<size_t>(rowStride, 1)) + 7) & ~7u));
    band = std::min(band, bh);
    const uint32_t perSlice = (bh + band - 1) / band;
    const size_t units = (size_t)slices * perSlice;

    auto run = [&](size_t u) {
        const uint32_t z = (uint32_t)(u / perSlice);
        const uint32_t y0 = (uint32_t)(u % perSlice) * band, y1 = std::min(bh, y0 + band);
        static thread_local std::vector<uint8_t> tmp, check;  // the band in guest format (converted formats)
        uint8_t* rows;
        if (direct) rows = out + ((size_t)z * bh + y0) * rowStride;
        else { tmp.resize((size_t)(y1 - y0) * rowStride); rows = tmp.data(); }
        LatteAddrLib::CachedSurfaceAddrInfo ci;
        if (macro) LatteAddrLib::SetupCachedSurfaceAddrInfo(&ci, z, 0, bpp, pitch, height, slices, 1, tm, false, pipeSwizzle, bankSwizzle);
        // the per-block reference: any tile mode, any position
        auto slow = [&](uint32_t x, uint32_t y, uint8_t* to) {
            uint32_t off;
            if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, z, 0, bpp, pitch, height, slices);
            else if (!macro)
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, z, bpp, pitch, height, tm, false);
            else
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, &ci);
            memcpy(to, src + off, eb);
        };
        const uint32_t fy1 = std::min(y1, fullY);
        if (fullX && y0 < fy1) {
            switch (eb) {
            case 1: fast_tiles<1>(src, ci, fullX, y0, fy1, rows, rowStride); break;
            case 2: fast_tiles<2>(src, ci, fullX, y0, fy1, rows, rowStride); break;
            case 4: fast_tiles<4>(src, ci, fullX, y0, fy1, rows, rowStride); break;
            case 8: fast_tiles<8>(src, ci, fullX, y0, fy1, rows, rowStride); break;
            case 16: fast_tiles<16>(src, ci, fullX, y0, fy1, rows, rowStride); break;
            }
        }
        // what the fast path didn't cover (edges, other tile modes): per block
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = (y < fullY ? fullX : 0); x < bw; x++) slow(x, y, rows + (size_t)(y - y0) * rowStride + (size_t)x * eb);
        if (verify && fullX && fullY) {  // debug: the fast path must match the reference exactly (whole slice: band == bh)
            check.resize((size_t)bh * rowStride);
            for (uint32_t y = 0; y < bh; y++)
                for (uint32_t x = 0; x < bw; x++) slow(x, y, &check[(size_t)y * rowStride + (size_t)x * eb]);
            static int bad = 0, good = 0;
            if (memcmp(rows, check.data(), check.size())) {
                // the game may be writing this memory right now (streamed texture arrays): decode the
                // reference again; if it moved too, the difference is the race, not the fast path
                static thread_local std::vector<uint8_t> again;
                again.resize(check.size());
                for (uint32_t y = 0; y < bh; y++)
                    for (uint32_t x = 0; x < bw; x++) slow(x, y, &again[(size_t)y * rowStride + (size_t)x * eb]);
                const bool moved = memcmp(again.data(), check.data(), check.size()) != 0;
                if (bad++ < 20)
                    LOG("[detile] %s %08X %ux%u fmt %X bpp %u level size %ux%u slice %u", moved ? "memory changed while checking (not a decode error)" : "MISMATCH",
                        s->addr, s->width, s->height, s->format, bpp, bw, bh, z);
            } else if (good++ < 5 || good % 500 == 0) {
                LOG("[detile] ok (%d checked) %ux%u bpp %u", good, bw, bh, bpp);
            }
        }
        if (!direct)
            for (uint32_t y = y0; y < y1; y++) {
                const uint8_t* row = rows + (size_t)(y - y0) * rowStride;
                uint8_t* dst = out + ((size_t)z * bh + y) * outRow;
                if (f.convert == Convert::NONE) memcpy(dst, row, rowStride);
                else convert_row(f.convert, row, dst, bw);
            }
    };
    static const bool serial = getenv("WWHD_SERIAL_DECODE") != nullptr;  // debug: decode on the render thread only
    if (units <= 1 || verify || serial) {
        for (size_t u = 0; u < units; u++) run(u);
    } else {
        dispatch_apply(units, DISPATCH_APPLY_AUTO, ^(size_t u) { run(u); });
    }
}

// fallback without write tracking: 256 words spread over each level
static uint64_t sparse_hash(Surface* s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (auto& [a, n] : level_ranges(s)) {
        uint32_t step = std::max<uint32_t>((n / 256) & ~7u, 8);
        for (uint32_t o = 0; o + 8 <= n; o += step) {
            uint64_t v;
            memcpy(&v, mem::ptr(a + o), 8);
            h = (h ^ v) * 0x100000001b3ull;
        }
    }
    return h;
}

// every byte of every level the upload reads
uint64_t g_stat_hashed_bytes;
static uint64_t content_hash(Surface* s) {
    const auto& ranges = level_ranges(s);
    uint64_t total = 0;
    for (auto& r : ranges) total += r.second;
    g_stat_hashed_bytes += total;
    static const bool serial = getenv("WWHD_SERIAL_DECODE") != nullptr;
    if (total < (4u << 20) || serial) {
        uint64_t h = 0;
        for (auto& [a, n] : ranges) h = XXH3_64bits_withSeed(mem::ptr(a), n, h);
        return h;
    }
    // large textures: 1 MiB chunks hashed on all cores, then the chunk hashes (a surface's ranges
    // never change, so it is always hashed the same way)
    struct Chunk { uint32_t addr, size; };
    std::vector<Chunk> chunks;
    for (auto& [a, n] : ranges)
        for (uint64_t o = 0; o < n; o += 1u << 20) chunks.push_back({(uint32_t)(a + o), (uint32_t)std::min<uint64_t>(1u << 20, n - o)});
    std::vector<uint64_t> hs(chunks.size());
    uint64_t* out = hs.data();
    const Chunk* in = chunks.data();
    dispatch_apply(chunks.size(), DISPATCH_APPLY_AUTO, ^(size_t i) { out[i] = XXH3_64bits(mem::ptr(in[i].addr), in[i].size); });
    return XXH3_64bits(hs.data(), hs.size() * sizeof(uint64_t));
}

// time spent in uploads (render thread), reported with the texture statistics
double g_stat_hash_ms, g_stat_decode_ms;
uint64_t g_stat_pass_splits;
static double ms_now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void upload_surface(Surface* s) {
    // depth buffers are not loaded from guest memory (their contents come from the GPU)
    if (!s->tex || s->gpuWritten || s->fmt.depth) return;
    const FormatInfo& f = s->fmt;
    const auto& ranges = level_ranges(s);
    const double t0 = ms_now();
    uint64_t hash = content_hash(s);
    g_stat_hash_ms += ms_now() - t0;
    if (hash == s->contentHash && s->writeSeq) return;
    s->contentHash = hash;
    s->writeSeq = next_write_seq();  // fresh CPU data is now the newest version of this memory
    g_stat_uploads++;

    if (R.enc) g_stat_pass_splits++;  // an upload in the middle of a render pass ends it
    end_encoder();
    const double t1 = ms_now();
    // a texture at the internal resolution (its size matched a scaling rule): the guest-size picture is
    // uploaded to a scratch texture and filtered up into level 0 (a CPU texture at a render target's size)
    const bool scaled = s->sx != 1.0f || s->sy != 1.0f;
    id<MTLTexture> target = s->tex;
    if (scaled) {
        if (f.compressed || s->tex.textureType == MTLTextureType3D) return;
        MTLTextureDescriptor* td = [MTLTextureDescriptor new];
        td.textureType = s->slices > 1 ? MTLTextureType2DArray : MTLTextureType2D;
        td.pixelFormat = s->tex.pixelFormat;
        td.width = s->width;
        td.height = s->height;
        td.arrayLength = s->slices > 1 ? s->slices : 1;
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        target = [R.device newTextureWithDescriptor:td];
        if (!target) return;
    }
    id<MTLBlitCommandEncoder> blit = TIMED_BLIT();
    for (uint32_t level = 0; level < (scaled ? 1u : (uint32_t)ranges.size()); level++) {
        uint32_t base = ranges[level].first;
        uint32_t w, h, slices, bw, bh;
        level_dims(s, level, w, h, slices, bw, bh);
        // decode straight into upload-pool memory the blit reads (no temporary copy, no new buffer)
        uint32_t stagingOffset = 0;
        uint8_t* dst = nullptr;
        id<MTLBuffer> staging = upload_reserve((uint32_t)((size_t)bw * bh * slices * f.hostBytesPerBlock), &stagingOffset, &dst);
        decode_level(s, level, base, dst);
        bool is3D = target.textureType == MTLTextureType3D;
        uint32_t layers = (uint32_t)layer_count(target);
        uint32_t perSlice = bw * bh * f.hostBytesPerBlock;
        for (uint32_t z = 0; z < (is3D ? 1 : std::min(slices, layers)); z++) {
            [blit copyFromBuffer:staging
                     sourceOffset:stagingOffset + (NSUInteger)z * perSlice
                sourceBytesPerRow:bw * f.hostBytesPerBlock
              sourceBytesPerImage:perSlice
                       sourceSize:MTLSizeMake(f.compressed ? std::min<NSUInteger>(bw * 4, std::max<NSUInteger>(target.width >> level, 1)) : w,
                                              f.compressed ? std::min<NSUInteger>(bh * 4, std::max<NSUInteger>(target.height >> level, 1)) : h,
                                              is3D ? slices : 1)
                        toTexture:target
                 destinationSlice:z
                 destinationLevel:level
                destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
    }
    [blit endEncoding];
    if (scaled)
        for (uint32_t z = 0; z < std::min<uint32_t>(s->slices, (uint32_t)layer_count(s->tex)); z++) {
            id<MTLTexture> sv = [target newTextureViewWithPixelFormat:target.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1)
                                                               slices:NSMakeRange(z, 1)];
            id<MTLTexture> dv = [s->tex newTextureViewWithPixelFormat:s->tex.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1)
                                                               slices:NSMakeRange(z, 1)];
            g_resample_why = "scaled upload";
            resample(sv, dv, f, 1);
            g_resample_why = nullptr;
        }
    g_stat_decode_ms += ms_now() - t1;
}

// ---------------------------------------------------------------- GX2CopySurface
static uint32_t level_address(GX2Surface* s, uint32_t level) {
    if (level == 0) return s->imagePtr;
    if (level == 1) return s->mipPtr;
    return s->mipPtr + s->mipOffset[level - 1];
}

static uint32_t element_offset(const LatteAddrLib::AddrSurfaceInfo_OUT& info, Latte::E_HWTILEMODE tm, uint32_t x, uint32_t y,
                               uint32_t slice, uint32_t bpp, uint32_t swizzle, LatteAddrLib::CachedSurfaceAddrInfo* ci) {
    if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
        return LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, slice, 0, bpp, info.pitch, info.height, info.depth);
    if (!Latte::TM_IsMacroTiled(tm))
        return LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, slice, bpp, info.pitch, info.height, tm, false);
    return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, ci);
}

// ---------------------------------------------------------------- write-back to guest memory
// Render results stay on the GPU, except where the game reads them with the CPU: render targets and
// GX2CopySurface destinations with a linear tile mode (as Cemu reads back linear surfaces). The Picto
// Box renders its picture into a linear-aligned target and JPEG-encodes it from guest memory (issue
// #53). Texels are written at the guest size; only formats whose host texels are the guest's bytes.
static bool can_write_back(const Surface* img) {
    const FormatInfo& f = img->fmt;
    bool ok = img->tex && !f.compressed && !f.depth && f.convert == Convert::NONE && f.hostBytesPerBlock == f.bytesPerBlock &&
              img->tex.textureType == MTLTextureType2D;
    if (!ok) {
        static bool logged = false;
        if (!logged) LOG("[gfx] linear surface %08X (format %X) is not written back to guest memory", img->addr, img->format);
        logged = true;
    }
    return ok;
}
// the texels of img at the guest size w x h (its top-left), read from the GPU (waits for it)
// encodes the copy of img's texels at the guest size w x h (its top-left) into a shared buffer; the
// buffer holds them once the current command buffer has completed
static id<MTLBuffer> encode_texel_read(Surface* img, uint32_t w, uint32_t h) {
    const uint32_t bytes = img->fmt.bytesPerBlock;
    id<MTLTexture> src = img->tex;
    if (src.width != img->width || src.height != img->height) {
        // a render target at the internal resolution: filter it down to the guest size first
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:img->width
                                                                                     height:img->height mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> small = [R.device newTextureWithDescriptor:td];
        end_encoder();
        g_resample_why = "readback";
        resample(src, small, img->fmt, 1);
        g_resample_why = nullptr;
        src = small;
    }
    id<MTLBuffer> buf = [R.device newBufferWithLength:(NSUInteger)w * h * bytes options:MTLResourceStorageModeShared];
    end_encoder();
    id<MTLBlitCommandEncoder> b = TIMED_BLIT();
    [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * bytes destinationBytesPerImage:(NSUInteger)w * h * bytes];
    [b endEncoding];
    return buf;
}
static std::vector<uint8_t> read_guest_texels(Surface* img, uint32_t w, uint32_t h) {
    id<MTLBuffer> buf = encode_texel_read(img, w, h);
    wait_idle();
    const uint8_t* raw = (const uint8_t*)buf.contents;
    return std::vector<uint8_t>(raw, raw + (size_t)w * h * img->fmt.bytesPerBlock);
}
// GX2CopySurface done on the GPU into a linear destination: the game reads it once the call returns
static void write_back_linear_copy(Surface* img, GX2Surface* d, uint32_t dbase, uint32_t dstMip, uint32_t dstSlice, uint32_t w, uint32_t h) {
    LatteAddrLib::AddrSurfaceInfo_OUT di{};
    LatteAddrLib::GX2CalculateSurfaceInfo(d->format, d->width, d->height, d->depth, d->dim, d->tileMode, d->aa, dstMip, &di);
    auto dtm = (Latte::E_HWTILEMODE)di.hwTileMode;
    if ((dtm != Latte::E_HWTILEMODE::TM_LINEAR_GENERAL && dtm != Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED) || dstSlice >= di.depth ||
        !can_write_back(img))
        return;
    const uint32_t bytes = img->fmt.bytesPerBlock;
    const uint64_t r0 = rprof::now_ns();
    auto texels = read_guest_texels(img, w, h);
    {
        uint64_t sum = 0;
        for (uint8_t v : texels) sum += v;
        static std::set<std::pair<uint32_t, uint32_t>> seen;
        if (seen.insert({w, h}).second || sum == 0) LOG("[copy] linear write-back %ux%u to %08X, mean byte %.1f", w, h, dbase, texels.empty() ? 0.0 : (double)sum / texels.size());
    }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            memcpy(mem::ptr(dbase + element_offset(di, dtm, x, y, dstSlice, bytes * 8, d->swizzle, nullptr)),
                   texels.data() + ((size_t)y * w + x) * bytes, bytes);
    img->writtenBackSeq = img->writeSeq;
    img->gpuWatched = false;  // our own write to its memory: not a CPU change
    rprof::add_write_back(0, 1, texels.size(), rprof::now_ns() - r0);
}
// GX2DrawDone: linear render targets drawn since their last write-back
// Linear render targets reach guest memory asynchronously: the copy is queued with the frame's GPU
// work and written to guest memory once that work has completed (a later DrawDone or swap), instead
// of waiting for an idle GPU at every DrawDone (BotW draws one every frame: 21% of the render
// thread in waitUntilCompleted). The CPU sees the result a frame or so later, like Cemu's
// asynchronous readback. WWHD_SYNC_WRITE_BACK=1 restores the wait.
struct PendingWriteBack {
    id<MTLBuffer> buf;
    id<MTLCommandBuffer> cmd;
    uint32_t addr, w, h, bytes, pitch;
};
static std::vector<PendingWriteBack> g_pending_write_backs;
void drain_write_backs(bool all) {
    size_t done = 0;
    for (auto& p : g_pending_write_backs) {
        if (!all && p.cmd.status < MTLCommandBufferStatusCompleted) break;  // in order: later ones follow
        if (all) [p.cmd waitUntilCompleted];
        const uint8_t* raw = (const uint8_t*)p.buf.contents;
        for (uint32_t y = 0; y < p.h; y++)
            memcpy(mem::ptr(p.addr + y * p.pitch * p.bytes), raw + (size_t)y * p.w * p.bytes, (size_t)p.w * p.bytes);
        done++;
    }
    g_pending_write_backs.erase(g_pending_write_backs.begin(), g_pending_write_backs.begin() + done);
}
std::atomic<uint64_t> g_copy_cpu{0}, g_copy_cpu_bytes{0}, g_copy_gpu{0}, g_write_backs{0}, g_write_back_bytes{0}, g_write_backs_sync{0};
void write_back_linear_targets() {
    static const bool sync = getenv("WWHD_SYNC_WRITE_BACK") && atoi(getenv("WWHD_SYNC_WRITE_BACK")) != 0;
    const uint64_t t0 = rprof::now_ns();
    uint64_t readNs = 0, bytesDone = 0;
    uint32_t count = 0;
    drain_write_backs(false);
    struct Probe { id<MTLBuffer> buf; uint32_t addr, w, h, bytes, pitch; };
    std::vector<Probe> probes;  // read together after one wait
    // linear aligned only: a tile mode of 0 can also be GX2's "default" (a tiled copy destination)
    for (Surface* s : R.linearTargets) {
        if (!s->gpuWritten || s->writtenBackSeq == s->writeSeq || !s->tex) continue;
        s->writtenBackSeq = s->writeSeq;
        s->gpuWatched = false;
        if (s->slices != 1 || !can_write_back(s)) continue;
        const uint32_t addr = s->addr;
        const uint32_t bytes = s->fmt.bytesPerBlock, pitch = std::max(s->pitch, s->width);
        {
            static std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
            if (seen.insert({s->width, s->height, s->format}).second)
                LOG("[gfx] linear render target written back: %ux%u format %X at %08X", s->width, s->height, s->format, addr);
        }
        // tiny targets are readback probes: the game draws a pixel (a colour sampled on the GPU), waits for
        // DrawDone and reads it. A frame late, its water colour table (BotW) took each value one slot off and
        // kept a white default in the first, on the boots where the copy lagged (white water). One wait for
        // all of a DrawDone's probes, only on the frames that draw them; the big targets stay asynchronous.
        const bool probe = (uint64_t)s->width * s->height <= 64;
        if (probe && !sync) {
            probes.push_back({encode_texel_read(s, s->width, s->height), addr, s->width, s->height, bytes, pitch});
            continue;
        }
        if (!sync) {
            id<MTLBuffer> buf = encode_texel_read(s, s->width, s->height);
            g_pending_write_backs.push_back({buf, command_buffer(), addr, s->width, s->height, bytes, pitch});
            count++;
            g_write_backs++;
            g_write_back_bytes += (uint64_t)s->width * s->height * bytes;
            continue;
        }
        const uint64_t r0 = rprof::now_ns();
        auto texels = read_guest_texels(s, s->width, s->height);
        for (uint32_t y = 0; y < s->height; y++)
            memcpy(mem::ptr(addr + y * pitch * bytes), texels.data() + (size_t)y * s->width * bytes, (size_t)s->width * bytes);
        readNs += rprof::now_ns() - r0;
        bytesDone += texels.size();
        count++;
    }
    if (!probes.empty()) {
        const uint64_t r0 = rprof::now_ns();
        wait_idle();
        for (const Probe& p : probes) {
            const uint8_t* raw = (const uint8_t*)p.buf.contents;
            for (uint32_t y = 0; y < p.h; y++)
                memcpy(mem::ptr(p.addr + y * p.pitch * p.bytes), raw + (size_t)y * p.w * p.bytes, (size_t)p.w * p.bytes);
            bytesDone += (uint64_t)p.w * p.h * p.bytes;
            count++;
        }
        g_write_backs_sync++;  // waits, not probes
        readNs += rprof::now_ns() - r0;
    }
    rprof::add_write_back(std::max<uint64_t>(1, rprof::now_ns() - t0 - readNs), count, bytesDone, readNs);
}

void copy_surface_impl(uint32_t srcAddr, uint32_t srcMip, uint32_t srcSlice, uint32_t dstAddr, uint32_t dstMip, uint32_t dstSlice) {
    auto* s = (GX2Surface*)mem::ptr(srcAddr);
    auto* d = (GX2Surface*)mem::ptr(dstAddr);
    uint32_t sbase = level_address(s, srcMip), dbase = level_address(d, dstMip);
    uint32_t w = std::max<uint32_t>(s->width >> srcMip, 1), h = std::max<uint32_t>(s->height >> srcMip, 1);
    FormatInfo f = format_info((uint32_t)s->format.value(), false);

    // Cemu (LatteSurfaceCopy_copySurfaceNew): with the source in the texture cache, a GPU copy into the
    // destination's surface (made if needed); else the copy is done in guest memory.
    LatteAddrLib::AddrSurfaceInfo_OUT si{}, di{};
    LatteAddrLib::GX2CalculateSurfaceInfo(s->format, s->width, s->height, s->depth, s->dim, s->tileMode, s->aa, srcMip, &si);
    LatteAddrLib::GX2CalculateSurfaceInfo(d->format, d->width, d->height, d->depth, d->dim, d->tileMode, d->aa, dstMip, &di);
    // each distinct kind of copy is logged once (size, formats, tile modes, path taken): finds
    // copies that silently produce nothing (save thumbnails were black)
    auto note = [&](const char* path) {
        static std::set<std::string> seen;
        char k[200];
        snprintf(k, sizeof k, "%ux%u fmt %X tm %u -> %ux%u fmt %X tm %u: %s", w, h, (uint32_t)s->format.value(),
                 (uint32_t)s->tileMode.value(), std::max<uint32_t>(d->width >> dstMip, 1), std::max<uint32_t>(d->height >> dstMip, 1),
                 (uint32_t)d->format.value(), (uint32_t)d->tileMode.value(), path);
        if (seen.insert(k).second) LOG("[copy] %s", k);
        // per-300-frame counts by path (metal_main.mm's report)
        if (path[0] == 'C') { g_copy_cpu++; g_copy_cpu_bytes += (uint64_t)w * h * 4; }
        else g_copy_gpu++;
    };
    auto split = [](uint32_t a, Latte::E_HWTILEMODE hwTileMode, uint32_t& swz) {
        swz = 0;
        if (Latte::TM_IsMacroTiled(hwTileMode)) { swz = a & 0x700; a &= ~0x700u; }
        return a;
    };
    const uint32_t sformat = (uint32_t)s->format.value(), dformat = (uint32_t)d->format.value();
    uint32_t sswzBits = 0, dswzBits = 0;
    const uint32_t sAddr = split(srcMip ? sbase : sbase ^ (Latte::TM_IsMacroTiled((Latte::E_HWTILEMODE)si.hwTileMode) ? (s->swizzle & 0x700) : 0),
                                 si.hwTileMode, sswzBits);
    const uint32_t dAddr = split(dstMip ? dbase : dbase ^ (Latte::TM_IsMacroTiled((Latte::E_HWTILEMODE)di.hwTileMode) ? (d->swizzle & 0x700) : 0),
                                 di.hwTileMode, dswzBits);
    const uint32_t sPitch = si.pitch, dPitch = di.pitch;
    TexView* sv = (Latte::E_GX2TILEMODE)s->tileMode.value() == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL
                      ? nullptr
                      : view_lookup_slice_min(sAddr, w, h, sPitch, 0, srcSlice, sformat);
    if (!sv && (Latte::E_GX2TILEMODE)s->tileMode.value() != Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL)
        sv = create_mapping(sAddr, 0, w, h, srcSlice + 1, sPitch, (uint32_t)si.hwTileMode, sswzBits, 0, 1, srcSlice, 1, sformat,
                            srcSlice ? (uint32_t)Latte::E_DIM::DIM_2D_ARRAY : (uint32_t)Latte::E_DIM::DIM_2D, (uint32_t)Latte::E_DIM::DIM_2D, false, false);
    if (sv && !sv->tex->gpuWritten) sv = nullptr;  // CPU data only: the guest-memory copy below is exact
    if (sv) {
        Surface* src = sv->tex;
        update_to_latest(src);
        const uint32_t dw = std::max<uint32_t>(d->width >> dstMip, 1), dh = std::max<uint32_t>(d->height >> dstMip, 1);
        TexView* dv = view_lookup_slice_min(dAddr, std::min(w, dw), std::min(h, dh), dPitch, 0, dstSlice, dformat);
        if (!dv) {
            const auto ddim = d->dim.value();
            uint32_t dimBase = (uint32_t)Latte::E_DIM::DIM_2D;
            uint32_t depth = 1;
            if (ddim == Latte::E_DIM::DIM_2D_ARRAY || ddim == Latte::E_DIM::DIM_CUBEMAP) {
                depth = std::max<uint32_t>((uint32_t)d->depth, dstSlice + 1);
                dimBase = ddim == Latte::E_DIM::DIM_CUBEMAP && depth % 6 == 0 ? (uint32_t)Latte::E_DIM::DIM_CUBEMAP : (uint32_t)Latte::E_DIM::DIM_2D_ARRAY;
            }
            dv = create_mapping(dAddr, 0, dw, dh, depth, dPitch, (uint32_t)di.hwTileMode, dswzBits, 0, 1, dstSlice, 1, dformat, dimBase,
                                (uint32_t)Latte::E_DIM::DIM_2D, false);
        }
        if (!dv) {
            note("GPU source, no destination surface (skipped)");
            return;
        }
        Surface* dst = dv->tex;
        if (!same_scale(src, dst)) {
            note("GPU source, other internal resolution: resampled");
            // not in Cemu (it skips these): filtered, so a picture survives (save thumbnails)
            const uint32_t cw = std::min(w, std::max(dst->width >> dv->firstMip, 1u)), ch = std::min(h, std::max(dst->height >> dv->firstMip, 1u));
            id<MTLTexture> sview = [src->tex newTextureViewWithPixelFormat:src->tex.pixelFormat textureType:MTLTextureType2D
                                                                    levels:NSMakeRange(sv->firstMip, 1) slices:NSMakeRange(sv->firstSlice, 1)];
            id<MTLTexture> dview = [dst->tex newTextureViewWithPixelFormat:dst->tex.pixelFormat textureType:MTLTextureType2D
                                                                    levels:NSMakeRange(dv->firstMip, 1) slices:NSMakeRange(dv->firstSlice, 1)];
            if (sview && dview && src->fmt.depth == dst->fmt.depth) {
                g_resample_why = "copy";
                resample(sview, dview, dst->fmt, 1, std::min(1.0f, cw * src->sx / sview.width), std::min(1.0f, ch * src->sy / sview.height),
                         eff(cw, dst->sx), eff(ch, dst->sy));
                g_resample_why = nullptr;
            }
        } else {
            note(src->fmt.pixel == dst->fmt.pixel ? "GPU source" : "GPU source, other format");
            gpu_copy(src, sv->firstSlice, sv->firstMip, 0, 0, dst, dv->firstSlice, dv->firstMip, 0, 0, eff(w, src->sx), eff(h, src->sy));
        }
        mark_gpu_written(dst, dv->firstSlice, dv->firstMip);
        dst->lastUseFrame = src->lastUseFrame = R.frame;
        if (dv->firstMip == 0 && dv->firstSlice == 0) write_back_linear_copy(dst, d, dbase, dstMip, dstSlice, std::min(w, dst->width), std::min(h, dst->height));
        return;
    }

    // CPU-produced source: re-tile in guest memory; texture uploads pick up the change
    {
        bool gpuAtAddr = false;
        auto range = R.surfaces.equal_range(sAddr);
        for (auto it = range.first; it != range.second; ++it) gpuAtAddr |= it->second->gpuWritten;
        note(gpuAtAddr ? "CPU path, but a GPU image of another size is at the source" : "CPU path");
    }
    auto stm = (Latte::E_HWTILEMODE)si.hwTileMode, dtm = (Latte::E_HWTILEMODE)di.hwTileMode;
    uint32_t bpp = f.bytesPerBlock * 8;
    uint32_t bw = f.compressed ? (w + 3) / 4 : w, bh = f.compressed ? (h + 3) / 4 : h;
    uint32_t sswz = s->swizzle, dswz = d->swizzle;
    LatteAddrLib::CachedSurfaceAddrInfo sci, dci;
    if (Latte::TM_IsMacroTiled(stm))
        LatteAddrLib::SetupCachedSurfaceAddrInfo(&sci, srcSlice, 0, bpp, si.pitch, si.height, si.depth, 1, stm, false, (sswz >> 8) & 1, (sswz >> 9) & 3);
    if (Latte::TM_IsMacroTiled(dtm))
        LatteAddrLib::SetupCachedSurfaceAddrInfo(&dci, dstSlice, 0, bpp, di.pitch, di.height, di.depth, 1, dtm, false, (dswz >> 8) & 1, (dswz >> 9) & 3);
    // debug: WWHD_COPYDBG=1 logs each CPU-path copy (destination range, time); see GX2CopySurface
    static const bool dbg = getenv("WWHD_COPYDBG") != nullptr;
    if (dbg)
        LOG("[copydbg] exec cpu copy t=%.3f src %08X dst %08X..%08X (%ux%u fmt %X tm %u->%u)", timebase::now() / (double)timebase::kTicksPerSec,
            sbase, dbase, dbase + (uint32_t)di.surfSize, w, h, (uint32_t)s->format.value(), (uint32_t)stm, (uint32_t)dtm);
    for (uint32_t y = 0; y < bh; y++)
        for (uint32_t x = 0; x < bw; x++) {
            uint32_t so = element_offset(si, stm, x, y, srcSlice, bpp, sswz, &sci);
            uint32_t dofs = element_offset(di, dtm, x, y, dstSlice, bpp, dswz, &dci);
            memcpy(mem::ptr(dbase + dofs), mem::ptr(sbase + so), f.bytesPerBlock);
        }
    // force re-upload of any texture made from the destination
    auto dr = R.surfaces.equal_range(dAddr);
    for (auto it = dr.first; it != dr.second; ++it) {
        it->second->lastCheckedFrame = ~0ull;
        it->second->dirty = true;
    }
}

}  // namespace gfx

namespace gfx {
// a save state replaced guest memory: every CPU-side texture gets a full check on next use (render
// targets keep their GPU contents; the next frame redraws them)
void ss_reset_surfaces() {
    R.mainDepthAddr = 0;
    for (auto& [a, s] : R.surfaces) {
        s->dirty = true;
        s->lastCheckedFrame = ~0ull;
    }
}
}  // namespace gfx
