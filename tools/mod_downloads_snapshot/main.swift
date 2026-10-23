// Draws DownloadsView without showing a window, the way the launcher's snapshot tool does: the view is hosted in a window that is never
// ordered in and drawn into a bitmap, so the stock controls (lists, toggles, buttons, the inspector) are the real macOS ones.
//   mod_downloads_snapshot <out-dir> [--dark] [--only <name>]
// A web view cannot be drawn into such a bitmap, so the page area shows an image of a local sample page taken from an off-screen web view.
import AppKit
import SwiftUI
import WebKit
@testable import ModDownloads

NSApplication.shared.setActivationPolicy(.prohibited)

var outputArgument: URL?
var dark = false
var only: String?
var arguments = Array(CommandLine.arguments.dropFirst())
while let argument = arguments.first {
    arguments.removeFirst()
    switch argument {
    case "--dark": dark = true
    case "--only": only = arguments.isEmpty ? nil : arguments.removeFirst()
    default: outputArgument = URL(fileURLWithPath: argument)
    }
}
guard let outputDirectory = outputArgument else {
    FileHandle.standardError.write(Data("usage: mod_downloads_snapshot <out-dir> [--dark] [--only <name>]\n".utf8))
    exit(2)
}

let samplePage = """
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Sample page</title>
<style>body{font-family:-apple-system,Helvetica,sans-serif;margin:40px;color:#222;background:#fff}
h1{font-size:28px}.card{border:1px solid #d5d8dc;border-radius:12px;padding:14px 18px;margin:12px 0;max-width:640px;background:#f7f8f9}
.btn{display:inline-block;background:#2a6df4;color:#fff;border-radius:8px;padding:10px 18px;font-weight:600}
small{color:#667}</style></head><body>
<h1>Web page area</h1>
<p>This is the web view. In the app it shows ModDB, GitHub or any other site; here a local sample page stands in.</p>
<div class="card"><b>Example addon</b><br><small>Version 1.0 &middot; 12.3 MB</small><br><br><span class="btn">Download</span></div>
<div class="card"><b>Another addon</b><br><small>Version 2.4 &middot; 48 MB</small><br><br><span class="btn">Download</span></div>
</body></html>
"""

@MainActor
func settle(_ seconds: Double = 0.3) {
    RunLoop.main.run(until: Date().addingTimeInterval(seconds))
}

@MainActor
func writePNG<V: View>(_ view: V, size: CGSize, to url: URL) {
    let host = NSHostingView(rootView: view)
    let window = NSWindow(contentRect: CGRect(origin: .zero, size: size), styleMask: [.titled], backing: .buffered, defer: false)
    window.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
    window.contentView = host
    host.frame = CGRect(origin: .zero, size: size)
    for _ in 0..<4 {
        settle(0.2)
        host.layoutSubtreeIfNeeded()
        host.displayIfNeeded()
    }
    guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { print("failed: \(url.lastPathComponent)"); return }
    host.cacheDisplay(in: host.bounds, to: rep)
    guard let data = rep.representation(using: .png, properties: [:]) else { print("failed: \(url.lastPathComponent)"); return }
    try? data.write(to: url)
    print("wrote \(url.path)")
}

@MainActor
func makeModel(in dir: URL, items: Bool, defaults: UserDefaults) -> DownloadsModel {
    let browser = BrowserModel(dataStore: .nonPersistent())
    let bookmarks = BookmarkStore(defaults: defaults)
    bookmarks.add(title: "Open-source release", url: URL(string: "https://github.com/example/project/releases")!)
    bookmarks.add(title: "My mod list", url: URL(string: "https://example.org/mods")!)
    let model = DownloadsModel(dir: dir, browser: browser, bookmarks: bookmarks)
    guard items else { return model }
    func add(_ name: String, _ url: String, _ state: DownloadItem.State, received: Int64 = 0, total: Int64? = nil, origin: DownloadedFile.Kind = .browser) {
        let item = DownloadItem(origin: origin, sourceURL: URL(string: url)!, fileName: name, state: state)
        item.receivedBytes = received
        item.totalBytes = total
        model.addPreviewItem(item)
    }
    add("example-addon-1.0.7z", "https://www.moddb.com/downloads/mirror/1/2/3", .downloading, received: 18_400_000, total: 51_200_000)
    add("another-addon.zip", "https://github.com/example/project/releases/download/v2/another-addon.zip", .downloading, received: 3_200_000, total: nil, origin: .link)
    add("tried-by-a-page.zip", "https://ads.example.net/start/9", .awaitingApproval)
    add("texture-pack.rar", "https://files.example.org/texture-pack.rar", .failed("The connection dropped after 20000000 of 91000000 bytes."), received: 20_000_000, total: 91_000_000)
    add("sound-fix.zip", "https://example.org/sound-fix.zip", .cancelled, received: 1_000_000, total: 7_000_000)
    let done = DownloadItem(origin: .link, sourceURL: URL(string: "https://example.org/done.zip")!, fileName: "weather-pack.zip", state: .finished)
    done.totalBytes = 24_000_000
    model.addPreviewItem(done)
    return model
}

@MainActor
func run() async {
    let work = FileManager.default.temporaryDirectory.appendingPathComponent("mod_downloads_snapshot-\(ProcessInfo.processInfo.processIdentifier)")
    let suite = "mod_downloads_snapshot.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: suite)!
    defer { defaults.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: work) }
    try? FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)

    // the page area: a real web view, off screen, rendered to an image
    let web = WKWebView(frame: CGRect(x: 0, y: 0, width: 900, height: 700), configuration: WKWebViewConfiguration())
    web.loadHTMLString(samplePage, baseURL: nil)
    for _ in 0..<200 where web.title != "Sample page" { try? await Task.sleep(nanoseconds: 50_000_000) }
    try? await Task.sleep(nanoseconds: 500_000_000)
    let pageImage = try? await web.takeSnapshot(configuration: nil)
    let page = AnyView(
        Color(nsColor: .textBackgroundColor).overlay(alignment: .topLeading) {
            if let pageImage { Image(nsImage: pageImage).resizable().aspectRatio(contentMode: .fill) }
        }
        .clipped()
    )

    let size = CGSize(width: 1200, height: 780)
    let shots: [(String, CGSize, @MainActor () -> AnyView)] = [
        ("01-page-with-downloads", size, {
            let model = makeModel(in: work, items: true, defaults: defaults)
            model.browser.addressText = "https://www.moddb.com/mods/stalker-anomaly/addons"
            model.showPanel = true
            return AnyView(DownloadsView(model: model, webContent: page).frame(width: size.width, height: size.height))
        }),
        ("02-page", size, {
            let model = makeModel(in: work, items: false, defaults: defaults)
            model.browser.addressText = "https://www.moddb.com/mods/stalker-anomaly/addons"
            return AnyView(DownloadsView(model: model, webContent: page).frame(width: size.width, height: size.height))
        }),
        ("03-start-page", size, {
            let model = makeModel(in: work, items: false, defaults: defaults)
            return AnyView(DownloadsView(model: model).frame(width: size.width, height: size.height))
        }),
        ("04-bookmarks", CGSize(width: 560, height: 620), {
            let model = makeModel(in: work, items: false, defaults: defaults)
            return AnyView(BookmarkEditor(bookmarks: model.bookmarks).tint(DownloadsTheme.anomaly.accent).frame(width: 560, height: 620))
        }),
    ]
    for (name, shotSize, build) in shots where only == nil || name.contains(only!) {
        let view = build().tint(DownloadsTheme.anomaly.accent)
        writePNG(view, size: shotSize, to: outputDirectory.appendingPathComponent("\(name)\(dark ? "-dark" : "").png"))
    }
}

await run()
exit(0)
