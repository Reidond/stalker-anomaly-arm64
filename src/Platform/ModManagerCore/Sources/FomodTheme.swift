import SwiftUI

// The wizard looks like the system: stock lists, switches, checkmark rows and buttons, with Liquid Glass button styles where the
// system has them (iOS 26 / macOS 26 and later) and the bordered styles before. The only things of its own are an accent colour
// and, if the host wants one, a backdrop.
//
//     FomodWizardView(...).fomodTheme(FomodTheme(accent: .orange, backdrop: nil))

/// What a host can change about the wizard's look.
public struct FomodTheme {
    /// Selection, the cursor ring and the primary button. The launcher's amber by default.
    public var accent: Color
    /// Drawn behind the page, with the list's own background hidden. Nil: the system's grouped background.
    public var backdrop: AnyView?

    public init(accent: Color, backdrop: AnyView? = nil) {
        self.accent = accent
        self.backdrop = backdrop
    }

    /// The amber of the game's menus, as in the launcher.
    public static let anomaly = FomodTheme(accent: Color(red: 0.93, green: 0.58, blue: 0.08))
}

struct FomodThemeKey: EnvironmentKey { static let defaultValue = FomodTheme.anomaly }

extension EnvironmentValues {
    public var fomodTheme: FomodTheme {
        get { self[FomodThemeKey.self] }
        set { self[FomodThemeKey.self] = newValue }
    }
}

extension View {
    /// Supplies the accent and backdrop of the FOMOD wizard below this view.
    public func fomodTheme(_ theme: FomodTheme) -> some View { environment(\.fomodTheme, theme) }
}

// MARK: - Settings the tests (and a caller with its own setting) can override

struct FomodReduceMotionOverrideKey: EnvironmentKey { static let defaultValue: Bool? = nil }
/// Forces the bordered button styles on systems that have Liquid Glass (offscreen renders cannot draw glass at all).
struct FomodGlassDisabledKey: EnvironmentKey { static let defaultValue = false }

extension EnvironmentValues {
    var fomodReduceMotionOverride: Bool? {
        get { self[FomodReduceMotionOverrideKey.self] }
        set { self[FomodReduceMotionOverrideKey.self] = newValue }
    }

    var fomodReduceMotion: Bool { fomodReduceMotionOverride ?? accessibilityReduceMotion }

    var fomodGlassDisabled: Bool {
        get { self[FomodGlassDisabledKey.self] }
        set { self[FomodGlassDisabledKey.self] = newValue }
    }
}

// MARK: - Buttons

/// The two footer buttons: Liquid Glass where the system has it, the bordered styles before.
struct FomodButtonStyle: ViewModifier {
    @Environment(\.fomodGlassDisabled) private var glassDisabled
    var prominent: Bool

    func body(content: Content) -> some View {
        if #available(iOS 26.0, macOS 26.0, *), !glassDisabled {
            if prominent { content.buttonStyle(.glassProminent) } else { content.buttonStyle(.glass) }
        } else {
            if prominent { content.buttonStyle(.borderedProminent) } else { content.buttonStyle(.bordered) }
        }
    }
}

/// Groups the footer's glass buttons so they blend and morph together (iOS 26 / macOS 26); a plain group otherwise.
struct FomodGlassGroup<Content: View>: View {
    @Environment(\.fomodGlassDisabled) private var glassDisabled
    var spacing: CGFloat = 12
    @ViewBuilder var content: () -> Content

    var body: some View {
        if #available(iOS 26.0, macOS 26.0, *), !glassDisabled {
            GlassEffectContainer(spacing: spacing) { content() }
        } else {
            content()
        }
    }
}

// MARK: - Motion

/// The wizard's motion: short springs without bounce, fades only when Reduce Motion is on.
struct FomodMotion {
    var reduce: Bool

    /// Steps and selection: under 300 ms.
    var spring: Animation { reduce ? .easeOut(duration: 0.15) : .spring(duration: 0.3, bounce: 0) }
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
