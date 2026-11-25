// Draws mproton Library's app icon as a 1024x1024 PNG: the page's accent blue in a macOS
// icon-grid rounded square, with a white play symbol. Usage: swift make-icon.swift out.png
// library/make-app.sh turns it into AppIcon.icns with sips and iconutil.

import AppKit

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon.png"
let size = 1024
guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
                                 bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                 colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else {
    fatalError("no bitmap")
}
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)

// macOS icon grid: 824x824 body centred in 1024, corner radius ~185, soft drop shadow.
let body = NSRect(x: 100, y: 100, width: 824, height: 824)
let shape = NSBezierPath(roundedRect: body, xRadius: 185, yRadius: 185)
NSGraphicsContext.saveGraphicsState()
let shadow = NSShadow()
shadow.shadowColor = NSColor.black.withAlphaComponent(0.3)
shadow.shadowBlurRadius = 24
shadow.shadowOffset = NSSize(width: 0, height: -10)
shadow.set()
NSColor(srgbRed: 0.04, green: 0.40, blue: 0.85, alpha: 1).setFill()
shape.fill()
NSGraphicsContext.restoreGraphicsState()

NSGradient(starting: NSColor(srgbRed: 0.27, green: 0.60, blue: 1.0, alpha: 1),
           ending: NSColor(srgbRed: 0.02, green: 0.26, blue: 0.66, alpha: 1))!
    .draw(in: shape, angle: -90)

// Play symbol, optically centred (nudged right), with rounded corners.
let tri = NSBezierPath()
let cx: CGFloat = 512 + 30, cy: CGFloat = 512, r: CGFloat = 250
tri.move(to: NSPoint(x: cx - r * 0.5, y: cy + r * 0.866))
tri.line(to: NSPoint(x: cx + r, y: cy))
tri.line(to: NSPoint(x: cx - r * 0.5, y: cy - r * 0.866))
tri.close()
tri.lineJoinStyle = .round
tri.lineWidth = 70
NSColor.white.setFill()
NSColor.white.setStroke()
tri.fill()
tri.stroke()

NSGraphicsContext.restoreGraphicsState()
guard let png = rep.representation(using: .png, properties: [:]) else { fatalError("no png") }
try! png.write(to: URL(fileURLWithPath: out))
