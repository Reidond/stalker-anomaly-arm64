// One mod: what it is, where it came from, how big, and which files it fights over with other mods. Cards of glass, big type.
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
        return ScrollView {
            GlassGroup(spacing: 16) {
                VStack(spacing: 16) {
                    if let description = meta?.description, !description.isEmpty {
                        GlassCard(title: "About") {
                            Text(description).font(.body).foregroundStyle(Theme.primaryText).fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    GlassCard(title: "Details") {
                        VStack(spacing: 0) {
                            if let version = mod.version, !version.isEmpty { fact("Version", version) }
                            if let author = meta?.author, !author.isEmpty { fact("Author", author) }
                            fact("Size", Format.bytes(mod.sizeBytes))
                            fact("Files", "\(mod.fileCount)")
                            fact("Load order", "\(position) of \(model.mods.count)")
                            if let source = sourceText { fact("Source", source) }
                            if let installed = Format.date(meta?.installedAt) { fact("Added", installed, last: true) }
                        }
                    }
                    conflictCard(mod, entry: entry)
                    actions(mod)
                }
                .padding(.horizontal, 28)
                .padding(.bottom, 96)
            }
            .frame(maxWidth: 820)
            .frame(maxWidth: .infinity)
        }
        .launcherScreen(mod.name, subtitle: subtitle(mod))
    }

    private func subtitle(_ mod: InstalledMod) -> String? {
        let parts = [mod.version.map { "Version \($0)" }, meta?.author.map { "by \($0)" }].compactMap { $0 }
        return parts.isEmpty ? nil : parts.joined(separator: " · ")
    }

    private func fact(_ title: String, _ value: String, last: Bool = false) -> some View {
        VStack(spacing: 0) {
            HStack(alignment: .firstTextBaseline, spacing: 16) {
                Text(title).font(.callout).foregroundStyle(Theme.secondaryText)
                Spacer(minLength: 12)
                Text(value).font(.body.monospacedDigit()).foregroundStyle(Theme.primaryText).multilineTextAlignment(.trailing)
            }
            .padding(.vertical, 12)
            if !last { Rectangle().fill(Color.white.opacity(0.10)).frame(height: 1) }
        }
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
    private func conflictCard(_ mod: InstalledMod, entry: ConflictSummary.Entry) -> some View {
        GlassCard(title: "File conflicts") {
            if !mod.enabled {
                Text("Counted between mods that are on.").font(.callout).foregroundStyle(Theme.secondaryText)
            } else if entry.wins.isEmpty && entry.loses.isEmpty {
                Label("No other mod that is on changes the same files.", systemImage: "checkmark.circle.fill")
                    .symbolRenderingMode(.palette)
                    .foregroundStyle(Theme.primaryText, Theme.radiation)
                    .font(.callout)
            } else {
                VStack(spacing: 8) {
                    ForEach(entry.wins) { pair in conflictGroup(pair, wins: true) }
                    ForEach(entry.loses) { pair in conflictGroup(pair, wins: false) }
                }
            }
        }
    }

    private func conflictGroup(_ pair: ConflictSummary.Pair, wins: Bool) -> some View {
        DisclosureGroup {
            VStack(alignment: .leading, spacing: 4) {
                ForEach(pair.paths.prefix(200), id: \.self) { path in
                    Text(path).font(.caption.monospaced()).foregroundStyle(Theme.secondaryText).lineLimit(1).truncationMode(.middle)
                }
                if pair.paths.count > 200 { Text("and \(pair.paths.count - 200) more").font(.caption).foregroundStyle(Theme.tertiaryText) }
            }
            .padding(.top, 6)
        } label: {
            Label(wins ? "Overrides \(Format.count(pair.paths.count, "file")) of \(model.name(of: pair.other))"
                       : "\(Format.count(pair.paths.count, "file")) overridden by \(model.name(of: pair.other))",
                  systemImage: wins ? "arrow.up.right.circle.fill" : "arrow.down.right.circle.fill")
                .symbolRenderingMode(.palette)
                .foregroundStyle(Theme.primaryText, wins ? Theme.radiation : Theme.amber)
                .font(.callout.weight(.medium))
        }
        .tint(Theme.secondaryText)
    }

    private func actions(_ mod: InstalledMod) -> some View {
        VStack(spacing: 12) {
            HStack(spacing: 16) {
                IconTile(systemImage: "power", tint: mod.enabled ? Theme.radiation : Theme.secondaryText)
                Text(mod.enabled ? "On" : "Off").font(.title3.weight(.semibold)).foregroundStyle(Theme.primaryText)
                Spacer()
                Toggle(mod.name, isOn: model.binding(for: mod)).labelsHidden().toggleStyle(.switch).tint(Theme.radiation)
            }
            .padding(Theme.rowPadding)
            .frame(maxWidth: .infinity, minHeight: Theme.minHit + 16)
            .glassSurface(radius: Theme.rowRadius)
            .padFocus("details.toggle")

            MenuRow(title: "Load Later", subtitle: "Higher priority: wins over the mods above it", systemImage: "arrow.down.circle", chevron: false) {
                model.move(mod.id, by: 1)
            }
            .disabled(model.mods.last?.id == mod.id)
            .padFocus("details.later")
            MenuRow(title: "Load Earlier", subtitle: "Lower priority: the mods below it win", systemImage: "arrow.up.circle", chevron: false) {
                model.move(mod.id, by: -1)
            }
            .disabled(model.mods.first?.id == mod.id)
            .padFocus("details.earlier")
            MenuRow(title: "Remove Mod…", subtitle: "Deletes the mod's files from this device", systemImage: "trash", chevron: false, tint: Theme.rust,
                    glassTint: .rust) { model.removalCandidate = mod.id }
                .padFocus("details.remove")

            Text("Changes apply the next time the game starts.").font(.footnote).foregroundStyle(Theme.secondaryText).padding(.top, 4)
        }
    }
}
