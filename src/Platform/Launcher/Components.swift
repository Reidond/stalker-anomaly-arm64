// The launcher's building blocks: focus styling, buttons, the pad glyphs of the hint bar, banners, the progress bar.
import SwiftUI
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif

// MARK: focus styling

extension View {
    /// The card look with the amber focus ring that moves with the D-pad.
    func consoleCard(focused: Bool, radius: CGFloat = Theme.radius, tint: Color? = nil, dimmed: Bool = false) -> some View {
        self
            .background(
                RoundedRectangle(cornerRadius: radius, style: .continuous)
                    .fill(focused ? Theme.panelRaised : Theme.panel)
            )
            .overlay(
                RoundedRectangle(cornerRadius: radius, style: .continuous)
                    .strokeBorder(focused ? (tint ?? Theme.accent) : Theme.line, lineWidth: focused ? 2.5 : 1)
            )
            .shadow(color: focused ? (tint ?? Theme.accent).opacity(0.30) : .clear, radius: 16)
            .scaleEffect(focused ? 1.012 : 1)
            .opacity(dimmed ? 0.45 : 1)
            .animation(.easeOut(duration: 0.12), value: focused)
    }
}

// MARK: pad glyphs and the hint bar

struct PadGlyph: View {
    enum Kind { case cross, circle, square, triangle, dpad, options, stick }
    let kind: Kind
    var size: CGFloat = 24

    private var color: Color {
        switch kind {
        case .cross: Color(red: 0.45, green: 0.66, blue: 1.0)
        case .circle: Color(red: 1.0, green: 0.42, blue: 0.42)
        case .square: Color(red: 0.93, green: 0.56, blue: 0.82)
        case .triangle: Color(red: 0.42, green: 0.84, blue: 0.62)
        default: Theme.dim
        }
    }

    var body: some View {
        ZStack {
            switch kind {
            case .dpad:
                Image(systemName: "dpad.fill").font(.system(size: size * 0.95)).foregroundStyle(Theme.dim)
            case .stick:
                Image(systemName: "l.joystick.fill").font(.system(size: size * 0.95)).foregroundStyle(Theme.dim)
            case .options:
                RoundedRectangle(cornerRadius: size * 0.16).stroke(Theme.dim, lineWidth: 1.6).frame(width: size * 0.9, height: size * 0.62)
                VStack(spacing: size * 0.08) {
                    ForEach(0..<3, id: \.self) { _ in Capsule().fill(Theme.dim).frame(width: size * 0.4, height: 1.6) }
                }
            default:
                Circle().strokeBorder(color.opacity(0.55), lineWidth: 1.6)
                symbol.stroke(color, style: StrokeStyle(lineWidth: 2.2, lineCap: .round, lineJoin: .round)).padding(size * 0.28)
            }
        }
        .frame(width: size, height: size)
    }

    private var symbol: some Shape {
        GlyphShape(kind: kind)
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
            .font(.system(size: 13, weight: .semibold, design: .rounded))
            .foregroundStyle(Theme.text)
            .padding(.horizontal, 7)
            .frame(minWidth: 24, minHeight: 24)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(Theme.panelRaised))
            .overlay(RoundedRectangle(cornerRadius: 6, style: .continuous).strokeBorder(Theme.line, lineWidth: 1))
    }
}

struct Hint: Identifiable {
    let glyph: PadGlyph.Kind
    let key: String
    let text: String
    var id: String { text }
}

struct HintBar: View {
    @EnvironmentObject private var focus: FocusStore
    let hints: [Hint]

    var body: some View {
        HStack(spacing: 26) {
            ForEach(hints) { hint in
                HStack(spacing: 8) {
                    if focus.device == .keyboard { KeyCap(text: hint.key) } else { PadGlyph(kind: hint.glyph) }
                    Text(hint.text).font(Theme.small(15)).foregroundStyle(Theme.dim)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, Theme.gutter)
        .frame(height: 44)
        .opacity(focus.device == .touch ? 0 : 1)
        .animation(.easeOut(duration: 0.15), value: focus.device)
    }
}

// MARK: controls

struct ConsoleSwitch: View {
    let isOn: Bool
    var enabled = true

    var body: some View {
        ZStack(alignment: isOn ? .trailing : .leading) {
            Capsule().fill(isOn ? Theme.accent : Color.white.opacity(0.12))
            Circle().fill(isOn ? Color.black.opacity(0.85) : Theme.dim).padding(3)
        }
        .frame(width: 58, height: 32)
        .opacity(enabled ? 1 : 0.4)
        .animation(.easeOut(duration: 0.14), value: isOn)
    }
}

struct Chip: View {
    let text: String
    var color: Color = Theme.dim
    var systemImage: String?

    var body: some View {
        HStack(spacing: 5) {
            if let systemImage { Image(systemName: systemImage).font(.system(size: 12, weight: .bold)) }
            Text(text).font(Theme.small(13))
        }
        .foregroundStyle(color)
        .padding(.horizontal, 9)
        .padding(.vertical, 4)
        .background(Capsule().fill(color.opacity(0.14)))
    }
}

/// A button for the pad: a row in the focus order, with a title, an optional line below and a trailing view.
struct ConsoleButton<Trailing: View>: View {
    let id: String
    let title: String
    var subtitle: String?
    var systemImage: String?
    var prominent = false
    var enabled = true
    var isDefault = false
    var destructive = false
    var action: () -> Void
    @ViewBuilder var trailing: () -> Trailing

    var body: some View {
        LauncherFocusable(id: id, isDefault: isDefault, enabled: enabled, onActivate: action) { focused in
            HStack(spacing: 16) {
                if let systemImage {
                    Image(systemName: systemImage)
                        .font(.system(size: prominent ? 30 : 22, weight: .semibold))
                        .frame(width: prominent ? 44 : 34)
                        .foregroundStyle(destructive ? Theme.bad : (prominent ? Color.black.opacity(0.85) : Theme.accent))
                }
                VStack(alignment: .leading, spacing: 3) {
                    Text(title.uppercased())
                        .font(prominent ? Theme.title(34) : Theme.heading(21))
                        .tracking(prominent ? 3 : 1.5)
                        .foregroundStyle(prominent ? Color.black.opacity(0.88) : (destructive ? Theme.bad : Theme.text))
                    if let subtitle {
                        Text(subtitle)
                            .font(Theme.small(16))
                            .foregroundStyle(prominent ? Color.black.opacity(0.62) : Theme.dim)
                            .lineLimit(2)
                    }
                }
                Spacer(minLength: 8)
                trailing()
            }
            .padding(.horizontal, 22)
            .padding(.vertical, prominent ? 24 : 16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(prominent ? RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).fill(Theme.accent.opacity(focused ? 1 : 0.88)) : nil)
            .consoleCard(focused: focused, tint: destructive ? Theme.bad : nil, dimmed: !enabled)
        }
    }
}

extension ConsoleButton where Trailing == EmptyView {
    init(id: String, title: String, subtitle: String? = nil, systemImage: String? = nil, prominent: Bool = false, enabled: Bool = true,
         isDefault: Bool = false, destructive: Bool = false, action: @escaping () -> Void) {
        self.init(id: id, title: title, subtitle: subtitle, systemImage: systemImage, prominent: prominent, enabled: enabled,
                  isDefault: isDefault, destructive: destructive, action: action, trailing: { EmptyView() })
    }
}

struct Banner: View {
    enum Style { case info, warning, error, success }
    let style: Style
    let title: String
    var message: String?
    var monospaced = false

    private var color: Color {
        switch style {
        case .info: Theme.accent
        case .warning: Theme.warn
        case .error: Theme.bad
        case .success: Theme.good
        }
    }

    private var symbol: String {
        switch style {
        case .info: "info.circle.fill"
        case .warning: "exclamationmark.triangle.fill"
        case .error: "xmark.octagon.fill"
        case .success: "checkmark.circle.fill"
        }
    }

    var body: some View {
        HStack(alignment: .top, spacing: 14) {
            Image(systemName: symbol).font(.system(size: 22)).foregroundStyle(color)
            VStack(alignment: .leading, spacing: 5) {
                Text(title).font(Theme.heading(18)).foregroundStyle(Theme.text)
                if let message, !message.isEmpty {
                    Text(message)
                        .font(monospaced ? Theme.mono(13) : Theme.body(16))
                        .foregroundStyle(Theme.dim)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).fill(color.opacity(0.10)))
        .overlay(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).strokeBorder(color.opacity(0.45), lineWidth: 1))
    }
}

struct ProgressBar: View {
    let fraction: Double?

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.white.opacity(0.10))
                if let fraction {
                    Capsule().fill(Theme.accent).frame(width: max(10, proxy.size.width * min(max(fraction, 0), 1)))
                } else {
                    IndeterminateBar(width: proxy.size.width)
                }
            }
        }
        .frame(height: 10)
        .clipShape(Capsule())
    }
}

private struct IndeterminateBar: View {
    let width: CGFloat
    @State private var phase = false

    var body: some View {
        Capsule().fill(Theme.accent)
            .frame(width: width * 0.3)
            .offset(x: phase ? width * 0.7 : 0)
            .onAppear { withAnimation(.easeInOut(duration: 0.9).repeatForever(autoreverses: true)) { phase = true } }
    }
}

struct ToastView: View {
    let toast: Toast

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: toast.isError ? "exclamationmark.triangle.fill" : "checkmark.circle.fill")
                .foregroundStyle(toast.isError ? Theme.bad : Theme.good)
            Text(toast.text).font(Theme.body(16)).foregroundStyle(Theme.text).lineLimit(3)
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 12)
        .background(Capsule().fill(Theme.panelRaised))
        .overlay(Capsule().strokeBorder(Theme.line, lineWidth: 1))
        .shadow(color: .black.opacity(0.5), radius: 14, y: 6)
    }
}

// MARK: the logo

/// The game's logo. The app bundle carries the generated app icon (tools/make_app_icon.sh: the Anomaly launcher's logo when the
/// game data was there at build time, the repository's own fallback artwork otherwise); a mark drawn here stands in when it is not.
struct LogoView: View {
    let path: String?
    var size: CGFloat = 220

    var body: some View {
        if let image = LogoImage.load(path: path) {
            image
                .resizable()
                .interpolation(.high)
                .aspectRatio(contentMode: .fit)
                .frame(width: size, height: size)
                .clipShape(RoundedRectangle(cornerRadius: size * 0.2237, style: .continuous))
                .shadow(color: Theme.accent.opacity(0.25), radius: 30)
        } else {
            ZStack {
                ForEach(0..<3, id: \.self) { ring in
                    Circle().strokeBorder(Theme.accent.opacity(0.9 - Double(ring) * 0.25), lineWidth: 3)
                        .frame(width: size * (1.0 - CGFloat(ring) * 0.26))
                }
                Text("A").font(.system(size: size * 0.34, weight: .black)).foregroundStyle(Theme.accent)
            }
            .frame(width: size, height: size)
        }
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
