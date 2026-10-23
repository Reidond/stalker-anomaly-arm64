// Small shared pieces: the pad glyphs of the hint bar, the toast, the logo and the type styles.
import SwiftUI
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif

// MARK: type

extension View {
    /// Large type that holds on glass at the iPad's distance: white, a touch of shadow against bright spots of the backdrop.
    func launcherTitle(_ size: CGFloat = 40) -> some View {
        font(.system(size: size, weight: .bold)).tracking(size > 30 ? 0.4 : 0).foregroundStyle(Theme.primaryText)
            .shadow(color: .black.opacity(0.35), radius: 6, y: 1)
    }
}

// MARK: pad glyphs and the hint bar

struct PadGlyph: View {
    enum Kind { case cross, circle, square, triangle, dpad, options }
    let kind: Kind
    var size: CGFloat = 24

    private var color: Color {
        switch kind {
        case .cross: Color(red: 0.45, green: 0.66, blue: 1.0)
        case .circle: Color(red: 1.0, green: 0.42, blue: 0.42)
        case .square: Color(red: 0.93, green: 0.56, blue: 0.82)
        case .triangle: Color(red: 0.42, green: 0.84, blue: 0.62)
        default: Theme.secondaryText
        }
    }

    var body: some View {
        ZStack {
            switch kind {
            case .dpad:
                Image(systemName: "dpad.fill").font(.system(size: size * 0.95)).foregroundStyle(Theme.secondaryText)
            case .options:
                RoundedRectangle(cornerRadius: size * 0.16).stroke(Theme.secondaryText, lineWidth: 1.5).frame(width: size * 0.9, height: size * 0.62)
                VStack(spacing: size * 0.08) {
                    ForEach(0..<3, id: \.self) { _ in Capsule().fill(Theme.secondaryText).frame(width: size * 0.4, height: 1.5) }
                }
            default:
                Circle().strokeBorder(color.opacity(0.7), lineWidth: 1.5)
                GlyphShape(kind: kind).stroke(color, style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round)).padding(size * 0.28)
            }
        }
        .frame(width: size, height: size)
    }
}

private struct GlyphShape: Shape {
    let kind: PadGlyph.Kind

    func path(in rect: CGRect) -> Path {
        var path = Path()
        switch kind {
        case .cross:
            path.move(to: CGPoint(x: rect.minX, y: rect.minY)); path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
            path.move(to: CGPoint(x: rect.maxX, y: rect.minY)); path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY))
        case .circle:
            path.addEllipse(in: rect.insetBy(dx: rect.width * 0.04, dy: rect.height * 0.04))
        case .square:
            path.addRect(rect.insetBy(dx: rect.width * 0.04, dy: rect.height * 0.04))
        case .triangle:
            path.move(to: CGPoint(x: rect.midX, y: rect.minY))
            path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
            path.addLine(to: CGPoint(x: rect.minX, y: rect.maxY))
            path.closeSubpath()
        default: break
        }
        return path
    }
}

struct KeyCap: View {
    let text: String

    var body: some View {
        Text(text)
            .font(.footnote.weight(.semibold))
            .foregroundStyle(Theme.primaryText)
            .padding(.horizontal, 8)
            .frame(minWidth: 26, minHeight: 24)
            .background(RoundedRectangle(cornerRadius: 7, style: .continuous).fill(Color.white.opacity(0.14)))
    }
}

struct Hint: Identifiable {
    let glyph: PadGlyph.Kind
    let key: String
    let text: String
    var id: String { text }
}

/// What the buttons do on this screen: a glass capsule at the bottom, there while a pad or a keyboard is in use.
struct HintBar: View {
    @EnvironmentObject private var focus: FocusStore
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let hints: [Hint]

    var body: some View {
        if focus.device != .pointer {
            HStack(spacing: 22) {
                ForEach(hints) { hint in
                    HStack(spacing: 8) {
                        if focus.device == .keyboard { KeyCap(text: hint.key) } else { PadGlyph(kind: hint.glyph) }
                        Text(hint.text).font(.callout.weight(.medium)).foregroundStyle(Theme.secondaryText)
                    }
                }
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 12)
            .glassCapsule()
            .padding(.bottom, 12)
            .transition(reduceMotion ? .opacity : .move(edge: .bottom).combined(with: .opacity))
        }
    }
}

struct ToastView: View {
    let toast: Toast

    var body: some View {
        Label(toast.text, systemImage: toast.isError ? "exclamationmark.triangle.fill" : "checkmark.circle.fill")
            .font(.callout.weight(.medium))
            .foregroundStyle(Theme.primaryText)
            .symbolRenderingMode(.palette)
            .foregroundStyle(Theme.primaryText, toast.isError ? Theme.rust : Theme.radiation)
            .padding(.horizontal, 20)
            .padding(.vertical, 14)
            .glassCapsule(tint: toast.isError ? .rust : .none)
    }
}

// MARK: the logo

/// The game's logo. The app bundle carries the generated app icon (tools/make_app_icon.sh: the logo of the Anomaly launcher when the
/// game data was there at build time, the repository's own fallback artwork otherwise); a mark drawn here stands in when it is not.
struct LogoView: View {
    let path: String?
    var size: CGFloat = 120

    var body: some View {
        Group {
            if let image = LogoImage.load(path: path) {
                image.resizable().interpolation(.high).aspectRatio(contentMode: .fit)
            } else {
                ZStack {
                    Color(white: 0.08)
                    ForEach(0..<3, id: \.self) { ring in
                        Circle().strokeBorder(Theme.amber.opacity(0.9 - Double(ring) * 0.25), lineWidth: size * 0.02)
                            .frame(width: size * (0.8 - CGFloat(ring) * 0.2))
                    }
                }
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.2237, style: .continuous))
        // the outline of an image: white at 10 %, so that it holds its edge against the glass and the dark picture
        .overlay(RoundedRectangle(cornerRadius: size * 0.2237, style: .continuous).strokeBorder(Color.white.opacity(0.10), lineWidth: 1))
        .shadow(color: .black.opacity(0.45), radius: size * 0.1, y: size * 0.04)
        .accessibilityHidden(true)
    }
}

enum LogoImage {
    nonisolated(unsafe) private static var cache: [String: Image] = [:]

    static func load(path: String?) -> Image? {
        let candidates = [path, Bundle.main.path(forResource: "launcher-logo", ofType: "png")].compactMap { $0 }
        for candidate in candidates {
            if let cached = cache[candidate] { return cached }
            #if canImport(UIKit)
            if let image = UIImage(contentsOfFile: candidate) {
                let result = Image(uiImage: image)
                cache[candidate] = result
                return result
            }
            #elseif canImport(AppKit)
            if let image = NSImage(contentsOfFile: candidate) {
                let result = Image(nsImage: image)
                cache[candidate] = result
                return result
            }
            #endif
        }
        return nil
    }
}
