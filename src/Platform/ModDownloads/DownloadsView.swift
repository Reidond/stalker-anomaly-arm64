import SwiftUI
import WebKit

/// The mod manager's browser: ModDB, GitHub or any other site in a web view, bookmarks, a field for pasting a link to a file,
/// and the list of downloads with progress, cancel and retry.
///
/// Downloads go to `downloadsDir` (the mods root's `.downloads/`). Each finished file is passed to `onDownloaded`, after
/// which the importer owns it (and deletes it once installed). Nothing is downloaded without a tap.
///
/// The page, the cookies and the list survive the view being recreated (one model per folder is kept); call
/// `DownloadsView.releaseSharedState()` before the game starts to free the web view's memory.
///
/// Look: stock SwiftUI, like the launcher. On iPadOS the controls live in the host's navigation bar (the address field in its
/// middle, a bottom bar for back and forward), the downloads in an inspector (a sheet on a narrow screen), and bookmarks and
/// downloads in inset-grouped lists; on the Mac the same controls sit in a bar above the page. Liquid Glass bars and buttons come with the
/// system on iPadOS / macOS 26 and later. Show it inside a `NavigationStack` (the launcher's Add Mod screen does); the
/// host sets the title. Only the accent is its own: `.downloadsTheme(_:)`.
public struct DownloadsView: View {
    @ObservedObject private var model: DownloadsModel
    private let webContent: AnyView?

    @State private var pasteText = ""
    @State private var showBookmarkEditor = false
    @FocusState private var addressFocused: Bool

    @Environment(\.downloadsTheme) private var theme

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
        content
            .tint(theme.accent)
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { iosToolbar }
            #endif
            .inspector(isPresented: $model.showPanel) {
                DownloadsInspector(model: model, pasteText: $pasteText)
                    .inspectorColumnWidth(min: 340, ideal: 400, max: 480)
            }
            .sheet(isPresented: $showBookmarkEditor) {
                BookmarkEditor(bookmarks: model.bookmarks)
                    .tint(theme.accent)
            }
            .overlay(alignment: .bottom) { toast }
            .animation(.easeOut(duration: 0.2), value: toastText)
            .onChange(of: addressFocused) { _, focused in model.browser.isEditingAddress = focused }
    }

    // MARK: Content

    private var showStartPage: Bool {
        webContent == nil && model.browser.currentURL == nil
    }

    @ViewBuilder
    private var content: some View {
        #if os(macOS)
        VStack(spacing: 0) {
            MacBar(model: model, browser: model.browser, addressFocused: $addressFocused, showBookmarkEditor: $showBookmarkEditor)
            Divider()
            page
        }
        #else
        page
        #endif
    }

    private var page: some View {
        ZStack {
            if let webContent {
                webContent
            } else {
                WebViewContainer(webView: model.browser.webView)
                    #if os(iOS)
                    .ignoresSafeArea()
                    #endif
                    .overlay(ControllerLayer(browser: model.browser))
            }
            if showStartPage {
                StartPage(model: model, pasteText: $pasteText, showBookmarkEditor: $showBookmarkEditor)
                    .transition(.opacity)
            }
        }
        .overlay(alignment: .top) {
            if model.browser.isLoading {
                ProgressView(value: model.browser.progress)
                    .progressViewStyle(.linear)
                    .transition(.opacity)
            }
        }
        .animation(.easeOut(duration: 0.2), value: model.browser.isLoading)
        .animation(.easeOut(duration: 0.2), value: showStartPage)
    }

    // MARK: Toast

    private var toastText: String? { model.browser.errorMessage ?? model.notice }

    @ViewBuilder
    private var toast: some View {
        if let text = toastText {
            let isError = model.browser.errorMessage != nil || model.noticeIsError
            Button {
                model.browser.errorMessage = nil
                model.dismissNotice()
            } label: {
                Label(text, systemImage: isError ? "exclamationmark.triangle.fill" : "info.circle.fill")
                    .font(.callout)
                    .symbolRenderingMode(.multicolor)
                    .multilineTextAlignment(.leading)
                    .padding(.horizontal, 18)
                    .padding(.vertical, 12)
                    .frame(maxWidth: 640)
                    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
                    .shadow(color: .black.opacity(0.18), radius: 12, y: 4)
            }
            .buttonStyle(.plain)
            .padding(.horizontal, 16)
            .padding(.bottom, 24)
            .transition(.move(edge: .bottom).combined(with: .opacity))
            .accessibilityHint("Dismiss")
        }
    }

    // MARK: iPadOS toolbar

    #if os(iOS)
    @ToolbarContentBuilder
    private var iosToolbar: some ToolbarContent {
        ToolbarItem(placement: .principal) {
            AddressField(browser: model.browser, addressFocused: $addressFocused)
                .frame(minWidth: 260, idealWidth: 520, maxWidth: 640)
        }
        ToolbarItemGroup(placement: .topBarTrailing) {
            if addressFocused || addressEdited {
                Button("Go") { model.browser.submitAddress() }
                    .buttonStyle(.borderedProminent)
            }
            ReloadButton(browser: model.browser)
            BookmarksMenu(model: model, browser: model.browser, bookmarks: model.bookmarks, showEditor: $showBookmarkEditor)
            DownloadsButton(model: model)
        }
        ToolbarItemGroup(placement: .bottomBar) {
            PageNavigationButtons(browser: model.browser)
            Spacer()
        }
    }

    private var addressEdited: Bool {
        !model.browser.addressText.isEmpty && model.browser.addressText != model.browser.currentURL?.absoluteString
    }
    #endif
}

// MARK: - Controls (shared by the iPad toolbar and the Mac bar)

private struct AddressField: View {
    @ObservedObject var browser: BrowserModel
    var addressFocused: FocusState<Bool>.Binding

    var body: some View {
        TextField("Search or paste a link", text: $browser.addressText)
            .textFieldStyle(.roundedBorder)
            .focused(addressFocused)
            .onSubmit { browser.submitAddress() }
            .submitLabel(.go)
            #if os(iOS)
            .textInputAutocapitalization(.never)
            .keyboardType(.URL)
            #endif
            .autocorrectionDisabled()
            .keyboardShortcut("l", modifiers: .command)
            .accessibilityLabel("Address")
    }
}

private struct ReloadButton: View {
    @ObservedObject var browser: BrowserModel

    var body: some View {
        Button {
            if browser.isLoading { browser.stop() } else { browser.reload() }
        } label: {
            Label(browser.isLoading ? "Stop" : "Reload", systemImage: browser.isLoading ? "xmark" : "arrow.clockwise")
                .contentTransition(.symbolEffect(.replace))
        }
        .keyboardShortcut("r", modifiers: .command)
        .help(browser.isLoading ? "Stop" : "Reload")
    }
}

private struct PageNavigationButtons: View {
    @ObservedObject var browser: BrowserModel

    var body: some View {
        Button { browser.goBack() } label: { Label("Back", systemImage: "chevron.backward") }
            .keyboardShortcut("[", modifiers: .command)
            .disabled(!browser.canGoBack)
            .help("Back")
        Button { browser.goForward() } label: { Label("Forward", systemImage: "chevron.forward") }
            .keyboardShortcut("]", modifiers: .command)
            .disabled(!browser.canGoForward)
            .help("Forward")
    }
}

private struct BookmarksMenu: View {
    @ObservedObject var model: DownloadsModel
    @ObservedObject var browser: BrowserModel
    @ObservedObject var bookmarks: BookmarkStore
    @Binding var showEditor: Bool

    var body: some View {
        Menu {
            Section("Built In") {
                ForEach(BookmarkStore.builtIn) { bookmark in
                    Button(bookmark.title) { browser.load(bookmark.url) }
                }
            }
            if !bookmarks.user.isEmpty {
                Section("Yours") {
                    ForEach(bookmarks.user) { bookmark in
                        Button(bookmark.title) { browser.load(bookmark.url) }
                    }
                }
            }
            Divider()
            if let url = browser.currentURL, BrowserModel.isWebURL(url) {
                if let existing = bookmarks.user.first(where: { $0.url == url }) {
                    Button(role: .destructive) { bookmarks.remove(id: existing.id) } label: { Label("Remove This Page", systemImage: "star.slash") }
                } else if !bookmarks.isBookmarked(url) {
                    Button { bookmarks.add(title: browser.pageTitle, url: url) } label: { Label("Bookmark This Page", systemImage: "star") }
                }
            }
            Button { showEditor = true } label: { Label("Edit Bookmarks…", systemImage: "slider.horizontal.3") }
        } label: {
            Label("Bookmarks", systemImage: bookmarks.isBookmarked(browser.currentURL) ? "book.fill" : "book")
        }
        .help("Bookmarks")
    }
}

private struct DownloadsButton: View {
    @ObservedObject var model: DownloadsModel
    @Environment(\.downloadsTheme) private var theme

    var body: some View {
        let count = model.activeCount + model.waitingCount
        Button { model.showPanel.toggle() } label: {
            Label("Downloads", systemImage: count > 0 ? "arrow.down.circle.fill" : "arrow.down.circle")
                .symbolEffect(.pulse, isActive: model.activeCount > 0)
        }
        .overlay(alignment: .topTrailing) {
            if count > 0 {
                Text("\(count)")
                    .font(.caption2.bold())
                    .monospacedDigit()
                    .foregroundStyle(.white)
                    .padding(.horizontal, 5)
                    .frame(minWidth: 16, minHeight: 16)
                    .background(Capsule().fill(theme.accent))
                    .offset(x: 6, y: -4)
                    .allowsHitTesting(false)
                    .transition(.scale.combined(with: .opacity))
            }
        }
        .animation(.snappy(duration: 0.25), value: count)
        .help("Downloads")
        .accessibilityValue(count > 0 ? "\(count) active" : "")
    }
}

#if os(macOS)
/// The Mac has no navigation bar of its own here (the launcher's window is a plain one): the same controls in a bar above the page.
private struct MacBar: View {
    @ObservedObject var model: DownloadsModel
    @ObservedObject var browser: BrowserModel
    var addressFocused: FocusState<Bool>.Binding
    @Binding var showBookmarkEditor: Bool

    var body: some View {
        HStack(spacing: 10) {
            PageNavigationButtons(browser: browser)
            ReloadButton(browser: browser)
            AddressField(browser: browser, addressFocused: addressFocused)
            Button("Go") { browser.submitAddress() }
                .buttonStyle(.borderedProminent)
            BookmarksMenu(model: model, browser: browser, bookmarks: model.bookmarks, showEditor: $showBookmarkEditor)
            DownloadsButton(model: model)
        }
        .labelStyle(.iconOnly)
        .controlSize(.large)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }
}
#endif

// MARK: - Start page

/// Before any page is open: the bookmarks and the paste-a-link field, as the grouped lists of Settings.
private struct StartPage: View {
    @ObservedObject var model: DownloadsModel
    @Binding var pasteText: String
    @Binding var showBookmarkEditor: Bool
    @ObservedObject private var bookmarks: BookmarkStore

    init(model: DownloadsModel, pasteText: Binding<String>, showBookmarkEditor: Binding<Bool>) {
        self.model = model
        self._pasteText = pasteText
        self._showBookmarkEditor = showBookmarkEditor
        self.bookmarks = model.bookmarks
    }

    var body: some View {
        DownloadsForm {
            Section {
                ForEach(bookmarks.all) { bookmark in
                    Button { model.browser.load(bookmark.url) } label: {
                        Label {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(bookmark.title)
                                Text(bookmark.url.host ?? bookmark.url.absoluteString).font(.subheadline).foregroundStyle(.secondary)
                            }
                        } icon: {
                            Image(systemName: bookmark.url.host?.contains("github") == true ? "chevron.left.forwardslash.chevron.right" : "globe")
                                .frame(width: 28)
                        }
                    }
                    .buttonStyle(.plain)
                }
                Button { showBookmarkEditor = true } label: { Label("Edit Bookmarks…", systemImage: "slider.horizontal.3") }
            } header: {
                Text("Bookmarks")
            } footer: {
                Text("Open an addon page and tap its download button. The file goes to the mod importer when it is done.")
            }
            PasteLinkSection(model: model, pasteText: $pasteText)
        }
        .background(.background)
    }
}

/// "Download a link": a field, Paste, and Download. A link that turns out to be a web page opens in the browser.
private struct PasteLinkSection: View {
    @ObservedObject var model: DownloadsModel
    @Binding var pasteText: String
    @FocusState private var focused: Bool

    var body: some View {
        Section {
            TextField("https://…", text: $pasteText)
                .focused($focused)
                .onSubmit { submit() }
                .submitLabel(.go)
                #if os(iOS)
                .textInputAutocapitalization(.never)
                .keyboardType(.URL)
                #endif
                .autocorrectionDisabled()
            PasteButton(payloadType: String.self) { strings in
                if let text = strings.first { pasteText = text }
            }
            Button { submit() } label: { Label("Download", systemImage: "arrow.down.circle.fill") }
                .disabled(pasteText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
        } header: {
            Text("Download a Link")
        } footer: {
            Text("A link to a file: a zip, 7z or rar archive. A link to a web page opens in the browser instead.")
        }
    }

    private func submit() {
        focused = false
        if model.downloadLink(pasteText) { pasteText = "" }
    }
}

// MARK: - Bookmark editor

struct BookmarkEditor: View {
    @ObservedObject var bookmarks: BookmarkStore
    @Environment(\.dismiss) private var dismiss
    @State private var newTitle = ""
    @State private var newURL = ""
    @State private var problem: String?
    #if os(iOS)
    @State private var editMode: EditMode = .inactive
    #endif

    var body: some View {
        NavigationStack {
            List {
                Section("Built In") {
                    ForEach(BookmarkStore.builtIn) { bookmark in
                        VStack(alignment: .leading, spacing: 2) {
                            Text(bookmark.title)
                            Text(bookmark.url.absoluteString).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                        }
                    }
                }
                Section {
                    if bookmarks.user.isEmpty {
                        Text("Bookmark the page you are on from the Bookmarks menu, or add a link below.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(bookmarks.user) { bookmark in
                        HStack {
                            TextField("Title", text: Binding(get: { bookmark.title }, set: { bookmarks.rename(id: bookmark.id, to: $0) }))
                            Text(bookmark.url.host ?? "").font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                        }
                    }
                    .onMove { bookmarks.move(from: $0, to: $1) }
                    .onDelete { offsets in offsets.map { bookmarks.user[$0].id }.forEach { bookmarks.remove(id: $0) } }
                } header: {
                    Text("Yours")
                }
                Section {
                    TextField("Title (optional)", text: $newTitle)
                    TextField("https://…", text: $newURL)
                        #if os(iOS)
                        .textInputAutocapitalization(.never)
                        .keyboardType(.URL)
                        #endif
                        .autocorrectionDisabled()
                    Button("Add Bookmark") { add() }
                        .disabled(newURL.trimmingCharacters(in: .whitespaces).isEmpty)
                } header: {
                    Text("Add a Link")
                } footer: {
                    if let problem { Text(problem).foregroundStyle(.red) }
                }
            }
            .downloadsListStyle()
            .navigationTitle("Bookmarks")
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            .environment(\.editMode, $editMode)
            #endif
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
                #if os(iOS)
                ToolbarItem(placement: .cancellationAction) { if !bookmarks.user.isEmpty { EditButton() } }
                #endif
            }
        }
        #if os(iOS)
        .presentationDetents([.medium, .large])
        #else
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

// MARK: - Downloads

private struct DownloadsInspector: View {
    @ObservedObject var model: DownloadsModel
    @Binding var pasteText: String
    @Environment(\.dismiss) private var dismiss

    private var hasFinished: Bool {
        model.items.contains { $0.state == .finished || $0.state == .openedInBrowser }
    }

    var body: some View {
        NavigationStack {
            DownloadsForm {
                PasteLinkSection(model: model, pasteText: $pasteText)
                Section {
                    ForEach(model.items) { item in
                        DownloadRow(model: model, item: item)
                    }
                } header: {
                    Text("Downloads")
                } footer: {
                    if model.items.isEmpty {
                        Text("Nothing downloaded yet. Open an addon page and tap its download button, or paste a link above.")
                    } else {
                        Text("A finished file goes straight to the mod importer.")
                    }
                }
            }
            .animation(.snappy(duration: 0.3), value: model.items.map(\.id))
            .navigationTitle("Downloads")
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            #endif
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button("Clear Finished") { model.clearFinished() }.disabled(!hasFinished)
                }
                ToolbarItem(placement: .cancellationAction) {
                    Button { model.showPanel = false } label: { Label("Close", systemImage: "xmark") }
                }
            }
        }
    }
}

private struct DownloadRow: View {
    let model: DownloadsModel
    @ObservedObject var item: DownloadItem
    @Environment(\.downloadsTheme) private var theme

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .center, spacing: 12) {
                Image(systemName: icon)
                    .symbolRenderingMode(item.state == .awaitingApproval || isFailed ? .multicolor : .monochrome)
                    .foregroundStyle(iconColor)
                    .font(.title3)
                    .frame(width: 28)
                    .contentTransition(.symbolEffect(.replace))
                    .accessibilityLabel(stateName)
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.fileName).font(.headline).lineLimit(2).truncationMode(.middle)
                    Text(subtitle)
                        .font(.subheadline)
                        .foregroundStyle(isFailed ? Color.red : Color.secondary)
                        .lineLimit(3)
                }
                Spacer(minLength: 0)
                actions
            }
            if item.state == .downloading {
                if let fraction = item.fraction {
                    ProgressView(value: fraction)
                } else {
                    ProgressView().progressViewStyle(.linear)
                }
            } else if item.state == .verifying {
                ProgressView().progressViewStyle(.linear)
            }
        }
        .padding(.vertical, 4)
        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
            if item.canRemove {
                Button(role: .destructive) { model.remove(item) } label: { Label("Remove", systemImage: "trash") }
            }
            if item.canCancel {
                Button { model.cancel(item) } label: { Label("Cancel", systemImage: "stop.circle") }.tint(.orange)
            }
        }
        .contextMenu {
            if item.canCancel { Button { model.cancel(item) } label: { Label("Cancel", systemImage: "stop.circle") } }
            if item.canRetry { Button { model.retry(item) } label: { Label("Retry", systemImage: "arrow.clockwise") } }
            if item.state == .awaitingApproval { Button { model.approve(item) } label: { Label("Allow", systemImage: "checkmark") } }
            if item.canRemove {
                Divider()
                Button(role: .destructive) { model.remove(item) } label: { Label("Remove from the List", systemImage: "trash") }
            }
        }
        .accessibilityElement(children: .contain)
    }

    @ViewBuilder
    private var actions: some View {
        HStack(spacing: 8) {
            if item.canCancel {
                Button("Cancel") { model.cancel(item) }.buttonStyle(.bordered)
            }
            if item.canRetry {
                Button("Retry") { model.retry(item) }.buttonStyle(.borderedProminent)
            }
            if item.state == .awaitingApproval {
                Button("Allow") { model.approve(item) }.buttonStyle(.borderedProminent)
                Button { model.remove(item) } label: { Label("Dismiss", systemImage: "xmark") }
                    .labelStyle(.iconOnly)
                    .buttonStyle(.bordered)
            }
        }
        .controlSize(.regular)
    }

    private var isFailed: Bool {
        if case .failed = item.state { return true }
        return false
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

    private var stateName: String {
        switch item.state {
        case .awaitingApproval: return "Waiting for your approval"
        case .downloading: return "Downloading"
        case .verifying: return "Checking"
        case .finished: return "Finished"
        case .failed: return "Failed"
        case .cancelled: return "Cancelled"
        case .openedInBrowser: return "Opened in the browser"
        }
    }

    /// The accent for "in progress", yellow-green once for "done", the system's colours for the rest.
    private var iconColor: Color {
        switch item.state {
        case .finished: return theme.signal
        case .downloading, .verifying: return theme.accent
        case .awaitingApproval, .failed: return .primary
        default: return .secondary
        }
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
            let resumable = item.origin == .link ? LinkDownloader.hasPartial(for: item.sourceURL, in: model.dir) : item.resumeData != nil
            return resumable && item.receivedBytes > 0 ? "Cancelled · \(fmt.string(fromByteCount: item.receivedBytes)) kept for resuming" : "Cancelled"
        case .openedInBrowser:
            return "That link is a web page, so it was opened in the browser"
        }
    }
}
