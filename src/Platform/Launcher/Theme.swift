// The launcher uses the system's own look: stock controls, semantic colors, Dynamic Type text styles, and on iPadOS / macOS 26 and
// later the Liquid Glass button styles. The only thing of its own is the accent: the amber of the game's menus.
import SwiftUI

enum Theme {
    static let accent = Color(red: 0.93, green: 0.58, blue: 0.08)
}

enum Format {
    static func bytes(_ value: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: max(value, 0), countStyle: .binary)
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

/// The prominent button of a screen (Play, Add anyway): Liquid Glass where the system has it, the bordered style before.
struct ProminentButtonStyle: ViewModifier {
    func body(content: Content) -> some View {
        if #available(iOS 26.0, macOS 26.0, *) {
            content.buttonStyle(.glassProminent)
        } else {
            content.buttonStyle(.borderedProminent)
        }
    }
}

extension View {
    func prominentButton() -> some View { modifier(ProminentButtonStyle()) }
}

/// The grouped sections of Settings: an inset-grouped list on iPadOS, a grouped form on the Mac.
struct LauncherForm<Content: View>: View {
    @ViewBuilder var content: () -> Content

    var body: some View {
        #if os(macOS)
        Form { content() }.formStyle(.grouped)
        #else
        List { content() }.listStyle(.insetGrouped)
        #endif
    }
}

extension View {
    /// The grouped list of Settings on iPadOS; the plain inset list on the Mac.
    func launcherListStyle() -> some View {
        #if os(iOS)
        listStyle(.insetGrouped)
        #else
        listStyle(.inset)
        #endif
    }
}
