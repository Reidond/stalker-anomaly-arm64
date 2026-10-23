// Renders DownloadsView with ImageRenderer. Nothing is shown on screen: there is no window, and the process is not a regular app.
//   mod_downloads_snapshot --out view.png [--width 1180 --height 760] [--dark] [--narrow]
// ImageRenderer cannot draw a WKWebView, so the page area shows a snapshot of a local sample page taken from an off-screen web view.
import AppKit
import SwiftUI
import WebKit
@testable import ModDownloads

NSApplication.shared.setActivationPolicy(.prohibited)

var out = "mod_downloads.png"
var width: CGFloat = 1180
var height: CGFloat = 760
var dark = false
var solid = false
var args = Array(CommandLine.arguments.dropFirst())
while let arg = args.first {
    args.removeFirst()
    switch arg {
    case "--out": out = args.removeFirst()
    case "--width": width = CGFloat(Double(args.removeFirst()) ?? 1180)
    case "--height": height = CGFloat(Double(args.removeFirst()) ?? 760)
    case "--dark": dark = true
    case "--solid": solid = true // Reduce Transparency / no glass
    case "--narrow": width = 700; height = 900
    default: print("unknown argument \(arg)"); exit(2)
    }
}

let samplePage = """
<!DOCTYPE html><html><head><meta charset="utf-8"><title>Sample page</title>
<style>body{font-family:-apple-system,Helvetica,sans-serif;margin:40px;padding-top:150px;color:#e6e2d8;background:#1c2530}
h1{font-size:28px}.card{border:1px solid #3a4756;border-radius:12px;padding:14px 18px;margin:12px 0;max-width:640px;background:#232f3c}
.btn{display:inline-block;background:#3b6ea5;color:#fff;border-radius:8px;padding:10px 18px;font-weight:600}
small{color:#93a1b1}</style></head><body>
<h1>Web page area</h1>
<p>This is the web view. In the app it shows ModDB, GitHub or any other site; here a local sample page stands in.</p>
<div class="card"><b>Example addon</b><br><small>Version 1.0 &middot; 12.3 MB</small><br><br><span class="btn">Download</span></div>
<div class="card"><b>Another addon</b><br><small>Version 2.4 &middot; 48 MB</small><br><br><span class="btn">Download</span></div>
</body></html>
"""

@MainActor
func run() async {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("mod_downloads_snapshot-\(UUID().uuidString)")
    let suite = "mod_downloads_snapshot.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: suite)!
    defer { defaults.removePersistentDomain(forName: suite); try? FileManager.default.removeItem(at: dir) }

    let browser = BrowserModel(dataStore: .nonPersistent())
    let bookmarks = BookmarkStore(defaults: defaults)
    bookmarks.add(title: "Open-source release", url: URL(string: "https://github.com/example/project/releases")!)
    bookmarks.add(title: "My mod list", url: URL(string: "https://example.org/mods")!)
    let model = DownloadsModel(dir: dir, browser: browser, bookmarks: bookmarks)
    browser.addressText = "https://www.moddb.com/mods/stalker-anomaly/addons"

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
    add("release-notes.html", "https://example.org/notes", .openedInBrowser, origin: .link)

    // a real, off-screen web view draws the page area
    let web = WKWebView(frame: CGRect(x: 0, y: 0, width: 820, height: 560), configuration: WKWebViewConfiguration())
    web.loadHTMLString(samplePage, baseURL: nil)
    for _ in 0..<200 where web.title != "Sample page" { try? await Task.sleep(nanoseconds: 50_000_000) }
    try? await Task.sleep(nanoseconds: 500_000_000)
    let snapshot = try? await web.takeSnapshot(configuration: nil)
    print("web view snapshot: \(snapshot == nil ? "failed" : "ok")")

    // the colour sets the size; the image only fills it
    let page: AnyView = AnyView(
        Color(red: 0.11, green: 0.145, blue: 0.19).overlay(alignment: .topLeading) {
            if let snapshot { Image(nsImage: snapshot).resizable().aspectRatio(contentMode: .fill) }
        }
        .clipped()
    )

    var theme = DownloadsTheme.anomaly
    theme.prefersGlass = !solid
    let view = DownloadsView(model: model, webContent: page)
        .frame(width: width, height: height)
        .environment(\.colorScheme, dark ? .dark : .light)
        .environment(\.snapshotMode, true)
        .downloadsTheme(theme)
    let renderer = ImageRenderer(content: view)
    renderer.scale = 2
    guard let image = renderer.nsImage, let tiff = image.tiffRepresentation, let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:]) else {
        print("render failed")
        exit(1)
    }
    try? png.write(to: URL(fileURLWithPath: out))
    print("wrote \(out) (\(Int(width))x\(Int(height)) pt, \(png.count) bytes)")
    model.shutdown()
}

await run()
exit(0)
