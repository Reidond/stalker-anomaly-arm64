# Mod manager

The mod manager inside the Anomaly app: a SwiftUI launcher with the mod list, mods stored apart from the game data and
mounted at engine start in load order, and several ways to get a mod in (Files app, an in-app browser, a pasted link,
FOMOD installers). Each part of it is documented in its own section below, by the session that built it.

## Downloads

`ModDownloads` is the part that fetches mod archives: an in-app browser for ModDB and GitHub (`DownloadsView`) and a
downloader for pasted links (`LinkDownloader`). It is a Swift static library for iOS 17 and macOS 14 that the launcher
links (`cmake/targets/ModDownloads.cmake`, sources in `src/Platform/ModDownloads`). It only downloads: a finished file goes to
the callback, and the importer owns it from there.

ModDB blocks scripted downloads (Cloudflare answers `curl` with 403), so ModDB goes through a real web view and a real
tap. GitHub works either way.

### Using it

```swift
import ModDownloads

DownloadsView(downloadsDir: modsRoot.appendingPathComponent(".downloads"),
              onDownloaded: { url in importer.install(archive: url) },            // the contract
              onDownloadedFile: { file in /* DownloadedFile: URLs, name, size, sha256 */ })  // optional, richer

DownloadsView.releaseSharedState()   // before the engine starts
```

- `onDownloaded(URL)` is the contract callback. `onDownloadedFile(DownloadedFile)` is the same event with what `mod.json`'s
  `source` needs; both are called, on the main actor, once the file is complete, hashed and in place in `downloadsDir`.
- The page, the cookies and the list of downloads survive the view being recreated (one model per folder is kept), so
  switching to another launcher screen and back does not reload the site or lose a running download.
  The price is a live web view: **call `DownloadsView.releaseSharedState()` before the engine starts** (it cancels
  running downloads, frees the web view and its content process).
- The importer deletes the file after a successful install. A row of the list whose file is gone reads "Imported or removed".
- The library has no dependency on the engine, the shell or the other mod manager modules.

`DownloadedFile` (`kind` is `.link` for a pasted link, `.browser` for the web view):

| field | for `mod.json` "source" |
|---|---|
| `kind` | `"url"` (`.link`) or `"browser"` (`.browser`) |
| `originalURL` | `url`: the pasted link, or the first request of the browser download |
| `finalURL` | the URL that served the bytes after every redirect |
| `pageURL` | the page the browser was showing (browser downloads only; the mod's page on ModDB) |
| `fileName` | `file_name`, as saved (made unique if the name was taken) |
| `sha256` | `sha256` of the file, lower-case hex |
| `size` | bytes |

### The browser

`DownloadsView` is a bar (back, forward, reload/stop, address field, Go, bookmark star, downloads toggle), a bar of
bookmarks, the web page (WKWebView), and the downloads list (a side panel on wide screens, over the page on narrow ones).

- **Address field**: accepts a link, a bare host (`github.com/x` gets https; `localhost` and LAN addresses get http) or
  words (a DuckDuckGo search). Only http and https open; `javascript:`, `file:`, `data:`, `itms-apps:` and the like are
  refused, in the field and in links on pages.
- **Bookmarks**: two built in (ModDB's Anomaly addons, a GitHub search for Anomaly addons), plus the user's own: the star adds
  or removes the current page, "Edit" renames, reorders, removes and adds by link. Kept in UserDefaults.
- **Downloads list**: progress, Cancel, Retry (resumes), Allow (for a download a page tried without a tap), an x to remove the row.
  "Clear finished" removes finished rows only; it never deletes a file.
- **Paste a link**: the field at the top of the list. It downloads straight to the downloads folder with `LinkDownloader`.
  If the link is a web page (the usual case with ModDB), the row says so and the page opens in the browser.
- **Touch, pointer, keyboard, controller**:
  - every control is a real `Button` of at least 44 x 44 points with a hover highlight and a focus ring, so touch, a mouse or trackpad
    (iPad pointer effects) and keyboard or controller focus all work. The system's focus engine moves between them.
  - web pages have no focus ring, so a gamepad drives the page itself: left stick or D-pad scrolls, the right stick moves a
    cursor (drawn while in use), A taps what is under it, L1 / B go back, R1 goes forward, R3 recentres the cursor. The
    code only reads the pad (no handlers are installed), so it cannot take input from anything else. A keyboard scrolls the page as usual.
- Both layouts (wide and narrow) are in the PR as offscreen renders.

### What is downloaded, and when

- **Nothing downloads without a tap.** WebKit turns a navigation into a download (an `<a download>`, an attachment, a type
  the web view cannot show); the app lets it through only if the user asked for it:
  - a tap inside the page: a user script listens for `click` events with `isTrusted == true` (a page cannot fake those) and
    tells the app. A tap is good for 30 s and for **one** download, which covers ModDB's "Download" button, mirror page and
    timed redirect;
  - the user loading a URL (address field, bookmark, back, reload) is good for 20 s, but only while that navigation is
    still unresolved: the moment it turns out to be a web page, the permission is dropped, so a page cannot use it for a
    download of its own.
  - anything else is not started. It shows as a row "<host> tried to start this without a tap" with Allow and an x, and a notice.
    Allow starts it with the web view's cookies, as the user's own tap.
  - scripted pop-ups (`window.open`) are dropped unless a tap caused them; links with other schemes are refused.
- **http and https only**, in the web view, in downloads and in every redirect (the downloader refuses a redirect to `ftp:` or `file:`).
  iPadOS's App Transport Security also blocks plain http for both the downloader and the web view; the app's Info.plist has no
  exception, so an http-only mirror shows "only offers a plain http connection". The Mac has no such rule.
- **Cookies stay in the web view.** The browser uses WebKit's persistent data store (a ModDB or GitHub login and Cloudflare's
  clearance survive restarts) and nothing else reads it. `LinkDownloader` uses its own ephemeral session with cookies
  switched off, so a pasted link never carries or collects any.
- Downloads go to the folder the caller passes (`<mods root>/.downloads/`), created on first use and excluded from backups.
  In-flight files are hidden dot files there (`.part-*` for the downloader, `.wk-*.part` for the browser), moved to their
  final name once complete and hashed. Partial files older than 7 days are deleted when the browser opens.
- File names come from Content-Disposition (`filename*` preferred), else the URL, and are reduced to one safe path
  component: no separators, `..`, control characters or leading dot, at most 200 bytes; a taken name becomes `name (1).zip`.

### `LinkDownloader`

```swift
public final class LinkDownloader {
    public init()
    public func download(_ url: URL, to dir: URL, progress: @escaping (Double) -> Void) async throws -> URL   // the contract
    public func downloadFile(_ url: URL, to dir: URL, progress: @escaping @Sendable (DownloadProgress) -> Void = { _ in }) async throws -> DownloadedFile
    public init(reserveBytes: Int64, freeSpace: @escaping @Sendable (URL) -> Int64?)   // tests
    public static func hasPartial(for:in:), partialSize(for:in:), discardPartial(for:in:), removeStalePartials(in:olderThan:), availableCapacity(at:)
}
```

- A URLSession data task streams into a hidden partial file. Redirects are followed (http(s) only); `finalURL` is where it ended.
- Progress is 0...1 (it stays 0 while the server has not said how big the file is), on the main queue, at most ten times a second.
  `downloadFile`'s `DownloadProgress` has the bytes too.
- **Not a file**: a `text/html` response, or an octet-stream / plain-text / untyped one whose first bytes are HTML, throws
  `LinkDownloadError.notAFile(url:, mimeType:)` before anything is written. The view opens `url` in the browser. An HTML
  file sent as an attachment (`Content-Disposition: attachment`) is a file.
- **Resume**: a download that stops (cancel, dropped connection) keeps its partial file. The next download of the same URL sends
  `Range: bytes=N-` with `If-Range`: the ETag (not a weak one), else Last-Modified. A `206` that starts at N continues; `200` (range
  ignored, or the file changed) starts over; `416` with the full size finishes. A server with neither validator is
  never resumed (it could not be told the file is the same).
- **Disk space**: the size from Content-Length (or Content-Range) plus a 128 MiB reserve is checked against the volume's
  space for user data before the first byte; without a size, a full disk surfaces as the same error while writing.
- **Cancel**: cancelling the Swift task throws `CancellationError` and keeps the partial for a later resume.
- Errors (`LinkDownloadError`): `unsupportedScheme`, `notAFile`, `httpStatus`, `insufficientDiskSpace`, `incomplete`,
  `network` (with a hint for the ATS case), `fileSystem`.

### Building and testing

```
cmake --preset macos-arm64            # builds ModDownloads with the rest; Swift is enabled in cmake/targets/ModDownloads.cmake
ninja -C build/macos-arm64 mod_downloads_test mod_downloads_snapshot
build/macos-arm64/tools/mod_downloads_test/mod_downloads_test        # or: ctest -R mod_downloads
build/macos-arm64/tools/mod_downloads_snapshot/mod_downloads_snapshot --out view.png [--narrow] [--dark]
```

`mod_downloads_test` runs without a window or a Dock icon (the process is `.prohibited`; WebKit runs headless with no window). It starts
a small HTTP server on 127.0.0.1 and checks:

- `LinkDownloader`: redirects (original and final URL), Content-Disposition (`filename*`, quotes, `../` names), HTML detection (page, redirect to a
  page, HTML under octet-stream, tiny body, HTML as an attachment), status errors, ftp/file links and redirects, the free-space check, chunked
  responses without a length, no cookies, resume after a dropped connection, no resume without a validator, a replaced file, a server
  that ignores Range, cancel then resume, cancel before start, progress, stale partials.
- the gate, address parsing, bookmarks and the controller mapping, as pure logic;
- the web view, headless: a typed link to a file downloads, a page's own script cannot start a download (both `a.click()` and a timed `location`), Allow
  starts a held one, a scripted click does not open the gate while the trusted-tap message does, one tap pays for one download, an unknown type
  is downloaded, `javascript:` and a page's `ftp:` link are refused, WKDownload cancel and Retry resume from the kept bytes, pasted links.

The Swift sources build with `Swift_COMPILATION_MODE wholemodule` and CMP0157: CMake 3.28's default smashes `-incremental` into the Debug flags,
and the driver then intermittently fails to link a Swift executable (`cannotResolveTempPath`).

The launcher's linking: a C++ target that links `ModDownloads` needs the Swift runtime (`-L$(xcrun --show-sdk-path)/usr/lib/swift`,
rpath `/usr/lib/swift`) and the frameworks the library lists (WebKit, SwiftUI); a Swift launcher gets them from the module.

### On the iPad

(see the results below)
