import CArchive
import Foundation

/// Thread-safe cancel flag shared between a Task's cancellation handler and the blocking extraction loop.
final class CancelFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var flag = false
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return flag }
    func cancel() { lock.lock(); flag = true; lock.unlock() }
    func check() throws { if isCancelled { throw CancellationError() } }
}

/// libarchive converts names with the calling thread's locale, and a game process runs in the "C" locale, which has no room for
/// non-ASCII names (a RAR with Cyrillic or Japanese file names would lose them). The reader therefore runs under a UTF-8
/// locale for its own thread only (`uselocale`), and puts the previous one back.
final class ThreadUTF8Locale {
    private static let locale: locale_t? = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nil) ?? newlocale(LC_CTYPE_MASK, "UTF-8", nil)
    private let previous: locale_t?

    init() {
        if let locale = Self.locale { previous = uselocale(locale) } else { previous = nil }
    }

    deinit {
        if let previous { _ = uselocale(previous) }
    }
}

/// One header of an archive, as libarchive returned it.
struct ArchiveHeader {
    enum Kind { case file, directory, symlink, hardlink, other }
    let kind: Kind
    /// Raw path (not sanitized).
    let path: String
    /// nil when the format does not know the size before the data is read.
    let size: Int64?
    let isEncrypted: Bool
}

/// A forward-only libarchive reader (zip, 7z, RAR 4 and 5). No entry is ever held in memory as a whole: data comes in the
/// blocks libarchive decodes.
final class ArchiveReader {
    private var handle: OpaquePointer?
    private var entry: OpaquePointer?
    private let path: String
    private let locale = ThreadUTF8Locale()   // restored when the reader goes away, on the same thread

    init(path: String) throws {
        self.path = path
        guard let a = archive_read_new() else { throw ModImportError.io("libarchive could not be started.") }
        handle = a
        archive_read_support_format_zip(a)
        archive_read_support_format_7zip(a)
        archive_read_support_format_rar(a)
        archive_read_support_format_rar5(a)
        // zip and 7z names are UTF-8 in practice; a name that is not gets read as raw bytes below.
        _ = archive_read_set_options(a, "hdrcharset=UTF-8")
        let status = archive_read_open_filename(a, path, 1 << 20)
        if status != ARCHIVE_OK {
            let message = lastError()
            archive_read_free(a)
            handle = nil
            throw Self.classify(message, opening: true)
        }
    }

    deinit {
        if let handle { archive_read_free(handle) }
    }

    private func lastError() -> String {
        guard let handle, let c = archive_error_string(handle) else { return "unknown error" }
        return String(cString: c)
    }

    static func classify(_ message: String, opening: Bool) -> ModImportError {
        let lower = message.lowercased()
        if lower.contains("passphrase") || lower.contains("encrypted") || lower.contains("password") { return .encrypted }
        if opening, lower.contains("unrecognized archive format") || lower.contains("unsupported") && lower.contains("format") {
            return .unsupportedFormat("unrecognized archive format")
        }
        return .corruptArchive(message)
    }

    /// The next header, or nil at the end of the archive.
    func nextHeader() throws -> ArchiveHeader? {
        guard let handle else { return nil }
        var retries = 0
        while true {
            var raw: OpaquePointer?
            let status = archive_read_next_header(handle, &raw)
            switch status {
            case ARCHIVE_EOF:
                return nil
            case ARCHIVE_OK, ARCHIVE_WARN:
                guard let raw else { return nil }
                entry = raw
                return Self.header(of: raw)
            case ARCHIVE_RETRY where retries < 3:
                retries += 1
            default:
                throw Self.classify(lastError(), opening: false)
            }
        }
    }

    private static func header(of e: OpaquePointer) -> ArchiveHeader {
        let mode = Int(archive_entry_filetype(e))
        var kind: ArchiveHeader.Kind
        switch mode & 0o170000 {
        case 0o100000: kind = .file
        case 0o040000: kind = .directory
        case 0o120000: kind = .symlink
        default: kind = .other
        }
        if archive_entry_hardlink(e) != nil { kind = .hardlink }
        if archive_entry_symlink(e) != nil, kind != .hardlink { kind = .symlink }
        var raw: String?
        if let c = archive_entry_pathname_utf8(e) {
            raw = String(cString: c)
        } else if let c = archive_entry_pathname(e) {
            // not valid UTF-8: keep the bytes distinct instead of dropping them
            let bytes = UnsafeBufferPointer(start: UnsafeRawPointer(c).assumingMemoryBound(to: UInt8.self), count: strlen(c))
            raw = String(validatingUTF8: c) ?? String(String.UnicodeScalarView(bytes.map { Unicode.Scalar($0) }))
        }
        let size: Int64? = archive_entry_size_is_set(e) != 0 ? archive_entry_size(e) : nil
        return ArchiveHeader(kind: kind, path: raw ?? "", size: size, isEncrypted: archive_entry_is_encrypted(e) != 0)
    }

    /// Streams the current entry's data. `body` gets each decoded block with the offset it belongs at.
    func readData(_ body: (UnsafeRawBufferPointer, Int64) throws -> Void) throws {
        guard let handle else { return }
        while true {
            var buffer: UnsafeRawPointer?
            var size = 0
            var offset: Int64 = 0
            let status = archive_read_data_block(handle, &buffer, &size, &offset)
            switch status {
            case ARCHIVE_EOF:
                return
            case ARCHIVE_OK:
                if let buffer, size > 0 { try body(UnsafeRawBufferPointer(start: buffer, count: size), offset) }
            default:
                // ARCHIVE_WARN while reading data is a failed checksum or a damaged stream: do not install such a file.
                throw Self.classify(lastError(), opening: false)
            }
        }
    }

    func skipData() throws {
        guard let handle else { return }
        let status = archive_read_data_skip(handle)
        if status != ARCHIVE_OK && status != ARCHIVE_WARN && status != ARCHIVE_EOF {
            throw Self.classify(lastError(), opening: false)
        }
    }
}
