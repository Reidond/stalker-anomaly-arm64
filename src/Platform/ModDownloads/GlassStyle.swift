import SwiftUI

// Liquid Glass where the system has it (iOS 26 / macOS 26), a material below that, opaque surfaces when the user turns
// Reduce Transparency on. Motion is a quick spring with no bounce, an exit that is faster than the enter, and nothing
// that moves when Reduce Motion is on.

enum Motion {
    /// State changes the user asked for (a panel opening, a chip appearing).
    static let spring = Animation.spring(duration: 0.3, bounce: 0)
    /// Press feedback and small swaps.
    static let quick = Animation.spring(duration: 0.18, bounce: 0)
    /// Exits are softer and faster than enters.
    static let exit = Animation.easeOut(duration: 0.15)
    /// What stays when Reduce Motion is on: a short fade, no movement.
    static let fade = Animation.easeOut(duration: 0.12)

    static func animation(_ animation: Animation, reduceMotion: Bool) -> Animation {
        reduceMotion ? fade : animation
    }
}

var liquidGlassAvailable: Bool {
    if #available(iOS 26, macOS 26, *) { return true }
    return false
}

// MARK: - Surfaces

struct ChromeSurface<S: InsettableShape>: ViewModifier {
    let shape: S
    var tint: Color?
    var interactive: Bool
    var raised: Bool

    @Environment(\.downloadsTheme) private var theme
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.snapshotMode) private var snapshotMode

    func body(content: Content) -> some View {
        if reduceTransparency || !theme.prefersGlass {
            content
                .background(shape.fill(raised ? theme.surfaceRaised : theme.surface))
                .background { if let tint { shape.fill(tint) } }
                .overlay(shape.strokeBorder(theme.outline, lineWidth: 1))
        } else if snapshotMode {
            // ImageRenderer has no backdrop to refract: a stand-in with the same shape language (a lit edge, a soft shadow)
            content
                .background { if raised { shape.fill(theme.surfaceRaised.opacity(0.9)) } }
                .background(shape.fill(LinearGradient(colors: [(tint ?? .white).opacity(tint == nil ? (raised ? 0.09 : 0.17) : 0.55), (tint ?? .white).opacity(tint == nil ? (raised ? 0.03 : 0.07) : 0.35)],
                                                      startPoint: .top, endPoint: .bottom)))
                .overlay(shape.strokeBorder(LinearGradient(colors: [.white.opacity(0.32), .white.opacity(0.07)], startPoint: .top, endPoint: .bottom), lineWidth: 1))
                .shadow(color: .black.opacity(0.35), radius: 10, y: 4)
        } else if #available(iOS 26, macOS 26, *) {
            content.glassEffect(Self.glass(tint: tint, interactive: interactive), in: shape)
        } else {
            content
                .background(shape.fill(tint.map { AnyShapeStyle($0.opacity(0.85)) } ?? AnyShapeStyle(.ultraThinMaterial)))
                .overlay(shape.strokeBorder(theme.outline, lineWidth: 1))
        }
    }

    @available(iOS 26, macOS 26, *)
    private static func glass(tint: Color?, interactive: Bool) -> Glass {
        var glass = Glass.regular
        if let tint { glass = glass.tint(tint) }
        if interactive { glass = glass.interactive() }
        return glass
    }
}

extension View {
    func chromeSurface<S: InsettableShape>(_ shape: S, tint: Color? = nil, interactive: Bool = false, raised: Bool = false) -> some View {
        modifier(ChromeSurface(shape: shape, tint: tint, interactive: interactive, raised: raised))
    }
}

/// Glass shapes inside blend and morph into each other when they come within `spacing` of one another.
struct ChromeContainer<Content: View>: View {
    var spacing: CGFloat = 6
    @ViewBuilder var content: Content

    var body: some View {
        if #available(iOS 26, macOS 26, *) {
            GlassEffectContainer(spacing: spacing) { content }
        } else {
            content
        }
    }
}

// MARK: - Focus

/// A ring that cannot be missed from across a room: 3 pt amber, a soft glow, a little growth. Focus moves with a controller or a
/// keyboard, which repeat all day, so it changes instantly.
struct FocusRing<S: InsettableShape>: ViewModifier {
    let shape: S
    var forced: Bool?
    @Environment(\.isFocused) private var focused
    @Environment(\.downloadsTheme) private var theme

    func body(content: Content) -> some View {
        let on = forced ?? focused
        content
            .overlay(shape.strokeBorder(theme.highlight, lineWidth: on ? 3 : 0))
            .shadow(color: theme.highlight.opacity(on ? 0.55 : 0), radius: on ? 7 : 0)
            .scaleEffect(on ? 1.03 : 1)
    }
}

extension View {
    func focusRing<S: InsettableShape>(_ shape: S, forced: Bool? = nil) -> some View {
        modifier(FocusRing(shape: shape, forced: forced))
    }

    /// Reachable by keyboard and controller focus; the pointer gets the system's hover effect.
    func focusableControl() -> some View {
        #if os(iOS)
        self.focusable(true).hoverEffect(.highlight)
        #else
        self.focusable(true)
        #endif
    }
}

// MARK: - Buttons

enum ChromeButtonKind {
    /// On its own glass.
    case glass
    /// Rust-tinted glass: the one thing to press (Go, Retry, Allow).
    case prominent
    /// No surface of its own: lives inside a glass cluster.
    case bare
}

struct ChromeButtonStyle: ButtonStyle {
    var kind: ChromeButtonKind = .glass
    var minWidth: CGFloat = 44

    func makeBody(configuration: Configuration) -> some View {
        ChromeButtonBody(configuration: configuration, kind: kind, minWidth: minWidth)
    }
}

private struct ChromeButtonBody: View {
    let configuration: ButtonStyleConfiguration
    let kind: ChromeButtonKind
    let minWidth: CGFloat
    @Environment(\.downloadsTheme) private var theme
    @Environment(\.isEnabled) private var enabled
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        let shape = Capsule()
        let pressed = configuration.isPressed
        configuration.label
            .font(.callout.weight(.medium))
            .foregroundStyle(kind == .prominent ? theme.onAccent : theme.text)
            .padding(.horizontal, kind == .bare ? 0 : 14)
            .frame(minWidth: minWidth, minHeight: 44)
            .background { if kind == .bare { shape.fill(Color.white.opacity(pressed ? 0.16 : 0)) } }
            .modifier(OptionalSurface(kind: kind, shape: shape))
            .focusRing(shape)
            // the system's interactive glass already reacts to touch; everything else gets the same 0.96 press
            .scaleEffect(pressed && !reduceMotion && !(liquidGlassAvailable && kind != .bare && theme.prefersGlass) ? 0.96 : 1)
            .animation(Motion.quick, value: pressed)
            .opacity(enabled ? 1 : 0.4)
            .contentShape(shape)
    }
}

private struct OptionalSurface: ViewModifier {
    let kind: ChromeButtonKind
    let shape: Capsule

    func body(content: Content) -> some View {
        switch kind {
        case .bare: content
        case .glass: content.chromeSurface(shape, interactive: true)
        case .prominent: content.modifier(ProminentSurface(shape: shape))
        }
    }
}

private struct ProminentSurface: ViewModifier {
    let shape: Capsule
    @Environment(\.downloadsTheme) private var theme

    func body(content: Content) -> some View {
        content.chromeSurface(shape, tint: theme.accent, interactive: true)
    }
}

// MARK: - Transitions

/// Opacity plus a small fixed slide (never the whole height): enters ease out, exits are quicker.
struct OffsetFade: ViewModifier {
    var x: CGFloat = 0
    var y: CGFloat = 0
    var opacity: Double
    var scale: CGFloat = 1

    func body(content: Content) -> some View {
        content.opacity(opacity).offset(x: x, y: y).scaleEffect(scale)
    }
}

extension AnyTransition {
    static func slideFade(x: CGFloat = 0, y: CGFloat = 0, scale: CGFloat = 1, reduceMotion: Bool) -> AnyTransition {
        if reduceMotion { return .opacity }
        return .asymmetric(
            insertion: .modifier(active: OffsetFade(x: x, y: y, opacity: 0, scale: scale), identity: OffsetFade(opacity: 1)),
            removal: .modifier(active: OffsetFade(x: x / 2, y: y / 2, opacity: 0), identity: OffsetFade(opacity: 1)).animation(Motion.exit)
        )
    }
}

/// An icon that swaps in with scale 0.25 -> 1, opacity 0 -> 1 and blur 4 -> 0.
struct ScaleBlur: ViewModifier {
    var scale: CGFloat
    var opacity: Double
    var blur: CGFloat

    func body(content: Content) -> some View {
        content.scaleEffect(scale).opacity(opacity).blur(radius: blur)
    }
}

extension AnyTransition {
    static func iconSwap(reduceMotion: Bool) -> AnyTransition {
        if reduceMotion { return .opacity }
        return .modifier(active: ScaleBlur(scale: 0.25, opacity: 0, blur: 4), identity: ScaleBlur(scale: 1, opacity: 1, blur: 0))
    }
}

// MARK: - Progress

/// A calm progress track: the fill follows the bytes with a short spring, and while it runs a faint highlight drifts
/// across it (2.4 s a pass). Unknown size is a soft segment sliding to and fro. Reduce Motion: no drift, no slide.
struct ProgressTrack: View {
    let fraction: Double?
    let running: Bool

    @Environment(\.downloadsTheme) private var theme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.snapshotMode) private var snapshotMode

    var body: some View {
        GeometryReader { geo in
            let width = geo.size.width
            ZStack(alignment: .leading) {
                Capsule().fill(Color.white.opacity(0.10))
                if let fraction {
                    fill(width: max(6, width * fraction), total: width)
                } else {
                    unknownSize(width: width)
                }
            }
            .clipShape(Capsule())
        }
        .frame(height: 4)
        .accessibilityElement()
        .accessibilityValue(fraction.map { "\(Int($0 * 100)) percent" } ?? "in progress")
    }

    private func fill(width: CGFloat, total: CGFloat) -> some View {
        Capsule()
            .fill(LinearGradient(colors: [theme.highlight.opacity(0.8), theme.highlight], startPoint: .leading, endPoint: .trailing))
            .frame(width: width)
            .overlay { if running && !reduceMotion && !snapshotMode { drift(width: width) } }
            .clipShape(Capsule())
            .animation(reduceMotion ? nil : Motion.spring, value: width)
    }

    private func drift(width: CGFloat) -> some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { context in
            let phase = context.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 2.4) / 2.4
            let band = max(24, width * 0.35)
            LinearGradient(colors: [.clear, .white.opacity(0.28), .clear], startPoint: .leading, endPoint: .trailing)
                .frame(width: band)
                .offset(x: CGFloat(phase) * (width + band) - band)
                .frame(width: width, alignment: .leading)
        }
        .allowsHitTesting(false)
    }

    @ViewBuilder
    private func unknownSize(width: CGFloat) -> some View {
        if reduceMotion || snapshotMode {
            Capsule().fill(theme.highlight.opacity(0.5)).frame(width: width * (snapshotMode ? 0.35 : 1))
        } else {
            TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { context in
                let t = context.date.timeIntervalSinceReferenceDate
                let phase = (sin(t * 2 * .pi / 1.6) + 1) / 2
                let segment = width * 0.3
                Capsule().fill(theme.highlight)
                    .frame(width: segment)
                    .offset(x: CGFloat(phase) * (width - segment))
                    .frame(width: width, alignment: .leading)
            }
        }
    }
}
