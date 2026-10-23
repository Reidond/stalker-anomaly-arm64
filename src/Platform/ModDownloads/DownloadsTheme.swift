import SwiftUI

/// What `DownloadsView` takes from its host: an accent and one signal colour. Everything else is the system's: stock controls,
/// semantic colours, Dynamic Type, Liquid Glass bars and buttons on iPadOS / macOS 26 and later, light or dark as the device is set.
///
/// The default matches the launcher: the amber of the game's menus as the accent, and radiation yellow-green kept for the one
/// moment it means something (a finished download). The launcher can pass its own with `.downloadsTheme(_:)`; a `.tint(_:)`
/// set by the host above `DownloadsView` is overridden only if you pass a theme.
public struct DownloadsTheme: Sendable {
    /// Tint of the controls, the progress bars and the page cursor.
    public var accent: Color
    /// A finished download.
    public var signal: Color

    public init(accent: Color, signal: Color) {
        self.accent = accent
        self.signal = signal
    }

    public static let anomaly = DownloadsTheme(
        accent: Color(red: 0.93, green: 0.58, blue: 0.08),
        signal: Color(red: 0.718, green: 0.831, blue: 0.227)
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
    /// Sets the accent and signal colour of every `DownloadsView` inside.
    public func downloadsTheme(_ theme: DownloadsTheme) -> some View {
        environment(\.downloadsTheme, theme)
    }
}
