// One mod: what it is, where it came from, how big, and which files it fights over with other mods.
import SwiftUI
import ModManagerCore

struct DetailsView: View {
    @EnvironmentObject private var model: LauncherModel
    let id: String
    @State private var meta: ModMeta?

    var body: some View {
        if let mod = model.mods.first(where: { $0.id == id }) {
            content(mod)
                .onAppear { meta = ModMeta.load(from: mod.folderURL) }
        } else {
            Color.clear.onAppear { model.pop() }
        }
    }

    private func content(_ mod: InstalledMod) -> some View {
        let entry = model.conflicts[mod.id]
        let position = (model.mods.firstIndex(where: { $0.id == mod.id }) ?? 0) + 1
        return LauncherForm {
            if let description = meta?.description, !description.isEmpty {
                Section("About") { Text(description) }
            }

            Section("Details") {
                if let version = mod.version, !version.isEmpty { LabeledContent("Version", value: version) }
                if let author = meta?.author, !author.isEmpty { LabeledContent("Author", value: author) }
                LabeledContent("Size", value: Format.bytes(mod.sizeBytes))
                LabeledContent("Files", value: "\(mod.fileCount)")
                LabeledContent("Load order", value: "\(position) of \(model.mods.count)")
                if let source = sourceText { LabeledContent("Source") { Text(source).multilineTextAlignment(.trailing) } }
                if let installed = Format.date(meta?.installedAt) { LabeledContent("Added", value: installed) }
            }

            conflictSections(mod, entry: entry)

            Section {
                Toggle(isOn: model.binding(for: mod)) { Label("Enabled", systemImage: "power") }
                    .toggleStyle(.switch)
                    .padFocus("details.toggle", inList: true)
                Button { model.move(mod.id, by: 1) } label: { Label("Load Later", systemImage: "arrow.down.circle") }
                    .disabled(model.mods.last?.id == mod.id)
                    .padFocus("details.later", inList: true)
                Button { model.move(mod.id, by: -1) } label: { Label("Load Earlier", systemImage: "arrow.up.circle") }
                    .disabled(model.mods.first?.id == mod.id)
                    .padFocus("details.earlier", inList: true)
            } footer: {
                Text("Mods lower in the list load later and win when two mods change the same file. Changes apply the next time the game starts.")
            }

            Section {
                Button(role: .destructive) { model.removalCandidate = mod.id } label: { Label("Remove Mod…", systemImage: "trash") }
                    .padFocus("details.remove", inList: true)
            }
        }
        .launcherScreen(mod.name, inline: true)
    }

    private var sourceText: String? {
        guard let source = meta?.source else { return nil }
        let kind: String
        switch source.kind {
        case "url": kind = "Link"
        case "browser": kind = "Browser"
        case "file": kind = "File"
        default: kind = source.kind?.capitalized ?? "Unknown"
        }
        let detail = source.url ?? source.fileName
        return detail.map { "\(kind) · \($0)" } ?? kind
    }

    @ViewBuilder
    private func conflictSections(_ mod: InstalledMod, entry: ConflictSummary.Entry) -> some View {
        if !mod.enabled {
            Section("File conflicts") { Text("Counted between mods that are on.").foregroundStyle(.secondary) }
        } else if entry.wins.isEmpty && entry.loses.isEmpty {
            Section("File conflicts") {
                Label("No other mod that is on changes the same files.", systemImage: "checkmark.circle.fill")
                    .symbolRenderingMode(.multicolor)
            }
        } else {
            Section("File conflicts") {
                ForEach(entry.wins) { pair in conflictGroup(pair, wins: true) }
                ForEach(entry.loses) { pair in conflictGroup(pair, wins: false) }
            }
        }
    }

    private func conflictGroup(_ pair: ConflictSummary.Pair, wins: Bool) -> some View {
        DisclosureGroup {
            ForEach(pair.paths.prefix(200), id: \.self) { path in
                Text(path).font(.caption.monospaced()).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            }
            if pair.paths.count > 200 { Text("and \(pair.paths.count - 200) more").font(.caption).foregroundStyle(.secondary) }
        } label: {
            Label(wins ? "Overrides \(Format.count(pair.paths.count, "file")) of \(model.name(of: pair.other))"
                       : "\(Format.count(pair.paths.count, "file")) overridden by \(model.name(of: pair.other))",
                  systemImage: wins ? "arrow.up.right.circle.fill" : "arrow.down.right.circle.fill")
                .symbolRenderingMode(.multicolor)
                .foregroundStyle(wins ? Color.green : Color.orange)
        }
    }
}
