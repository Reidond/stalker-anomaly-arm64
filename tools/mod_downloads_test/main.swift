// Host tests of ModDownloads. Exit code 0 when every check passes.
//   mod_downloads_test              all tests
//   mod_downloads_test <substring>  only the tests whose name contains it
import AppKit
import CryptoKit
import Foundation
@testable import ModDownloads

NSApplication.shared.setActivationPolicy(.prohibited)

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

func test(_ name: String, _ body: @MainActor () async throws -> Void) async {
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
    Thread.sleep(forTimeInterval: 600)
    print("FAIL watchdog: the tests did not finish in 600 s")
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
    case "/badrange.zip":
        // second attempt: a 206 that starts somewhere else than asked -> the client starts over without a Range
        let n = state.bump("/badrange.zip")
        if n == 2 {
            conn.respond(206, [("Content-Type", "application/zip"), ("ETag", "\"v1\""), ("Content-Range", "bytes 0-\(fileBody.count - 1)/\(fileBody.count)")], body: fileBody)
        } else {
            serve(FileRoute(body: fileBody, chunk: 10_000, delay: 0.004, dropAfter: n == 1 ? 100_000 : nil), req, conn)
        }
    case "/stale416.zip":
        // second attempt: 416 with a size that is not the partial's -> start over
        let n = state.bump("/stale416.zip")
        if n == 2 {
            conn.respond(416, [("Content-Range", "bytes */999")])
        } else {
            serve(FileRoute(body: fileBody, chunk: 10_000, delay: 0.004, dropAfter: n == 1 ? 100_000 : nil), req, conn)
        }
    case "/complete.zip":
        serve(FileRoute(body: fileBody), req, conn)
    case "/slow.zip":
        serve(FileRoute(body: slowBody, chunk: 16_384, delay: 0.03), req, conn)
    case "/dl.zip":
        serve(FileRoute(body: fileBody, extra: [("Content-Disposition", "attachment; filename=\"mod.zip\"")]), req, conn)
    case "/dl-r1":
        conn.respond(302, [("Location", "\(base)/dl.zip")])
    case "/dl-slow.zip":
        serve(FileRoute(body: slowBody, extra: [("Content-Disposition", "attachment; filename=\"slow.zip\"")], chunk: 16_384, delay: 0.03), req, conn)
    case "/plain.html":
        conn.respond(200, [("Content-Type", "text/html; charset=utf-8")], body: Data("<!DOCTYPE html><html><head><title>Plain page</title></head><body><p>hello</p></body></html>".utf8))
    case "/auto-download.html":
        // a page that clicks a download link by itself, without any tap
        conn.respond(200, [("Content-Type", "text/html; charset=utf-8")], body: Data("<!DOCTYPE html><html><head><title>Auto</title></head><body><a id=x href=\"/dl.zip\" download>get</a><script>document.getElementById('x').click();</script></body></html>".utf8))
    case "/auto-redirect.html":
        conn.respond(200, [("Content-Type", "text/html; charset=utf-8")], body: Data("<!DOCTYPE html><html><head><title>Auto2</title></head><body><script>setTimeout(function(){ location.href = '/dl.zip'; }, 100);</script></body></html>".utf8))
    case "/ftp-link.html":
        conn.respond(200, [("Content-Type", "text/html; charset=utf-8")], body: Data("<!DOCTYPE html><html><head><title>Ftp</title></head><body><script>location.href = 'ftp://example.invalid/a.zip';</script></body></html>".utf8))
    case "/octet.bin":
        // a type WebKit cannot show, no Content-Disposition
        serve(FileRoute(body: fileBody, type: "application/x-anomaly-test", etag: nil, ranges: false), req, conn)
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

await test("resume-edge-cases") {
    // 206 that starts at the wrong place
    var dir = freshDir()
    var target = url("/badrange.zip")
    server.clearRequests()
    _ = try? await LinkDownloader().downloadFile(target, to: dir)
    check(LinkDownloader.hasPartial(for: target, in: dir), "badrange: partial kept")
    var file = try await LinkDownloader().downloadFile(target, to: dir)
    checkEqual(file.sha256, sha256(fileBody), "badrange: restarted and complete")
    var ranges = server.requests(for: "/badrange.zip").map { $0.headers["range"] ?? "full" }
    checkEqual(ranges.count, 3, "badrange: three requests (\(ranges))")
    checkEqual(ranges.last, "full", "badrange: the restart carries no Range")

    // 416 that disagrees about the size
    dir = freshDir()
    target = url("/stale416.zip")
    server.clearRequests()
    _ = try? await LinkDownloader().downloadFile(target, to: dir)
    file = try await LinkDownloader().downloadFile(target, to: dir)
    checkEqual(file.sha256, sha256(fileBody), "416: restarted and complete")
    ranges = server.requests(for: "/stale416.zip").map { $0.headers["range"] ?? "full" }
    checkEqual(ranges.last, "full", "416: the restart carries no Range (\(ranges))")

    // a partial that already holds the whole file: the server answers 416 with the same size, and that finishes it
    dir = freshDir()
    target = url("/complete.zip")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let part = LinkDownloader.partURL(for: target, in: dir)
    try fileBody.write(to: part)
    let meta = PartialMeta(url: target.absoluteString, etag: "\"v1\"", lastModified: nil, finalURL: target.absoluteString, total: Int64(fileBody.count), fileName: "kept-name.zip")
    try JSONEncoder().encode(meta).write(to: LinkDownloader.metaURL(for: part))
    server.clearRequests()
    file = try await LinkDownloader().downloadFile(target, to: dir)
    checkEqual(file.sha256, sha256(fileBody), "complete partial: finished without downloading again")
    checkEqual(file.fileName, "kept-name.zip", "complete partial: keeps the name the first response asked for")
    checkEqual(server.requests(for: "/complete.zip").count, 1, "complete partial: one request")
    checkEqual(hiddenFiles(in: dir), [], "complete partial: nothing left")
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

// MARK: - Pure logic: gate, address field, bookmarks, controller mapping

await test("theme-and-motion") {
    let anomaly = DownloadsTheme.anomaly
    check(anomaly.prefersGlass, "the default theme uses glass")
    check(anomaly.panelRadius > 0, "panel radius")
    var custom = anomaly
    custom.prefersGlass = false
    check(!custom.prefersGlass && anomaly.prefersGlass, "a theme is a value: changing a copy leaves the default alone")
    check(Motion.animation(Motion.spring, reduceMotion: true) == Motion.fade, "Reduce Motion swaps every animation for a short fade")
    check(Motion.animation(Motion.spring, reduceMotion: false) == Motion.spring, "otherwise the spring stays")
    // yellow-green is for "done" only: nothing else in the default palette is that colour
    check(anomaly.signal != anomaly.highlight && anomaly.signal != anomaly.accent, "signal is its own colour")
}

await test("gate-tap-window-and-one-download-per-tap") {
    var gate = DownloadGate()
    check(!gate.isOpen(at: 100), "closed before any tap")
    check(!gate.consume(at: 100), "nothing to consume")
    gate.noteTap(at: 100)
    check(gate.isOpen(at: 100) && gate.isOpen(at: 129.9), "open for the tap window")
    check(!gate.isOpen(at: 130.1), "closed after it")
    check(!gate.isOpen(at: 99), "a tap from the future does not count")
    check(gate.consume(at: 110), "the first download passes")
    check(!gate.consume(at: 111), "one tap pays for one download")
    gate.noteTap(at: 200)
    check(gate.consume(at: 205), "a new tap opens it again")
}

await test("gate-typed-navigation") {
    var gate = DownloadGate()
    gate.noteTypedNavigation(at: 50)
    check(gate.isOpen(at: 60), "a typed link may be a file")
    gate.pageDisplayed()
    check(!gate.isOpen(at: 60), "once it showed a page, a script in that page cannot reuse it")
    gate.noteTypedNavigation(at: 70)
    check(!gate.isOpen(at: 90.5), "typed window is shorter than the tap window")
    check(gate.consume(at: 75), "typed navigation that ends in a file passes")
    check(!gate.consume(at: 76), "once")
    gate.noteTap(at: 80)
    gate.noteTypedNavigation(at: 81)
    gate.pageDisplayed()
    check(gate.isOpen(at: 82), "a page tap survives the page the mirror shows (ModDB's redirect page)")
}

await test("address-input") {
    func url(_ s: String) -> AddressInput.Result { .url(URL(string: s)!) }
    checkEqual(AddressInput.resolve("   "), .empty, "blank")
    checkEqual(AddressInput.resolve("https://www.moddb.com/mods/stalker-anomaly/addons\n"), url("https://www.moddb.com/mods/stalker-anomaly/addons"), "pasted with newline")
    checkEqual(AddressInput.resolve("http://example.com/a.zip"), url("http://example.com/a.zip"), "http")
    checkEqual(AddressInput.resolve("github.com/Reidond"), url("https://github.com/Reidond"), "bare host gets https")
    checkEqual(AddressInput.resolve("localhost:8000/x.zip"), url("http://localhost:8000/x.zip"), "localhost with port")
    checkEqual(AddressInput.resolve("192.168.1.5:8080"), url("http://192.168.1.5:8080"), "LAN address")
    checkEqual(AddressInput.resolve("javascript:alert(1)"), .unsupported(scheme: "javascript"), "javascript")
    checkEqual(AddressInput.resolve("file:///etc/passwd"), .unsupported(scheme: "file"), "file")
    checkEqual(AddressInput.resolve("itms-apps://apps.apple.com/x"), .unsupported(scheme: "itms-apps"), "app store")
    checkEqual(AddressInput.resolve("data:text/html,hi"), .unsupported(scheme: "data"), "data")
    if case .search(let search) = AddressInput.resolve("stalker anomaly addons") {
        checkEqual(search.host, "duckduckgo.com", "search host")
        check(search.absoluteString.contains("q=stalker%20anomaly%20addons") || search.absoluteString.contains("q=stalker+anomaly+addons"), "query kept: \(search)")
    } else { check(false, "words are a search") }
    if case .search = AddressInput.resolve("anomaly") { check(true, "a single word is a search") } else { check(false, "a single word is a search") }
    check(AddressInput.httpLink("https://example.com/a.zip") != nil, "httpLink accepts https")
    check(AddressInput.httpLink("example.com/a.zip") != nil, "httpLink accepts a bare host")
    check(AddressInput.httpLink("two words") == nil, "httpLink refuses a search")
    check(AddressInput.httpLink("ftp://example.com/a.zip") == nil, "httpLink refuses ftp")
}

await test("bookmarks") {
    let suite = "mod_downloads_test.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: suite)!
    defer { defaults.removePersistentDomain(forName: suite) }
    let store = BookmarkStore(defaults: defaults)
    checkEqual(store.all.count, 2, "two built-in bookmarks and nothing else")
    check(store.all.first?.url.host == "www.moddb.com" && store.all.first?.url.path == "/mods/stalker-anomaly/addons", "ModDB addons first")
    check(store.all.last?.url.host == "github.com", "GitHub search second")
    check(store.isBookmarked(URL(string: "https://www.moddb.com/mods/stalker-anomaly/addons")), "built-ins count as bookmarked")
    check(store.add(title: "Mod A", url: URL(string: "https://example.com/a")!), "add")
    check(store.add(title: "", url: URL(string: "https://example.com/b")!), "add without a title")
    check(!store.add(title: "dup", url: URL(string: "https://example.com/a")!), "duplicate refused")
    check(!store.add(title: "dup", url: URL(string: "https://www.moddb.com/mods/stalker-anomaly/addons")!), "built-in duplicate refused")
    check(!store.add(title: "ftp", url: URL(string: "ftp://example.com/a")!), "ftp refused")
    check(!store.add(title: "js", url: URL(string: "javascript:alert(1)")!), "javascript refused")
    checkEqual(store.user.map(\.title), ["Mod A", "example.com"], "titles")
    store.rename(id: store.user[0].id, to: "  Renamed ")
    store.move(from: IndexSet(integer: 1), to: 0)
    checkEqual(store.user.map(\.title), ["example.com", "Renamed"], "rename and move")
    let again = BookmarkStore(defaults: defaults)
    checkEqual(again.user.map(\.title), ["example.com", "Renamed"], "kept across launches")
    again.remove(id: again.user[0].id)
    checkEqual(BookmarkStore(defaults: defaults).user.map(\.title), ["Renamed"], "removal is saved")
    check(BookmarkStore.builtIn.count == 2 && BookmarkStore(defaults: defaults).all.count == 3, "built-ins cannot be removed")
}

await test("controller-mapping") {
    let dead = ControllerMapping.shape(x: 0.1, y: 0.1)
    check(dead.x == 0 && dead.y == 0, "inside the deadzone nothing moves")
    let full = ControllerMapping.shape(x: 1, y: 0)
    check(abs(full.x - 1) < 1e-6 && full.y == 0, "full push is full speed")
    let half = ControllerMapping.shape(x: 0.59, y: 0)
    check(half.x > 0 && half.x < 0.3, "gentle push is slow (got \(half.x))")
    let up = ControllerMapping.scrollDelta(x: 0, y: 1, dt: 0.1)
    check(up.dy < 0 && abs(up.dy + 110) < 1e-6 && up.dx == 0, "stick up scrolls up (\(up.dy))")
    let right = ControllerMapping.scrollDelta(x: 1, y: 0, dt: 0.1)
    check(right.dx > 0, "stick right scrolls right")
    var cursor = (x: 0.5, y: 0.5)
    for _ in 0..<100 { cursor = ControllerMapping.movedCursor(cursor, stickX: 1, stickY: 1, dt: 0.1) }
    check(cursor.x == 1 && cursor.y == 0, "cursor stays inside the page (up-right corner: \(cursor))")
    let center = ControllerMapping.movedCursor((0.5, 0.5), stickX: 0, stickY: 0, dt: 0.1)
    check(center == (0.5, 0.5), "cursor holds still")
}

// MARK: - The web view (headless: a WKWebView with no window)

@MainActor
func makeWebModel() -> (model: DownloadsModel, dir: URL, files: FinishedLog) {
    let dir = freshDir()
    let suite = UserDefaults(suiteName: "mod_downloads_test.\(UUID().uuidString)")!
    let model = DownloadsModel(dir: dir, browser: BrowserModel(dataStore: .nonPersistent()), bookmarks: BookmarkStore(defaults: suite))
    let log = FinishedLog()
    model.onDownloadedFile = { log.files.append($0) }
    model.onDownloaded = { log.urls.append($0) }
    return (model, dir, log)
}

final class FinishedLog: @unchecked Sendable {
    var files: [DownloadedFile] = []
    var urls: [URL] = []
}

@MainActor
func waitUntil(_ seconds: Double = 45, _ condition: () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(seconds)
    while Date() < deadline {
        if condition() { return true }
        try? await Task.sleep(nanoseconds: 50_000_000)
    }
    return condition()
}

await test("web-page-loads-without-a-download") {
    let (model, dir, log) = makeWebModel()
    defer { model.shutdown() }
    model.browser.load(url("/plain.html"))
    let loaded = await waitUntil { model.browser.pageTitle == "Plain page" }
    check(loaded, "the page loaded (title \"\(model.browser.pageTitle)\")")
    check(model.items.isEmpty, "a page is not a download")
    checkEqual(model.browser.addressText, url("/plain.html").absoluteString, "address field follows the page")
    checkEqual(visibleFiles(in: dir), [], "nothing saved")
    check(log.files.isEmpty, "no callback")
}

await test("web-typed-link-to-a-file-downloads") {
    let (model, dir, log) = makeWebModel()
    defer { model.shutdown() }
    model.browser.load(url("/dl-r1")) // the user typed a link that redirects to an attachment
    let done = await waitUntil { model.items.first?.state == .finished }
    check(done, "download finished (state \(String(describing: model.items.first?.state)))")
    guard let item = model.items.first, let file = item.file else { return }
    checkEqual(file.fileName, "mod.zip", "name from Content-Disposition")
    checkEqual(file.sha256, sha256(fileBody), "sha256")
    checkEqual(file.size, Int64(fileBody.count), "size")
    checkEqual(file.kind, .browser, "kind")
    checkEqual(file.finalURL, url("/dl.zip"), "final URL after the redirect")
    check(file.originalURL == url("/dl-r1") || file.originalURL == url("/dl.zip"), "original URL is the first request (got \(file.originalURL))")
    let webOnDisk = try Data(contentsOf: file.fileURL)
    checkEqual(webOnDisk, fileBody, "content")
    checkEqual(file.fileURL.deletingLastPathComponent().standardizedFileURL, dir.standardizedFileURL, "in the downloads folder")
    checkEqual(visibleFiles(in: dir), ["mod.zip"], "folder contents")
    checkEqual(hiddenFiles(in: dir), [], "no .wk part left")
    checkEqual(log.files.count, 1, "onDownloadedFile once")
    checkEqual(log.urls.count, 1, "onDownloaded once")
    checkEqual(log.urls.first?.standardizedFileURL, file.fileURL.standardizedFileURL, "same file")
}

await test("web-page-cannot-start-a-download-by-itself") {
    let (model, dir, log) = makeWebModel()
    defer { model.shutdown() }
    for path in ["/auto-download.html", "/auto-redirect.html"] {
        model.browser.load(url(path)) // the user opens the page; its script then starts a download on its own
        let blocked = await waitUntil { model.items.contains { $0.state == .awaitingApproval } }
        check(blocked, "\(path): the attempt is held for approval")
        try? await Task.sleep(nanoseconds: 500_000_000)
        checkEqual(visibleFiles(in: dir), [], "\(path): nothing saved")
        checkEqual(hiddenFiles(in: dir), [], "\(path): nothing partial")
        check(log.files.isEmpty, "\(path): no callback")
        if let item = model.items.first(where: { $0.state == .awaitingApproval }) { model.remove(item) }
    }
}

await test("web-blocked-download-can-be-allowed") {
    let (model, dir, log) = makeWebModel()
    defer { model.shutdown() }
    model.browser.load(url("/auto-download.html"))
    _ = await waitUntil { model.items.contains { $0.state == .awaitingApproval } }
    guard let item = model.items.first(where: { $0.state == .awaitingApproval }) else { check(false, "no blocked item"); return }
    checkEqual(item.sourceURL, url("/dl.zip"), "the blocked URL")
    check(model.notice != nil, "the user is told")
    model.approve(item)
    let done = await waitUntil { item.state == .finished }
    check(done, "allowed download finishes (state \(item.state))")
    checkEqual(item.file?.sha256, sha256(fileBody), "content")
    checkEqual(visibleFiles(in: dir), ["mod.zip"], "saved after the user's Allow")
    checkEqual(log.files.count, 1, "callback once")
}

await test("web-tap-in-page-allows-one-download") {
    let (model, dir, _) = makeWebModel()
    defer { model.shutdown() }
    model.browser.load(url("/plain.html"))
    _ = await waitUntil { model.browser.pageTitle == "Plain page" }
    // a page's own scripted click is not a tap: its event is not trusted, so the user script says nothing
    _ = try? await model.browser.webView.evaluateJavaScript("document.dispatchEvent(new MouseEvent('click', {bubbles: true}))")
    try? await Task.sleep(nanoseconds: 300_000_000)
    check(!model.browser.gate.isOpen(at: DownloadGate.now), "a scripted click does not open the gate")
    // the message the user script sends for a trusted click (a real tap cannot be made without a window)
    _ = try? await model.browser.webView.evaluateJavaScript("window.webkit.messageHandlers.userGesture.postMessage(1)", in: nil, contentWorld: .defaultClient)
    let opened = await waitUntil(5) { model.browser.gate.isOpen(at: DownloadGate.now) }
    check(opened, "a trusted tap message opens the gate")
    model.browser.webView.load(URLRequest(url: url("/dl.zip"))) // the navigation the tap led to
    let done = await waitUntil { model.items.first?.state == .finished }
    check(done, "download after a tap finishes")
    check(!model.browser.gate.isOpen(at: DownloadGate.now), "the tap is used up")
    model.browser.webView.load(URLRequest(url: url("/dl.zip")))
    let second = await waitUntil { model.items.contains { $0.state == .awaitingApproval } }
    check(second, "a second download needs a second tap")
    checkEqual(visibleFiles(in: dir), ["mod.zip"], "only the first was saved")
}

await test("web-unknown-type-without-disposition-is-a-download") {
    let (model, _, _) = makeWebModel()
    defer { model.shutdown() }
    model.browser.load(url("/octet.bin"))
    let done = await waitUntil { model.items.first?.state == .finished }
    check(done, "a type WebKit cannot show is downloaded when the user typed the link (state \(String(describing: model.items.first?.state)))")
    checkEqual(model.items.first?.file?.fileName, "octet.bin", "name from the URL")
}

await test("web-only-http-and-https") {
    let (model, _, _) = makeWebModel()
    defer { model.shutdown() }
    model.browser.addressText = "javascript:alert(1)"
    model.browser.submitAddress()
    check(model.browser.errorMessage?.contains("javascript") == true, "address field refuses javascript: (\(String(describing: model.browser.errorMessage)))")
    model.browser.errorMessage = nil
    model.browser.load(url("/ftp-link.html")) // a page that navigates to ftp://
    let refused = await waitUntil { model.browser.errorMessage != nil }
    check(refused, "the page's ftp link is refused")
    check(model.items.isEmpty, "no download from it")
}

await test("web-cancel-and-retry") {
    let (model, dir, _) = makeWebModel()
    defer { model.shutdown() }
    server.clearRequests()
    model.browser.load(url("/dl-slow.zip"))
    let started = await waitUntil { (model.items.first?.receivedBytes ?? 0) >= 60_000 }
    check(started, "download is running (\(model.items.first?.receivedBytes ?? 0) bytes)")
    guard let item = model.items.first else { return }
    model.cancel(item)
    check(item.state == .cancelled, "cancelled")
    try? await Task.sleep(nanoseconds: 500_000_000)
    check(item.state == .cancelled, "stays cancelled after WebKit reports the cancel")
    checkEqual(visibleFiles(in: dir), [], "no finished file")
    print("   resume data after cancel: \(item.resumeData.map { "\($0.count) bytes" } ?? "none")")
    model.retry(item)
    let done = await waitUntil(90) { item.state == .finished }
    check(done, "retry finishes (state \(item.state))")
    checkEqual(item.file?.sha256, sha256(slowBody), "the file is complete and identical")
    checkEqual(visibleFiles(in: dir), ["slow.zip"], "one file")
    checkEqual(hiddenFiles(in: dir), [], "no part left")
    print("   requests for /dl-slow.zip: \(server.requests(for: "/dl-slow.zip").map { $0.headers["range"] ?? "full" })")
}

await test("web-download-whose-delegate-came-too-late-is-restarted") {
    let (model, dir, log) = makeWebModel()
    defer { model.shutdown() }
    model.stallSeconds = 3
    model.testingAttachDelay = 2.0 // the first attempt gets its delegate late and never moves
    model.browser.load(url("/auto-download.html"))
    _ = await waitUntil { model.items.contains { $0.state == .awaitingApproval } }
    guard let item = model.items.first(where: { $0.state == .awaitingApproval }) else { check(false, "no blocked item"); return }
    model.approve(item)
    let done = await waitUntil(90) { item.state == .finished }
    check(done, "the stuck download was started again and finished (state \(item.state))")
    checkEqual(item.file?.sha256, sha256(fileBody), "complete and identical")
    checkEqual(item.restarts, 1, "restarted once")
    checkEqual(visibleFiles(in: dir), ["mod.zip"], "one file")
    checkEqual(hiddenFiles(in: dir), [], "no part left")
    checkEqual(log.files.count, 1, "reported once")
}

await test("web-download-list-actions") {
    let (model, dir, _) = makeWebModel()
    defer { model.shutdown() }
    model.browser.load(url("/dl.zip"))
    _ = await waitUntil { model.items.first?.state == .finished }
    check(model.activeCount == 0 && model.waitingCount == 0, "counts")
    model.clearFinished()
    check(model.items.isEmpty, "clearFinished empties the list")
    checkEqual(visibleFiles(in: dir), ["mod.zip"], "the file stays for the importer")
}

await test("link-item-states") {
    let (model, dir, log) = makeWebModel()
    defer { model.shutdown() }
    check(!model.downloadLink("two words"), "not a link")
    check(model.notice != nil, "told why")
    check(model.downloadLink(url("/file.zip").absoluteString), "link accepted")
    let done = await waitUntil { model.items.first?.state == .finished }
    check(done, "link download finishes")
    checkEqual(model.items.first?.origin, .link, "origin")
    checkEqual(log.files.first?.kind, .link, "kind")
    checkEqual(visibleFiles(in: dir), ["file.zip"], "saved")
    // a web page link: reported, and the browser opens it
    check(model.downloadLink(url("/plain.html").absoluteString), "page link accepted")
    let reported = await waitUntil { model.items.first?.state == .openedInBrowser }
    check(reported, "the page is reported as not a file")
    let opened = await waitUntil { model.browser.pageTitle == "Plain page" }
    check(opened, "and opened in the browser")
    // cancel and retry of a link download
    check(model.downloadLink(url("/slow.zip").absoluteString), "slow link accepted")
    let moving = await waitUntil { (model.items.first?.receivedBytes ?? 0) > 50_000 }
    check(moving, "slow link running")
    guard let slow = model.items.first else { return }
    model.cancel(slow)
    let cancelled = await waitUntil { slow.state == .cancelled }
    check(cancelled, "cancelled")
    model.retry(slow)
    let resumed = await waitUntil(90) { slow.state == .finished }
    check(resumed, "retried and finished (state \(slow.state))")
    checkEqual(slow.file?.sha256, sha256(slowBody), "resumed content is identical")
    check(model.items.allSatisfy { $0.state != .downloading }, "nothing left running")
}

// MARK: - Done

server.stop()
try? FileManager.default.removeItem(at: tempRoot)
print("\n\(testCount.value) tests, \(results.passed) checks passed, \(results.failed) failed")
exit(results.failed == 0 ? 0 : 1)
