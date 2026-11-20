// Makes platendoos's app icon: rekordbox's icon with its colours inverted (white tile, black logo), so the two
// apps are easy to tell apart in the Dock. The corners are filled in too: macOS rounds a square icon itself, but
// puts one with its own rounded corners in a grey frame. The .icns's logo fills its tile, but the Dock shows
// rekordbox's Assets.car icon, whose logo is smaller (measured: 0.80 of the .icns's), so the logo is scaled to match.
//   swift tools/build/invert_icon.swift <rekordbox.icns> <out.icns>
import AppKit

let args = CommandLine.arguments
let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("platendoos-icon-\(getpid())")
let input = tmp.appendingPathComponent("in.iconset"), output = tmp.appendingPathComponent("out.iconset")
try? FileManager.default.removeItem(at: tmp)
try! FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)

func iconutil(_ a: String...) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
    p.arguments = a
    try! p.run()
    p.waitUntilExit()
    if p.terminationStatus != 0 { exit(1) }
}

iconutil("-c", "iconset", "-o", input.path, args[1])
for name in try! FileManager.default.contentsOfDirectory(atPath: input.path) where name.hasSuffix(".png") {
    let src = NSBitmapImageRep(data: try! Data(contentsOf: input.appendingPathComponent(name)))!
    let w = src.pixelsWide, h = src.pixelsHigh
    let out = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    for y in 0..<h {
        for x in 0..<w {
            let c = src.colorAt(x: x, y: y)!.usingColorSpace(.deviceRGB)!
            // Over the tile's colour (rekordbox's dark grey, inverted to light) where the original is see-through.
            let a = c.alphaComponent, bg: CGFloat = 1 - 0.13
            func inv(_ v: CGFloat) -> CGFloat { (1 - v) * a + bg * (1 - a) }
            out.setColor(NSColor(deviceRed: inv(c.redComponent), green: inv(c.greenComponent), blue: inv(c.blueComponent),
                                 alpha: 1), atX: x, y: y)
        }
    }
    // The logo at rekordbox's size, centred, on the tile's colour.
    let scale: CGFloat = 0.80
    let fin = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: w, pixelsHigh: h, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    let ctx = NSGraphicsContext(bitmapImageRep: fin)!
    ctx.imageInterpolation = .high
    NSGraphicsContext.current = ctx
    out.colorAt(x: w / 2, y: h * 3 / 100 + 1)!.setFill()  // the tile, between its rim and the logo
    NSRect(x: 0, y: 0, width: w, height: h).fill()
    // Only the middle (the logo is more than 7% from the edges): leaves out the original tile's faint rim.
    let W = CGFloat(w), H = CGFloat(h)
    let part = NSRect(x: W * 0.055, y: H * 0.055, width: W * 0.89, height: H * 0.89)
    let dst = NSRect(x: W * (0.5 - 0.445 * scale), y: H * (0.5 - 0.445 * scale), width: part.width * scale, height: part.height * scale)
    out.draw(in: dst, from: part, operation: .copy, fraction: 1, respectFlipped: false, hints: nil)
    NSGraphicsContext.restoreGraphicsState()
    try! fin.representation(using: .png, properties: [:])!.write(to: output.appendingPathComponent(name))
}
iconutil("-c", "icns", "-o", args[2], output.path)
try? FileManager.default.removeItem(at: tmp)
