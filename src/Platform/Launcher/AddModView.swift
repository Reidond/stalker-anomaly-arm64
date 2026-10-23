// Add mod: from the Files app (zip, 7z, rar or a folder), from the in-app browser (ModDB, GitHub) or from a pasted link.
import SwiftUI
import UniformTypeIdentifiers
#if canImport(ModDownloads)
import ModDownloads
#endif
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif

struct LauncherSnapshotKey: EnvironmentKey { static let defaultValue = false }

extension EnvironmentValues {
    /// Set by the snapshot tool: ImageRenderer cannot draw platform text fields, so a plain text stands in for them.
    var launcherSnapshot: Bool {
        get { self[LauncherSnapshotKey.self] }
        set { self[LauncherSnapshotKey.self] = newValue }
    }
}

/// A ScrollView, except in the snapshot tool: ImageRenderer draws nothing for the content of one.
struct LauncherScroll<Content: View>: View {
    @Environment(\.launcherSnapshot) private var snapshot
    @ViewBuilder var content: () -> Content

    var body: some View {
        if snapshot {
            content().frame(maxHeight: .infinity, alignment: .top)
        } else {
            ScrollView { content() }
        }
    }
}

enum ModFileTypes {
    static var importable: [UTType] {
        var types: [UTType] = [.zip, .archive, .folder]
        for ext in ["7z", "rar"] {
            if let type = UTType(filenameExtension: ext) { types.append(type) }
        }
        return types
    }
}

struct AddModView: View {
    @EnvironmentObject private var model: LauncherModel
    @EnvironmentObject private var focus: FocusStore
    @Environment(\.launcherSnapshot) private var snapshot
    @State private var link = ""
    @FocusState private var linkFocused: Bool

    private var linkURL: URL? {
        let trimmed = link.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: trimmed), let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https", url.host != nil else { return nil }
        return url
    }

    var body: some View {
        VStack(spacing: 0) {
            ScreenHeader(title: "Add mod", subtitle: "Nothing is overwritten: each mod is kept in its own folder") { EmptyView() }
            LauncherScroll {
                VStack(spacing: 14) {
                    ConsoleButton(id: "add.files", title: "Files", subtitle: "A zip, 7z or rar archive, or a folder, from the Files app",
                                  systemImage: "folder.fill", isDefault: true) { model.showFilePicker = true }
                    ConsoleButton(id: "add.browse", title: "Browse", subtitle: "ModDB and GitHub in the built-in browser",
                                  systemImage: "globe") { model.push(.browse) }
                    linkCard
                }
                .padding(.horizontal, Theme.gutter)
                .padding(.bottom, 20)
                .frame(maxWidth: 820)
                .frame(maxWidth: .infinity)
            }
        }
    }

    private var linkCard: some View {
        VStack(spacing: 10) {
            LauncherFocusable(id: "add.link", onActivate: { linkFocused = true }) { focused in
                HStack(spacing: 16) {
                    Image(systemName: "link").font(.system(size: 22, weight: .semibold)).frame(width: 34).foregroundStyle(Theme.accent)
                    VStack(alignment: .leading, spacing: 6) {
                        Text("LINK").font(Theme.heading(21)).tracking(1.5).foregroundStyle(Theme.text)
                        field
                    }
                }
                .padding(.horizontal, 22)
                .padding(.vertical, 16)
                .frame(maxWidth: .infinity, alignment: .leading)
                .consoleCard(focused: focused)
            }
            HStack(spacing: 10) {
                ConsoleButton(id: "add.paste", title: "Paste", systemImage: "doc.on.clipboard") { paste() }
                ConsoleButton(id: "add.download", title: "Download", subtitle: linkURL == nil ? "Enter a link first" : nil,
                              systemImage: "arrow.down.circle.fill", enabled: linkURL != nil) { start() }
            }
        }
    }

    @ViewBuilder
    private var field: some View {
        if snapshot {
            Text(link.isEmpty ? "https://www.moddb.com/addons/…" : link)
                .font(Theme.mono(16)).foregroundStyle(link.isEmpty ? Theme.dim.opacity(0.6) : Theme.text)
        } else {
            TextField("https://www.moddb.com/addons/…", text: $link)
                .textFieldStyle(.plain)
                .font(Theme.mono(16))
                .foregroundStyle(Theme.text)
                .focused($linkFocused)
                .autocorrectionDisabled()
                #if os(iOS)
                .textInputAutocapitalization(.never)
                .keyboardType(.URL)
                #endif
                .submitLabel(.go)
                .onSubmit { if linkURL != nil { start() } }
        }
    }

    private func paste() {
        #if canImport(UIKit)
        if let text = UIPasteboard.general.string { link = text }
        #elseif canImport(AppKit)
        if let text = NSPasteboard.general.string(forType: .string) { link = text }
        #endif
    }

    private func start() {
        guard let url = linkURL else { return }
        linkFocused = false
        model.imports.download(link: url)
        link = ""
    }
}

/// The in-app browser (ModDownloads). A download that finishes goes straight into the import flow.
struct BrowseView: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        VStack(spacing: 0) {
            ScreenHeader(title: "Browse", subtitle: "Downloads are added as mods when they finish") { EmptyView() }
            DownloadsView(downloadsDir: model.downloadsDir) { file in
                model.pop()
                model.imports.begin(url: file, temporary: true)
            }
            .clipShape(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous))
            .padding(.horizontal, Theme.gutter)
            .padding(.bottom, 8)
        }
    }
}
