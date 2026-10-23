import SwiftUI
import WebKit

/// The mod manager's browser: ModDB, GitHub or any other site in a web view, a bar of bookmarks, a field for pasting a
/// link to a file, and the list of downloads with progress, cancel and retry.
///
/// Downloads go to `downloadsDir` (the mods root's `.downloads/`). Each finished file is passed to `onDownloaded`, after
/// which the importer owns it (and deletes it once installed). Nothing is downloaded without a tap.
///
/// The page, the cookies and the list survive the view being recreated (one model per folder is kept); call
/// `DownloadsView.releaseSharedState()` before the game starts to free the web view's memory.
public struct DownloadsView: View {
    @ObservedObject private var model: DownloadsModel
    private let webContent: AnyView?

    @State private var pasteText = ""
    @State private var showBookmarkEditor = false
    @FocusState private var addressFocused: Bool

    public init(downloadsDir: URL, onDownloaded: @escaping (URL) -> Void) {
        self.init(downloadsDir: downloadsDir, onDownloaded: onDownloaded, onDownloadedFile: { _ in })
    }

    /// Also reports the URLs, the file name and the SHA-256 of each finished download, for `mod.json`'s "source".
    /// Both closures are called, on the main actor, when a file is complete and in place.
    public init(downloadsDir: URL, onDownloaded: @escaping (URL) -> Void, onDownloadedFile: @escaping (DownloadedFile) -> Void) {
        let model = DownloadsModel.shared(for: downloadsDir)
        model.onDownloaded = onDownloaded
        model.onDownloadedFile = onDownloadedFile
        self._model = ObservedObject(wrappedValue: model)
        self.webContent = nil
    }

    init(model: DownloadsModel, webContent: AnyView? = nil) {
        self._model = ObservedObject(wrappedValue: model)
        self.webContent = webContent
    }

    /// Cancels running downloads and frees the web view and its content process. Call it before the engine starts.
    @MainActor
    public static func releaseSharedState() {
        DownloadsModel.releaseAll()
    }

    public var body: some View {
        GeometryReader { geo in
            let wide = geo.size.width >= 820
            VStack(spacing: 0) {
                BrowserBar(model: model, browser: model.browser, addressFocused: $addressFocused)
                BookmarksBar(model: model, bookmarks: model.bookmarks, showEditor: $showBookmarkEditor)
                if let notice = model.notice {
                    NoticeBanner(text: notice) { model.dismissNotice() }
                }
                Divider()
                HStack(spacing: 0) {
                    pageArea
                    if model.showPanel && wide {
                        Divider()
                        DownloadsPanel(model: model, pasteText: $pasteText)
                            .frame(width: 360)
                    }
                }
                .overlay {
                    if model.showPanel && !wide {
                        DownloadsPanel(model: model, pasteText: $pasteText)
                            .background(Color.pageBackground)
                    }
                }
            }
            .background(Color.pageBackground)
        }
        .sheet(isPresented: $showBookmarkEditor) {
            BookmarkEditor(bookmarks: model.bookmarks)
        }
        .onChange(of: addressFocused) { _, focused in model.browser.isEditingAddress = focused }
    }

    @ViewBuilder
    private var pageArea: some View {
        ZStack(alignment: .top) {
            if let webContent {
                webContent
            } else {
                WebViewContainer(webView: model.browser.webView)
                    .overlay(ControllerLayer(browser: model.browser))
            }
            if model.browser.isLoading {
                ProgressView(value: model.browser.progress)
                    .progressViewStyle(.linear)
                    .tint(.accentColor)
            }
            if let error = model.browser.errorMessage {
                NoticeBanner(text: error, isError: true) { model.browser.errorMessage = nil }
                    .frame(maxHeight: .infinity, alignment: .top)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - Address bar

private struct BrowserBar: View {
    @ObservedObject var model: DownloadsModel
    @ObservedObject var browser: BrowserModel
    @ObservedObject private var bookmarks: BookmarkStore
    var addressFocused: FocusState<Bool>.Binding

    init(model: DownloadsModel, browser: BrowserModel, addressFocused: FocusState<Bool>.Binding) {
        self.model = model
        self.browser = browser
        self.bookmarks = model.bookmarks
        self.addressFocused = addressFocused
    }

    var body: some View {
        HStack(spacing: 6) {
            Button { browser.goBack() } label: { Image(systemName: "chevron.left") }
                .disabled(!browser.canGoBack)
                .help("Back")
                .accessibilityLabel("Back")
            Button { browser.goForward() } label: { Image(systemName: "chevron.right") }
                .disabled(!browser.canGoForward)
                .help("Forward")
                .accessibilityLabel("Forward")
            Button {
                if browser.isLoading { browser.stop() } else { browser.reload() }
            } label: { Image(systemName: browser.isLoading ? "xmark" : "arrow.clockwise") }
                .help(browser.isLoading ? "Stop" : "Reload")
                .accessibilityLabel(browser.isLoading ? "Stop" : "Reload")

            HStack(spacing: 4) {
                TextField("Search or paste a link", text: $browser.addressText)
                    .textFieldStyle(.plain)
                    .focused(addressFocused)
                    .onSubmit { browser.submitAddress() }
                    .submitLabel(.go)
                    #if os(iOS)
                    .textInputAutocapitalization(.never)
                    .keyboardType(.URL)
                    #endif
                    .autocorrectionDisabled()
                    .padding(.leading, 10)
                if !browser.addressText.isEmpty {
                    Button { browser.addressText = "" } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary) }
                        .buttonStyle(.plain)
                        .frame(width: 32, height: 44)
                        .accessibilityLabel("Clear address")
                }
            }
            .frame(minHeight: 44)
            .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.fieldBackground))

            Button { browser.submitAddress() } label: { Text("Go").fontWeight(.semibold) }
                .buttonStyle(BarButtonStyle(prominent: true))
                .focusableControl()
                .help("Open the address")

            Button { toggleBookmark() } label: {
                Image(systemName: bookmarks.isBookmarked(browser.currentURL) ? "star.fill" : "star")
            }
            .disabled(browser.currentURL == nil || !BrowserModel.isWebURL(browser.currentURL))
            .help("Bookmark this page")
            .accessibilityLabel("Bookmark this page")

            Button { model.showPanel.toggle() } label: {
                HStack(spacing: 4) {
                    Image(systemName: "arrow.down.circle")
                    if model.activeCount + model.waitingCount > 0 {
                        Text("\(model.activeCount + model.waitingCount)").font(.caption.bold()).monospacedDigit()
                    }
                }
            }
            .help("Downloads")
            .accessibilityLabel("Downloads")
        }
        .buttonStyle(BarButtonStyle())
        .focusableControl()
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Color.barBackground)
    }

    private func toggleBookmark() {
        guard let url = browser.currentURL else { return }
        if let existing = bookmarks.user.first(where: { $0.url == url }) {
            bookmarks.remove(id: existing.id)
        } else {
            bookmarks.add(title: browser.pageTitle, url: url)
        }
    }
}

// MARK: - Bookmarks

private struct BookmarksBar: View {
    @ObservedObject var model: DownloadsModel
    @ObservedObject var bookmarks: BookmarkStore
    @Binding var showEditor: Bool

    var body: some View {
        HStack(spacing: 6) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(bookmarks.all) { bookmark in
                        Button { model.browser.load(bookmark.url) } label: {
                            Text(bookmark.title).lineLimit(1).font(.callout)
                        }
                        .buttonStyle(BarButtonStyle(minWidth: 0))
                        .focusableControl()
                        .help(bookmark.url.absoluteString)
                    }
                }
                .padding(.horizontal, 10)
            }
            Button { showEditor = true } label: { Text("Edit").font(.callout) }
                .buttonStyle(BarButtonStyle(minWidth: 0))
                .focusableControl()
                .padding(.trailing, 10)
                .help("Rename, reorder or remove bookmarks")
        }
        .padding(.bottom, 4)
        .background(Color.barBackground)
    }
}

private struct BookmarkEditor: View {
    @ObservedObject var bookmarks: BookmarkStore
    @Environment(\.dismiss) private var dismiss
    @State private var newTitle = ""
    @State private var newURL = ""
    @State private var problem: String?

    var body: some View {
        NavigationStack {
            List {
                Section("Built in") {
                    ForEach(BookmarkStore.builtIn) { bookmark in
                        VStack(alignment: .leading) {
                            Text(bookmark.title)
                            Text(bookmark.url.absoluteString).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                        }
                    }
                }
                Section("Yours") {
                    if bookmarks.user.isEmpty {
                        Text("Bookmark the page you are on with the star in the address bar, or add a link below.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(bookmarks.user) { bookmark in
                        HStack {
                            TextField("Title", text: Binding(get: { bookmark.title }, set: { bookmarks.rename(id: bookmark.id, to: $0) }))
                            Text(bookmark.url.host ?? "").font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            Button(role: .destructive) { bookmarks.remove(id: bookmark.id) } label: { Image(systemName: "trash") }
                                .buttonStyle(BarButtonStyle())
                                .accessibilityLabel("Remove \(bookmark.title)")
                        }
                    }
                    .onMove { bookmarks.move(from: $0, to: $1) }
                    .onDelete { offsets in offsets.map { bookmarks.user[$0].id }.forEach { bookmarks.remove(id: $0) } }
                }
                Section("Add a link") {
                    TextField("Title (optional)", text: $newTitle)
                    TextField("https://…", text: $newURL)
                        #if os(iOS)
                        .textInputAutocapitalization(.never)
                        .keyboardType(.URL)
                        #endif
                        .autocorrectionDisabled()
                    if let problem { Text(problem).foregroundStyle(.red).font(.callout) }
                    Button("Add bookmark") { add() }
                        .disabled(newURL.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
            .navigationTitle("Bookmarks")
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            #endif
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
        #if os(macOS)
        .frame(minWidth: 480, minHeight: 460)
        #endif
    }

    private func add() {
        guard let url = AddressInput.httpLink(newURL) else {
            problem = "That is not an http or https link."
            return
        }
        if bookmarks.add(title: newTitle, url: url) {
            newTitle = ""
            newURL = ""
            problem = nil
        } else {
            problem = "That link is already bookmarked."
        }
    }
}

// MARK: - Banner

private struct NoticeBanner: View {
    let text: String
    var isError = false
    let dismiss: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: isError ? "exclamationmark.triangle.fill" : "info.circle.fill")
                .foregroundStyle(isError ? Color.red : Color.accentColor)
            Text(text).font(.callout).frame(maxWidth: .infinity, alignment: .leading)
            Button { dismiss() } label: { Image(systemName: "xmark") }
                .buttonStyle(BarButtonStyle())
                .focusableControl()
                .accessibilityLabel("Dismiss")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 4)
        .background((isError ? Color.red : Color.accentColor).opacity(0.14))
    }
}

// MARK: - Downloads list

private struct DownloadsPanel: View {
    @ObservedObject var model: DownloadsModel
    @Binding var pasteText: String

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("Downloads").font(.headline)
                Spacer()
                if model.items.contains(where: { $0.state == .finished || $0.state == .openedInBrowser }) {
                    Button { model.clearFinished() } label: { Text("Clear finished").font(.callout) }
                        .buttonStyle(BarButtonStyle(minWidth: 0))
                        .focusableControl()
                }
                Button { model.showPanel = false } label: { Image(systemName: "xmark") }
                    .buttonStyle(BarButtonStyle())
                    .focusableControl()
                    .help("Hide the list")
                    .accessibilityLabel("Hide the list")
            }
            .padding(.horizontal, 12)
            .padding(.top, 8)

            pasteField
                .padding(.horizontal, 12)
                .padding(.vertical, 8)

            Divider()

            if model.items.isEmpty {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Nothing downloaded yet.").font(.callout.weight(.semibold))
                    Text("Open an addon page and tap its download button, or paste a link to a file above. Finished files go straight to the mod importer.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                .padding(12)
                Spacer(minLength: 0)
            } else {
                ScrollView {
                    VStack(spacing: 0) {
                        ForEach(model.items) { item in
                            DownloadRow(model: model, item: item)
                            Divider()
                        }
                    }
                }
            }
        }
        .frame(maxHeight: .infinity, alignment: .top)
        .background(Color.pageBackground)
    }

    private var pasteField: some View {
        HStack(spacing: 6) {
            TextField("Paste a link to a file", text: $pasteText)
                .textFieldStyle(.plain)
                .padding(.horizontal, 10)
                .frame(minHeight: 44)
                .background(RoundedRectangle(cornerRadius: 10, style: .continuous).fill(Color.fieldBackground))
                .onSubmit { submit() }
                #if os(iOS)
                .textInputAutocapitalization(.never)
                .keyboardType(.URL)
                #endif
                .autocorrectionDisabled()
            PasteButton(payloadType: String.self) { strings in
                if let text = strings.first { pasteText = text }
            }
            .labelStyle(.iconOnly)
            .frame(minHeight: 44)
            Button { submit() } label: { Text("Download").fontWeight(.semibold) }
                .buttonStyle(BarButtonStyle(prominent: true, minWidth: 0))
                .focusableControl()
                .disabled(pasteText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
        }
    }

    private func submit() {
        if model.downloadLink(pasteText) { pasteText = "" }
    }
}

private struct DownloadRow: View {
    let model: DownloadsModel
    @ObservedObject var item: DownloadItem

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .top, spacing: 8) {
                Image(systemName: icon).foregroundStyle(iconColor).frame(width: 22)
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.fileName).font(.callout.weight(.semibold)).lineLimit(2).truncationMode(.middle)
                    Text(subtitle).font(.caption).foregroundStyle(subtitleIsError ? Color.red : Color.secondary)
                }
                Spacer(minLength: 0)
                if item.canRemove {
                    Button { model.remove(item) } label: { Image(systemName: "xmark") }
                        .buttonStyle(BarButtonStyle())
                        .focusableControl()
                        .help("Remove from the list")
                        .accessibilityLabel("Remove \(item.fileName) from the list")
                }
            }
            if item.state == .downloading || item.state == .verifying {
                if let fraction = item.fraction, item.state == .downloading {
                    ProgressView(value: fraction).progressViewStyle(.linear)
                } else {
                    ProgressView().progressViewStyle(.linear)
                }
            }
            HStack(spacing: 8) {
                if item.canCancel {
                    Button { model.cancel(item) } label: { Label("Cancel", systemImage: "stop.circle") }
                        .buttonStyle(BarButtonStyle(minWidth: 0))
                        .focusableControl()
                }
                if item.canRetry {
                    Button { model.retry(item) } label: { Label("Retry", systemImage: "arrow.clockwise") }
                        .buttonStyle(BarButtonStyle(prominent: true, minWidth: 0))
                        .focusableControl()
                }
                if item.state == .awaitingApproval {
                    Button { model.approve(item) } label: { Label("Allow", systemImage: "checkmark") }
                        .buttonStyle(BarButtonStyle(prominent: true, minWidth: 0))
                        .focusableControl()
                }
            }
            .font(.callout)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    private var icon: String {
        switch item.state {
        case .awaitingApproval: return "hand.raised.fill"
        case .downloading, .verifying: return "arrow.down.circle"
        case .finished: return "checkmark.circle.fill"
        case .failed: return "exclamationmark.triangle.fill"
        case .cancelled: return "stop.circle"
        case .openedInBrowser: return "safari"
        }
    }

    private var iconColor: Color {
        switch item.state {
        case .finished: return .green
        case .failed: return .red
        case .awaitingApproval: return .orange
        default: return .accentColor
        }
    }

    private var subtitleIsError: Bool {
        if case .failed = item.state { return true }
        return false
    }

    private var subtitle: String {
        let fmt = ByteCountFormatter()
        let host = item.sourceURL.host ?? ""
        switch item.state {
        case .awaitingApproval:
            return "\(host) tried to start this without a tap"
        case .downloading:
            if let total = item.totalBytes {
                return "\(fmt.string(fromByteCount: item.receivedBytes)) of \(fmt.string(fromByteCount: total)) · \(host)"
            }
            return "\(fmt.string(fromByteCount: item.receivedBytes)) · \(host)"
        case .verifying:
            return "Checking the file…"
        case .finished:
            let gone = item.file.map { !FileManager.default.fileExists(atPath: $0.fileURL.path) } ?? false
            return gone ? "Imported or removed" : "Downloaded · \(fmt.string(fromByteCount: item.totalBytes ?? item.receivedBytes)) · \(host)"
        case .failed(let message):
            return message
        case .cancelled:
            return item.receivedBytes > 0 ? "Cancelled · \(fmt.string(fromByteCount: item.receivedBytes)) kept for resuming" : "Cancelled"
        case .openedInBrowser:
            return "That link is a web page, so it was opened in the browser"
        }
    }
}
