import Foundation

/// A finished download, handed to the importer. The URLs and the hash are what the launcher writes into `mod.json`'s "source".
public struct DownloadedFile: Sendable, Equatable {
    public enum Kind: String, Sendable {
        /// A pasted link fetched by `LinkDownloader` (mod.json source kind "url").
        case link
        /// A file the in-app browser downloaded (mod.json source kind "browser").
        case browser
    }

    /// The file in the downloads folder. The importer deletes it after a successful install.
    public let fileURL: URL
    /// The URL the user asked for: the pasted link, or the first request of the download in the browser.
    public let originalURL: URL
    /// The URL that served the bytes, after every redirect.
    public let finalURL: URL
    /// The page the browser was showing when the download started (browser downloads only).
    public let pageURL: URL?
    public let fileName: String
    /// Lower-case hex SHA-256 of the file.
    public let sha256: String
    public let size: Int64
    public let kind: Kind

    public init(fileURL: URL, originalURL: URL, finalURL: URL, pageURL: URL? = nil, fileName: String, sha256: String, size: Int64, kind: Kind) {
        self.fileURL = fileURL
        self.originalURL = originalURL
        self.finalURL = finalURL
        self.pageURL = pageURL
        self.fileName = fileName
        self.sha256 = sha256
        self.size = size
        self.kind = kind
    }
}

/// Bytes received so far and, when the server said, the total.
public struct DownloadProgress: Sendable, Equatable {
    public var receivedBytes: Int64
    public var totalBytes: Int64?

    public init(receivedBytes: Int64, totalBytes: Int64?) {
        self.receivedBytes = receivedBytes
        self.totalBytes = totalBytes
    }

    /// 0...1; stays 0 while the total is unknown.
    public var fraction: Double {
        guard let total = totalBytes, total > 0 else { return 0 }
        return min(1, max(0, Double(receivedBytes) / Double(total)))
    }
}

public enum LinkDownloadError: Error, Equatable, LocalizedError {
    /// The link (or a redirect) is not http or https.
    case unsupportedScheme(String)
    /// The link served a web page, not a file. The view opens `url` in the browser.
    case notAFile(url: URL, mimeType: String?)
    case httpStatus(Int)
    case insufficientDiskSpace(needed: Int64, available: Int64)
    /// The connection ended early; the partial file is kept and the next attempt resumes it where the server allows.
    case incomplete(received: Int64, expected: Int64)
    case network(String)
    case fileSystem(String)

    public var errorDescription: String? {
        switch self {
        case .unsupportedScheme(let scheme):
            return "Only http and https links can be downloaded (got \"\(scheme)\")."
        case .notAFile:
            return "That link is a web page, not a file."
        case .httpStatus(let code):
            return "The server answered \(code) \(HTTPURLResponse.localizedString(forStatusCode: code))."
        case .insufficientDiskSpace(let needed, let available):
            let fmt = ByteCountFormatter()
            return "Not enough free space: the file needs \(fmt.string(fromByteCount: needed)), \(fmt.string(fromByteCount: max(0, available))) available."
        case .incomplete(let received, let expected):
            return "The connection dropped after \(received) of \(expected) bytes."
        case .network(let message):
            return message
        case .fileSystem(let message):
            return message
        }
    }
}

/// Wraps a value that is only touched from one queue at a time (URLSession's delegate queue, or the main actor).
struct UncheckedSendable<T>: @unchecked Sendable {
    var value: T
    init(_ value: T) { self.value = value }
}
