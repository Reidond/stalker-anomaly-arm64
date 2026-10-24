import SwiftUI
import WebKit

#if os(iOS)
import UIKit

/// Hosts the model's WKWebView. The web view outlives the SwiftUI view, so a new container just takes it over. Placed under the
/// navigation bar (`ignoresSafeArea`), UIKit insets the page by the bar's height, so it starts below the bar and scrolls under it.
struct WebViewContainer: UIViewRepresentable {
    let webView: WKWebView

    func makeUIView(context: Context) -> WKWebView { webView }
    func updateUIView(_ uiView: WKWebView, context: Context) {}
}
#else
import AppKit

struct WebViewContainer: NSViewRepresentable {
    let webView: WKWebView

    func makeNSView(context: Context) -> WKWebView { webView }
    func updateNSView(_ nsView: WKWebView, context: Context) {}
}
#endif

/// The grouped sections of Settings: an inset-grouped list on iPadOS, a grouped form on the Mac.
struct DownloadsForm<Content: View>: View {
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
    /// For a List that reorders (the bookmark editor): inset grouped on iPadOS, the inset list on the Mac.
    func downloadsListStyle() -> some View {
        #if os(iOS)
        listStyle(.insetGrouped)
        #else
        listStyle(.inset)
        #endif
    }
}
