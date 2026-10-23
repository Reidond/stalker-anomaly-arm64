// The launcher's look: dark, amber like the game's own menus, big enough to read from a couch.
import SwiftUI

enum Theme {
    static let background = Color(red: 0.047, green: 0.051, blue: 0.045)
    static let panel = Color(red: 0.093, green: 0.098, blue: 0.088)
    static let panelRaised = Color(red: 0.135, green: 0.140, blue: 0.125)
    static let line = Color.white.opacity(0.09)
    static let accent = Color(red: 0.91, green: 0.66, blue: 0.22)
    static let accentDim = Color(red: 0.91, green: 0.66, blue: 0.22).opacity(0.16)
    static let text = Color(red: 0.90, green: 0.90, blue: 0.88)
    static let dim = Color(red: 0.55, green: 0.57, blue: 0.55)
    static let good = Color(red: 0.43, green: 0.84, blue: 0.47)
    static let warn = Color(red: 0.94, green: 0.75, blue: 0.31)
    static let bad = Color(red: 0.94, green: 0.39, blue: 0.35)

    static let rowHeight: CGFloat = 76
    static let radius: CGFloat = 14
    static let gutter: CGFloat = 32

    static func title(_ size: CGFloat = 40) -> Font { .system(size: size, weight: .heavy, design: .default) }
    static func heading(_ size: CGFloat = 22) -> Font { .system(size: size, weight: .bold, design: .default) }
    static func body(_ size: CGFloat = 19) -> Font { .system(size: size, weight: .regular, design: .default) }
    static func small(_ size: CGFloat = 15) -> Font { .system(size: size, weight: .medium, design: .default) }
    static func mono(_ size: CGFloat = 14) -> Font { .system(size: size, weight: .regular, design: .monospaced) }
}

enum Format {
    static func bytes(_ value: Int64) -> String {
        let units = ["B", "KiB", "MiB", "GiB", "TiB"]
        var amount = Double(max(value, 0))
        var unit = 0
        while amount >= 1024, unit < units.count - 1 {
            amount /= 1024
            unit += 1
        }
        return unit == 0 ? "\(Int(amount)) B" : String(format: amount >= 100 ? "%.0f %@" : "%.1f %@", amount, units[unit])
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

/// The screen behind everything: near black with a faint warm glow from the top left, like the game's menu.
struct LauncherBackground: View {
    var body: some View {
        ZStack {
            Theme.background
            RadialGradient(colors: [Theme.accent.opacity(0.10), .clear], center: .topLeading, startRadius: 0, endRadius: 900)
            LinearGradient(colors: [.clear, Color.black.opacity(0.35)], startPoint: .center, endPoint: .bottom)
        }
        .ignoresSafeArea()
    }
}
