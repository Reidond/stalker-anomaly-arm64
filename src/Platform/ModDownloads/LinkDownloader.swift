import Foundation

/// Downloads a file from an http(s) link into a folder: redirects, resume, progress, the file name from Content-Disposition,
/// a free-space check, and a clear error when the link serves a web page instead of a file.
///
/// It uses its own ephemeral URLSession and never sends or stores cookies: sites that need a session (ModDB's mirrors) go
/// through the in-app browser, whose web view keeps its own cookie store.
///
/// A download that stops early (dropped connection, cancel) leaves a hidden partial file in the folder; the next
/// `download` of the same URL continues it when the server supports range requests (and has an ETag or Last-Modified to
/// prove the file did not change), otherwise starts over.
public final class LinkDownloader: Sendable {
    let reserveBytes: Int64
    let freeSpace: @Sendable (URL) -> Int64?

    public init() {
        self.reserveBytes = 128 << 20
        self.freeSpace = LinkDownloader.availableCapacity(at:)
    }

    /// `reserveBytes` is kept free on top of the file's size; `freeSpace` reports the free bytes of the volume holding a folder.
    public init(reserveBytes: Int64, freeSpace: @escaping @Sendable (URL) -> Int64?) {
        self.reserveBytes = reserveBytes
        self.freeSpace = freeSpace
    }

    /// Downloads `url` into `dir` and returns the file. `progress` gets 0...1 (it stays 0 while the server has not said how
    /// big the file is) on the main queue, at most ten times a second. Cancelling the task throws `CancellationError` and
    /// keeps the partial file for a later resume.
    public func download(_ url: URL, to dir: URL, progress: @escaping (Double) -> Void) async throws -> URL {
        let sink = UncheckedSendable(progress)
        let file = try await downloadFile(url, to: dir) { p in sink.value(p.fraction) } // already on the main queue
        return file.fileURL
    }

    /// Like `download`, but reports bytes (on the main queue) and returns the URLs, name, size and SHA-256 too.
    public func downloadFile(_ url: URL, to dir: URL, progress: @escaping @Sendable (DownloadProgress) -> Void = { _ in }) async throws -> DownloadedFile {
        guard let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https", url.host != nil else {
            throw LinkDownloadError.unsupportedScheme(url.scheme ?? "")
        }
        do {
            try LinkDownloader.prepareDirectory(dir)
        } catch {
            throw LinkDownloadError.fileSystem("Cannot use the downloads folder: \(error.localizedDescription)")
        }
        let sink = UncheckedSendable(progress)
        let onMain: @Sendable (DownloadProgress) -> Void = { p in DispatchQueue.main.async { sink.value(p) } }
        var allowResume = true
        for _ in 0..<2 {
            let transfer = Transfer(url: url, dir: dir, allowResume: allowResume, reserveBytes: reserveBytes, freeSpace: freeSpace, progress: onMain)
            do {
                return try await transfer.run()
            } catch is Transfer.RestartFresh {
                LinkDownloader.discardPartial(for: url, in: dir)
                allowResume = false
            }
        }
        throw LinkDownloadError.network("The server keeps rejecting the request.")
    }

    // MARK: Partial files

    /// Whether an interrupted download of `url` is waiting in `dir`.
    public static func hasPartial(for url: URL, in dir: URL) -> Bool {
        partialSize(for: url, in: dir) > 0
    }

    /// Bytes of an interrupted download of `url` kept in `dir` (0 when none).
    public static func partialSize(for url: URL, in dir: URL) -> Int64 {
        let size = (try? FileManager.default.attributesOfItem(atPath: partURL(for: url, in: dir).path)[.size] as? NSNumber)?.int64Value
        return size ?? 0
    }

    /// Deletes the partial file of `url`.
    public static func discardPartial(for url: URL, in dir: URL) {
        let part = partURL(for: url, in: dir)
        try? FileManager.default.removeItem(at: part)
        try? FileManager.default.removeItem(at: metaURL(for: part))
    }

    /// Deletes the partial files in `dir` that have not been touched for `age` seconds.
    public static func removeStalePartials(in dir: URL, olderThan age: TimeInterval) {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return }
        let cutoff = Date().addingTimeInterval(-age)
        for name in names where name.hasPrefix(".part-") || name.hasPrefix(".wk-") {
            let url = dir.appendingPathComponent(name)
            if let modified = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate, modified < cutoff {
                try? fm.removeItem(at: url)
            }
        }
    }

    static func partURL(for url: URL, in dir: URL) -> URL {
        dir.appendingPathComponent(".part-" + String(FileHash.sha256(of: url.absoluteString).prefix(24)))
    }

    static func metaURL(for part: URL) -> URL {
        part.appendingPathExtension("json")
    }

    // MARK: Disk

    /// Free bytes on the volume holding `url` (or its nearest existing parent), as the system counts them for user data.
    public static func availableCapacity(at url: URL) -> Int64? {
        var probe = url
        let fm = FileManager.default
        while !fm.fileExists(atPath: probe.path), probe.pathComponents.count > 1 {
            probe.deleteLastPathComponent()
        }
        guard let values = try? probe.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeAvailableCapacityKey]) else { return nil }
        if let important = values.volumeAvailableCapacityForImportantUsage, important > 0 { return important }
        return values.volumeAvailableCapacity.map(Int64.init)
    }

    /// Creates the downloads folder if needed and keeps it out of backups.
    static func prepareDirectory(_ dir: URL) throws {
        let fm = FileManager.default
        if !fm.fileExists(atPath: dir.path) {
            try fm.createDirectory(at: dir, withIntermediateDirectories: true)
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            var mutable = dir
            try? mutable.setResourceValues(values)
        }
    }
}

// MARK: - One transfer

struct PartialMeta: Codable {
    var url: String
    var etag: String?
    var lastModified: String?
    var finalURL: String
    var total: Int64?
    /// The name the first response asked for, kept so a resumed download (whose answer may lack the header) lands under it.
    var fileName: String?

    /// What `If-Range` must carry: a strong ETag, else the Last-Modified date. Nil: the file cannot be resumed safely.
    var validator: String? {
        if let etag, !etag.hasPrefix("W/") { return etag }
        return lastModified
    }
}

/// One attempt at one URL. Everything below the initialiser runs on `queue`, the URLSession delegate queue (serial).
final class Transfer: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    /// The server did not accept the range request in a usable way; start over without one.
    struct RestartFresh: Error {}

    private let url: URL
    private let dir: URL
    private let allowResume: Bool
    private let reserveBytes: Int64
    private let freeSpace: @Sendable (URL) -> Int64?
    private let progressSink: @Sendable (DownloadProgress) -> Void
    private let partURL: URL
    private let queue: OperationQueue

    private var continuation: CheckedContinuation<DownloadedFile, Error>?
    private var session: URLSession?
    private var task: URLSessionDataTask?
    private var cancelled = false
    private var failure: Error?
    private var rejectedRedirectScheme: String?

    private var resumeOffset: Int64 = 0
    private var resumeMeta: PartialMeta?
    private var finalURL: URL
    private var mimeType: String?
    private var contentDisposition: String?
    private var handle: FileHandle?
    private var received: Int64 = 0
    private var total: Int64?
    private var sniffBuffer: Data?
    private var completeFromPartial = false
    private var resolvedName: String?
    private var lastProgressTime: TimeInterval = 0

    init(url: URL, dir: URL, allowResume: Bool, reserveBytes: Int64, freeSpace: @escaping @Sendable (URL) -> Int64?, progress: @escaping @Sendable (DownloadProgress) -> Void) {
        self.url = url
        self.dir = dir
        self.allowResume = allowResume
        self.reserveBytes = reserveBytes
        self.freeSpace = freeSpace
        self.progressSink = progress
        self.partURL = LinkDownloader.partURL(for: url, in: dir)
        self.finalURL = url
        self.queue = OperationQueue()
        queue.maxConcurrentOperationCount = 1
        queue.name = "ModDownloads.Transfer"
    }

    func run() async throws -> DownloadedFile {
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (cont: CheckedContinuation<DownloadedFile, Error>) in
                queue.addOperation { self.begin(cont) }
            }
        } onCancel: {
            queue.addOperation { self.cancelRequested() }
        }
    }

    // MARK: Start and stop

    private func begin(_ cont: CheckedContinuation<DownloadedFile, Error>) {
        continuation = cont
        if cancelled {
            finish(.failure(CancellationError()))
            return
        }
        var request = URLRequest(url: url)
        request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
        if allowResume, let meta = loadMeta(), meta.url == url.absoluteString, let validator = meta.validator {
            let size = (try? FileManager.default.attributesOfItem(atPath: partURL.path)[.size] as? NSNumber)?.int64Value ?? 0
            if size > 0 {
                request.setValue("bytes=\(size)-", forHTTPHeaderField: "Range")
                request.setValue(validator, forHTTPHeaderField: "If-Range")
                resumeOffset = size
                resumeMeta = meta
            }
        }
        let config = URLSessionConfiguration.ephemeral
        config.httpCookieStorage = nil
        config.httpShouldSetCookies = false
        config.httpCookieAcceptPolicy = .never
        config.urlCache = nil
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        config.timeoutIntervalForRequest = 30
        config.timeoutIntervalForResource = 12 * 3600
        config.waitsForConnectivity = false
        let session = URLSession(configuration: config, delegate: self, delegateQueue: queue)
        self.session = session
        let task = session.dataTask(with: request)
        self.task = task
        task.resume()
    }

    private func cancelRequested() {
        cancelled = true
        task?.cancel()
    }

    /// Records why the transfer is being abandoned and stops it; `didCompleteWithError` reports it.
    private func fail(_ error: Error) {
        if failure == nil { failure = error }
        task?.cancel()
    }

    private func finish(_ result: Result<DownloadedFile, Error>) {
        session?.invalidateAndCancel()
        session = nil
        task = nil
        guard let cont = continuation else { return }
        continuation = nil
        cont.resume(with: result)
    }

    // MARK: Redirects and response

    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        guard let scheme = request.url?.scheme?.lowercased(), scheme == "http" || scheme == "https" else {
            rejectedRedirectScheme = request.url?.scheme ?? ""
            completionHandler(nil)
            return
        }
        completionHandler(request)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        let disposition = handleResponse(response)
        completionHandler(disposition)
    }

    private func handleResponse(_ response: URLResponse) -> URLSession.ResponseDisposition {
        if let scheme = rejectedRedirectScheme {
            fail(LinkDownloadError.unsupportedScheme(scheme))
            return .cancel
        }
        guard let http = response as? HTTPURLResponse else {
            fail(LinkDownloadError.network("The server did not answer with HTTP."))
            return .cancel
        }
        finalURL = http.url ?? url
        mimeType = http.mimeType
        contentDisposition = http.value(forHTTPHeaderField: "Content-Disposition")
        let status = http.statusCode
        let resuming = resumeMeta != nil

        if status == 416 && resuming {
            // The range starts at or after the end of the file: complete if the sizes agree, otherwise stale.
            if let range = http.value(forHTTPHeaderField: "Content-Range"), let size = Self.totalFromContentRange(range), size == resumeOffset {
                completeFromPartial = true
                total = size
                received = resumeOffset
                task?.cancel()
                return .cancel
            }
            fail(RestartFresh())
            return .cancel
        }
        guard status == 200 || status == 206 else {
            fail(LinkDownloadError.httpStatus(status))
            return .cancel
        }

        let disposition = FileNaming.parseContentDisposition(contentDisposition)
        let mime = http.mimeType?.lowercased()
        if !disposition.isAttachment, mime == "text/html" || mime == "application/xhtml+xml" {
            fail(LinkDownloadError.notAFile(url: finalURL, mimeType: mime))
            return .cancel
        }

        var appendFrom: Int64 = 0
        var expectedTotal: Int64?
        if status == 206 {
            guard resuming, let range = http.value(forHTTPHeaderField: "Content-Range"),
                  let start = Self.startFromContentRange(range), start == resumeOffset else {
                fail(RestartFresh())
                return .cancel
            }
            appendFrom = resumeOffset
            expectedTotal = Self.totalFromContentRange(range)
                ?? (http.expectedContentLength >= 0 ? resumeOffset + http.expectedContentLength : nil)
        } else if http.expectedContentLength >= 0 {
            expectedTotal = http.expectedContentLength
        }
        total = expectedTotal

        if let expectedTotal {
            let needed = expectedTotal - appendFrom
            if let free = freeSpace(dir), needed + reserveBytes > free {
                fail(LinkDownloadError.insufficientDiskSpace(needed: needed, available: free))
                return .cancel
            }
        }

        do {
            try openPartialFile(appendingFrom: appendFrom, http: http)
        } catch {
            fail(LinkDownloadError.fileSystem("Cannot write the download: \(error.localizedDescription)"))
            return .cancel
        }
        received = appendFrom
        // Octet-stream, plain text or no type: look at the first bytes before trusting it is a file.
        if !disposition.isAttachment, mime == nil || mime == "application/octet-stream" || mime == "binary/octet-stream" || mime == "text/plain" {
            sniffBuffer = Data()
        }
        emitProgress(force: true)
        return .allow
    }

    private func openPartialFile(appendingFrom offset: Int64, http: HTTPURLResponse) throws {
        let fm = FileManager.default
        if offset == 0 || !fm.fileExists(atPath: partURL.path) {
            fm.createFile(atPath: partURL.path, contents: nil)
        }
        let handle = try FileHandle(forWritingTo: partURL)
        if offset == 0 {
            try handle.truncate(atOffset: 0)
        } else {
            try handle.seek(toOffset: UInt64(offset))
            try handle.truncate(atOffset: UInt64(offset))
        }
        self.handle = handle
        let name = (offset > 0 ? resumeMeta?.fileName : nil)
            ?? FileNaming.fileName(contentDisposition: contentDisposition, mimeType: mimeType, finalURL: finalURL)
        resolvedName = name
        let meta = PartialMeta(url: url.absoluteString, etag: http.value(forHTTPHeaderField: "ETag") ?? (offset > 0 ? resumeMeta?.etag : nil),
                               lastModified: http.value(forHTTPHeaderField: "Last-Modified") ?? (offset > 0 ? resumeMeta?.lastModified : nil),
                               finalURL: finalURL.absoluteString, total: total, fileName: name)
        if let data = try? JSONEncoder().encode(meta) {
            try? data.write(to: LinkDownloader.metaURL(for: partURL), options: .atomic)
        }
    }

    // MARK: Data

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        guard failure == nil, !cancelled else { return }
        if var buffer = sniffBuffer {
            buffer.append(data)
            if buffer.count < 512 {
                sniffBuffer = buffer
                return
            }
            sniffBuffer = nil
            if looksLikeHTML(buffer) {
                abandonAsWebPage()
                return
            }
            write(buffer)
            return
        }
        write(data)
    }

    private func write(_ data: Data) {
        guard let handle else { return }
        do {
            try handle.write(contentsOf: data)
        } catch {
            let ns = error as NSError
            if ns.domain == NSCocoaErrorDomain && ns.code == NSFileWriteOutOfSpaceError || ns.domain == NSPOSIXErrorDomain && ns.code == Int(ENOSPC) {
                let free = freeSpace(dir) ?? 0
                fail(LinkDownloadError.insufficientDiskSpace(needed: max(0, (total ?? received) - received), available: free))
            } else {
                fail(LinkDownloadError.fileSystem("Writing the download failed: \(error.localizedDescription)"))
            }
            return
        }
        received += Int64(data.count)
        emitProgress(force: false)
    }

    private func looksLikeHTML(_ data: Data) -> Bool {
        var head = data.prefix(512)
        if head.starts(with: [0xEF, 0xBB, 0xBF]) { head = head.dropFirst(3) }
        let text = String(decoding: head, as: UTF8.self).drop(while: { $0.isWhitespace }).lowercased()
        return text.hasPrefix("<!doctype html") || text.hasPrefix("<html") || text.hasPrefix("<head") || text.hasPrefix("<body")
    }

    private func abandonAsWebPage() {
        try? handle?.close()
        handle = nil
        LinkDownloader.discardPartial(for: url, in: dir)
        fail(LinkDownloadError.notAFile(url: finalURL, mimeType: mimeType))
    }

    private func emitProgress(force: Bool) {
        let now = ProcessInfo.processInfo.systemUptime
        guard force || now - lastProgressTime >= 0.1 else { return }
        lastProgressTime = now
        progressSink(DownloadProgress(receivedBytes: received, totalBytes: total))
    }

    // MARK: Completion

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let failure {
            try? handle?.close()
            handle = nil
            finish(.failure(failure))
            return
        }
        if cancelled {
            try? handle?.close()
            handle = nil
            finish(.failure(CancellationError()))
            return
        }
        if let error, !completeFromPartial {
            try? handle?.close()
            handle = nil
            finish(.failure(Self.map(error)))
            return
        }
        if let buffer = sniffBuffer {
            sniffBuffer = nil
            if looksLikeHTML(buffer) {
                abandonAsWebPage()
                finish(.failure(failure ?? LinkDownloadError.notAFile(url: finalURL, mimeType: mimeType)))
                return
            }
            if !buffer.isEmpty { write(buffer) }
            if let failure {
                try? handle?.close()
                handle = nil
                finish(.failure(failure))
                return
            }
        }
        do {
            try handle?.synchronize()
            try handle?.close()
        } catch {
            handle = nil
            finish(.failure(LinkDownloadError.fileSystem("Closing the download failed: \(error.localizedDescription)")))
            return
        }
        handle = nil
        if let total, received != total {
            finish(.failure(LinkDownloadError.incomplete(received: received, expected: total)))
            return
        }
        emitProgress(force: true)
        finish(finalize())
    }

    private func finalize() -> Result<DownloadedFile, Error> {
        do {
            let name = resolvedName ?? resumeMeta?.fileName
                ?? FileNaming.fileName(contentDisposition: contentDisposition, mimeType: mimeType, finalURL: finalURL)
            let digest = try FileHash.sha256(of: partURL)
            let size = (try FileManager.default.attributesOfItem(atPath: partURL.path)[.size] as? NSNumber)?.int64Value ?? received
            let target = try FileNaming.moveUnique(partURL, to: dir, name: name)
            try? FileManager.default.removeItem(at: LinkDownloader.metaURL(for: partURL))
            return .success(DownloadedFile(fileURL: target, originalURL: url, finalURL: finalURL, fileName: target.lastPathComponent,
                                           sha256: digest, size: size, kind: .link))
        } catch {
            return .failure(LinkDownloadError.fileSystem("Finishing the download failed: \(error.localizedDescription)"))
        }
    }

    private func loadMeta() -> PartialMeta? {
        guard let data = try? Data(contentsOf: LinkDownloader.metaURL(for: partURL)) else { return nil }
        return try? JSONDecoder().decode(PartialMeta.self, from: data)
    }

    // MARK: Helpers

    static func map(_ error: Error) -> Error {
        let ns = error as NSError
        guard ns.domain == NSURLErrorDomain else { return LinkDownloadError.network(error.localizedDescription) }
        if ns.code == NSURLErrorCancelled { return CancellationError() }
        if ns.code == NSURLErrorAppTransportSecurityRequiresSecureConnection {
            return LinkDownloadError.network("This server only offers a plain http connection, which iPadOS blocks. Try the https version of the link.")
        }
        return LinkDownloadError.network(ns.localizedDescription)
    }

    /// "bytes 100-199/1000" -> 100
    static func startFromContentRange(_ value: String) -> Int64? {
        let trimmed = value.trimmingCharacters(in: .whitespaces)
        guard trimmed.lowercased().hasPrefix("bytes ") else { return nil }
        let range = trimmed.dropFirst(6).split(separator: "/", maxSplits: 1)[0]
        return Int64(range.split(separator: "-", maxSplits: 1).first.map(String.init) ?? "")
    }

    /// "bytes 100-199/1000" and "bytes */1000" -> 1000; "bytes 0-9/*" -> nil
    static func totalFromContentRange(_ value: String) -> Int64? {
        guard let slash = value.lastIndex(of: "/") else { return nil }
        return Int64(value[value.index(after: slash)...].trimmingCharacters(in: .whitespaces))
    }
}
