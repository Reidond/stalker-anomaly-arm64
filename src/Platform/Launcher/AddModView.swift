// Add mod: from the Files app (zip, 7z, rar or a folder), from the in-app browser (ModDB, GitHub) or from a pasted link.
import SwiftUI
import UniformTypeIdentifiers
import ModDownloads

struct LauncherSnapshotKey: EnvironmentKey { static let defaultValue = false }

extension EnvironmentValues {
    /// Set by the snapshot tool (a window nobody sees): nothing that needs a live run loop.
    var launcherSnapshot: Bool {
        get { self[LauncherSnapshotKey.self] }
        set { self[LauncherSnapshotKey.self] = newValue }
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
    @FocusState private var linkFocused: Bool

    var body: some View {
        LauncherForm {
            Section {
                Button { model.showFilePicker = true } label: {
                    row("Files", detail: "A zip, 7z or rar archive, or a folder, from the Files app", systemImage: "folder.fill")
                }
                .padFocus("add.files", inList: true)
                NavigationLink(value: Screen.browse) {
                    row("Browse", detail: "ModDB and GitHub in the built-in browser", systemImage: "globe")
                }
                .padFocus("add.browse", inList: true)
            } footer: {
                Text("Nothing is overwritten: each mod is kept in its own folder and loaded in the order you choose.")
            }

            Section("From a link") {
                TextField("https://www.moddb.com/addons/…", text: $model.linkText)
                    .focused($linkFocused)
                    .autocorrectionDisabled()
                    #if os(iOS)
                    .textInputAutocapitalization(.never)
                    .keyboardType(.URL)
                    #endif
                    .submitLabel(.go)
                    .onSubmit { model.startLinkDownload() }
                    .padFocus("add.link", inList: true)
                Button { model.pasteLink() } label: { Label("Paste", systemImage: "doc.on.clipboard") }
                    .padFocus("add.paste", inList: true)
                Button { linkFocused = false; model.startLinkDownload() } label: { Label("Download", systemImage: "arrow.down.circle.fill") }
                    .disabled(model.linkURL == nil)
                    .padFocus("add.download", inList: true)
            }
        }
        .onChange(of: model.linkFocusRequest) { _, _ in linkFocused = true }
        .launcherScreen("Add Mod")
    }

    private func row(_ title: String, detail: String, systemImage: String) -> some View {
        Label {
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                Text(detail).font(.subheadline).foregroundStyle(.secondary)
            }
        } icon: {
            Image(systemName: systemImage)
        }
    }
}

/// The in-app browser (ModDownloads). A download that finishes goes straight into the import flow.
struct BrowseView: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        DownloadsView(downloadsDir: model.downloadsDir) { file in
            model.pop()
            model.imports.begin(url: file, temporary: true)
        }
        .launcherScreen("Browse", inline: true)
    }
}
