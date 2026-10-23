// Draws the 1024x1024 app icons (CoreGraphics, no dependencies). Run by tools/make_app_icon.sh.
//
//   swift compose_icon.swift logo <logo.png> <out.png>      the launcher logo on a dark background, with margins
//   swift compose_icon.swift fallback <out.png>             the repository's own icon (anomaly rings), no game artwork
//   swift compose_icon.swift mac <square-1024.png> <out.png> the macOS variant: rounded body with a drop shadow
//
// `logo` and `fallback` write an opaque full-bleed square (what the iOS asset catalog wants; iOS rounds the corners).
// `mac` writes the 824 px rounded body on a transparent 1024 canvas (what a .icns wants).

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let canvas = 1024
let srgb = CGColorSpace(name: CGColorSpace.sRGB)!

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

func makeContext(opaque: Bool) -> CGContext {
    let info = opaque ? CGImageAlphaInfo.noneSkipLast.rawValue : CGImageAlphaInfo.premultipliedLast.rawValue
    guard let ctx = CGContext(data: nil, width: canvas, height: canvas, bitsPerComponent: 8, bytesPerRow: 0, space: srgb, bitmapInfo: info) else {
        fail("cannot create a bitmap context")
    }
    ctx.interpolationQuality = .high
    ctx.setAllowsAntialiasing(true)
    return ctx
}

func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: srgb, components: [CGFloat((hex >> 16) & 255) / 255, CGFloat((hex >> 8) & 255) / 255, CGFloat(hex & 255) / 255, alpha])!
}

func loadImage(_ path: String) -> CGImage {
    guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil), let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
        fail("cannot read " + path)
    }
    return image
}

func save(_ ctx: CGContext, _ path: String) {
    guard let image = ctx.makeImage(),
          let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil) else {
        fail("cannot write " + path)
    }
    CGImageDestinationAddImage(dest, image, nil)
    if !CGImageDestinationFinalize(dest) { fail("cannot write " + path) }
}

func radialGradient(_ ctx: CGContext, center: CGPoint, radius: CGFloat, inner: CGColor, outer: CGColor) {
    let gradient = CGGradient(colorsSpace: srgb, colors: [inner, outer] as CFArray, locations: [0, 1])!
    ctx.drawRadialGradient(gradient, startCenter: center, startRadius: 0, endCenter: center, endRadius: radius, options: [.drawsAfterEndLocation])
}

// Near-black with a faint olive-green glow in the middle, shared by both icons.
func drawBackground(_ ctx: CGContext) {
    ctx.setFillColor(rgb(0x070908))
    ctx.fill(CGRect(x: 0, y: 0, width: canvas, height: canvas))
    let middle = CGPoint(x: canvas / 2, y: canvas / 2)
    radialGradient(ctx, center: middle, radius: 760, inner: rgb(0x2f3b25), outer: rgb(0x070908, 0))
}

// The logo is a 256 px badge: blowing it up to fill the icon would blur it, so it sits at a bit over half the canvas.
func composeLogo(logoPath: String, outPath: String) {
    let logo = loadImage(logoPath)
    // Bounding box of the opaque part, so the badge (not its soft drop shadow) is what gets centred.
    let w = logo.width, h = logo.height
    var pixels = [UInt8](repeating: 0, count: w * h * 4)
    let probe = CGContext(data: &pixels, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: srgb,
                          bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    probe.draw(logo, in: CGRect(x: 0, y: 0, width: w, height: h))
    var minX = w, maxX = 0, minY = h, maxY = 0
    for y in 0..<h {
        for x in 0..<w where pixels[(y * w + x) * 4 + 3] > 160 {
            minX = min(minX, x); maxX = max(maxX, x); minY = min(minY, y); maxY = max(maxY, y)
        }
    }
    if maxX <= minX || maxY <= minY { fail("the logo has no opaque pixels") }
    // The bitmap above is stored top row first, CoreGraphics draws bottom-up.
    let boxW = CGFloat(maxX - minX + 1), boxH = CGFloat(maxY - minY + 1)
    let boxCenter = CGPoint(x: CGFloat(minX) + boxW / 2, y: CGFloat(h) - (CGFloat(minY) + boxH / 2))

    let scale = 640 / max(boxW, boxH)
    let ctx = makeContext(opaque: true)
    drawBackground(ctx)
    let drawn = CGRect(x: CGFloat(canvas) / 2 - boxCenter.x * scale, y: CGFloat(canvas) / 2 - boxCenter.y * scale,
                       width: CGFloat(w) * scale, height: CGFloat(h) * scale)
    ctx.draw(logo, in: drawn)
    save(ctx, outPath)
}

// An anomaly: broken, offset rings around a bright core, with a few drifting sparks. Original artwork.
func composeFallback(outPath: String) {
    let ctx = makeContext(opaque: true)
    drawBackground(ctx)
    let c = CGPoint(x: CGFloat(canvas) / 2, y: CGFloat(canvas) / 2)
    ctx.setLineCap(.round)

    // Rings: radius, line width, dash pattern, phase, colour. Different dashes and phases read as a distorted field.
    let rings: [(CGFloat, CGFloat, [CGFloat], CGFloat, UInt32, CGFloat)] = [
        (385, 14, [260, 130, 40, 150], 0, 0x5f7a2c, 0.55),
        (310, 20, [210, 90, 380, 70], 120, 0x8fb339, 0.75),
        (235, 26, [150, 70, 260, 90], 40, 0xc3dc4a, 0.9),
        (160, 30, [90, 50, 170, 60], 200, 0xe9f06a, 1.0),
    ]
    for (radius, width, dashes, phase, color, alpha) in rings {
        ctx.saveGState()
        ctx.setShadow(offset: .zero, blur: 36, color: rgb(color, 0.9))
        ctx.setStrokeColor(rgb(color, alpha))
        ctx.setLineWidth(width)
        ctx.setLineDash(phase: phase, lengths: dashes)
        ctx.addEllipse(in: CGRect(x: c.x - radius, y: c.y - radius, width: radius * 2, height: radius * 2))
        ctx.strokePath()
        ctx.restoreGState()
    }

    // Core
    radialGradient(ctx, center: c, radius: 130, inner: rgb(0xfffbd0, 1), outer: rgb(0xc3dc4a, 0))
    ctx.setFillColor(rgb(0xfffef0))
    ctx.fillEllipse(in: CGRect(x: c.x - 36, y: c.y - 36, width: 72, height: 72))

    // Sparks thrown off the field, on a fixed spiral so the icon is reproducible.
    for i in 0..<14 {
        let angle = CGFloat(i) * 2.399963 + 0.4  // golden angle
        let radius = 190 + CGFloat(i) * 21
        let p = CGPoint(x: c.x + cos(angle) * radius, y: c.y + sin(angle) * radius)
        let r = CGFloat(11 - i / 2 + (i % 3) * 2)
        ctx.saveGState()
        ctx.setShadow(offset: .zero, blur: 18, color: rgb(0xe9f06a, 0.9))
        ctx.setFillColor(rgb(0xe9f06a, 0.95))
        ctx.fillEllipse(in: CGRect(x: p.x - r, y: p.y - r, width: r * 2, height: r * 2))
        ctx.restoreGState()
    }
    save(ctx, outPath)
}

// The macOS icon grid: an 824 px rounded body centred on the 1024 canvas, with a soft shadow below it.
func composeMac(squarePath: String, outPath: String) {
    let square = loadImage(squarePath)
    let ctx = makeContext(opaque: false)
    let body = CGRect(x: 100, y: 100, width: 824, height: 824)
    let path = CGPath(roundedRect: body, cornerWidth: 185, cornerHeight: 185, transform: nil)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -12), blur: 28, color: CGColor(gray: 0, alpha: 0.5))
    ctx.setFillColor(CGColor(gray: 0, alpha: 1))
    ctx.addPath(path)
    ctx.fillPath()
    ctx.restoreGState()
    ctx.saveGState()
    ctx.addPath(path)
    ctx.clip()
    ctx.draw(square, in: body)
    ctx.restoreGState()
    // A hairline of light along the edge keeps the dark body from melting into a dark Dock.
    ctx.setStrokeColor(CGColor(gray: 1, alpha: 0.12))
    ctx.setLineWidth(2)
    ctx.addPath(CGPath(roundedRect: body.insetBy(dx: 1, dy: 1), cornerWidth: 184, cornerHeight: 184, transform: nil))
    ctx.strokePath()
    save(ctx, outPath)
}

let args = CommandLine.arguments
switch (args.count > 1 ? args[1] : "", args.count) {
case ("logo", 4): composeLogo(logoPath: args[2], outPath: args[3])
case ("fallback", 3): composeFallback(outPath: args[2])
case ("mac", 4): composeMac(squarePath: args[2], outPath: args[3])
default: fail("usage: compose_icon.swift logo <logo.png> <out.png> | fallback <out.png> | mac <square-1024.png> <out.png>")
}
