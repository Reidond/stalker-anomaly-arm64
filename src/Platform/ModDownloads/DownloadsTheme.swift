import SwiftUI

/// The look of `DownloadsView`. The launcher can supply its own with `.downloadsTheme(_:)` on the view (or any ancestor);
/// the default is `.anomaly`: dark, rust and amber accents, radiation yellow-green kept for the one moment it means something
/// (a finished download).
///
/// ```swift
/// DownloadsView(...).downloadsTheme(.anomaly)
/// DownloadsView(...).downloadsTheme(DownloadsTheme(background: ..., accent: ..., ...))
/// ```
public struct DownloadsTheme: Sendable {
    /// Behind the page while it loads, and under the glass when there is nothing to refract.
    public var background: Color
    /// A panel's body when glass is off (Reduce Transparency, or `prefersGlass == false`).
    public var surface: Color
    /// Raised surfaces: the downloads panel without glass, recessed fields.
    public var surfaceRaised: Color
    public var text: Color
    public var secondaryText: Color
    /// Rust: the prominent buttons (Go, Retry, Allow) and the tint of their glass.
    public var accent: Color
    /// Text on `accent`.
    public var onAccent: Color
    /// Amber: the focus ring, progress, the bookmark star, badges.
    public var highlight: Color
    /// Radiation yellow-green: a finished download. Use sparingly.
    public var signal: Color
    public var danger: Color
    /// Hairlines and the edge of surfaces that are not glass.
    public var outline: Color
    /// Corner radius of the downloads panel; controls inside it use concentric radii.
    public var panelRadius: CGFloat
    /// Liquid Glass (iOS 26 / macOS 26) or a material where available; false draws opaque surfaces everywhere.
    public var prefersGlass: Bool

    public init(background: Color, surface: Color, surfaceRaised: Color, text: Color, secondaryText: Color, accent: Color,
                onAccent: Color = .white, highlight: Color, signal: Color, danger: Color, outline: Color,
                panelRadius: CGFloat = 28, prefersGlass: Bool = true) {
        self.background = background
        self.surface = surface
        self.surfaceRaised = surfaceRaised
        self.text = text
        self.secondaryText = secondaryText
        self.accent = accent
        self.onAccent = onAccent
        self.highlight = highlight
        self.signal = signal
        self.danger = danger
        self.outline = outline
        self.panelRadius = panelRadius
        self.prefersGlass = prefersGlass
    }

    /// Dark, warm blacks; rust `#B4532A` and amber `#F0A830`; radiation yellow-green `#B7D43A` only for "done".
    public static let anomaly = DownloadsTheme(
        background: Color(red: 0.051, green: 0.047, blue: 0.039),
        surface: Color(red: 0.106, green: 0.098, blue: 0.078),
        surfaceRaised: Color(red: 0.149, green: 0.137, blue: 0.106),
        text: Color(red: 0.929, green: 0.906, blue: 0.851),
        secondaryText: Color(red: 0.667, green: 0.639, blue: 0.576),
        accent: Color(red: 0.706, green: 0.325, blue: 0.165),
        onAccent: Color(red: 1, green: 0.973, blue: 0.937),
        highlight: Color(red: 0.941, green: 0.659, blue: 0.188),
        signal: Color(red: 0.718, green: 0.831, blue: 0.227),
        danger: Color(red: 0.886, green: 0.341, blue: 0.247),
        outline: Color.white.opacity(0.12)
    )
}

private struct DownloadsThemeKey: EnvironmentKey {
    static let defaultValue = DownloadsTheme.anomaly
}

extension EnvironmentValues {
    public var downloadsTheme: DownloadsTheme {
        get { self[DownloadsThemeKey.self] }
        set { self[DownloadsThemeKey.self] = newValue }
    }
}

extension View {
    /// Sets the look of every `DownloadsView` inside.
    public func downloadsTheme(_ theme: DownloadsTheme) -> some View {
        environment(\.downloadsTheme, theme)
    }
}
