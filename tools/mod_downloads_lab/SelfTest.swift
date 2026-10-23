// Scripted runs of the browser flows for the iPad, started with `--selftest <name>`. The report goes to Documents/lab-report.txt
// (and the console). A real tap on a page cannot be made from here, so the script notes a tap (what the page's trusted click
// would do) and then clicks the element from JavaScript; everything after that is the real path.
import Foundation
import SwiftUI
@testable import ModDownloads

@MainActor
enum SelfTest {
    static func note(_ text: String) {
        let line = "[\(String(format: "%.1f", ProcessInfo.processInfo.systemUptime.truncatingRemainder(dividingBy: 10_000)))] \(text)\n"
        print(line, terminator: "")
        if let data = line.data(using: .utf8) {
            if let handle = try? FileHandle(forWritingTo: LabPaths.report) {
                _ = try? handle.seekToEnd()
                try? handle.write(contentsOf: data)
                try? handle.close()
            }
        }
    }

    static func runIfRequested() async {
        let args = CommandLine.arguments
        guard let index = args.firstIndex(of: "--selftest"), index + 1 < args.count else { return }
        try? FileManager.default.removeItem(at: LabPaths.report)
        FileManager.default.createFile(atPath: LabPaths.report.path, contents: nil)
        let name = args[index + 1]
        note("selftest \(name)")
        let model = DownloadsModel.shared(for: LabPaths.downloads)
        switch name {
        case "github": await github(model)
        case "moddb": await moddb(model)
        case "local": await local(model)
        default: note("unknown scenario")
        }
        let leftovers = (try? FileManager.default.contentsOfDirectory(atPath: LabPaths.downloads.path)) ?? []
        note("downloads folder after the run: \(leftovers.isEmpty ? "empty" : leftovers.joined(separator: ", "))")
        note("DONE")
        try? await Task.sleep(nanoseconds: 500_000_000)
        exit(0)
    }

    // MARK: helpers

    static func wait(_ seconds: Double, _ condition: () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(seconds)
        while Date() < deadline {
            if condition() { return true }
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        return condition()
    }

    static func js(_ model: DownloadsModel, _ script: String) async -> String {
        do {
            let value = try await model.browser.webView.evaluateJavaScript(script)
            return value.map { "\($0)" } ?? ""
        } catch {
            return "error: \(error.localizedDescription)"
        }
    }

    static func describe(_ item: DownloadItem) -> String {
        "\(item.fileName) [\(item.origin)] \(item.state) \(item.receivedBytes)/\(item.totalBytes.map(String.init) ?? "?") bytes"
    }

    static func cleanUp(_ model: DownloadsModel) {
        for item in model.items {
            if item.state == .downloading { model.cancel(item) }
        }
        for item in model.items { model.remove(item) }
        if let names = try? FileManager.default.contentsOfDirectory(atPath: LabPaths.downloads.path) {
            for name in names { try? FileManager.default.removeItem(at: LabPaths.downloads.appendingPathComponent(name)) }
        }
    }

    // MARK: scenarios

    /// GitHub release asset of an open-source project (jq's sha256sum.txt, 2,129 bytes): browser flow and pasted-link flow.
    static func github(_ model: DownloadsModel) async {
        let release = URL(string: "https://github.com/jqlang/jq/releases/tag/jq-1.7.1")!
        let assetPath = "/jqlang/jq/releases/download/jq-1.7.1/sha256sum.txt"
        let expectedSHA = "f1f692fe9f923a4f5c6741729fb5931212ebccfff976225feb7a2ccec8635da4"
        model.browser.load(release)
        let loaded = await wait(45) { model.browser.pageTitle.contains("jq") }
        note("release page loaded: \(loaded), title: \(model.browser.pageTitle)")
        let selector = "a[href$=\"\(assetPath)\"]"
        var present = false
        for _ in 0..<150 {
            if await js(model, "document.querySelector('\(selector)') ? 'yes' : 'no'") == "yes" { present = true; break }
            try? await Task.sleep(nanoseconds: 300_000_000)
        }
        note("asset link on the page: \(present)")

        // 1. untapped: the page's own script clicks the link with no tap
        let before = model.items.count
        _ = await js(model, "document.querySelector('\(selector)').click()")
        let blocked = await wait(20) { model.items.contains { $0.state == .awaitingApproval } }
        note("untapped click held for approval: \(blocked) (items \(before) -> \(model.items.count))")
        for item in model.items where item.state == .awaitingApproval { model.remove(item) }

        // 2. tapped: one tap, one download
        model.browser.noteTap()
        _ = await js(model, "document.querySelector('\(selector)').click()")
        let started = await wait(30) { model.items.contains { $0.origin == .browser } }
        note("tapped click started a browser download: \(started)")
        let done = await wait(60) { model.items.contains { $0.origin == .browser && $0.state == .finished } }
        if let item = model.items.first(where: { $0.origin == .browser }) {
            note("browser download: \(describe(item)); done=\(done)")
            if let file = item.file {
                note("  final host \(file.finalURL.host ?? "?"), original \(file.originalURL.absoluteString)")
                note("  sha256 \(file.sha256) matches expected: \(file.sha256 == expectedSHA)")
                note("  page URL \(file.pageURL?.absoluteString ?? "none")")
            }
        }
        for item in model.items { model.remove(item) }

        // 3. pasted link: LinkDownloader, redirect to GitHub's asset host
        let pasted = "https://github.com\(assetPath)"
        _ = model.downloadLink(pasted)
        let linkDone = await wait(60) { model.items.contains { $0.origin == .link && $0.state == .finished } }
        if let item = model.items.first(where: { $0.origin == .link }) {
            note("pasted link: \(describe(item)); done=\(linkDone)")
            if let file = item.file {
                note("  final host \(file.finalURL.host ?? "?"), sha256 matches expected: \(file.sha256 == expectedSHA)")
            }
        }
        for item in model.items { model.remove(item) }

        // 4. a pasted link to a web page is reported and opened in the browser
        _ = model.downloadLink("https://github.com/jqlang/jq/releases")
        let page = await wait(30) { model.items.contains { $0.state == .openedInBrowser } }
        note("pasted web page link reported as a page: \(page)")
        let reopened = await wait(30) { model.browser.currentURL?.path == "/jqlang/jq/releases" }
        note("and the browser shows it: \(reopened)")
        cleanUp(model)
    }

    /// ModDB up to the point the download starts; nothing is kept.
    static func moddb(_ model: DownloadsModel) async {
        model.browser.load(URL(string: "https://www.moddb.com/mods/stalker-anomaly/addons")!)
        let loaded = await wait(60) { !model.browser.pageTitle.isEmpty && !model.browser.isLoading }
        note("addons list: loaded=\(loaded) title=\(model.browser.pageTitle) url=\(model.browser.currentURL?.absoluteString ?? "")")
        let challenge = model.browser.pageTitle.lowercased().contains("just a moment") || model.browser.pageTitle.lowercased().contains("attention")
        note("looks like a challenge page: \(challenge)")
        if challenge {
            _ = await wait(30) { !model.browser.pageTitle.lowercased().contains("just a moment") }
            note("after waiting, title: \(model.browser.pageTitle)")
        }
        // first addon page
        let href = await js(model, "(function(){var a=document.querySelector('a[href*=\"/mods/stalker-anomaly/addons/\"]'); return a ? a.href : '';})()")
        note("first addon link: \(href)")
        guard let addon = URL(string: href), BrowserModel.isWebURL(addon) else { note("no addon link found"); return }
        model.browser.load(addon)
        _ = await wait(60) { model.browser.currentURL?.absoluteString == addon.absoluteString && !model.browser.isLoading }
        note("addon page: \(model.browser.pageTitle)")
        let button = await js(model, "(function(){var a=document.querySelector('a[href*=\"/downloads/start/\"]'); return a ? a.href : '';})()")
        note("download button link: \(button)")
        guard !button.isEmpty else { note("no download button found"); return }
        // a tap on the button
        model.browser.noteTap()
        _ = await js(model, "document.querySelector('a[href*=\"/downloads/start/\"]').click()")
        var lastURL = ""
        let begun = await wait(60) {
            if let url = model.browser.currentURL?.absoluteString, url != lastURL {
                lastURL = url
                note("web view at \(url)")
            }
            return model.items.contains { $0.state == .downloading || $0.state == .finished }
        }
        note("download started: \(begun)")
        if let item = model.items.first { note("item: \(describe(item)) source \(item.sourceURL.absoluteString)") }
        try? await Task.sleep(nanoseconds: 1_500_000_000)
        if let item = model.items.first { note("a moment later: \(describe(item))") }
        cleanUp(model)
    }

    /// No network: a page served by nothing but the web view's own HTML, checking the gate on the device.
    static func local(_ model: DownloadsModel) async {
        let html = "<!DOCTYPE html><html><head><title>Lab page</title></head><body><a id=x href=\"https://example.com/\">a link</a></body></html>"
        model.browser.webView.loadHTMLString(html, baseURL: URL(string: "https://example.com/"))
        let ok = await wait(20) { model.browser.pageTitle == "Lab page" }
        note("local page: \(ok)")
        note("gate open without a tap: \(model.browser.gate.isOpen(at: DownloadGate.now))")
        model.browser.noteTap()
        note("gate open after a tap: \(model.browser.gate.isOpen(at: DownloadGate.now))")
    }
}
