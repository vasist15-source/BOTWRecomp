// usage: xcrun swift tools/svg2png.swift in.svg out.png width
// Renders an SVG to a PNG with alpha (AppKit's own SVG reader). tools/controller_images.py uses it.
import AppKit
let a = CommandLine.arguments
guard a.count == 4, let img = NSImage(contentsOfFile: a[1]), let w = Int(a[3]) else { print("usage / cannot read"); exit(1) }
let h = Int((Double(w) * Double(img.size.height) / Double(img.size.width)).rounded())
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
rep.size = NSSize(width: w, height: h)
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
img.draw(in: NSRect(x: 0, y: 0, width: w, height: h), from: .zero, operation: .copy, fraction: 1)
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: a[2]))
print(a[2], w, h)
