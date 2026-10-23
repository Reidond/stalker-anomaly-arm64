import Foundation
import SwiftUI
import WebKit

/// Where the browser sends downloads. The model owning the downloads list implements it.
@MainActor
protocol BrowserDownloadHandler: AnyObject {
    /// WebKit turned a navigation into a download (the gate let it through).
    func browser(_ browser: BrowserModel, didStart download: WKDownload, pageURL: URL?)
    /// A page tried to start a download without a recent tap; nothing was started.
    func browser(_ browser: BrowserModel, blocked request: URLRequest, suggestedName: String?, pageURL: URL?)
}

/// The in-app browser: one WKWebView with its own persistent cookie store, http(s) only, and a gate that lets a
/// download start only after a user tap.
@MainActor
final class BrowserModel: NSObject, ObservableObject {
    let webView: WKWebView

    @Published private(set) var canGoBack = false
    @Published private(set) var canGoForward = false
    @Published private(set) var isLoading = false
    @Published private(set) var progress: Double = 0
    @Published private(set) var pageTitle = ""
    @Published private(set) var currentURL: URL?
    @Published var addressText = ""
    @Published var errorMessage: String?
    /// While the address field has focus the page's URL does not overwrite what is being typed.
    var isEditingAddress = false

    weak var downloads: BrowserDownloadHandler?
    var gate = DownloadGate()

    private var observations: [NSKeyValueObservation] = []
    private let gestureHandler = GestureMessageHandler()

    /// `dataStore` is where cookies and site data live. The default store persists them, so a ModDB or GitHub session
    /// (and Cloudflare's clearance) survives restarts; nothing outside this web view reads it.
    init(dataStore: WKWebsiteDataStore = .default()) {
        let config = WKWebViewConfiguration()
        config.websiteDataStore = dataStore
        config.defaultWebpagePreferences.allowsContentJavaScript = true
        config.mediaTypesRequiringUserActionForPlayback = .all
        #if os(iOS)
        config.defaultWebpagePreferences.preferredContentMode = .desktop
        config.allowsInlineMediaPlayback = true
        #endif
        let script = WKUserScript(source: Self.gestureScript, injectionTime: .atDocumentStart, forMainFrameOnly: false, in: .defaultClient)
        config.userContentController.addUserScript(script)
        config.userContentController.add(gestureHandler, contentWorld: .defaultClient, name: "userGesture")
        webView = WKWebView(frame: .zero, configuration: config)
        super.init()
        gestureHandler.model = self
        webView.navigationDelegate = self
        webView.uiDelegate = self
        webView.allowsBackForwardNavigationGestures = true
        #if DEBUG
        webView.isInspectable = true
        #endif
        observe()
    }

    /// Frees the web view's content process. Call before the game starts.
    func shutdown() {
        webView.stopLoading()
        webView.navigationDelegate = nil
        webView.uiDelegate = nil
        webView.configuration.userContentController.removeAllUserScripts()
        webView.configuration.userContentController.removeScriptMessageHandler(forName: "userGesture", contentWorld: .defaultClient)
        observations.removeAll()
        webView.loadHTMLString("", baseURL: nil)
    }

    // MARK: Native actions (every one is a user tap)

    func load(_ url: URL) {
        noteTypedNavigation()
        errorMessage = nil
        webView.load(URLRequest(url: url))
    }

    /// The address field's Go: a link, a bare host or a search.
    func submitAddress() {
        switch AddressInput.resolve(addressText) {
        case .empty:
            break
        case .url(let url), .search(let url):
            load(url)
        case .unsupported(let scheme):
            errorMessage = "Only http and https links can be opened (\"\(scheme)\" is not)."
        }
    }

    func goBack() { noteTypedNavigation(); webView.goBack() }
    func goForward() { noteTypedNavigation(); webView.goForward() }
    func reload() { noteTypedNavigation(); errorMessage = nil; webView.reload() }
    func stop() { webView.stopLoading() }

    private func noteTypedNavigation() {
        gate.noteTypedNavigation(at: DownloadGate.now)
    }

    func noteTap() {
        gate.noteTap(at: DownloadGate.now)
    }

    /// Scrolls the page by a number of points (controller and keyboard use).
    func scroll(byX dx: Double, y dy: Double) {
        webView.evaluateJavaScript("window.scrollBy(\(dx), \(dy))", completionHandler: nil)
    }

    /// Taps the element under a point given as a fraction (0...1) of the visible page, as a click. Counts as a user tap.
    func tap(atFractionX fx: Double, y fy: Double) {
        noteTap()
        let js = """
        (function(fx, fy) {
          var vv = window.visualViewport;
          var w = vv ? vv.width : window.innerWidth, h = vv ? vv.height : window.innerHeight;
          var ox = vv ? vv.offsetLeft : 0, oy = vv ? vv.offsetTop : 0;
          var x = ox + fx * w, y = oy + fy * h;
          var el = document.elementFromPoint(x, y);
          if (!el) return;
          var target = el.closest('a[href], button, input, select, textarea, summary, label, [role=button], [onclick]') || el;
          if (target.focus) target.focus();
          target.dispatchEvent(new MouseEvent('click', {bubbles: true, cancelable: true, view: window, clientX: x, clientY: y}));
        })(\(fx), \(fy));
        """
        webView.evaluateJavaScript(js, completionHandler: nil)
    }

    // MARK: State

    private func observe() {
        observations = [
            webView.observe(\.canGoBack, options: [.initial, .new]) { [weak self] web, _ in
                MainActor.assumeIsolated { self?.canGoBack = web.canGoBack }
            },
            webView.observe(\.canGoForward, options: [.initial, .new]) { [weak self] web, _ in
                MainActor.assumeIsolated { self?.canGoForward = web.canGoForward }
            },
            webView.observe(\.isLoading, options: [.initial, .new]) { [weak self] web, _ in
                MainActor.assumeIsolated { self?.isLoading = web.isLoading }
            },
            webView.observe(\.estimatedProgress, options: [.initial, .new]) { [weak self] web, _ in
                MainActor.assumeIsolated { self?.progress = web.estimatedProgress }
            },
            webView.observe(\.title, options: [.initial, .new]) { [weak self] web, _ in
                MainActor.assumeIsolated { self?.pageTitle = web.title ?? "" }
            },
            webView.observe(\.url, options: [.initial, .new]) { [weak self] web, _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.currentURL = web.url
                    if !self.isEditingAddress, let url = web.url, url.scheme != "about" {
                        self.addressText = url.absoluteString
                    }
                }
            },
        ]
    }

    // MARK: Downloads

    private func authorizeDownload(request: URLRequest, suggestedName: String?) -> Bool {
        if gate.consume(at: DownloadGate.now) { return true }
        downloads?.browser(self, blocked: request, suggestedName: suggestedName, pageURL: webView.url)
        return false
    }

    static func isWebURL(_ url: URL?) -> Bool {
        guard let scheme = url?.scheme?.lowercased() else { return false }
        return scheme == "http" || scheme == "https"
    }

    /// Reports taps made inside the page. Only the page's trusted `click` events call this (`event.isTrusted`), so a
    /// page cannot give itself permission by scripting clicks.
    fileprivate func pageReportedUserTap() {
        noteTap()
    }

    private static let gestureScript = """
    (function() {
      if (window.__anomalyTap) return;
      window.__anomalyTap = true;
      document.addEventListener('click', function(e) {
        if (e.isTrusted) { window.webkit.messageHandlers.userGesture.postMessage(1); }
      }, true);
    })();
    """
}

/// WKUserContentController keeps its handlers alive; this one only holds the model weakly.
@MainActor
private final class GestureMessageHandler: NSObject, WKScriptMessageHandler {
    weak var model: BrowserModel?

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        model?.pageReportedUserTap()
    }
}

// MARK: - Navigation

extension BrowserModel: WKNavigationDelegate {
    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction) async -> WKNavigationActionPolicy {
        guard let url = navigationAction.request.url else { return .cancel }
        let scheme = url.scheme?.lowercased() ?? ""
        if scheme == "about" { return .allow }
        guard Self.isWebURL(url) else {
            errorMessage = "Blocked a link that is not http or https (\(scheme):)."
            return .cancel
        }
        if navigationAction.shouldPerformDownload {
            return authorizeDownload(request: navigationAction.request, suggestedName: nil) ? .download : .cancel
        }
        return .allow
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationResponse: WKNavigationResponse) async -> WKNavigationResponsePolicy {
        let http = navigationResponse.response as? HTTPURLResponse
        let attachment = FileNaming.parseContentDisposition(http?.value(forHTTPHeaderField: "Content-Disposition")).isAttachment
        guard attachment || !navigationResponse.canShowMIMEType else {
            if navigationResponse.isForMainFrame { gate.pageDisplayed() }
            return .allow
        }
        guard let url = navigationResponse.response.url, Self.isWebURL(url) else { return .cancel }
        let allowed = authorizeDownload(request: URLRequest(url: url), suggestedName: navigationResponse.response.suggestedFilename)
        return allowed ? .download : .cancel
    }

    func webView(_ webView: WKWebView, navigationAction: WKNavigationAction, didBecome download: WKDownload) {
        downloads?.browser(self, didStart: download, pageURL: webView.url)
    }

    func webView(_ webView: WKWebView, navigationResponse: WKNavigationResponse, didBecome download: WKDownload) {
        downloads?.browser(self, didStart: download, pageURL: webView.url)
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        report(error)
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        report(error)
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        // iPadOS reclaims the web process under memory pressure; bring the page back
        webView.reload()
    }

    private func report(_ error: Error) {
        let ns = error as NSError
        if ns.domain == NSURLErrorDomain && ns.code == NSURLErrorCancelled { return }
        // a navigation that became a download or was refused by a policy decision
        if ns.domain == "WebKitErrorDomain" && (ns.code == 102 || ns.code == 101) { return }
        if ns.domain == NSURLErrorDomain && ns.code == NSURLErrorAppTransportSecurityRequiresSecureConnection {
            errorMessage = "This site only offers a plain http connection, which iPadOS blocks. Try its https address."
        } else {
            errorMessage = ns.localizedDescription
        }
    }
}

// MARK: - UI delegate

extension BrowserModel: WKUIDelegate {
    /// target=_blank and window.open: the page opens in the same view, and only for a tap. Scripted pop-ups are dropped.
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                 for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        if gate.isOpen(at: DownloadGate.now), Self.isWebURL(navigationAction.request.url) {
            webView.load(navigationAction.request)
        }
        return nil
    }
}
