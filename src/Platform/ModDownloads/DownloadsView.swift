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
///
/// Look: a dark theme with Liquid Glass chrome floating over the page (a material below iOS 26 / macOS 26, opaque surfaces
/// with Reduce Transparency). Supply the launcher's own with `.downloadsTheme(_:)`.
public struct DownloadsView: View {
    @ObservedObject private var model: DownloadsModel
    private let webContent: AnyView?

    @State private var pasteText = ""
    @State private var showBookmarkEditor = false
    @State private var chromeHeight: CGFloat = 0
    @FocusState private var addressFocused: Bool

    @Environment(\.downloadsTheme) private var theme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency

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
            // the page shows through glass only where the system can do it honestly; otherwise it sits below an opaque bar
            let overlap = liquidGlassAvailable && theme.prefersGlass && !reduceTransparency
            let panelBeside = wide && model.showPanel
            ZStack(alignment: .top) {
                theme.background.ignoresSafeArea()
                pageArea(overlap: overlap)
                    .padding(.top, overlap ? 0 : chromeHeight)
                    .padding(.trailing, panelBeside ? 372 : 0)
                if model.showPanel && !wide {
                    // on a narrow screen the list covers the page: dim it, and a tap outside closes the list
                    theme.background.opacity(0.6)
                        .ignoresSafeArea()
                        .onTapGesture { model.showPanel = false }
                        .transition(.opacity)
                        .accessibilityHidden(true)
                }
                chrome
                if let notice = model.notice {
                    NoticeBanner(text: notice) { model.dismissNotice() }
                        .padding(.top, chromeHeight + 10)
                        .padding(.horizontal, 16)
                        .frame(maxHeight: .infinity, alignment: .top)
                        .transition(.slideFade(y: -10, reduceMotion: reduceMotion))
                }
                panelLayer(wide: wide)
            }
            .animation(Motion.animation(Motion.spring, reduceMotion: reduceMotion), value: model.showPanel)
            .animation(Motion.animation(Motion.spring, reduceMotion: reduceMotion), value: model.notice)
        }
        .environment(\.colorScheme, .dark)
        .tint(theme.highlight)
        .foregroundStyle(theme.text)
        .sheet(isPresented: $showBookmarkEditor) {
            BookmarkEditor(bookmarks: model.bookmarks)
                .environment(\.downloadsTheme, theme)
        }
        .onChange(of: addressFocused) { _, focused in model.browser.isEditingAddress = focused }
    }

    // MARK: Pieces

    private var chrome: some View {
        ChromeContainer(spacing: 12) {
            VStack(spacing: 8) {
                BrowserBar(model: model, browser: model.browser, addressFocused: $addressFocused)
                BookmarksBar(model: model, bookmarks: model.bookmarks, showEditor: $showBookmarkEditor)
            }
        }
        .padding(.horizontal, 12)
        .padding(.top, 8)
        .padding(.bottom, 8)
        .background(
            GeometryReader { proxy in
                Color.clear.preference(key: ChromeHeightKey.self, value: proxy.size.height)
            }
        )
        .onPreferenceChange(ChromeHeightKey.self) { chromeHeight = $0 }
        .frame(maxHeight: .infinity, alignment: .top)
    }

    @ViewBuilder
    private func pageArea(overlap: Bool) -> some View {
        ZStack(alignment: .top) {
            if let webContent {
                webContent
            } else {
                WebViewContainer(webView: model.browser.webView, topInset: overlap ? chromeHeight : 0)
                    .overlay(ControllerLayer(browser: model.browser))
            }
            if let error = model.browser.errorMessage {
                NoticeBanner(text: error, isError: true) { model.browser.errorMessage = nil }
                    .padding(.top, (overlap ? chromeHeight : 0) + 10)
                    .padding(.horizontal, 16)
                    .frame(maxHeight: .infinity, alignment: .top)
                    .transition(.slideFade(y: -10, reduceMotion: reduceMotion))
            }
        }
        .animation(Motion.animation(Motion.spring, reduceMotion: reduceMotion), value: model.browser.errorMessage)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    @ViewBuilder
    private func panelLayer(wide: Bool) -> some View {
        if model.showPanel {
            DownloadsPanel(model: model, pasteText: $pasteText)
                .frame(maxWidth: wide ? 360 : .infinity)
                .padding(.top, chromeHeight + 4)
                .padding(.horizontal, 12)
                .padding(.bottom, 12)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: wide ? .topTrailing : .top)
                .transition(.slideFade(x: wide ? 24 : 0, y: wide ? 0 : 16, scale: wide ? 1 : 0.98, reduceMotion: reduceMotion))
        }
    }
}

private struct ChromeHeightKey: PreferenceKey {
    static let defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}

// MARK: - Bar

private struct BrowserBar: View {
    @ObservedObject var model: DownloadsModel
    @ObservedObject var browser: BrowserModel
    @ObservedObject private var bookmarks: BookmarkStore
    var addressFocused: FocusState<Bool>.Binding

    @Environment(\.downloadsTheme) private var theme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.snapshotMode) private var snapshotMode

    init(model: DownloadsModel, browser: BrowserModel, addressFocused: FocusState<Bool>.Binding) {
        self.model = model
        self.browser = browser
        self.bookmarks = model.bookmarks
        self.addressFocused = addressFocused
    }

    private var edited: Bool {
        !browser.addressText.isEmpty && browser.addressText != browser.currentURL?.absoluteString
    }

    private var showGo: Bool { addressFocused.wrappedValue || edited }

    var body: some View {
        HStack(spacing: 8) {
            navigationCluster
            addressField
            if showGo {
                Button { browser.submitAddress() } label: { Text("Go").fontWeight(.semibold) }
                    .buttonStyle(ChromeButtonStyle(kind: .prominent))
                    .focusableControl()
                    .help("Open the address")
                    .transition(.iconSwap(reduceMotion: reduceMotion))
            }
            actionCluster
        }
        .animation(Motion.animation(Motion.quick, reduceMotion: reduceMotion), value: showGo)
    }

    private var navigationCluster: some View {
        HStack(spacing: 0) {
            Button { browser.goBack() } label: { Image(systemName: "chevron.left") }
                .keyboardShortcut("[", modifiers: .command)
                .disabled(!browser.canGoBack)
                .help("Back")
                .accessibilityLabel("Back")
            Button { browser.goForward() } label: { Image(systemName: "chevron.right") }
                .keyboardShortcut("]", modifiers: .command)
                .disabled(!browser.canGoForward)
                .help("Forward")
                .accessibilityLabel("Forward")
            Button {
                if browser.isLoading { browser.stop() } else { browser.reload() }
            } label: {
                Image(systemName: browser.isLoading ? "xmark" : "arrow.clockwise")
                    .contentTransition(.symbolEffect(.replace))
            }
            .keyboardShortcut("r", modifiers: .command)
            .help(browser.isLoading ? "Stop" : "Reload")
            .accessibilityLabel(browser.isLoading ? "Stop" : "Reload")
        }
        .buttonStyle(ChromeButtonStyle(kind: .bare))
        .focusableControl()
        .padding(4)
        .chromeSurface(Capsule())
    }

    private var addressField: some View {
        HStack(spacing: 6) {
            Image(systemName: addressIcon)
                .font(.callout)
                .foregroundStyle(theme.secondaryText)
                .frame(width: 20)
                .accessibilityHidden(true)
            if snapshotMode {
                Text(browser.addressText).lineLimit(1).frame(maxWidth: .infinity, alignment: .leading)
            } else {
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
            }
            if !browser.addressText.isEmpty {
                Button { browser.addressText = "" } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(theme.secondaryText) }
                    .buttonStyle(ChromeButtonStyle(kind: .bare, minWidth: 36))
                    .focusableControl()
                    .accessibilityLabel("Clear address")
                    .transition(.iconSwap(reduceMotion: reduceMotion))
            }
            Button { addressFocused.wrappedValue = true } label: { EmptyView() }
                .keyboardShortcut("l", modifiers: .command) // Cmd+L: type an address
                .frame(width: 0, height: 0)
                .opacity(0)
                .accessibilityHidden(true)
        }
        .padding(.leading, 14)
        .padding(.trailing, 8)
        .frame(minHeight: 52)
        .chromeSurface(Capsule())
        .overlay(alignment: .bottomLeading) { loadingLine }
        .clipShape(Capsule())
        .focusRing(Capsule(), forced: addressFocused.wrappedValue)
    }

    /// Page load progress: a 2 pt amber line along the bottom edge of the address field.
    @ViewBuilder
    private var loadingLine: some View {
        if browser.isLoading {
            GeometryReader { geo in
                Capsule().fill(theme.highlight)
                    .frame(width: geo.size.width * max(0.04, browser.progress), height: 2)
                    .animation(reduceMotion ? nil : Motion.spring, value: browser.progress)
                    .frame(maxHeight: .infinity, alignment: .bottom)
            }
            .transition(.opacity)
        }
    }

    private var addressIcon: String {
        guard let url = browser.currentURL, !browser.addressText.isEmpty else { return "magnifyingglass" }
        return url.scheme == "https" ? "lock.fill" : "exclamationmark.triangle"
    }

    private var actionCluster: some View {
        HStack(spacing: 0) {
            Button { toggleBookmark() } label: {
                Image(systemName: bookmarks.isBookmarked(browser.currentURL) ? "star.fill" : "star")
                    .foregroundStyle(bookmarks.isBookmarked(browser.currentURL) ? theme.highlight : theme.text)
                    .contentTransition(.symbolEffect(.replace))
            }
            .disabled(browser.currentURL == nil || !BrowserModel.isWebURL(browser.currentURL))
            .help("Bookmark this page")
            .accessibilityLabel("Bookmark this page")

            Button { model.showPanel.toggle() } label: {
                Image(systemName: "arrow.down.circle")
                    .overlay(alignment: .topTrailing) { badge }
            }
            .help("Downloads")
            .accessibilityLabel("Downloads")
            .accessibilityValue(model.activeCount + model.waitingCount > 0 ? "\(model.activeCount + model.waitingCount) active" : "")
        }
        .buttonStyle(ChromeButtonStyle(kind: .bare))
        .focusableControl()
        .padding(4)
        .chromeSurface(Capsule())
    }

    @ViewBuilder
    private var badge: some View {
        let count = model.activeCount + model.waitingCount
        if count > 0 {
            Text("\(count)")
                .font(.caption2.bold())
                .monospacedDigit()
                .foregroundStyle(Color.black.opacity(0.85))
                .padding(.horizontal, 5)
                .frame(minWidth: 16, minHeight: 16)
                .background(Capsule().fill(theme.highlight))
                .offset(x: 9, y: -8)
                .transition(.iconSwap(reduceMotion: reduceMotion))
        }
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
    @Environment(\.snapshotMode) private var snapshotMode
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private var chips: some View {
        HStack(spacing: 8) {
            ForEach(bookmarks.all) { bookmark in
                Button { model.browser.load(bookmark.url) } label: {
                    Text(bookmark.title).lineLimit(1)
                }
                .buttonStyle(ChromeButtonStyle(kind: .glass, minWidth: 0))
                .focusableControl()
                .help(bookmark.url.absoluteString)
                .transition(.iconSwap(reduceMotion: reduceMotion))
            }
        }
        .padding(.horizontal, 2)
        .padding(.vertical, 2)
        .animation(Motion.animation(Motion.spring, reduceMotion: reduceMotion), value: bookmarks.all)
    }

    var body: some View {
        HStack(spacing: 8) {
            if snapshotMode {
                chips.frame(maxWidth: .infinity, alignment: .leading)
            } else {
                ScrollView(.horizontal, showsIndicators: false) { chips }
                    .scrollClipDisabled()
            }
            Button { showEditor = true } label: { Image(systemName: "slider.horizontal.3") }
                .buttonStyle(ChromeButtonStyle(kind: .glass))
                .focusableControl()
                .help("Rename, reorder or remove bookmarks")
                .accessibilityLabel("Edit bookmarks")
        }
    }
}

private struct BookmarkEditor: View {
    @ObservedObject var bookmarks: BookmarkStore
    @Environment(\.dismiss) private var dismiss
    @Environment(\.downloadsTheme) private var theme
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
                            Text(bookmark.url.absoluteString).font(.caption).foregroundStyle(theme.secondaryText).lineLimit(1)
                        }
                        .listRowBackground(theme.surface.opacity(0.6))
                    }
                }
                Section("Yours") {
                    if bookmarks.user.isEmpty {
                        Text("Bookmark the page you are on with the star in the address bar, or add a link below.")
                            .foregroundStyle(theme.secondaryText)
                            .listRowBackground(theme.surface.opacity(0.6))
                    }
                    ForEach(bookmarks.user) { bookmark in
                        HStack {
                            TextField("Title", text: Binding(get: { bookmark.title }, set: { bookmarks.rename(id: bookmark.id, to: $0) }))
                            Text(bookmark.url.host ?? "").font(.caption).foregroundStyle(theme.secondaryText).lineLimit(1)
                            Button(role: .destructive) { bookmarks.remove(id: bookmark.id) } label: { Image(systemName: "trash") }
                                .buttonStyle(ChromeButtonStyle(kind: .bare))
                                .accessibilityLabel("Remove \(bookmark.title)")
                        }
                        .listRowBackground(theme.surface.opacity(0.6))
                    }
                    .onMove { bookmarks.move(from: $0, to: $1) }
                    .onDelete { offsets in offsets.map { bookmarks.user[$0].id }.forEach { bookmarks.remove(id: $0) } }
                }
                Section("Add a link") {
                    TextField("Title (optional)", text: $newTitle)
                        .listRowBackground(theme.surface.opacity(0.6))
                    TextField("https://…", text: $newURL)
                        #if os(iOS)
                        .textInputAutocapitalization(.never)
                        .keyboardType(.URL)
                        #endif
                        .autocorrectionDisabled()
                        .listRowBackground(theme.surface.opacity(0.6))
                    if let problem { Text(problem).foregroundStyle(theme.danger).font(.callout).listRowBackground(theme.surface.opacity(0.6)) }
                    Button("Add bookmark") { add() }
                        .disabled(newURL.trimmingCharacters(in: .whitespaces).isEmpty)
                        .listRowBackground(theme.surface.opacity(0.6))
                }
            }
            .scrollContentBackground(.hidden)
            .background(theme.background)
            .navigationTitle("Bookmarks")
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            #endif
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
        .environment(\.colorScheme, .dark)
        .tint(theme.highlight)
        #if os(iOS)
        .presentationDetents([.medium, .large])
        #endif
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

// MARK: - Notice

private struct NoticeBanner: View {
    let text: String
    var isError = false
    let dismiss: () -> Void
    @Environment(\.downloadsTheme) private var theme

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: 22, style: .continuous)
        HStack(alignment: .center, spacing: 10) {
            Image(systemName: isError ? "exclamationmark.triangle.fill" : "info.circle.fill")
                .foregroundStyle(isError ? theme.danger : theme.highlight)
                .accessibilityHidden(true)
            Text(text).font(.callout).frame(maxWidth: .infinity, alignment: .leading)
            Button { dismiss() } label: { Image(systemName: "xmark") }
                .buttonStyle(ChromeButtonStyle(kind: .bare))
                .focusableControl()
                .accessibilityLabel("Dismiss")
        }
        .padding(.leading, 16)
        .padding(.trailing, 6)
        .padding(.vertical, 4)
        .chromeSurface(shape, tint: isError ? theme.danger.opacity(0.35) : nil)
        .frame(maxWidth: 640)
        .accessibilityElement(children: .combine)
    }
}

// MARK: - Downloads panel

private struct DownloadsPanel: View {
    @ObservedObject var model: DownloadsModel
    @Binding var pasteText: String
    @Environment(\.downloadsTheme) private var theme
    @Environment(\.snapshotMode) private var snapshotMode
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private var hasFinished: Bool {
        model.items.contains { $0.state == .finished || $0.state == .openedInBrowser }
    }

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: theme.panelRadius, style: .continuous)
        VStack(alignment: .leading, spacing: 0) {
            header
            pasteField
                .padding(.horizontal, 12)
                .padding(.bottom, 12)
            Rectangle().fill(theme.outline).frame(height: 1)
            if model.items.isEmpty {
                emptyState
            } else if snapshotMode {
                rows
                Spacer(minLength: 0)
            } else {
                ScrollView { rows }
            }
        }
        .frame(maxHeight: .infinity, alignment: .top)
        .clipShape(shape)
        .chromeSurface(shape, raised: true)
    }

    private var header: some View {
        HStack(spacing: 8) {
            Text("Downloads")
                .font(.footnote.weight(.semibold))
                .textCase(.uppercase)
                .tracking(1.4)
                .foregroundStyle(theme.secondaryText)
            Spacer()
            if hasFinished {
                Button { model.clearFinished() } label: { Text("Clear finished") }
                    .buttonStyle(ChromeButtonStyle(kind: .glass, minWidth: 0))
                    .focusableControl()
                    .transition(.iconSwap(reduceMotion: reduceMotion))
            }
            Button { model.showPanel = false } label: { Image(systemName: "xmark") }
                .buttonStyle(ChromeButtonStyle(kind: .glass))
                .focusableControl()
                .help("Hide the list")
                .accessibilityLabel("Hide the list")
        }
        .padding(.leading, 18)
        .padding(.trailing, 10)
        .padding(.vertical, 8)
        .animation(Motion.animation(Motion.quick, reduceMotion: reduceMotion), value: hasFinished)
    }

    private var emptyState: some View {
        VStack(alignment: .leading, spacing: 8) {
            Image(systemName: "tray.and.arrow.down")
                .font(.title2)
                .foregroundStyle(theme.highlight)
                .accessibilityHidden(true)
            Text("Nothing downloaded yet").font(.callout.weight(.semibold))
            Text("Open an addon page and tap its download button, or paste a link to a file above. Finished files go straight to the mod importer.")
                .font(.callout)
                .foregroundStyle(theme.secondaryText)
        }
        .padding(18)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var rows: some View {
        VStack(spacing: 0) {
            ForEach(model.items) { item in
                DownloadRow(model: model, item: item)
                    .transition(.slideFade(y: 8, reduceMotion: reduceMotion))
                Rectangle().fill(theme.outline.opacity(0.6)).frame(height: 1).padding(.leading, 18)
            }
        }
        .animation(Motion.animation(Motion.spring, reduceMotion: reduceMotion), value: model.items.map(\.id))
    }

    private var pasteField: some View {
        // concentric: the field's ends are 4 pt from the button inside it, which is as round as the field
        HStack(spacing: 6) {
            if snapshotMode {
                Text(pasteText.isEmpty ? "Paste a link to a file" : pasteText)
                    .foregroundStyle(theme.secondaryText)
                    .lineLimit(1)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Image(systemName: "doc.on.clipboard").frame(width: 44, height: 44)
            } else {
                TextField("Paste a link to a file", text: $pasteText)
                    .textFieldStyle(.plain)
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
                .buttonBorderShape(.capsule)
                .tint(theme.surfaceRaised)
                .frame(minHeight: 44)
            }
            Button { submit() } label: { Text("Download").fontWeight(.semibold) }
                .buttonStyle(ChromeButtonStyle(kind: .prominent, minWidth: 0))
                .focusableControl()
                .disabled(pasteText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
        }
        .padding(.leading, 14)
        .padding(.trailing, 4)
        .padding(.vertical, 4)
        .background(Capsule().fill(Color.black.opacity(0.28)))
        .overlay(Capsule().strokeBorder(theme.outline, lineWidth: 1))
    }

    private func submit() {
        if model.downloadLink(pasteText) { pasteText = "" }
    }
}

// MARK: - A download

private struct DownloadRow: View {
    let model: DownloadsModel
    @ObservedObject var item: DownloadItem
    @Environment(\.downloadsTheme) private var theme
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.snapshotMode) private var snapshotMode

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .center, spacing: 10) {
                stateIcon
                    .frame(width: 26, height: 26)
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.fileName)
                        .font(.callout.weight(.semibold))
                        .lineLimit(2)
                        .truncationMode(.middle)
                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(subtitleIsError ? theme.danger : theme.secondaryText)
                        .lineLimit(3)
                        .contentTransition(.numericText())
                }
                Spacer(minLength: 0)
                actions
            }
            if item.state == .downloading || item.state == .verifying {
                ProgressTrack(fraction: item.state == .downloading ? (item.fraction ?? (snapshotMode ? 0.35 : nil)) : 1,
                              running: item.state == .downloading)
                    .transition(.opacity)
            }
        }
        .padding(.leading, 18)
        .padding(.trailing, 10)
        .padding(.vertical, 10)
        .animation(Motion.animation(Motion.quick, reduceMotion: reduceMotion), value: item.state)
        .accessibilityElement(children: .contain)
    }

    @ViewBuilder
    private var actions: some View {
        HStack(spacing: 6) {
            if item.canCancel {
                Button { model.cancel(item) } label: { Label("Cancel", systemImage: "stop.circle") }
                    .buttonStyle(ChromeButtonStyle(kind: .glass, minWidth: 0))
                    .focusableControl()
            }
            if item.canRetry {
                Button { model.retry(item) } label: { Label("Retry", systemImage: "arrow.clockwise") }
                    .buttonStyle(ChromeButtonStyle(kind: .prominent, minWidth: 0))
                    .focusableControl()
            }
            if item.state == .awaitingApproval {
                Button { model.approve(item) } label: { Label("Allow", systemImage: "checkmark") }
                    .buttonStyle(ChromeButtonStyle(kind: .prominent, minWidth: 0))
                    .focusableControl()
            }
            if item.canRemove {
                Button { model.remove(item) } label: { Image(systemName: "xmark") }
                    .buttonStyle(ChromeButtonStyle(kind: .bare))
                    .focusableControl()
                    .help(item.state == .awaitingApproval ? "Dismiss" : "Remove from the list")
                    .accessibilityLabel("Remove \(item.fileName) from the list")
            }
        }
    }

    /// The icon swaps with scale, opacity and blur, so a state change is also a visible one without relying on motion alone.
    private var stateIcon: some View {
        ZStack {
            Image(systemName: icon)
                .font(.title3)
                .foregroundStyle(iconColor)
                .id(icon)
                .transition(.iconSwap(reduceMotion: reduceMotion))
        }
        .accessibilityLabel(stateName)
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

    /// Amber is "in motion", rust is "needs you", red is "failed", and yellow-green appears once, for "done".
    private var iconColor: Color {
        switch item.state {
        case .finished: return theme.signal
        case .failed: return theme.danger
        case .awaitingApproval: return theme.accent
        case .downloading, .verifying: return theme.highlight
        default: return theme.secondaryText
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
            let resumable = item.origin == .link ? LinkDownloader.hasPartial(for: item.sourceURL, in: model.dir) : item.resumeData != nil
            return resumable && item.receivedBytes > 0 ? "Cancelled · \(fmt.string(fromByteCount: item.receivedBytes)) kept for resuming" : "Cancelled"
        case .openedInBrowser:
            return "That link is a web page, so it was opened in the browser"
        }
    }
}
