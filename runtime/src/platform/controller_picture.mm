// The controller pictures of Settings > Controls on the AppKit host (hostui::controller_picture, gfx/overlay_appkit.mm).
// Each picture is a HEIC with alpha in a section of the executable, __PADS,<name>: CMakeLists.txt puts
// runtime/data/controllers/<name>.heic there, so the app stays one file. Decoded with ImageIO.
// No AppKit and nothing of the runtime, so runtime/tools/controller_picture_test.mm runs it as it is.
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace host {

// RGBA8, not premultiplied, top row first; false when the executable has no such picture or it does not decode
bool controller_picture(const char* name, int& width, int& height, std::vector<unsigned char>& rgba) {
    unsigned long size = 0;
    const uint8_t* data = getsectiondata(&_mh_execute_header, "__PADS", name, &size);
    if (!data || !size) return false;
    CFDataRef bytes = CFDataCreateWithBytesNoCopy(nullptr, data, (CFIndex)size, kCFAllocatorNull);
    CGImageSourceRef src = bytes ? CGImageSourceCreateWithData(bytes, nullptr) : nullptr;
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
    bool ok = false;
    if (img) {
        const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
        if (w && h && w <= 4096 && h <= 4096) {
            rgba.assign(w * h * 4, 0);
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, cs,
                                                     (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            if (ctx) {
                CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
                CGContextRelease(ctx);
                // the context's pixels are premultiplied; the overlay blends with plain alpha
                for (size_t i = 0; i < w * h; i++) {
                    uint8_t* px = &rgba[i * 4];
                    if (const unsigned a = px[3]; a && a < 255)
                        for (int c = 0; c < 3; c++) px[c] = (uint8_t)std::min(255u, (px[c] * 255u + a / 2) / a);
                }
                width = (int)w;
                height = (int)h;
                ok = true;
            }
            CGColorSpaceRelease(cs);
        }
        CGImageRelease(img);
    }
    if (src) CFRelease(src);
    if (bytes) CFRelease(bytes);
    return ok;
}

}  // namespace host
