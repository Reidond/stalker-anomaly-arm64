import Foundation
import SwiftUI
import WebKit

/// Everything behind `DownloadsView`: the browser, the bookmarks and the list of downloads (pasted links and browser downloads).
///
/// One model per downloads folder is kept alive between views (`shared(for:)`), so flipping to another screen of the
/// launcher and back keeps the page, the list and any running download. `releaseAll()` drops them (before the game starts).
@MainActor
final class DownloadsModel: NSObject, ObservableObject {
    private static var registry: [String: DownloadsModel] = [:]

    static func shared(for dir: URL) -> DownloadsModel {
        let key = dir.standardizedFileURL.path
        if let existing = registry[key] { return existing }
        let model = DownloadsModel(dir: dir)
        registry[key] = model
        return model
    }

    /// Cancels running downloads, frees every web view and forgets the shared models.
    static func releaseAll() {
        for model in registry.values { model.shutdown() }
        registry.removeAll()
    }

    let dir: URL
    let browser: BrowserModel
    let bookmarks: BookmarkStore
    @Published private(set) var items: [DownloadItem] = []
    @Published var notice: String?
    /// The downloads list next to (or over) the page.
    @Published var showPanel = true

    var onDownloaded: (URL) -> Void = { _ in }
    var onDownloadedFile: (DownloadedFile) -> Void = { _ in }

    let linkDownloader: LinkDownloader
    private var webItems: [ObjectIdentifier: DownloadItem] = [:]
    private var noticeGeneration = 0
    static let reserveBytes: Int64 = 128 << 20

    init(dir: URL, browser: BrowserModel? = nil, bookmarks: BookmarkStore? = nil, linkDownloader: LinkDownloader = LinkDownloader()) {
        self.dir = dir
        self.browser = browser ?? BrowserModel()
        self.bookmarks = bookmarks ?? BookmarkStore()
        self.linkDownloader = linkDownloader
        super.init()
        self.browser.downloads = self
        let folder = dir
        DispatchQueue.global(qos: .utility).async {
            LinkDownloader.removeStalePartials(in: folder, olderThan: 7 * 24 * 3600)
        }
    }

    /// Adds a row without starting anything (the offscreen snapshot tool).
    func addPreviewItem(_ item: DownloadItem) {
        items.append(item)
    }

    func shutdown() {
        for item in items where item.state == .downloading {
            item.task?.cancel()
            if item.origin == .browser, let download = item.webDownload {
                item.state = .cancelled
                Task { _ = await download.cancel() }
                if let part = item.partURL { try? FileManager.default.removeItem(at: part) }
            }
        }
        webItems.removeAll()
        browser.shutdown()
    }

    // MARK: Notices

    func show(notice message: String) {
        notice = message
        noticeGeneration += 1
        let generation = noticeGeneration
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 10_000_000_000)
            if let self, self.noticeGeneration == generation { self.notice = nil }
        }
    }

    func dismissNotice() {
        noticeGeneration += 1
        notice = nil
    }

    // MARK: Pasted links

    /// The "download this link" field. Returns false (with a notice) when the text is not an http(s) link.
    @discardableResult
    func downloadLink(_ text: String) -> Bool {
        guard let url = AddressInput.httpLink(text) else {
            show(notice: "Paste an http or https link to a file.")
            return false
        }
        if items.contains(where: { $0.sourceURL == url && $0.origin == .link && $0.state == .downloading }) {
            return true // already on its way
        }
        let item = DownloadItem(origin: .link, sourceURL: url, state: .downloading)
        items.insert(item, at: 0)
        run(item)
        return true
    }

    private func run(_ item: DownloadItem) {
        item.state = .downloading
        item.receivedBytes = LinkDownloader.partialSize(for: item.sourceURL, in: dir)
        let url = item.sourceURL
        let folder = dir
        let downloader = linkDownloader
        item.task = Task { [weak self] in
            do {
                let file = try await downloader.downloadFile(url, to: folder) { progress in
                    MainActor.assumeIsolated { item.update(progress) }
                }
                self?.complete(item, with: file)
            } catch is CancellationError {
                item.state = .cancelled
            } catch LinkDownloadError.notAFile(let pageURL, _) {
                item.state = .openedInBrowser
                self?.openInBrowser(pageURL)
            } catch {
                item.state = .failed(error.localizedDescription)
            }
        }
    }

    private func openInBrowser(_ url: URL) {
        browser.load(url)
        showPanel = false
        show(notice: "That link is a web page, not a file. Opened it in the browser: use the page's own download button.")
    }

    private func complete(_ item: DownloadItem, with file: DownloadedFile) {
        item.file = file
        item.fileName = file.fileName
        item.finalURL = file.finalURL
        item.receivedBytes = file.size
        item.totalBytes = file.size
        item.state = .finished
        onDownloadedFile(file)
        onDownloaded(file.fileURL)
    }

    // MARK: List actions

    func cancel(_ item: DownloadItem) {
        guard item.state == .downloading else { return }
        item.state = .cancelled // first, so the failure callback of a cancelled web download is ignored
        item.task?.cancel()
        if let download = item.webDownload {
            Task { item.resumeData = await download.cancel() }
        }
    }

    func retry(_ item: DownloadItem) {
        guard item.canRetry else { return }
        switch item.origin {
        case .link:
            run(item)
        case .browser:
            if let data = item.resumeData {
                item.resumeData = nil
                item.state = .downloading
                Task {
                    let download = await browser.webView.resumeDownload(fromResumeData: data)
                    attach(download, to: item)
                }
            } else {
                startWeb(item, request: item.request ?? URLRequest(url: item.sourceURL))
            }
        }
    }

    /// Allow a download a page tried to start without a tap.
    func approve(_ item: DownloadItem) {
        guard item.state == .awaitingApproval else { return }
        startWeb(item, request: item.request ?? URLRequest(url: item.sourceURL))
    }

    func remove(_ item: DownloadItem) {
        guard item.canRemove else { return }
        if item.state != .finished {
            if item.origin == .link { LinkDownloader.discardPartial(for: item.sourceURL, in: dir) }
            if let part = item.partURL { try? FileManager.default.removeItem(at: part) }
        }
        if let download = item.webDownload { webItems[ObjectIdentifier(download)] = nil }
        items.removeAll { $0.id == item.id }
    }

    func clearFinished() {
        for item in items where item.state == .finished || item.state == .openedInBrowser {
            remove(item)
        }
    }

    var activeCount: Int { items.filter { $0.state == .downloading || $0.state == .verifying }.count }
    var waitingCount: Int { items.filter { $0.state == .awaitingApproval }.count }

    // MARK: Browser downloads

    private func startWeb(_ item: DownloadItem, request: URLRequest) {
        item.state = .downloading
        item.request = request
        Task {
            let download = await browser.webView.startDownload(using: request)
            attach(download, to: item)
        }
    }

    fileprivate func attach(_ download: WKDownload, to item: DownloadItem) {
        download.delegate = self
        item.webDownload = download
        item.request = item.request ?? download.originalRequest
        webItems[ObjectIdentifier(download)] = item
        let progress = download.progress
        item.progressObservations = [
            progress.observe(\.completedUnitCount, options: [.initial, .new]) { progress, _ in
                let done = progress.completedUnitCount
                Task { @MainActor in if item.state == .downloading { item.receivedBytes = done } }
            },
            progress.observe(\.totalUnitCount, options: [.initial, .new]) { progress, _ in
                let total = progress.totalUnitCount
                Task { @MainActor in if item.state == .downloading { item.totalBytes = total > 0 ? total : nil } }
            },
        ]
    }

    private func finishWeb(_ item: DownloadItem) {
        guard let part = item.partURL else {
            item.state = .failed("The browser did not report where it saved the file.")
            return
        }
        item.state = .verifying
        item.progressObservations.removeAll()
        let folder = dir
        let name = item.fileName
        Task { [weak self] in
            do {
                let result = try await Task.detached(priority: .utility) { () -> (URL, String, Int64) in
                    let digest = try FileHash.sha256(of: part)
                    let size = (try FileManager.default.attributesOfItem(atPath: part.path)[.size] as? NSNumber)?.int64Value ?? 0
                    let target = try FileNaming.moveUnique(part, to: folder, name: name)
                    return (target, digest, size)
                }.value
                let file = DownloadedFile(fileURL: result.0, originalURL: item.sourceURL, finalURL: item.finalURL ?? item.sourceURL,
                                          pageURL: item.pageURL, fileName: result.0.lastPathComponent, sha256: result.1,
                                          size: result.2, kind: .browser)
                self?.complete(item, with: file)
            } catch {
                item.state = .failed("Saving the file failed: \(error.localizedDescription)")
            }
        }
    }
}

// MARK: - Downloads started by the web view

extension DownloadsModel: BrowserDownloadHandler {
    func browser(_ browser: BrowserModel, didStart download: WKDownload, pageURL: URL?) {
        let source = download.originalRequest?.url ?? pageURL ?? URL(string: "about:blank")!
        let item = DownloadItem(origin: .browser, sourceURL: source, pageURL: pageURL, state: .downloading)
        items.insert(item, at: 0)
        attach(download, to: item)
    }

    func browser(_ browser: BrowserModel, blocked request: URLRequest, suggestedName: String?, pageURL: URL?) {
        guard let url = request.url else { return }
        if items.contains(where: { $0.sourceURL == url && $0.state == .awaitingApproval }) { return }
        let item = DownloadItem(origin: .browser, sourceURL: url, pageURL: pageURL,
                                fileName: suggestedName.map(FileNaming.sanitize), state: .awaitingApproval)
        item.request = request
        items.insert(item, at: 0)
        showPanel = true
        show(notice: "\(url.host ?? "A page") tried to download a file without a tap. Nothing was saved: allow it in the list if you want it.")
    }
}

extension DownloadsModel: WKDownloadDelegate {
    func download(_ download: WKDownload, decideDestinationUsing response: URLResponse, suggestedFilename: String) async -> URL? {
        guard let item = webItems[ObjectIdentifier(download)] else { return nil }
        item.finalURL = response.url
        var name = FileNaming.sanitize(suggestedFilename)
        if (name as NSString).pathExtension.isEmpty, let ext = FileNaming.pathExtension(forMIME: response.mimeType) { name += "." + ext }
        item.fileName = name
        let expected = response.expectedContentLength
        item.totalBytes = expected > 0 ? expected : nil
        do {
            try LinkDownloader.prepareDirectory(dir)
        } catch {
            item.state = .failed("Cannot use the downloads folder: \(error.localizedDescription)")
            return nil
        }
        if expected > 0, let free = LinkDownloader.availableCapacity(at: dir), expected + Self.reserveBytes > free {
            let fmt = ByteCountFormatter()
            item.state = .failed("Not enough free space: the file needs \(fmt.string(fromByteCount: expected)), \(fmt.string(fromByteCount: max(0, free))) available.")
            return nil
        }
        // a resumed download continues in the file the first attempt left
        if let kept = item.partURL, FileManager.default.fileExists(atPath: kept.path), item.receivedBytes > 0 {
            return kept
        }
        let part = dir.appendingPathComponent(".wk-\(item.id.uuidString).part")
        try? FileManager.default.removeItem(at: part)
        item.partURL = part
        return part
    }

    func downloadDidFinish(_ download: WKDownload) {
        guard let item = webItems[ObjectIdentifier(download)], item.state == .downloading else { return }
        finishWeb(item)
    }

    func download(_ download: WKDownload, didFailWithError error: Error, resumeData: Data?) {
        guard let item = webItems[ObjectIdentifier(download)] else { return }
        item.progressObservations.removeAll()
        item.resumeData = resumeData
        guard item.state == .downloading else { return } // cancelled by the user, or already failed with a better message
        let ns = error as NSError
        if ns.domain == NSURLErrorDomain && ns.code == NSURLErrorCancelled {
            item.state = .cancelled
        } else {
            item.state = .failed(ns.localizedDescription)
        }
    }

    func download(_ download: WKDownload, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest) async -> WKDownload.RedirectPolicy {
        BrowserModel.isWebURL(request.url) ? .allow : .cancel
    }
}
