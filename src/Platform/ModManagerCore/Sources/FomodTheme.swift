import SwiftUI

// The wizard's look, injectable: colours and the backdrop come from the environment, so the launcher can hand over its own.
//
//     FomodWizardView(...).fomodTheme(myTheme)
//
// Material: Liquid Glass (`glassEffect`, `GlassEffectContainer`, `.glass` button styles) on iOS 26 / macOS 26 and later, an
// ultra-thin material with a hairline outline before that, and a solid panel when Reduce Transparency is on.

/// Colours, radii and the backdrop of the FOMOD wizard. `.anomaly` is the default: near-black with amber, rust and, sparingly,
/// radiation yellow-green.
public struct FomodTheme {
    /// Selection, the cursor ring, the primary button. Amber.
    public var accent: Color
    /// Hairlines, locked and cautionary marks. Rust. Decorative or paired with text, never the only carrier of text.
    public var rust: Color
    /// Radiation yellow-green: only the "Recommended" mark.
    public var signal: Color
    public var text: Color
    public var dimText: Color
    public var danger: Color
    /// Panel colour when there is no transparency (Reduce Transparency).
    public var solid: Color
    public var solidRaised: Color
    public var line: Color
    /// Drawn behind everything; glass needs something to sample. Nil: the wizard draws nothing and what is behind it shows.
    public var backdrop: AnyView?
    /// Radius of the large surfaces (header, detail card). Rows and images inside are concentric: radius minus padding.
    public var cornerRadius: CGFloat

    public init(accent: Color, rust: Color, signal: Color, text: Color, dimText: Color, danger: Color, solid: Color,
                solidRaised: Color, line: Color, backdrop: AnyView?, cornerRadius: CGFloat = 28) {
        self.accent = accent
        self.rust = rust
        self.signal = signal
        self.text = text
        self.dimText = dimText
        self.danger = danger
        self.solid = solid
        self.solidRaised = solidRaised
        self.line = line
        self.backdrop = backdrop
        self.cornerRadius = cornerRadius
    }

    public static let anomaly = FomodTheme(
        accent: Color(red: 0.91, green: 0.66, blue: 0.22),
        rust: Color(red: 0.72, green: 0.31, blue: 0.14),
        signal: Color(red: 0.74, green: 0.92, blue: 0.22),
        text: Color(red: 0.92, green: 0.92, blue: 0.90),
        dimText: Color(red: 0.66, green: 0.68, blue: 0.65),
        danger: Color(red: 0.96, green: 0.45, blue: 0.40),
        solid: Color(red: 0.093, green: 0.098, blue: 0.088),
        solidRaised: Color(red: 0.145, green: 0.150, blue: 0.135),
        line: Color.white.opacity(0.10),
        backdrop: AnyView(FomodBackdrop()))
}

/// Near black, a warm amber glow from the top left and a rust one from the bottom right, like the game's menu.
public struct FomodBackdrop: View {
    public init() {}

    public var body: some View {
        ZStack {
            Color(red: 0.047, green: 0.051, blue: 0.045)
            RadialGradient(colors: [Color(red: 0.91, green: 0.66, blue: 0.22).opacity(0.22), .clear], center: .topLeading,
                           startRadius: 0, endRadius: 620)
            RadialGradient(colors: [Color(red: 0.72, green: 0.31, blue: 0.14).opacity(0.26), .clear], center: .bottomTrailing,
                           startRadius: 0, endRadius: 560)
        }
        .ignoresSafeArea()
    }
}

struct FomodThemeKey: EnvironmentKey { static let defaultValue = FomodTheme.anomaly }

extension EnvironmentValues {
    public var fomodTheme: FomodTheme {
        get { self[FomodThemeKey.self] }
        set { self[FomodThemeKey.self] = newValue }
    }
}

extension View {
    /// Supplies the colours and backdrop the FOMOD wizard below this view uses.
    public func fomodTheme(_ theme: FomodTheme) -> some View { environment(\.fomodTheme, theme) }
}

// MARK: - Accessibility settings

/// The system's Reduce Transparency and Reduce Motion, which the tests (and a caller with its own setting) can override.
struct FomodReduceTransparencyOverrideKey: EnvironmentKey { static let defaultValue: Bool? = nil }
struct FomodReduceMotionOverrideKey: EnvironmentKey { static let defaultValue: Bool? = nil }
/// Forces the material fallback on systems that have Liquid Glass (offscreen renders cannot draw glass at all).
struct FomodGlassDisabledKey: EnvironmentKey { static let defaultValue = false }

extension EnvironmentValues {
    var fomodReduceTransparencyOverride: Bool? {
        get { self[FomodReduceTransparencyOverrideKey.self] }
        set { self[FomodReduceTransparencyOverrideKey.self] = newValue }
    }

    var fomodReduceMotionOverride: Bool? {
        get { self[FomodReduceMotionOverrideKey.self] }
        set { self[FomodReduceMotionOverrideKey.self] = newValue }
    }

    var fomodGlassDisabled: Bool {
        get { self[FomodGlassDisabledKey.self] }
        set { self[FomodGlassDisabledKey.self] = newValue }
    }

    var fomodReduceTransparency: Bool { fomodReduceTransparencyOverride ?? accessibilityReduceTransparency }
    var fomodReduceMotion: Bool { fomodReduceMotionOverride ?? accessibilityReduceMotion }
}

// MARK: - Material

/// Glass, material or solid for one surface, by what the system offers and the player allows.
struct FomodSurface<S: InsettableShape>: ViewModifier {
    @Environment(\.fomodTheme) private var theme
    @Environment(\.fomodReduceTransparency) private var reduceTransparency
    @Environment(\.fomodGlassDisabled) private var glassDisabled

    let shape: S
    var tint: Color?
    var tintStrength = 0.30
    var interactive = false
    var glassID: String?
    var namespace: Namespace.ID?

    func body(content: Content) -> some View {
        if reduceTransparency {
            // later .background calls sit behind earlier ones: the tint first, the panel colour behind it
            content
                .background(shape.fill((tint ?? .clear).opacity(tint == nil ? 0 : tintStrength)))
                .background(shape.fill(theme.solidRaised))
                .overlay(shape.strokeBorder(theme.line, lineWidth: 1))
        } else if #available(iOS 26, macOS 26, *), !glassDisabled {
            glass(content)
        } else {
            content
                .background(shape.fill((tint ?? .clear).opacity(tint == nil ? 0 : tintStrength)))
                .background(shape.fill(.ultraThinMaterial))
                .overlay(shape.strokeBorder(Color.white.opacity(0.12), lineWidth: 1))
        }
    }

    @available(iOS 26, macOS 26, *)
    @ViewBuilder
    private func glass(_ content: Content) -> some View {
        let style: Glass = {
            var g = Glass.regular
            if let tint { g = g.tint(tint.opacity(tintStrength)) }
            if interactive { g = g.interactive() }
            return g
        }()
        if let namespace, let glassID {
            content.glassEffect(style, in: shape).glassEffectID(glassID, in: namespace)
        } else {
            content.glassEffect(style, in: shape)
        }
    }
}

extension View {
    func fomodSurface<S: InsettableShape>(_ shape: S, tint: Color? = nil, strength: Double = 0.30, interactive: Bool = false,
                                id: String? = nil, in namespace: Namespace.ID? = nil) -> some View {
        modifier(FomodSurface(shape: shape, tint: tint, tintStrength: strength, interactive: interactive, glassID: id,
                              namespace: namespace))
    }
}

/// Groups glass shapes so they blend and morph together (iOS 26 / macOS 26); a plain group otherwise.
struct FomodGlassGroup<Content: View>: View {
    @Environment(\.fomodReduceTransparency) private var reduceTransparency
    @Environment(\.fomodGlassDisabled) private var glassDisabled
    var spacing: CGFloat = 8
    @ViewBuilder var content: () -> Content

    var body: some View {
        if !reduceTransparency, !glassDisabled, #available(iOS 26, macOS 26, *) {
            GlassEffectContainer(spacing: spacing) { content() }
        } else {
            content()
        }
    }
}

/// False while the first page is on screen (it is there, not an entrance), true from the first step change on.
struct FomodAnimateEntranceKey: EnvironmentKey { static let defaultValue = false }

extension EnvironmentValues {
    var fomodAnimateEntrance: Bool {
        get { self[FomodAnimateEntranceKey.self] }
        set { self[FomodAnimateEntranceKey.self] = newValue }
    }
}

/// True when surfaces are real Liquid Glass.
struct FomodGlassActiveKey: EnvironmentKey { static let defaultValue = false }

extension EnvironmentValues {
    var fomodGlassActive: Bool {
        get { self[FomodGlassActiveKey.self] }
        set { self[FomodGlassActiveKey.self] = newValue }
    }
}

// MARK: - Motion

/// The wizard's motion: short springs without bounce, fades only when Reduce Motion is on.
struct FomodMotion {
    var reduce: Bool

    /// Steps, groups, selection: under 300 ms.
    var spring: Animation { reduce ? .easeOut(duration: 0.15) : .spring(duration: 0.3, bounce: 0) }
    /// Hover, detail text: the player is moving fast, so only opacity and quickly.
    var quick: Animation { .easeOut(duration: 0.12) }
    /// Press feedback.
    var press: Animation { .easeOut(duration: 0.14) }

    func stagger(_ index: Int) -> Animation { reduce ? quick : spring.delay(0.06 * Double(min(index, 4))) }
}

/// An icon that appears with scale 0.25 to 1, opacity 0 to 1 and blur 4 to 0 (opacity only with Reduce Motion).
struct FomodIconPop: ViewModifier {
    var progress: Double
    var reduce: Bool

    func body(content: Content) -> some View {
        content
            .scaleEffect(reduce ? 1 : 0.25 + 0.75 * progress)
            .opacity(progress)
            .blur(radius: reduce ? 0 : 4 * (1 - progress))
    }
}

extension AnyTransition {
    static func fomodIcon(reduce: Bool) -> AnyTransition {
        .modifier(active: FomodIconPop(progress: 0, reduce: reduce), identity: FomodIconPop(progress: 1, reduce: reduce))
    }
}

/// Scale 0.96 while pressed, with the ease-out the rest of the wizard uses. Real Liquid Glass brings its own press response.
struct FomodPressStyle: ButtonStyle {
    @Environment(\.fomodReduceMotion) private var reduceMotion
    @Environment(\.fomodGlassActive) private var glassActive

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed && !reduceMotion && !glassActive ? 0.96 : 1)
            .opacity(configuration.isPressed && (reduceMotion || glassActive) ? 0.85 : 1)
            .animation(FomodMotion(reduce: reduceMotion).press, value: configuration.isPressed)
    }
}
