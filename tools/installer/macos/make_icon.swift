// Draws the installer's icon (a golden triangle on a dark teal squircle) as a 1024 px PNG.
// usage: swift make_icon.swift OUT.png   (tools/installer/macos/build.sh turns it into AppIcon.icns)
import AppKit

let size = 1024.0
let img = NSImage(size: NSSize(width: size, height: size))
img.lockFocus()
let inset = size * 0.1
let rect = NSRect(x: inset, y: inset, width: size - 2 * inset, height: size - 2 * inset)
let bg = NSBezierPath(roundedRect: rect, xRadius: size * 0.18, yRadius: size * 0.18)
NSGradient(colors: [NSColor(calibratedRed: 0.10, green: 0.27, blue: 0.32, alpha: 1),
                    NSColor(calibratedRed: 0.04, green: 0.11, blue: 0.16, alpha: 1)])!.draw(in: bg, angle: -90)
let tri = NSBezierPath()
tri.move(to: NSPoint(x: size / 2, y: size * 0.74))
tri.line(to: NSPoint(x: size * 0.27, y: size * 0.30))
tri.line(to: NSPoint(x: size * 0.73, y: size * 0.30))
tri.close()
NSGradient(colors: [NSColor(calibratedRed: 1.0, green: 0.86, blue: 0.40, alpha: 1),
                    NSColor(calibratedRed: 0.93, green: 0.56, blue: 0.16, alpha: 1)])!.draw(in: tri, angle: -90)
img.unlockFocus()
let rep = NSBitmapImageRep(data: img.tiffRepresentation!)!
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
