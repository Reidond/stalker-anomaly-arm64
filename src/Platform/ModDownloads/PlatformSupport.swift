import SwiftUI
import WebKit

#if os(iOS)
import UIKit

/// Hosts the model's WKWebView. The web view outlives the SwiftUI view, so a new container just takes it over.
/// `topInset` is the height of the glass bar floating over the page: with it the page lays out and scrolls from below the
/// bar and shows through it (iOS 26's `obscuredContentInsets`).
struct WebViewContainer: UIViewRepresentable {
    let webView: WKWebView
    var topInset: CGFloat = 0
    @Environment(\.downloadsTheme) private var theme

    func makeUIView(context: Context) -> WKWebView { webView }

    func updateUIView(_ uiView: WKWebView, context: Context) {
        uiView.underPageBackgroundColor = UIColor(theme.background)
        if #available(iOS 26, *) {
            let insets = UIEdgeInsets(top: topInset, left: 0, bottom: 0, right: 0)
            if uiView.obscuredContentInsets != insets { uiView.obscuredContentInsets = insets }
        }
    }
}
#else
import AppKit

struct WebViewContainer: NSViewRepresentable {
    let webView: WKWebView
    var topInset: CGFloat = 0
    @Environment(\.downloadsTheme) private var theme

    func makeNSView(context: Context) -> WKWebView { webView }

    func updateNSView(_ nsView: WKWebView, context: Context) {
        nsView.underPageBackgroundColor = NSColor(theme.background)
        if #available(macOS 26, *) {
            let insets = NSEdgeInsets(top: topInset, left: 0, bottom: 0, right: 0)
            if nsView.obscuredContentInsets.top != insets.top { nsView.obscuredContentInsets = insets }
        }
    }
}
#endif

/// ImageRenderer (used by tools/mod_downloads_snapshot) cannot draw text fields, scroll views, glass or progress views; with this
/// set the views draw static stand-ins instead. Never set in the app.
private struct SnapshotModeKey: EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    var snapshotMode: Bool {
        get { self[SnapshotModeKey.self] }
        set { self[SnapshotModeKey.self] = newValue }
    }
}
