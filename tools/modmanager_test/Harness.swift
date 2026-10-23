import Foundation

// A tiny test runner: each test is an async closure that throws on the first failed expectation. The process exits non-zero
// when any test fails, so ctest sees it.

struct TestFailure: Error, CustomStringConvertible {
    let message: String
    var description: String { message }
}

struct TestCase {
    let name: String
    let body: () async throws -> Void
}

nonisolated(unsafe) var allTests: [TestCase] = []

func test(_ name: String, _ body: @escaping () async throws -> Void) {
    allTests.append(TestCase(name: name, body: body))
}

func expect(_ condition: @autoclosure () -> Bool, _ message: @autoclosure () -> String = "expectation failed",
            file: StaticString = #fileID, line: UInt = #line) throws {
    if !condition() { throw TestFailure(message: "\(file):\(line): \(message())") }
}

func expectEqual<T: Equatable>(_ actual: T, _ expected: T, _ what: String = "", file: StaticString = #fileID, line: UInt = #line) throws {
    if actual != expected {
        throw TestFailure(message: "\(file):\(line): \(what.isEmpty ? "" : what + ": ")expected \(expected), got \(actual)")
    }
}

/// Runs `body` and requires it to throw an error for which `matches` holds.
func expectError(_ what: String = "", file: StaticString = #fileID, line: UInt = #line,
                 matches: (Error) -> Bool, _ body: () async throws -> Void) async throws {
    do {
        try await body()
    } catch {
        if matches(error) { return }
        throw TestFailure(message: "\(file):\(line): \(what.isEmpty ? "" : what + ": ")unexpected error \(error)")
    }
    throw TestFailure(message: "\(file):\(line): \(what.isEmpty ? "" : what + ": ")expected an error, none was thrown")
}

/// A fresh empty directory, removed after `body`.
func withTempDir<T>(_ body: (URL) async throws -> T) async throws -> T {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("modmanager-test-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    return try await body(dir)
}

func runAllTests() async -> Int32 {
    let filter = ProcessInfo.processInfo.environment["MODMANAGER_TEST_FILTER"]
    var failed: [String] = []
    var ran = 0
    for t in allTests where filter == nil || t.name.contains(filter!) {
        ran += 1
        let start = Date()
        do {
            try await t.body()
            print(String(format: "ok   %@ (%.2fs)", t.name, Date().timeIntervalSince(start)))
        } catch {
            failed.append(t.name)
            print("FAIL \(t.name)\n     \(error)")
        }
    }
    print("\(ran - failed.count)/\(ran) passed")
    return failed.isEmpty ? 0 : 1
}
