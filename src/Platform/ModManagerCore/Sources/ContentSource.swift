import Foundation

/// One file or folder inside an archive or source folder, path already sanitized (`/` separators, no `..`).
struct SourceEntry {
    /// Position among the source's headers; the extraction pass pairs on it.
    let index: Int
    let path: String
    let size: Int64
    let isDirectory: Bool
}

/// Where a mod's files come from: an archive or a plain folder.
protocol ContentSource: AnyObject {
    var url: URL { get }
    var isArchive: Bool { get }

    /// Lists the entries without extracting anything (macOS metadata junk is left out). Refuses links and paths that leave
    /// the root. `capture` may name a place to copy an entry's bytes to (the FOMOD config and its images).
    func list(cancel: CancelFlag, capture: (SourceEntry) -> URL?) throws -> [SourceEntry]

    /// Writes `jobs` (entry index -> destinations relative to `content`) below `content`. `progress` gets byte deltas.
    func extract(jobs: [Int: [String]], entries: [SourceEntry], into content: URL, cancel: CancelFlag,
                 progress: (Int64) -> Void) throws
}

let maxArchiveEntries = 400_000

// MARK: - Archive

final class ArchiveContentSource: ContentSource {
    let url: URL
    var isArchive: Bool { true }

    init(url: URL) { self.url = url }

    func list(cancel: CancelFlag, capture: (SourceEntry) -> URL?) throws -> [SourceEntry] {
        let reader = try ArchiveReader(path: url.path)
        var entries: [SourceEntry] = []
        var seq = 0
        while let header = try reader.nextHeader() {
            try cancel.check()
            defer { seq += 1 }
            if seq >= maxArchiveEntries { throw ModImportError.tooManyEntries(seq) }
            switch header.kind {
            case .symlink, .hardlink: throw ModImportError.symlinkRefused(header.path)
            case .other: throw ModImportError.unsupportedEntry(header.path)
            case .file, .directory: break
            }
            if header.isEncrypted { throw ModImportError.encrypted }
            guard let path = try ModPath.sanitize(header.path), !ModPath.isJunk(path) else {
                try reader.skipData()
                continue
            }
            if header.kind == .directory {
                entries.append(SourceEntry(index: seq, path: path, size: 0, isDirectory: true))
                continue
            }
            let entry = SourceEntry(index: seq, path: path, size: header.size ?? 0, isDirectory: false)
            entries.append(entry)
            if let target = capture(entry) {
                try StagedWriter.copyOut(reader: reader, to: target, cancel: cancel)
            } else {
                try reader.skipData()
            }
        }
        return entries
    }

    func extract(jobs: [Int: [String]], entries: [SourceEntry], into content: URL, cancel: CancelFlag,
                 progress: (Int64) -> Void) throws {
        guard !jobs.isEmpty else { return }
        let byIndex = Dictionary(entries.map { ($0.index, $0) }, uniquingKeysWith: { a, _ in a })
        let reader = try ArchiveReader(path: url.path)
        let writer = StagedWriter(root: content)
        var remaining = jobs.count
        var seq = 0
        while let header = try reader.nextHeader() {
            try cancel.check()
            let current = seq
            seq += 1
            guard let destinations = jobs[current], let planned = byIndex[current] else {
                try reader.skipData()
                continue
            }
            guard header.kind == .file, (try? ModPath.sanitize(header.path)) == planned.path else {
                throw ModImportError.archiveChanged
            }
            try writer.write(to: destinations[0], from: reader, cancel: cancel, progress: progress)
            for extra in destinations.dropFirst() {
                try writer.duplicate(from: destinations[0], to: extra)
            }
            remaining -= 1
            if remaining == 0 { return }   // the rest of the archive is not needed
        }
        if remaining > 0 { throw ModImportError.corruptArchive("the archive ended before all files were found") }
    }
}

// MARK: - Folder

final class FolderContentSource: ContentSource {
    let url: URL
    var isArchive: Bool { false }
    private var files: [Int: URL] = [:]

    init(url: URL) { self.url = url }

    func list(cancel: CancelFlag, capture: (SourceEntry) -> URL?) throws -> [SourceEntry] {
        var entries: [SourceEntry] = []
        var seq = 0
        files = [:]
        try walk(url, relative: "") { child, relative, isDirectory, size in
            try cancel.check()
            if seq >= maxArchiveEntries { throw ModImportError.tooManyEntries(seq) }
            guard let path = try ModPath.sanitize(relative), !ModPath.isJunk(path) else { return }
            let entry = SourceEntry(index: seq, path: path, size: size, isDirectory: isDirectory)
            seq += 1
            entries.append(entry)
            if !isDirectory {
                files[entry.index] = child
                if let target = capture(entry) {
                    try StagedWriter.copyFile(from: child, to: target, cancel: cancel, progress: { _ in })
                }
            }
        }
        return entries
    }

    private func walk(_ dir: URL, relative: String,
                      visit: (URL, String, Bool, Int64) throws -> Void) throws {
        let keys: [URLResourceKey] = [.isDirectoryKey, .isSymbolicLinkKey, .fileSizeKey]
        let children: [URL]
        do {
            children = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: keys, options: [])
        } catch {
            throw ModImportError.io("The folder cannot be read: \(error.localizedDescription)")
        }
        for child in children.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            let values = try? child.resourceValues(forKeys: Set(keys))
            let name = child.lastPathComponent
            let rel = relative.isEmpty ? name : relative + "/" + name
            if values?.isSymbolicLink == true { throw ModImportError.symlinkRefused(rel) }
            if values?.isDirectory == true {
                try visit(child, rel, true, 0)
                try walk(child, relative: rel, visit: visit)
            } else {
                try visit(child, rel, false, Int64(values?.fileSize ?? 0))
            }
        }
    }

    func extract(jobs: [Int: [String]], entries: [SourceEntry], into content: URL, cancel: CancelFlag,
                 progress: (Int64) -> Void) throws {
        let writer = StagedWriter(root: content)
        for entry in entries where !entry.isDirectory {
            guard let destinations = jobs[entry.index] else { continue }
            try cancel.check()
            guard let source = files[entry.index], FileManager.default.fileExists(atPath: source.path) else {
                throw ModImportError.archiveChanged
            }
            try writer.copy(from: source, to: destinations[0], cancel: cancel, progress: progress)
            for extra in destinations.dropFirst() {
                try writer.duplicate(from: destinations[0], to: extra)
            }
        }
    }
}

// MARK: - Writing

/// Creates files below one root. Everything it is asked for was sanitized before, and the root starts out empty, so there
/// are no links to follow; files are still opened with O_NOFOLLOW.
final class StagedWriter {
    let root: URL
    private var madeDirectories = Set<String>()

    init(root: URL) { self.root = root }

    private func url(for relative: String) -> URL { root.appendingPathComponent(relative) }

    private func makeParents(of relative: String) throws {
        let parent = url(for: relative).deletingLastPathComponent()
        if madeDirectories.contains(parent.path) { return }
        do {
            try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
        } catch {
            throw ModImportError.corruptArchive("a file and a folder share the name \(relative)")
        }
        madeDirectories.insert(parent.path)
    }

    /// Streams the reader's current entry into `relative`.
    func write(to relative: String, from reader: ArchiveReader, cancel: CancelFlag, progress: (Int64) -> Void) throws {
        try makeParents(of: relative)
        let fd = try Self.openForWrite(url(for: relative))
        defer { close(fd) }
        var position: Int64 = 0
        try reader.readData { block, offset in
            try cancel.check()
            if offset != position {
                if lseek(fd, off_t(offset), SEEK_SET) < 0 { throw ModImportError.io("seek failed") }
                position = offset
            }
            try Self.writeAll(fd, block)
            position += Int64(block.count)
            progress(Int64(block.count))
        }
    }

    func copy(from source: URL, to relative: String, cancel: CancelFlag, progress: (Int64) -> Void) throws {
        try makeParents(of: relative)
        try Self.copyFile(from: source, to: url(for: relative), cancel: cancel, progress: progress)
    }

    /// A second destination for bytes that are already on disk: clone when the volume can, copy otherwise.
    func duplicate(from existing: String, to relative: String) throws {
        try makeParents(of: relative)
        let src = url(for: existing), dst = url(for: relative)
        try? FileManager.default.removeItem(at: dst)
        do { try FileManager.default.copyItem(at: src, to: dst) } catch {
            throw ModImportError.io("A file cannot be copied: \(error.localizedDescription)")
        }
    }

    static func openForWrite(_ url: URL) throws -> Int32 {
        let fd = open(url.path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0o644)
        if fd < 0 {
            if errno == ENOSPC { throw ModImportError.diskFull }
            throw ModImportError.io("A file cannot be created: \(String(cString: strerror(errno)))")
        }
        return fd
    }

    static func writeAll(_ fd: Int32, _ block: UnsafeRawBufferPointer) throws {
        var written = 0
        while written < block.count {
            let n = Darwin.write(fd, block.baseAddress! + written, block.count - written)
            if n < 0 {
                if errno == EINTR { continue }
                if errno == ENOSPC || errno == EDQUOT { throw ModImportError.diskFull }
                throw ModImportError.io("Writing failed: \(String(cString: strerror(errno)))")
            }
            written += n
        }
    }

    /// Copies the reader's current entry to an arbitrary file (FOMOD capture).
    static func copyOut(reader: ArchiveReader, to target: URL, cancel: CancelFlag) throws {
        try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
        let fd = try openForWrite(target)
        defer { close(fd) }
        var position: Int64 = 0
        try reader.readData { block, offset in
            try cancel.check()
            if offset != position {
                if lseek(fd, off_t(offset), SEEK_SET) < 0 { throw ModImportError.io("seek failed") }
                position = offset
            }
            try writeAll(fd, block)
            position += Int64(block.count)
        }
    }

    static func copyFile(from source: URL, to target: URL, cancel: CancelFlag, progress: (Int64) -> Void) throws {
        try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
        // Same volume on APFS: a clone costs nothing.
        try? FileManager.default.removeItem(at: target)
        if clonefile(source.path, target.path, 0) == 0 {
            let size = (try? FileManager.default.attributesOfItem(atPath: target.path)[.size] as? Int64) ?? nil
            progress(size ?? 0)
            return
        }
        let input = open(source.path, O_RDONLY | O_CLOEXEC)
        if input < 0 { throw ModImportError.io("A file cannot be read: \(source.lastPathComponent)") }
        defer { close(input) }
        let output = try openForWrite(target)
        defer { close(output) }
        let chunk = 1 << 20
        let buffer = UnsafeMutableRawPointer.allocate(byteCount: chunk, alignment: 16)
        defer { buffer.deallocate() }
        while true {
            try cancel.check()
            let n = Darwin.read(input, buffer, chunk)
            if n < 0 {
                if errno == EINTR { continue }
                throw ModImportError.io("Reading failed: \(String(cString: strerror(errno)))")
            }
            if n == 0 { break }
            try writeAll(output, UnsafeRawBufferPointer(start: buffer, count: n))
            progress(Int64(n))
        }
    }
}
