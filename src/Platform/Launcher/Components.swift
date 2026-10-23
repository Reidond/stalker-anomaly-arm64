// Small shared pieces: the pad glyphs of the hint bar, the toast and the logo.
import SwiftUI
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif

// MARK: pad glyphs and the hint bar

struct PadGlyph: View {
    enum Kind { case cross, circle, square, triangle, dpad, options }
    let kind: Kind
    var size: CGFloat = 22

    private var color: Color {
        switch kind {
        case .cross: Color.blue
        case .circle: Color.red
        case .square: Color.pink
        case .triangle: Color.green
        default: Color.secondary
        }
    }

    var body: some View {
        ZStack {
            switch kind {
            case .dpad:
                Image(systemName: "dpad.fill").font(.system(size: size * 0.95)).foregroundStyle(.secondary)
            case .options:
                RoundedRectangle(cornerRadius: size * 0.16).stroke(Color.secondary, lineWidth: 1.5).frame(width: size * 0.9, height: size * 0.62)
                VStack(spacing: size * 0.08) {
                    ForEach(0..<3, id: \.self) { _ in Capsule().fill(Color.secondary).frame(width: size * 0.4, height: 1.5) }
                }
            default:
                Circle().strokeBorder(color.opacity(0.6), lineWidth: 1.5)
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
            .padding(.horizontal, 7)
            .frame(minWidth: 24, minHeight: 22)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(.quaternary))
    }
}

struct Hint: Identifiable {
    let glyph: PadGlyph.Kind
    let key: String
    let text: String
    var id: String { text }
}

/// What the buttons do on this screen, in the system's bottom bar. It is there while a pad or a keyboard is in use.
struct HintBar: View {
    @EnvironmentObject private var focus: FocusStore
    let hints: [Hint]

    var body: some View {
        if focus.device != .pointer {
            HStack(spacing: 22) {
                ForEach(hints) { hint in
                    HStack(spacing: 7) {
                        if focus.device == .keyboard { KeyCap(text: hint.key) } else { PadGlyph(kind: hint.glyph) }
                        Text(hint.text).font(.footnote).foregroundStyle(.secondary)
                    }
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 10)
            .frame(maxWidth: .infinity)
            .background(.bar)
            .transition(.move(edge: .bottom).combined(with: .opacity))
        }
    }
}

struct ToastView: View {
    let toast: Toast

    var body: some View {
        Label(toast.text, systemImage: toast.isError ? "exclamationmark.triangle.fill" : "checkmark.circle.fill")
            .font(.callout)
            .symbolRenderingMode(.multicolor)
            .padding(.horizontal, 18)
            .padding(.vertical, 12)
            .background(.regularMaterial, in: Capsule())
            .shadow(color: .black.opacity(0.18), radius: 12, y: 4)
    }
}

// MARK: the logo

/// The game's logo. The app bundle carries the generated app icon (tools/make_app_icon.sh: the Anomaly launcher's logo when the
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
                        Circle().strokeBorder(Theme.accent.opacity(0.9 - Double(ring) * 0.25), lineWidth: size * 0.02)
                            .frame(width: size * (0.8 - CGFloat(ring) * 0.2))
                    }
                }
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.2237, style: .continuous))
        .shadow(color: .black.opacity(0.25), radius: size * 0.08, y: size * 0.03)
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
