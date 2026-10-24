import Foundation

/// "Nothing downloads without a user tap." The web view may only start a download when the user asked for it, and one
/// request pays for one download. Two kinds of request count:
///
/// - a tap inside a page (the page's own trusted `click` event, or the controller's A button): good for `tapWindow`
///   seconds, long enough for ModDB's chain of "Download" button, mirror page and timed redirect to the file;
/// - the user asking the app to load a URL (address bar, bookmark, reload, back): good for `typedWindow` seconds, but only
///   for that navigation, so it is dropped as soon as the navigation turns out to be a web page and not a file. A page
///   cannot use it to start a download of its own.
///
/// Scripted events (`isTrusted == false`) never count.
struct DownloadGate {
    var tapWindow: TimeInterval = 30
    var typedWindow: TimeInterval = 20
    private var lastTap: TimeInterval?
    private var lastTyped: TimeInterval?

    mutating func noteTap(at now: TimeInterval) {
        lastTap = now
    }

    mutating func noteTypedNavigation(at now: TimeInterval) {
        lastTyped = now
    }

    /// A main-frame response is being shown as a page: the navigation the user asked for was not a download.
    mutating func pageDisplayed() {
        lastTyped = nil
    }

    func isOpen(at now: TimeInterval) -> Bool {
        isRecent(lastTap, window: tapWindow, now: now) || isRecent(lastTyped, window: typedWindow, now: now)
    }

    /// True once per request: a download that passes uses it up.
    mutating func consume(at now: TimeInterval) -> Bool {
        if isRecent(lastTap, window: tapWindow, now: now) {
            lastTap = nil
            return true
        }
        if isRecent(lastTyped, window: typedWindow, now: now) {
            lastTyped = nil
            return true
        }
        return false
    }

    private func isRecent(_ time: TimeInterval?, window: TimeInterval, now: TimeInterval) -> Bool {
        guard let time else { return false }
        return now >= time && now - time <= window
    }
}

extension DownloadGate {
    static var now: TimeInterval { ProcessInfo.processInfo.systemUptime }
}
