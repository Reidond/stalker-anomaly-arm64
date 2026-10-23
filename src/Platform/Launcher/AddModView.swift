// Add Mod: from the Files app (zip, 7z, rar or a folder), from the in-app browser (ModDB, GitHub) or from a pasted link.
import SwiftUI
import UniformTypeIdentifiers
#if canImport(ModDownloads)
import ModDownloads
#endif

struct LauncherSnapshotKey: EnvironmentKey { static let defaultValue = false }

extension EnvironmentValues {
    /// Set by the snapshot tool (a window nobody sees): nothing that needs a live run loop, no entrance animation.
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
    @Environment(\.launcherSnapshot) private var snapshot
    @FocusState private var linkFocused: Bool

    var body: some View {
        ScrollView {
            GlassGroup(spacing: 14) {
                VStack(spacing: 14) {
                    MenuRow(title: "Files", subtitle: "A zip, 7z or rar archive, or a folder, from the Files app", systemImage: "folder.fill") {
                        model.showFilePicker = true
                    }
                    .padFocus("add.files")
                    MenuRow(title: "Browse", subtitle: "ModDB and GitHub in the built-in browser", systemImage: "globe") { model.push(.browse) }
                        .padFocus("add.browse")

                    GlassCard(title: "From a link") {
                        VStack(spacing: 12) {
                            field
                                .padFocus("add.link", radius: Theme.cardInnerRadius)
                            HStack(spacing: 12) {
                                Button { model.pasteLink() } label: {
                                    Label("Paste", systemImage: "doc.on.clipboard").frame(maxWidth: .infinity, minHeight: 44)
                                }
                                .glassButton()
                                .buttonBorderShape(.roundedRectangle(radius: Theme.cardInnerRadius))
                                .padFocus("add.paste", radius: Theme.cardInnerRadius)
                                Button { linkFocused = false; model.startLinkDownload() } label: {
                                    Label("Download", systemImage: "arrow.down.circle.fill").frame(maxWidth: .infinity, minHeight: 44)
                                }
                                .prominentButton(radius: Theme.cardInnerRadius)
                                .buttonBorderShape(.roundedRectangle(radius: Theme.cardInnerRadius))
                                .tint(Theme.amber)
                                .disabled(model.linkURL == nil)
                                .padFocus("add.download", radius: Theme.cardInnerRadius, ring: .white)
                            }
                            .font(.headline)
                        }
                    }

                    Text("Nothing is overwritten: each mod is kept in its own folder and loaded in the order you choose.")
                        .font(.footnote).foregroundStyle(Theme.secondaryText).padding(.top, 4)
                }
                .padding(.horizontal, 28)
                .padding(.bottom, 96)
            }
            .frame(maxWidth: 820)
            .frame(maxWidth: .infinity)
        }
        .onChange(of: model.linkFocusRequest) { _, _ in linkFocused = true }
        .launcherScreen("Add Mod")
    }

    /// The link field: its radius is the card's minus the card's padding, so that its corners follow the card's.
    @ViewBuilder
    private var field: some View {
        Group {
            if snapshot {
                Text(model.linkText.isEmpty ? "https://www.moddb.com/addons/…" : model.linkText)
                    .foregroundStyle(model.linkText.isEmpty ? Theme.tertiaryText : Theme.primaryText)
            } else {
                TextField("", text: $model.linkText, prompt: Text("https://www.moddb.com/addons/…").foregroundStyle(Theme.tertiaryText))
                    .focused($linkFocused)
                    .autocorrectionDisabled()
                    #if os(iOS)
                    .textInputAutocapitalization(.never)
                    .keyboardType(.URL)
                    #endif
                    .submitLabel(.go)
                    .onSubmit { model.startLinkDownload() }
                    .foregroundStyle(Theme.primaryText)
            }
        }
        .font(.system(.body, design: .monospaced))
        .padding(.horizontal, 16)
        .frame(maxWidth: .infinity, minHeight: Theme.minHit, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: Theme.cardInnerRadius, style: .continuous).fill(Color.black.opacity(0.28)))
        .overlay(RoundedRectangle(cornerRadius: Theme.cardInnerRadius, style: .continuous).strokeBorder(Color.white.opacity(linkFocused ? 0.5 : 0.12), lineWidth: 1))
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
        .clipShape(RoundedRectangle(cornerRadius: Theme.cardRadius - 8, style: .continuous))
        .padding(8)
        .glassSurface(radius: Theme.cardRadius)
        .padding(.horizontal, 28)
        .padding(.bottom, 24)
        .frame(maxWidth: 1100)
        .launcherScreen("Browse", subtitle: "Downloads are added as mods when they finish")
    }
}
