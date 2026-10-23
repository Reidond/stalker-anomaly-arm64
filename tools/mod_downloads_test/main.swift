// Host tests of ModDownloads. Exit code 0 when every check passes.
//   mod_downloads_test              all tests
//   mod_downloads_test <substring>  only the tests whose name contains it
import CryptoKit
import Foundation
@testable import ModDownloads

// MARK: - Tiny test runner

final class Results: @unchecked Sendable {
    let lock = NSLock()
    var passed = 0
    var failed = 0
    var currentTest = ""
    func record(_ ok: Bool, _ message: String, _ location: String) {
        lock.lock(); defer { lock.unlock() }
        if ok { passed += 1 } else {
            failed += 1
            print("  FAIL [\(currentTest)] \(message) (\(location))")
        }
    }
}

let results = Results()

func check(_ condition: @autoclosure () -> Bool, _ message: String, file: String = #fileID, line: Int = #line) {
    results.record(condition(), message, "\(file):\(line)")
}

func checkEqual<T: Equatable>(_ a: @autoclosure () -> T, _ b: @autoclosure () -> T, _ message: String, file: String = #fileID, line: Int = #line) {
    let (x, y) = (a(), b())
    results.record(x == y, "\(message): \(x) != \(y)", "\(file):\(line)")
}

let filter = CommandLine.arguments.dropFirst().first
final class Counter: @unchecked Sendable { var value = 0 }
let testCount = Counter()

func test(_ name: String, _ body: () async throws -> Void) async {
    if let filter, !name.contains(filter) { return }
    testCount.value += 1
    results.currentTest = name
    let before = results.failed
    do { try await body() } catch {
        results.record(false, "threw \(error)", "test body")
    }
    print("\(results.failed == before ? "ok  " : "FAIL") \(name)")
}

// Nothing here should take more than a couple of seconds; a hang is a failure.
let watchdog = Thread {
    Thread.sleep(forTimeInterval: 180)
    print("FAIL watchdog: the tests did not finish in 180 s")
    exit(2)
}
watchdog.start()

// MARK: - Fixtures

func makeBody(_ count: Int, seed: Int = 1) -> Data {
    Data((0..<count).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ seed &* 7 &+ ($0 >> 8)) })
}

func sha256(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

func visibleFiles(in dir: URL) -> [String] {
    ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []).filter { !$0.hasPrefix(".") }.sorted()
}

func hiddenFiles(in dir: URL) -> [String] {
    ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []).filter { $0.hasPrefix(".") }.sorted()
}

let tempRoot = FileManager.default.temporaryDirectory.appendingPathComponent("mod_downloads_test-\(UUID().uuidString)")
try FileManager.default.createDirectory(at: tempRoot, withIntermediateDirectories: true)
var dirCounter = 0
func freshDir() -> URL {
    dirCounter += 1
    return tempRoot.appendingPathComponent("d\(dirCounter)/nested/.downloads")
}

/// Server state shared with the handler thread.
final class ServerState: @unchecked Sendable {
    let lock = NSLock()
    var counts: [String: Int] = [:]
    var changingETag = "\"v1\""
    func bump(_ key: String) -> Int {
        lock.lock(); defer { lock.unlock() }
        counts[key, default: 0] += 1
        return counts[key]!
    }
    func reset(_ key: String) {
        lock.lock(); counts[key] = 0; lock.unlock()
    }
}

let state = ServerState()
let fileBody = makeBody(300_000)
let newBody = makeBody(300_000, seed: 9)
let slowBody = makeBody(400_000, seed: 3)

struct FileRoute {
    var body: Data
    var type = "application/zip"
    var etag: String? = "\"v1\""
    var ranges = true
    var extra: [(String, String)] = []
    var chunk = 0           // > 0: send in pieces of this size
    var delay: TimeInterval = 0
    var dropAfter: Int?     // send only this many body bytes, then close the connection
    var chunked = false     // Transfer-Encoding: chunked, no Content-Length
}

func serve(_ route: FileRoute, _ req: TestServer.Request, _ conn: TestServer.Connection) {
    var status = 200
    var body = route.body
    var headers: [(String, String)] = [("Content-Type", route.type)] + route.extra
    if let etag = route.etag { headers.append(("ETag", etag)) }
    if route.ranges { headers.append(("Accept-Ranges", "bytes")) }
    let total = route.body.count
    if route.ranges, let range = req.headers["range"], range.hasPrefix("bytes="), let start = Int(range.dropFirst(6).split(separator: "-", omittingEmptySubsequences: false)[0]) {
        let validatorOK = req.headers["if-range"].map { $0 == route.etag } ?? true
        if validatorOK {
            if start >= total {
                conn.respond(416, [("Content-Range", "bytes */\(total)")])
                return
            }
            status = 206
            body = route.body.subdata(in: start..<total)
            headers.append(("Content-Range", "bytes \(start)-\(total - 1)/\(total)"))
        }
    }
    if route.chunked {
        headers.append(("Transfer-Encoding", "chunked"))
        conn.head(status, headers)
        var offset = 0
        while offset < body.count {
            let piece = body.subdata(in: offset..<min(body.count, offset + 50_000))
            conn.write(Data("\(String(piece.count, radix: 16))\r\n".utf8) + piece + Data("\r\n".utf8))
            offset += piece.count
        }
        conn.write(Data("0\r\n\r\n".utf8))
        return
    }
    headers.append(("Content-Length", String(body.count)))
    conn.head(status, headers)
    let limit = route.dropAfter.map { min($0, body.count) } ?? body.count
    var offset = 0
    let step = route.chunk > 0 ? route.chunk : max(1, limit)
    while offset < limit {
        let end = min(limit, offset + step)
        if !conn.write(body.subdata(in: offset..<end)) { return }
        offset = end
        if route.delay > 0 { Thread.sleep(forTimeInterval: route.delay) }
    }
}

final class PortBox: @unchecked Sendable { var value: UInt16 = 0 }
let portBox = PortBox()

let server = try TestServer { req, conn in
    let base = "http://127.0.0.1:\(portBox.value)"
    switch req.path {
    case "/file.zip":
        serve(FileRoute(body: fileBody, extra: [("Set-Cookie", "session=abc; Path=/")]), req, conn)
    case "/r1":
        conn.respond(302, [("Location", "\(base)/r2")])
    case "/r2":
        conn.respond(301, [("Location", "\(base)/file.zip")])
    case "/cd-utf8":
        serve(FileRoute(body: fileBody, type: "application/octet-stream",
                        extra: [("Content-Disposition", "attachment; filename=\"fallback.zip\"; filename*=UTF-8''na%C3%AFve%20mod%20%E2%9C%93.zip")]), req, conn)
    case "/cd-quoted":
        serve(FileRoute(body: fileBody, extra: [("Content-Disposition", "attachment; filename=\"my \\\"mod\\\" v1; final.zip\"")]), req, conn)
    case "/cd-evil":
        serve(FileRoute(body: fileBody, extra: [("Content-Disposition", "attachment; filename=\"../../evil.zip\"")]), req, conn)
    case "/cd-redirect":
        conn.respond(302, [("Location", "\(base)/cd-utf8")])
    case "/page":
        conn.respond(200, [("Content-Type", "text/html; charset=utf-8")], body: Data("<!DOCTYPE html><html><body>Download page</body></html>".utf8))
    case "/page-redirect":
        conn.respond(302, [("Location", "\(base)/page")])
    case "/html-octet":
        conn.respond(200, [("Content-Type", "application/octet-stream")], body: Data("  \n<!doctype HTML>\n<html><head><title>Not a file</title></head></html>".utf8))
    case "/html-tiny":
        conn.respond(200, [("Content-Type", "application/octet-stream")], body: Data("<html></html>".utf8))
    case "/html-attachment":
        conn.respond(200, [("Content-Type", "text/html"), ("Content-Disposition", "attachment; filename=page.html")], body: Data("<html>saved</html>".utf8))
    case "/404":
        conn.respond(404, [("Content-Type", "text/plain")], body: Data("nope".utf8))
    case "/chunked.bin":
        serve(FileRoute(body: fileBody, type: "application/octet-stream", etag: nil, ranges: false, chunked: true), req, conn)
    case "/redirect-ftp":
        conn.respond(302, [("Location", "ftp://example.invalid/file.zip")])
    case "/redirect-file":
        conn.respond(302, [("Location", "file:///etc/hosts")])
    case "/drop.zip":
        // first attempt: the connection dies after 100,000 bytes of 300,000; later ones are complete and honour Range
        let n = state.bump("/drop.zip")
        serve(FileRoute(body: fileBody, chunk: 10_000, delay: 0.004, dropAfter: n == 1 ? 100_000 : nil), req, conn)
    case "/drop-noetag.zip":
        let n = state.bump("/drop-noetag.zip")
        serve(FileRoute(body: fileBody, etag: nil, chunk: 10_000, delay: 0.004, dropAfter: n == 1 ? 100_000 : nil), req, conn)
    case "/changing.zip":
        // the file is replaced between the two attempts: a new ETag, so If-Range makes the server send all of it
        let n = state.bump("/changing.zip")
        if n == 1 {
            serve(FileRoute(body: fileBody, etag: "\"old\"", chunk: 10_000, delay: 0.004, dropAfter: 100_000), req, conn)
        } else {
            serve(FileRoute(body: newBody, etag: "\"new\""), req, conn)
        }
    case "/noranges.zip":
        // has an ETag but ignores Range: the download restarts from zero
        let n = state.bump("/noranges.zip")
        serve(FileRoute(body: fileBody, ranges: false, chunk: 10_000, delay: 0.004, dropAfter: n == 1 ? 100_000 : nil), req, conn)
    case "/slow.zip":
        serve(FileRoute(body: slowBody, chunk: 16_384, delay: 0.03), req, conn)
    case "/huge.bin":
        conn.head(200, [("Content-Type", "application/octet-stream"), ("Content-Length", "4398046511104")])
    default:
        conn.respond(404, body: Data("unknown".utf8))
    }
}

portBox.value = server.port
let base = server.baseURL

func url(_ path: String) -> URL { URL(string: path, relativeTo: base)!.absoluteURL }

func expectError(_ expected: LinkDownloadError, _ body: () async throws -> Void, _ label: String, file: String = #fileID, line: Int = #line) async {
    do {
        try await body()
        results.record(false, "\(label): expected \(expected), but it succeeded", "\(file):\(line)")
    } catch let error as LinkDownloadError {
        results.record(error == expected, "\(label): expected \(expected), got \(error)", "\(file):\(line)")
    } catch {
        results.record(false, "\(label): expected \(expected), got \(error)", "\(file):\(line)")
    }
}

// MARK: - Pure logic

await test("content-disposition") {
    let cases: [(String?, Bool, String?)] = [
        (nil, false, nil),
        ("attachment; filename=\"a.zip\"", true, "a.zip"),
        ("attachment; filename=a b.zip", true, "a b.zip"),
        ("inline; filename=\"x.png\"", false, "x.png"),
        ("attachment; filename=\"fallback.zip\"; filename*=UTF-8''na%C3%AFve.zip", true, "naïve.zip"),
        ("attachment; filename*=UTF-8'en'%E2%82%AC%20rates.7z", true, "€ rates.7z"),
        ("attachment; filename*=ISO-8859-1''caf%E9.zip", true, "café.zip"),
        ("attachment; filename=\"semi;colon.zip\"", true, "semi;colon.zip"),
        ("attachment; filename=\"q\\\"uote.zip\"", true, "q\"uote.zip"),
        ("Attachment ; FileName=\"UP.zip\"", true, "UP.zip"),
        ("attachment", true, nil),
        ("attachment; filename=\"na\u{00C3}\u{00AF}ve.zip\"", true, "naïve.zip"), // UTF-8 bytes decoded as Latin-1
    ]
    for (header, attachment, name) in cases {
        let parsed = FileNaming.parseContentDisposition(header)
        checkEqual(parsed.isAttachment, attachment, "attachment flag of \(header ?? "nil")")
        checkEqual(parsed.fileName, name, "file name of \(header ?? "nil")")
    }
}

await test("sanitize-and-name") {
    checkEqual(FileNaming.sanitize("../../etc/passwd"), "passwd", "traversal")
    checkEqual(FileNaming.sanitize("..\\..\\evil.zip"), "evil.zip", "backslash traversal")
    checkEqual(FileNaming.sanitize(".hidden"), "hidden", "leading dot")
    checkEqual(FileNaming.sanitize(".part-abc"), "part-abc", "partial-file prefix")
    checkEqual(FileNaming.sanitize(""), "download", "empty")
    checkEqual(FileNaming.sanitize("..."), "download", "only dots")
    checkEqual(FileNaming.sanitize("a\u{0000}b\nc:d.zip"), "abcd.zip", "control characters and colon")
    checkEqual(FileNaming.sanitize("a\u{202E}piz.exe"), "apiz.exe", "bidi override")
    let long = String(repeating: "é", count: 300) + ".zip"
    let cut = FileNaming.sanitize(long)
    check(cut.utf8.count <= 200 && cut.hasSuffix(".zip"), "long name keeps its extension, got \(cut.utf8.count) bytes")
    let u = URL(string: "https://example.com/files/My%20Mod%20v2.7z?x=1")!
    checkEqual(FileNaming.fileName(contentDisposition: nil, mimeType: nil, finalURL: u), "My Mod v2.7z", "name from the URL")
    checkEqual(FileNaming.fileName(contentDisposition: nil, mimeType: "application/zip", finalURL: URL(string: "https://example.com/download/123")!), "123.zip", "extension from the MIME type")
    checkEqual(FileNaming.fileName(contentDisposition: nil, mimeType: "application/zip", finalURL: URL(string: "https://example.com/")!), "download.zip", "no name at all")
}

await test("unique-name") {
    let dir = freshDir()
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    for expected in ["mod.zip", "mod (1).zip", "mod (2).zip"] {
        let target = FileNaming.uniqueURL(in: dir, name: "mod.zip")
        checkEqual(target.lastPathComponent, expected, "next free name")
        try Data([1]).write(to: target)
    }
    let src = dir.appendingPathComponent("incoming")
    try Data([2]).write(to: src)
    let moved = try FileNaming.moveUnique(src, to: dir, name: "mod.zip")
    checkEqual(moved.lastPathComponent, "mod (3).zip", "moveUnique")
}

await test("content-range") {
    checkEqual(Transfer.startFromContentRange("bytes 100-199/1000"), 100, "start")
    checkEqual(Transfer.totalFromContentRange("bytes 100-199/1000"), 1000, "total")
    checkEqual(Transfer.totalFromContentRange("bytes */1000"), 1000, "unsatisfied total")
    check(Transfer.totalFromContentRange("bytes 0-9/*") == nil, "unknown total")
    check(Transfer.startFromContentRange("items 0-9/10") == nil, "wrong unit")
}

// MARK: - LinkDownloader against the local server

await test("basic-download") {
    let dir = freshDir()
    let downloader = LinkDownloader()
    let seen = ProgressLog()
    let file = try await downloader.downloadFile(url("/file.zip"), to: dir) { seen.add($0) }
    checkEqual(file.fileName, "file.zip", "name")
    checkEqual(file.size, Int64(fileBody.count), "size")
    checkEqual(file.sha256, sha256(fileBody), "sha256")
    let onDisk = try Data(contentsOf: file.fileURL)
    checkEqual(onDisk, fileBody, "content")
    checkEqual(file.originalURL, url("/file.zip"), "original URL")
    checkEqual(file.finalURL, url("/file.zip"), "final URL")
    checkEqual(file.kind, .link, "kind")
    checkEqual(file.fileURL.deletingLastPathComponent().standardizedFileURL, dir.standardizedFileURL, "lands in the folder")
    checkEqual(visibleFiles(in: dir), ["file.zip"], "folder contents")
    checkEqual(hiddenFiles(in: dir), [], "no partial files left")
    try await Task.sleep(nanoseconds: 100_000_000)
    let log = seen.snapshot()
    check(!log.isEmpty && log.last?.fraction == 1, "progress ends at 1 (got \(String(describing: log.last)))")
    check(zip(log, log.dropFirst()).allSatisfy { $0.receivedBytes <= $1.receivedBytes }, "progress never goes back")
    check(log.allSatisfy { $0.totalBytes == Int64(fileBody.count) }, "total known from Content-Length")
    let backup = try dir.resourceValues(forKeys: [.isExcludedFromBackupKey]).isExcludedFromBackup
    check(backup == true, "downloads folder is excluded from backups")
}

await test("contract-api-url-result") {
    let dir = freshDir()
    let box = ProgressLog()
    let out = try await LinkDownloader().download(url("/file.zip"), to: dir) { box.add(DownloadProgress(receivedBytes: Int64($0 * 1000), totalBytes: 1000)) }
    checkEqual(out.lastPathComponent, "file.zip", "file name")
    try await Task.sleep(nanoseconds: 100_000_000)
    check(box.snapshot().last?.fraction == 1, "the Double progress reaches 1")
}

await test("redirects") {
    let dir = freshDir()
    let file = try await LinkDownloader().downloadFile(url("/r1"), to: dir)
    checkEqual(file.originalURL, url("/r1"), "original URL")
    checkEqual(file.finalURL, url("/file.zip"), "final URL after two redirects")
    checkEqual(file.fileName, "file.zip", "name comes from the final URL")
    checkEqual(file.sha256, sha256(fileBody), "content")
    let again = try await LinkDownloader().downloadFile(url("/file.zip"), to: dir)
    checkEqual(again.fileName, "file (1).zip", "second download of the same name does not overwrite")
    checkEqual(visibleFiles(in: dir), ["file (1).zip", "file.zip"], "both files kept")
}

await test("content-disposition-names") {
    let dir = freshDir()
    let a = try await LinkDownloader().downloadFile(url("/cd-utf8"), to: dir)
    checkEqual(a.fileName, "naïve mod ✓.zip", "filename* wins and is decoded")
    let b = try await LinkDownloader().downloadFile(url("/cd-quoted"), to: dir)
    checkEqual(b.fileName, "my \"mod\" v1; final.zip", "quoted name with quotes and a semicolon")
    let c = try await LinkDownloader().downloadFile(url("/cd-evil"), to: dir)
    checkEqual(c.fileName, "evil.zip", "path traversal is dropped")
    check(c.fileURL.standardizedFileURL.path.hasPrefix(dir.standardizedFileURL.path + "/"), "stays inside the folder")
    let d = try await LinkDownloader().downloadFile(url("/cd-redirect"), to: dir)
    checkEqual(d.fileName, "naïve mod ✓ (1).zip", "header on the final response of a redirect, made unique")
    check(!FileManager.default.fileExists(atPath: dir.deletingLastPathComponent().appendingPathComponent("evil.zip").path), "nothing written outside the folder")
}

await test("web-page-is-reported") {
    let dir = freshDir()
    await expectError(.notAFile(url: url("/page"), mimeType: "text/html"), {
        _ = try await LinkDownloader().downloadFile(url("/page"), to: dir)
    }, "html page")
    await expectError(.notAFile(url: url("/page"), mimeType: "text/html"), {
        _ = try await LinkDownloader().downloadFile(url("/page-redirect"), to: dir)
    }, "redirect to an html page reports the final URL")
    await expectError(.notAFile(url: url("/html-octet"), mimeType: "application/octet-stream"), {
        _ = try await LinkDownloader().downloadFile(url("/html-octet"), to: dir)
    }, "html body under octet-stream")
    await expectError(.notAFile(url: url("/html-tiny"), mimeType: "application/octet-stream"), {
        _ = try await LinkDownloader().downloadFile(url("/html-tiny"), to: dir)
    }, "tiny html body under octet-stream")
    checkEqual(visibleFiles(in: dir), [], "no file left behind")
    checkEqual(hiddenFiles(in: dir), [], "no partial left behind")
    let saved = try await LinkDownloader().downloadFile(url("/html-attachment"), to: dir)
    checkEqual(saved.fileName, "page.html", "an html file sent as an attachment is a file")
}

await test("http-status") {
    let dir = freshDir()
    await expectError(.httpStatus(404), { _ = try await LinkDownloader().downloadFile(url("/404"), to: dir) }, "404")
    checkEqual(hiddenFiles(in: dir), [], "no partial after an error status")
}

await test("only-http-and-https") {
    let dir = freshDir()
    await expectError(.unsupportedScheme("ftp"), { _ = try await LinkDownloader().downloadFile(URL(string: "ftp://example.invalid/a.zip")!, to: dir) }, "ftp link")
    await expectError(.unsupportedScheme("file"), { _ = try await LinkDownloader().downloadFile(URL(fileURLWithPath: "/etc/hosts"), to: dir) }, "file link")
    await expectError(.unsupportedScheme("ftp"), { _ = try await LinkDownloader().downloadFile(url("/redirect-ftp"), to: dir) }, "redirect to ftp")
    do {
        _ = try await LinkDownloader().downloadFile(url("/redirect-file"), to: dir)
        check(false, "redirect to file: must not succeed")
    } catch {
        check(true, "redirect to file: refused (\(error))")
    }
    checkEqual(visibleFiles(in: dir), [], "nothing downloaded")
}

await test("free-space-check") {
    let dir = freshDir()
    let tight = LinkDownloader(reserveBytes: 0, freeSpace: { _ in 100_000 })
    await expectError(.insufficientDiskSpace(needed: Int64(fileBody.count), available: 100_000), {
        _ = try await tight.downloadFile(url("/file.zip"), to: dir)
    }, "needs more than is free")
    let reserve = LinkDownloader(reserveBytes: 50_000, freeSpace: { _ in Int64(fileBody.count) + 10_000 })
    await expectError(.insufficientDiskSpace(needed: Int64(fileBody.count), available: Int64(fileBody.count) + 10_000), {
        _ = try await reserve.downloadFile(url("/file.zip"), to: dir)
    }, "reserve counts")
    let exact = LinkDownloader(reserveBytes: 50_000, freeSpace: { _ in Int64(fileBody.count) + 50_000 })
    let ok = try await exact.downloadFile(url("/file.zip"), to: dir)
    checkEqual(ok.size, Int64(fileBody.count), "fits exactly")
    let huge = LinkDownloader()
    do {
        _ = try await huge.downloadFile(url("/huge.bin"), to: freshDir())
        check(false, "a 4 TiB file must not start")
    } catch let error as LinkDownloadError {
        if case .insufficientDiskSpace(let needed, _) = error { checkEqual(needed, 4_398_046_511_104, "needed bytes") } else { check(false, "wrong error \(error)") }
    }
    check((LinkDownloader.availableCapacity(at: dir) ?? 0) > 0, "availableCapacity works on a folder that does not exist yet")
}

await test("chunked-without-length") {
    let dir = freshDir()
    let seen = ProgressLog()
    let file = try await LinkDownloader().downloadFile(url("/chunked.bin"), to: dir) { seen.add($0) }
    checkEqual(file.sha256, sha256(fileBody), "content")
    checkEqual(file.fileName, "chunked.bin", "name")
    try await Task.sleep(nanoseconds: 100_000_000)
    check(seen.snapshot().allSatisfy { $0.totalBytes == nil && $0.fraction == 0 }, "unknown total stays unknown")
}

await test("no-cookies") {
    let dir = freshDir()
    server.clearRequests()
    _ = try await LinkDownloader().downloadFile(url("/file.zip"), to: dir)
    _ = try await LinkDownloader().downloadFile(url("/file.zip"), to: dir)
    let requests = server.requests(for: "/file.zip")
    checkEqual(requests.count, 2, "two requests")
    check(requests.allSatisfy { $0.headers["cookie"] == nil }, "the downloader never sends a cookie back")
    check(requests.allSatisfy { $0.headers["accept-encoding"] == "identity" }, "asks for the bytes as they are")
}

await test("resume-after-dropped-connection") {
    let dir = freshDir()
    let target = url("/drop.zip")
    server.clearRequests()
    do {
        _ = try await LinkDownloader().downloadFile(target, to: dir)
        check(false, "the first attempt should fail")
    } catch let error as LinkDownloadError {
        check(true, "first attempt fails: \(error)")
    }
    let partial = LinkDownloader.partialSize(for: target, in: dir)
    check(partial >= 50_000 && partial <= 100_000, "partial file kept (\(partial) bytes)")
    check(LinkDownloader.hasPartial(for: target, in: dir), "hasPartial")
    let file = try await LinkDownloader().downloadFile(target, to: dir)
    checkEqual(file.sha256, sha256(fileBody), "resumed file is complete and identical")
    checkEqual(file.size, Int64(fileBody.count), "size")
    checkEqual(file.fileName, "drop.zip", "name")
    let requests = server.requests(for: "/drop.zip")
    checkEqual(requests.count, 2, "two requests")
    checkEqual(requests.last?.headers["range"], "bytes=\(partial)-", "the second request asks for the rest")
    checkEqual(requests.last?.headers["if-range"], "\"v1\"", "and proves it is the same file")
    check(!LinkDownloader.hasPartial(for: target, in: dir), "partial is gone after success")
    checkEqual(hiddenFiles(in: dir), [], "no hidden files left")
}

await test("no-resume-without-validator") {
    let dir = freshDir()
    let target = url("/drop-noetag.zip")
    server.clearRequests()
    _ = try? await LinkDownloader().downloadFile(target, to: dir)
    check(LinkDownloader.hasPartial(for: target, in: dir), "partial kept")
    let file = try await LinkDownloader().downloadFile(target, to: dir)
    checkEqual(file.sha256, sha256(fileBody), "complete")
    check(server.requests(for: "/drop-noetag.zip").last?.headers["range"] == nil, "without an ETag or Last-Modified it starts over instead of guessing")
}

await test("changed-file-restarts") {
    let dir = freshDir()
    let target = url("/changing.zip")
    _ = try? await LinkDownloader().downloadFile(target, to: dir)
    check(LinkDownloader.hasPartial(for: target, in: dir), "partial of the old file kept")
    let file = try await LinkDownloader().downloadFile(target, to: dir)
    checkEqual(file.sha256, sha256(newBody), "the new file, whole, not old head + new tail")
}

await test("server-ignores-range") {
    let dir = freshDir()
    let target = url("/noranges.zip")
    server.clearRequests()
    _ = try? await LinkDownloader().downloadFile(target, to: dir)
    let file = try await LinkDownloader().downloadFile(target, to: dir)
    checkEqual(file.sha256, sha256(fileBody), "restarted from zero when the answer was 200")
    check(server.requests(for: "/noranges.zip").last?.headers["range"] != nil, "it did try a range first")
}

await test("cancel-keeps-partial-then-resume") {
    let dir = freshDir()
    let target = url("/slow.zip")
    server.clearRequests()
    let gate = ProgressLog()
    let task = Task {
        try await LinkDownloader().downloadFile(target, to: dir) { gate.add($0) }
    }
    // wait until some bytes have arrived, then cancel
    var waited = 0
    while (gate.snapshot().last?.receivedBytes ?? 0) < 60_000 && waited < 200 {
        try await Task.sleep(nanoseconds: 20_000_000)
        waited += 1
    }
    task.cancel()
    do {
        _ = try await task.value
        check(false, "cancelled download must throw")
    } catch is CancellationError {
        check(true, "throws CancellationError")
    } catch {
        check(false, "wrong error \(error)")
    }
    try await Task.sleep(nanoseconds: 200_000_000)
    let kept = LinkDownloader.partialSize(for: target, in: dir)
    check(kept > 0 && kept < Int64(slowBody.count), "partial kept (\(kept) bytes)")
    checkEqual(visibleFiles(in: dir), [], "nothing visible yet")
    let before = server.requests(for: "/slow.zip").count
    let file = try await LinkDownloader().downloadFile(target, to: dir)
    checkEqual(file.sha256, sha256(slowBody), "resumed to the identical file")
    let requests = server.requests(for: "/slow.zip")
    checkEqual(requests.count, before + 1, "one more request")
    checkEqual(requests.last?.headers["range"], "bytes=\(kept)-", "resumed from the kept bytes")
}

await test("cancel-before-start") {
    let dir = freshDir()
    let task = Task {
        try await LinkDownloader().downloadFile(url("/slow.zip"), to: dir)
    }
    task.cancel()
    do {
        _ = try await task.value
        check(false, "must throw")
    } catch is CancellationError {
        check(true, "CancellationError")
    } catch {
        check(false, "wrong error \(error)")
    }
}

await test("discard-and-stale-partials") {
    let dir = freshDir()
    let target = url("/drop.zip")
    state.reset("/drop.zip")
    _ = try? await LinkDownloader().downloadFile(target, to: dir)
    check(LinkDownloader.hasPartial(for: target, in: dir), "partial exists")
    LinkDownloader.removeStalePartials(in: dir, olderThan: 3600)
    check(LinkDownloader.hasPartial(for: target, in: dir), "a fresh partial is not stale")
    LinkDownloader.removeStalePartials(in: dir, olderThan: -1)
    check(!LinkDownloader.hasPartial(for: target, in: dir), "an old one is removed")
    checkEqual(hiddenFiles(in: dir), [], "metadata removed with it")
}

final class ProgressLog: @unchecked Sendable {
    private let lock = NSLock()
    private var items: [DownloadProgress] = []
    func add(_ p: DownloadProgress) { lock.lock(); items.append(p); lock.unlock() }
    func snapshot() -> [DownloadProgress] { lock.lock(); defer { lock.unlock() }; return items }
}

// MARK: - Done

server.stop()
try? FileManager.default.removeItem(at: tempRoot)
print("\n\(testCount.value) tests, \(results.passed) checks passed, \(results.failed) failed")
exit(results.failed == 0 ? 0 : 1)
