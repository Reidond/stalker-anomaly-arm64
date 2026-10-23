// The launcher's look: Liquid Glass over a dark backdrop of the Zone, in the game's colors. The palette is rust and amber, with the
// radiation yellow-green used sparingly (what is switched on, what succeeded). Everything is guarded for the deployment minimum
// (iOS 17 / macOS 14): where Liquid Glass is missing, or Reduce Transparency is on, the same shapes are drawn in a material or opaque.
import SwiftUI

enum Theme {
    /// The accent: amber, the color of the game's menus. Focus, the primary button, links.
    static let amber = Color(red: 0.96, green: 0.67, blue: 0.20)
    /// Rust: the warm glow of the backdrop, conflicts and warnings, destructive actions.
    static let rust = Color(red: 0.78, green: 0.33, blue: 0.14)
    /// Radiation yellow-green. Sparingly: a switch that is on, a success.
    static let radiation = Color(red: 0.72, green: 0.85, blue: 0.24)

    static let primaryText = Color.white.opacity(0.96)
    static let secondaryText = Color.white.opacity(0.68)
    static let tertiaryText = Color.white.opacity(0.46)

    /// Concentric radii: a surface's radius is the radius of what it holds plus the padding between them.
    static let cardRadius: CGFloat = 28
    static let rowRadius: CGFloat = 22
    static let rowPadding: CGFloat = 14
    static let cardPadding: CGFloat = 16
    static var cardInnerRadius: CGFloat { cardRadius - cardPadding }   // a field or a button inside a card's padding
    static var tileRadius: CGFloat { rowRadius - rowPadding }   // an icon tile inside a row's padding
    static let minHit: CGFloat = 56                             // rows and buttons: well above 44 pt, for a pad and a couch
}

enum Format {
    static func bytes(_ value: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.countStyle = .binary
        formatter.allowsNonnumericFormatting = false
        return formatter.string(fromByteCount: max(value, 0))
    }

    static func count(_ value: Int, _ noun: String) -> String {
        "\(value) \(noun)\(value == 1 ? "" : "s")"
    }

    static func date(_ iso: String?) -> String? {
        guard let iso else { return nil }
        let parser = ISO8601DateFormatter()
        guard let date = parser.date(from: iso) ?? ISO8601DateFormatter.fractional.date(from: iso) else { return nil }
        return date.formatted(date: .abbreviated, time: .shortened)
    }
}

extension ISO8601DateFormatter {
    static let fractional: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter
    }()
}

/// True where Liquid Glass exists (iPadOS / macOS 26 and later).
var liquidGlassAvailable: Bool {
    if #available(iOS 26.0, macOS 26.0, *) { return true }
    return false
}

/// Motion that is quick and has a purpose: a spring that settles without bounce for what moves between places, a short ease-out for
/// feedback. Reduce Motion keeps the state change and drops the movement.
struct LauncherMotion {
    let reduced: Bool
    var screen: Animation { reduced ? .easeOut(duration: 0.12) : .spring(duration: 0.3, bounce: 0) }
    var content: Animation { reduced ? .easeOut(duration: 0.12) : .spring(duration: 0.28, bounce: 0) }
    /// Moving the focus is as frequent as pressing a key (the stick repeats every 90 ms): nearly instant.
    var focus: Animation { .easeOut(duration: 0.1) }
    var press: Animation { .easeOut(duration: 0.14) }
}

/// The prominent button of a screen (Play, Add Anyway): Liquid Glass on iPadOS / macOS 26 and later, the bordered style elsewhere (and in
/// the snapshot tool: an offscreen window has no compositor to draw glass, so it shows the fallback).
struct ProminentButtonStyle: ViewModifier {
    @Environment(\.launcherSnapshot) private var snapshot
    var radius: CGFloat?

    func body(content: Content) -> some View {
        if snapshot {
            content.buttonStyle(SnapshotProminentStyle(radius: radius))
        } else if #available(iOS 26.0, macOS 26.0, *) {
            content.buttonStyle(.glassProminent)
        } else {
            content.buttonStyle(.borderedProminent)
        }
    }
}

/// What the prominent button looks like in the snapshot tool: the tint (amber, or rust for a destructive one) as a capsule, or a rounded
/// rectangle where the button has a border shape, with dark type, as the tinted glass button reads.
struct SnapshotProminentStyle: ButtonStyle {
    @Environment(\.isEnabled) private var isEnabled
    var radius: CGFloat?

    func makeBody(configuration: Configuration) -> some View {
        let shape = RoundedRectangle(cornerRadius: radius ?? 1000, style: .continuous)
        configuration.label
            .foregroundStyle(Color.black.opacity(0.85))
            .padding(.vertical, 7)   // what the bordered style puts around its label
            .background(shape.fill(.tint).opacity(isEnabled ? 1 : 0.35))
    }
}

/// The ordinary button on glass.
struct GlassButtonStyleModifier: ViewModifier {
    @Environment(\.launcherSnapshot) private var snapshot

    func body(content: Content) -> some View {
        if #available(iOS 26.0, macOS 26.0, *), !snapshot {
            content.buttonStyle(.glass)
        } else {
            content.buttonStyle(.bordered)
        }
    }
}

extension View {
    /// `radius`: the corner radius of the button where it is not a capsule (only the snapshot tool needs it; the system draws the real one
    /// from `buttonBorderShape`).
    func prominentButton(radius: CGFloat? = nil) -> some View { modifier(ProminentButtonStyle(radius: radius)) }
    func glassButton() -> some View { modifier(GlassButtonStyleModifier()) }
}
