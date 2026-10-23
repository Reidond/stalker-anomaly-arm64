import Foundation
import WebKit

/// One row of the downloads list: a pasted link fetched by `LinkDownloader`, or a file the browser downloaded.
@MainActor
final class DownloadItem: ObservableObject, Identifiable {
    enum State: Equatable {
        /// A page tried to start this download without a tap; waits for the user's Allow.
        case awaitingApproval
        case downloading
        /// The bytes are in; hashing the file and moving it into place.
        case verifying
        case finished
        case failed(String)
        case cancelled
        /// A pasted link turned out to be a web page; it was opened in the browser instead.
        case openedInBrowser
    }

    let id = UUID()
    let origin: DownloadedFile.Kind
    /// The URL the user asked for (the pasted link, or the first request of the browser download).
    let sourceURL: URL
    /// The page the browser was showing when the download started.
    let pageURL: URL?

    @Published var fileName: String
    @Published var state: State
    @Published var receivedBytes: Int64 = 0
    @Published var totalBytes: Int64?
    @Published var file: DownloadedFile?
    var finalURL: URL?

    // LinkDownloader items
    var task: Task<Void, Never>?
    // browser items
    var webDownload: WKDownload?
    var resumeData: Data?
    var request: URLRequest?
    var partURL: URL?
    var progressObservations: [NSKeyValueObservation] = []
    /// When the browser last said anything about this download (a delegate call or a progress change), for the stall watchdog.
    var lastEvent: TimeInterval = ProcessInfo.processInfo.systemUptime
    var restarts = 0

    init(origin: DownloadedFile.Kind, sourceURL: URL, pageURL: URL? = nil, fileName: String? = nil, state: State) {
        self.origin = origin
        self.sourceURL = sourceURL
        self.pageURL = pageURL
        self.fileName = fileName ?? Self.guessName(from: sourceURL)
        self.state = state
    }

    static func guessName(from url: URL) -> String {
        let last = url.lastPathComponent.removingPercentEncoding ?? url.lastPathComponent
        return last.isEmpty || last == "/" ? (url.host ?? "download") : last
    }

    var fraction: Double? {
        guard let total = totalBytes, total > 0 else { return nil }
        return min(1, max(0, Double(receivedBytes) / Double(total)))
    }

    var isActive: Bool { state == .downloading || state == .verifying }

    var canCancel: Bool { state == .downloading }

    var canRetry: Bool {
        switch state {
        case .failed, .cancelled: return true
        default: return false
        }
    }

    /// Can go from the list's "x": anything that is not transferring right now.
    var canRemove: Bool { state != .downloading }

    func update(_ progress: DownloadProgress) {
        guard state == .downloading else { return }
        receivedBytes = progress.receivedBytes
        totalBytes = progress.totalBytes
    }
}
