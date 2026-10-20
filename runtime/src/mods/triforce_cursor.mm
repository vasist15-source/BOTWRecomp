// The pointer shown over the game while the mouse is not captured (mods/mouse.mm): three gold
// triangles in a triangle, leaning like an arrow pointer, its tip the hot spot. Drawn here with
// AppKit paths at any scale; no image file.
#import <AppKit/AppKit.h>

#include <cmath>
#include <initializer_list>

namespace mods {

NSCursor* triforce_cursor() {
    static NSCursor* cursor = [] {
        const CGFloat size = 28, h = 19, side = 2 * h / std::sqrt(3.0), lean = -12 * M_PI / 180;
        // upright, tip at the origin, y down; then leaned so the body points down-right
        NSPoint top = {0, 0}, bl = {-side / 2, h}, br = {side / 2, h};
        NSPoint ml = {-side / 4, h / 2}, mr = {side / 4, h / 2}, mb = {0, h};
        NSPoint* pts[] = {&top, &bl, &br, &ml, &mr, &mb};
        CGFloat minx = 1e9, miny = 1e9;
        for (NSPoint* p : pts) {
            const CGFloat x = p->x * std::cos(lean) - p->y * std::sin(lean), y = p->x * std::sin(lean) + p->y * std::cos(lean);
            *p = NSMakePoint(x, y);
            minx = std::fmin(minx, x), miny = std::fmin(miny, y);
        }
        const CGFloat pad = 2.5;
        for (NSPoint* p : pts) *p = NSMakePoint(p->x - minx + pad, p->y - miny + pad);
        const NSPoint hot = top;
        NSImage* img = [NSImage imageWithSize:NSMakeSize(size, size) flipped:YES drawingHandler:^BOOL(NSRect) {
            auto tri = [](NSPoint a, NSPoint b, NSPoint c) {
                NSBezierPath* p = [NSBezierPath bezierPath];
                [p moveToPoint:a];
                [p lineToPoint:b];
                [p lineToPoint:c];
                [p closePath];
                p.lineJoinStyle = NSLineJoinStyleRound;
                return p;
            };
            NSBezierPath* outer = tri(top, bl, br);
            // a soft shadow and a dark rim, so the pointer reads on snow and in caves alike
            [NSGraphicsContext saveGraphicsState];
            NSShadow* sh = [NSShadow new];
            sh.shadowOffset = NSMakeSize(0.6, 1.2);  // flipped: down
            sh.shadowBlurRadius = 1.8;
            sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.55];
            [sh set];
            [[NSColor colorWithSRGBRed:0.17 green:0.11 blue:0.02 alpha:1] setFill];
            outer.lineWidth = 2.6;
            [[NSColor colorWithSRGBRed:0.17 green:0.11 blue:0.02 alpha:1] setStroke];
            [outer fill];
            [outer stroke];
            [NSGraphicsContext restoreGraphicsState];
            NSGradient* gold = [[NSGradient alloc] initWithColorsAndLocations:
                [NSColor colorWithSRGBRed:1.00 green:0.93 blue:0.62 alpha:1], 0.0,
                [NSColor colorWithSRGBRed:0.96 green:0.78 blue:0.30 alpha:1], 0.45,
                [NSColor colorWithSRGBRed:0.76 green:0.53 blue:0.10 alpha:1], 1.0, nil];
            for (NSBezierPath* p : {tri(top, ml, mr), tri(ml, bl, mb), tri(mr, mb, br)}) {
                [gold drawInBezierPath:p angle:90 + lean * 180 / M_PI];  // flipped: light at the tip
                [[NSColor colorWithSRGBRed:1.0 green:0.97 blue:0.80 alpha:0.9] setStroke];
                p.lineWidth = 0.6;
                [p stroke];
            }
            return YES;
        }];
        return [[NSCursor alloc] initWithImage:img hotSpot:hot];
    }();
    return cursor;
}

}  // namespace mods
