import CArchive
import CryptoKit
import Foundation
@testable import ModManagerCore

// Fixture archives are written at test time with libarchive's own writers (zip, 7-zip), so nothing binary is committed.
// RAR files cannot be made without non-free tools: those come from libarchive's test directory.

enum FixtureFormat {
    case zip
    case sevenZip(compression: String)
}

struct FixtureEntry {
    enum Kind { case file, directory, symlink(String) }
    var path: String
    var data: Data
    var kind: Kind = .file

    init(_ path: String, _ text: String) { self.path = path; data = Data(text.utf8) }
    init(_ path: String, data: Data) { self.path = path; self.data = data }
    init(directory path: String) { self.path = path; data = Data(); kind = .directory }
    init(_ path: String, symlinkTo target: String) { self.path = path; data = Data(); kind = .symlink(target) }
}

func writeArchive(_ entries: [FixtureEntry], format: FixtureFormat = .zip, to url: URL) throws {
    let locale = ThreadUTF8Locale()   // libarchive converts names with the thread's locale
    defer { _ = locale }
    guard let a = archive_write_new() else { throw TestFailure(message: "archive_write_new failed") }
    defer { archive_write_free(a) }
    switch format {
    case .zip:
        archive_write_set_format_zip(a)
        archive_write_set_options(a, "zip:compression=deflate")
    case .sevenZip(let compression):
        archive_write_set_format_7zip(a)
        archive_write_set_options(a, "7zip:compression=\(compression)")
    }
    guard archive_write_open_filename(a, url.path) == ARCHIVE_OK else {
        throw TestFailure(message: "cannot create \(url.path): \(String(cString: archive_error_string(a)))")
    }
    for item in entries {
        guard let e = archive_entry_new() else { throw TestFailure(message: "archive_entry_new failed") }
        defer { archive_entry_free(e) }
        archive_entry_set_pathname(e, item.path)
        switch item.kind {
        case .file:
            archive_entry_set_filetype(e, 0o100000)
            archive_entry_set_perm(e, 0o644)
            archive_entry_set_size(e, Int64(item.data.count))
        case .directory:
            archive_entry_set_filetype(e, 0o040000)
            archive_entry_set_perm(e, 0o755)
            archive_entry_set_size(e, 0)
        case .symlink(let target):
            archive_entry_set_filetype(e, 0o120000)
            archive_entry_set_perm(e, 0o777)
            archive_entry_set_symlink(e, target)
            archive_entry_set_size(e, 0)
        }
        archive_entry_set_mtime(e, 1_700_000_000, 0)
        guard archive_write_header(a, e) == ARCHIVE_OK else {
            throw TestFailure(message: "header \(item.path): \(String(cString: archive_error_string(a)))")
        }
        if case .file = item.kind, !item.data.isEmpty {
            let written = item.data.withUnsafeBytes { archive_write_data(a, $0.baseAddress, $0.count) }
            guard written == item.data.count else { throw TestFailure(message: "data \(item.path)") }
        }
    }
    guard archive_write_close(a) == ARCHIVE_OK else { throw TestFailure(message: "close: \(String(cString: archive_error_string(a)))") }
}

/// Plain folder with the same description as an archive.
func writeFolder(_ entries: [FixtureEntry], to url: URL) throws {
    let fm = FileManager.default
    try fm.createDirectory(at: url, withIntermediateDirectories: true)
    for item in entries {
        let target = url.appendingPathComponent(item.path)
        switch item.kind {
        case .directory: try fm.createDirectory(at: target, withIntermediateDirectories: true)
        case .symlink(let dest):
            try fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try fm.createSymbolicLink(atPath: target.path, withDestinationPath: dest)
        case .file:
            try fm.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try item.data.write(to: target)
        }
    }
}

func sha256Hex(_ url: URL) throws -> String {
    SHA256.hash(data: try Data(contentsOf: url)).map { String(format: "%02x", $0) }.joined()
}

/// The text of `path` below a mod's content folder, or nil.
func contentText(_ mod: InstalledModProbe, _ path: String) -> String? {
    try? String(contentsOf: mod.content.appendingPathComponent(path), encoding: .utf8)
}

struct InstalledModProbe {
    let content: URL
    func exists(_ path: String) -> Bool { FileManager.default.fileExists(atPath: content.appendingPathComponent(path).path) }
    /// Every file below content, relative, sorted.
    func allFiles() -> [String] {
        var out: [String] = []
        let base = content.resolvingSymlinksInPath().path
        if let e = FileManager.default.enumerator(at: content.resolvingSymlinksInPath(), includingPropertiesForKeys: [.isDirectoryKey]) {
            for case let url as URL in e where (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) != true {
                out.append(String(url.resolvingSymlinksInPath().path.dropFirst(base.count + 1)))
            }
        }
        return out.sorted()
    }
}

// MARK: - libarchive's own RAR fixtures

func uudecode(_ text: String) -> Data? {
    var out = Data()
    var started = false
    for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
        if line.hasPrefix("begin ") { started = true; continue }
        if !started { continue }
        if line == "end" { break }
        let bytes = Array(line.utf8)
        guard let first = bytes.first else { continue }
        let count = Int((first &- 32) & 0x3f)
        if count == 0 { continue }
        var decoded: [UInt8] = []
        var i = 1
        while decoded.count < count {
            func sextet(_ k: Int) -> UInt8 { i + k < bytes.count ? (bytes[i + k] &- 32) & 0x3f : 0 }
            let c0 = sextet(0), c1 = sextet(1), c2 = sextet(2), c3 = sextet(3)
            decoded.append((c0 << 2) | (c1 >> 4))
            decoded.append((c1 << 4) | (c2 >> 2))
            decoded.append((c2 << 6) | c3)
            i += 4
        }
        out.append(contentsOf: decoded.prefix(count))
    }
    return out
}

/// A libarchive test file (e.g. "test_read_format_rar.rar") decoded into `dir`, or nil when the test directory is unknown.
func libarchiveFixture(_ name: String, into dir: URL) throws -> URL? {
    guard let root = ProcessInfo.processInfo.environment["ANOMALY_LIBARCHIVE_TEST_DIR"] else { return nil }
    let source = URL(fileURLWithPath: root).appendingPathComponent(name + ".uu")
    guard let text = try? String(contentsOf: source, encoding: .isoLatin1), let data = uudecode(text) else {
        throw TestFailure(message: "cannot read libarchive fixture \(source.path)")
    }
    let target = dir.appendingPathComponent(name)
    try data.write(to: target)
    return target
}
