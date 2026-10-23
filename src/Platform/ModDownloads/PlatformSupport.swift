import SwiftUI
import WebKit

#if os(iOS)
import UIKit

/// Hosts the model's WKWebView. The web view outlives the SwiftUI view, so a new container just takes it over.
struct WebViewContainer: UIViewRepresentable {
    let webView: WKWebView
    func makeUIView(context: Context) -> WKWebView { webView }
    func updateUIView(_ uiView: WKWebView, context: Context) {}
}

extension Color {
    static var barBackground: Color { Color(uiColor: .secondarySystemBackground) }
    static var fieldBackground: Color { Color(uiColor: .tertiarySystemBackground) }
    static var pageBackground: Color { Color(uiColor: .systemBackground) }
}
#else
import AppKit

struct WebViewContainer: NSViewRepresentable {
    let webView: WKWebView
    func makeNSView(context: Context) -> WKWebView { webView }
    func updateNSView(_ nsView: WKWebView, context: Context) {}
}

extension Color {
    static var barBackground: Color { Color(nsColor: .controlBackgroundColor) }
    static var fieldBackground: Color { Color(nsColor: .textBackgroundColor) }
    static var pageBackground: Color { Color(nsColor: .windowBackgroundColor) }
}
#endif

/// A button for the browser bar and the lists: at least 44 x 44 points (touch), a hover highlight (pointer), and a
/// visible ring while it has focus (keyboard, controller).
struct BarButtonStyle: ButtonStyle {
    var prominent = false
    var minWidth: CGFloat = 44

    func makeBody(configuration: Configuration) -> some View {
        BarButtonBody(configuration: configuration, prominent: prominent, minWidth: minWidth)
    }
}

private struct BarButtonBody: View {
    let configuration: ButtonStyleConfiguration
    let prominent: Bool
    let minWidth: CGFloat
    @Environment(\.isFocused) private var focused
    @Environment(\.isEnabled) private var enabled
    @State private var hovering = false

    var body: some View {
        configuration.label
            .frame(minWidth: minWidth, minHeight: 44)
            .padding(.horizontal, 6)
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(prominent ? Color.accentColor.opacity(configuration.isPressed ? 0.7 : 1) : Color.primary.opacity(configuration.isPressed ? 0.18 : (hovering ? 0.12 : 0.06)))
            )
            .foregroundStyle(prominent ? Color.white : Color.primary)
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .strokeBorder(Color.accentColor, lineWidth: focused ? 3 : 0)
            )
            .opacity(enabled ? 1 : 0.35)
            .contentShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
            .onHover { hovering = $0 }
    }
}

extension View {
    /// Reachable with keyboard and controller focus, and a pointer hover effect on iPad.
    func focusableControl() -> some View {
        #if os(iOS)
        self.focusable(true).hoverEffect(.highlight)
        #else
        self.focusable(true)
        #endif
    }
}

/// ImageRenderer (used by tools/mod_downloads_snapshot) cannot draw text fields, scroll views or paste buttons; with this set
/// the views draw static stand-ins instead. Never set in the app.
private struct SnapshotModeKey: EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    var snapshotMode: Bool {
        get { self[SnapshotModeKey.self] }
        set { self[SnapshotModeKey.self] = newValue }
    }
}
