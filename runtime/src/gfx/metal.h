#include <atomic>
// Metal renderer internals (Objective-C++).
#pragma once
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "formats.h"

namespace gfx {
// layers of a texture (array slices, cube faces): render-pass slice, blit and upload indices
inline NSUInteger layer_count(id<MTLTexture> t) {
    switch (t.textureType) {
    case MTLTextureType3D: return 1;
    case MTLTextureTypeCube: return 6;
    case MTLTextureTypeCubeArray: return 6 * std::max<NSUInteger>(t.arrayLength, 1);
    default: return std::max<NSUInteger>(t.arrayLength, 1);
    }
}
void peek_z(const uint32_t*, uint32_t);

// Texture cache after Cemu's (Cafe/HW/Latte/Core/LatteTexture*.cpp, MPL-2.0): a Surface is one host
// texture for one guest surface definition (address, size, pitch, format, tile mode, its mip levels and
// slices). What draws sample and render to are views into it (a mip and slice range, TexView). Surfaces
// that share guest memory at the same layout are related (TexRelation) and copy each other's GPU-written
// slices when used. metal_surfaces.mm.
struct Surface;
void note_surface_range(Surface* s, uint32_t addr, uint32_t size);
void forget_surface_ranges(Surface* s);  // metal_main.mm: the surface is being deleted
// staging memory from the upload pool (metal_draw.mm), filled by the caller
id<MTLBuffer> upload_reserve(uint32_t size, uint32_t* offset, uint8_t** ptr);

struct TexView {
    Surface* tex = nullptr;
    uint32_t firstMip = 0, numMip = 1, firstSlice = 0, numSlice = 1;
    uint32_t format = 0, dim = 1;  // E_GX2SURFFMT (always the texture's), E_DIM
    std::vector<uint32_t> keys, keysNoRes;  // view lookup buckets it is registered in
};
// one slice of one mip level: its guest memory and when the GPU last wrote it
struct SliceMip {
    uint32_t addrStart = 0, addrEnd = 0, subIndex = 0, pitch = 0, tileMode = 0;
    uint32_t slice = 0, mip = 0;
    Surface* tex = nullptr;
    uint64_t lastDynamicUpdate = 0;
    std::vector<std::pair<Surface*, SliceMip*>> overlaps;  // other surfaces' slices overlapping this memory at an offset
};
// sub is the same memory as base's (baseSlice, baseMip) onward, at a compatible layout
struct TexRelation {
    Surface* base;
    Surface* sub;
    uint32_t baseSlice, baseMip, sliceCount, mipCount;
};

struct Surface {
    id<MTLTexture> tex = nil;
    uint32_t addr = 0, mipAddr = 0;
    uint32_t width = 0, height = 0, slices = 1, pitch = 0, mips = 1;
    uint32_t format = 0;       // E_GX2SURFFMT
    uint32_t dim = 1;          // E_DIM
    uint32_t tileMode = 0;     // E_HWTILEMODE
    uint32_t swizzle = 0;      // pipe/bank swizzle bits (address & 0x700)
    uint32_t lastRenderTargetSwizzle = 0;
    bool isDepth = false;
    bool gpuWritten = false;   // contents produced by the GPU; never reload from guest memory
    uint64_t lastWriteFrame = 0;  // frame of the last GPU write (save states keep the ones not redrawn every frame)
    uint64_t writeSeq = 0;     // the latest GPU write event (next_write_seq) of any of its slices
    uint64_t writtenBackSeq = 0;  // writeSeq when last written back to guest memory (linear surfaces)
    uint64_t contentHash = 0;  // hash of all guest bytes (every level) at the last check
    uint64_t lastCheckedFrame = ~0ull;
    uint64_t sparseHash = 0;   // fallback without write tracking: cheap per-frame sampled check
    uint32_t dataSize = 0;     // base level size in guest memory
    bool dirty = true;         // new, or invalidated by the game: do a full check
    // CPU textures: guest ranges of all levels (base first) and the write stamp (write_watch.h) they
    // were armed with at the last full check; no newer stamp on their pages = unchanged
    std::vector<std::pair<uint32_t, uint32_t>> levelRanges;
    uint64_t watchStamp = 0;
    bool watched = false;
    // GPU-written surfaces outside MEM1: guest memory hash when watched, so a CPU write reloads them (Cemu)
    bool gpuWatched = false;
    uint64_t gpuRamHash = 0;
    FormatInfo fmt;
    // internal resolution: width/height above are the guest's (logical) size, used for every lookup and
    // guest-memory computation; the texture may be larger. sx/sy = texture size / logical size.
    float scale = 1.0f;        // the resolution factor the texture was made for
    float ax = 1.0f, ay = 1.0f;  // ... and the aspect-ratio factors (screen-shaped targets, metal_surfaces.mm)
    float sx = 1.0f, sy = 1.0f;
    float minScale = 0.0f;  // at least this factor: a depth buffer drawn with colour targets scaled more (match_depth_scale)
    // texture cache
    std::vector<SliceMip> sm;  // [mip * slices + slice]
    std::vector<TexRelation*> relations;
    std::vector<TexView*> views;
    TexView* baseView = nullptr;
    bool reloadFromDynamic = false;  // a related surface was written: sync before use
    bool deleted = false;            // removed from the cache; freed at the end of the frame
    uint64_t lastUseFrame = 0;
    SliceMip* slice_mip(uint32_t slice, uint32_t mip) { return &sm[(size_t)mip * slices + slice]; }
    uint32_t mip_slices(uint32_t mip) const { return dim == 2 /* DIM_3D */ ? std::max(slices >> mip, 1u) : slices; }
};

// A display output: the TV or the GamePad screen, each in its own window.
struct Screen {
    id<MTLTexture> tex = nil;          // last image copied to the scan buffer
    CAMetalLayer* layer = nil;
    std::atomic<bool> srgb{false};     // scan buffer is sRGB: the display applies the encoding
    std::atomic<bool> visible{true};   // nextDrawable blocks while the window is covered
};

uint64_t next_write_seq();
Surface* match_depth_scale(Surface* depth, const Surface* color);
// a draw, clear or copy wrote this slice of this level on the GPU (related surfaces sync from it)
void mark_gpu_written(Surface* s, uint32_t slice, uint32_t mip);
// frame boundary: unused / overwritten surfaces are dropped and deleted ones freed (swap)
void texture_cache_frame_end();
extern bool g_repeat_texture_init;  // a lookup created or deleted surfaces: the draw redoes its lookups

struct Renderer {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;

    id<MTLCommandBuffer> cmd = nil;
    id<MTLRenderCommandEncoder> enc = nil;
    bool binding = false;  // a draw is binding its textures to the open encoder (no blits now)
    // attachments of the open render encoder
    Surface* passColor[8] = {};
    uint32_t mainDepthAddr = 0;
    Surface* passDepth = nullptr;
    uint32_t passColorSlice[8] = {}, passDepthSlice = 0;
    uint32_t passColorMip[8] = {}, passDepthMip = 0;

    // guest memory visible to the GPU (no-copy buffers)
    struct GuestRange {
        uint32_t base, size;
        id<MTLBuffer> buf;
    };
    std::vector<GuestRange> guest;

    // outputs
    Screen tv, drc;
    id<MTLRenderPipelineState> presentPipeline = nil;
    id<MTLRenderPipelineState> presentPipelineSRGB = nil;
    id<MTLSamplerState> linearClamp = nil;

    // surfaces keyed by guest address (several may share an address with different shapes)
    std::unordered_multimap<uint32_t, std::unique_ptr<Surface>> surfaces;
    std::vector<Surface*> linearTargets;  // linear-aligned colour surfaces (never removed): GX2DrawDone write-back

    uint64_t frame = 0;
    uint64_t drawCount = 0;
};
extern Renderer R;

// command buffer / encoder management
id<MTLCommandBuffer> command_buffer();
// WWHD_GPU_PASS_TIMES: blit encoders and stray render passes are timed too
id<MTLBlitCommandEncoder> timed_blit_encoder(const char* file, int line);
void time_render_pass(MTLRenderPassDescriptor* rp, const char* file, int line);
#define TIMED_BLIT() timed_blit_encoder(__FILE_NAME__, __LINE__)
void end_encoder();

// guest memory -> (buffer, offset); returns nil if the address is not GPU-visible
id<MTLBuffer> guest_buffer(uint32_t addr, uint32_t* offset);

// views: what draws sample (SQ_TEX_RESOURCE words) and render to (CB_COLOR* / DB_DEPTH_* registers, or
// GX2ColorBuffer / GX2DepthBuffer structs; numSlices: the view's slice count for clears)
TexView* sampled_texture(const uint32_t* texWords, bool isDepthSampler);
TexView* color_target(const uint32_t* regs, int index);
TexView* depth_target(const uint32_t* regs);
TexView* surface_from_color_buffer(uint32_t gx2ColorBuffer, uint32_t* numSlices = nullptr);
TexView* surface_from_depth_buffer(uint32_t gx2DepthBuffer, uint32_t* numSlices = nullptr);
void upload_surface(Surface* s);

// internal resolution (Graphics menu / R / WWHD_RES_SCALE): render targets are allocated at this
// multiple of their guest size; a change takes effect at the next frame (targets are resized on next use)
float res_scale();          // the factor in effect this frame
void set_res_scale(float f);
void latch_res_scale();     // frame boundary: apply a requested change (resolution and aspect ratio)
// aspect ratio of the TV picture the game is drawing from the next frame on (aspect.cpp via the swap
// command); screen-shaped render targets are made that much wider/taller than their guest size
void set_frame_aspect(float a);
// draws `src` (whole texture, or its top-left uvMax fraction) stretched over `dst`'s rect (pixels; w=0: all)
void resample(id<MTLTexture> src, id<MTLTexture> dst, const FormatInfo& fmt, uint32_t slices, float uMax = 1, float vMax = 1,
              uint32_t dstW = 0, uint32_t dstH = 0);
void forget_texture_views();  // metal_draw.mm: textures were replaced

}  // namespace gfx
