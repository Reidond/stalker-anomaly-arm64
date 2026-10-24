import Foundation

/// A tiny HTTP/1.1 server on 127.0.0.1 for the downloader tests: one thread per connection, `Connection: close`,
/// every request recorded. Handlers write the response themselves so they can stream slowly, truncate or chunk.
final class TestServer: @unchecked Sendable {
    struct Request: Sendable {
        let method: String
        let target: String
        let headers: [String: String] // lower-case names

        var path: String { target.split(separator: "?", maxSplits: 1).first.map(String.init) ?? target }
    }

    final class Connection: @unchecked Sendable {
        let fd: Int32
        init(fd: Int32) { self.fd = fd }

        @discardableResult
        func write(_ data: Data) -> Bool {
            var sent = 0
            return data.withUnsafeBytes { raw -> Bool in
                while sent < data.count {
                    let n = Darwin.send(fd, raw.baseAddress!.advanced(by: sent), data.count - sent, 0)
                    if n <= 0 { return false }
                    sent += n
                }
                return true
            }
        }

        @discardableResult
        func head(_ status: Int, _ headers: [(String, String)]) -> Bool {
            var text = "HTTP/1.1 \(status) \(TestServer.reason(status))\r\n"
            for (k, v) in headers { text += "\(k): \(v)\r\n" }
            text += "Connection: close\r\n\r\n"
            return write(Data(text.utf8))
        }

        func respond(_ status: Int, _ headers: [(String, String)] = [], body: Data = Data()) {
            var h = headers
            if !h.contains(where: { $0.0.lowercased() == "content-length" }) { h.append(("Content-Length", String(body.count))) }
            head(status, h)
            write(body)
        }
    }

    let port: UInt16
    private let listenFD: Int32
    private let handler: @Sendable (Request, Connection) -> Void
    private let lock = NSLock()
    private var recorded: [Request] = []
    private var stopped = false

    var requests: [Request] {
        lock.lock(); defer { lock.unlock() }
        return recorded
    }

    func requests(for path: String) -> [Request] { requests.filter { $0.path == path } }

    func clearRequests() {
        lock.lock(); recorded.removeAll(); lock.unlock()
    }

    var baseURL: URL { URL(string: "http://127.0.0.1:\(port)")! }

    init(handler: @escaping @Sendable (Request, Connection) -> Void) throws {
        self.handler = handler
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw POSIXError(.EIO) }
        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout<Int32>.size))
        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = 0
        addr.sin_addr = in_addr(s_addr: inet_addr("127.0.0.1"))
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard bound == 0, listen(fd, 16) == 0 else { close(fd); throw POSIXError(.EADDRINUSE) }
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        withUnsafeMutablePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { _ = getsockname(fd, $0, &len) }
        }
        self.port = UInt16(bigEndian: addr.sin_port)
        self.listenFD = fd
        let acceptor = Thread { [self] in self.acceptLoop() }
        acceptor.name = "TestServer.accept"
        acceptor.start()
    }

    func stop() {
        lock.lock()
        let was = stopped
        stopped = true
        lock.unlock()
        if !was {
            shutdown(listenFD, SHUT_RDWR)
            close(listenFD)
        }
    }

    private func acceptLoop() {
        while true {
            let client = accept(listenFD, nil, nil)
            if client < 0 { return }
            var yes: Int32 = 1
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout<Int32>.size))
            let thread = Thread { [self] in self.serve(client) }
            thread.start()
        }
    }

    private func serve(_ fd: Int32) {
        defer { close(fd) }
        var buffer = Data()
        var chunk = [UInt8](repeating: 0, count: 4096)
        let terminator = Data("\r\n\r\n".utf8)
        while buffer.range(of: terminator) == nil {
            let n = recv(fd, &chunk, chunk.count, 0)
            if n <= 0 { return }
            buffer.append(chunk, count: n)
            if buffer.count > 64 * 1024 { return }
        }
        let headEnd = buffer.range(of: terminator)!.lowerBound
        let text = String(decoding: buffer[..<headEnd], as: UTF8.self)
        var lines = text.components(separatedBy: "\r\n")
        let first = lines.removeFirst().split(separator: " ")
        guard first.count >= 2 else { return }
        var headers: [String: String] = [:]
        for line in lines {
            guard let colon = line.firstIndex(of: ":") else { continue }
            headers[line[..<colon].lowercased()] = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
        }
        let request = Request(method: String(first[0]), target: String(first[1]), headers: headers)
        lock.lock(); recorded.append(request); lock.unlock()
        handler(request, Connection(fd: fd))
    }

    static func reason(_ status: Int) -> String {
        switch status {
        case 200: return "OK"
        case 206: return "Partial Content"
        case 301: return "Moved Permanently"
        case 302: return "Found"
        case 404: return "Not Found"
        case 416: return "Range Not Satisfiable"
        case 500: return "Internal Server Error"
        default: return "Status"
        }
    }
}
