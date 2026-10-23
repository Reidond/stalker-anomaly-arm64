// One mod: what it is, where it came from, how big, and which files it fights over with other mods.
import SwiftUI
#if canImport(ModManagerCore)
import ModManagerCore
#endif

struct DetailsView: View {
    @EnvironmentObject private var model: LauncherModel
    let id: String
    @State private var meta: ModMeta?
    @State private var showAllPaths: Set<String> = []

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
        return VStack(spacing: 0) {
            ScreenHeader(title: mod.name, subtitle: subtitle(mod)) {
                Chip(text: mod.enabled ? "On" : "Off", color: mod.enabled ? Theme.good : Theme.dim)
            }
            LauncherScroll {
                VStack(alignment: .leading, spacing: 18) {
                    Color.clear.frame(height: 0)
                    if let description = meta?.description, !description.isEmpty {
                        Text(description).font(Theme.body(18)).foregroundStyle(Theme.text.opacity(0.9))
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    facts(mod, position: position)
                    conflictSection(mod, entry: entry)
                    actions(mod)
                }
                .padding(.horizontal, Theme.gutter)
                .padding(.bottom, 24)
            }
        }
    }

    private func subtitle(_ mod: InstalledMod) -> String {
        [mod.version.map { "Version \($0)" }, meta?.author.map { "by \($0)" }].compactMap { $0 }.joined(separator: " · ")
    }

    private func facts(_ mod: InstalledMod, position: Int) -> some View {
        VStack(spacing: 0) {
            fact("Load order", "\(position) of \(model.mods.count)" + (position == model.mods.count ? " · wins conflicts" : ""))
            fact("Size", Format.bytes(mod.sizeBytes))
            fact("Files", "\(mod.fileCount)")
            if let source = sourceText { fact("Source", source) }
            if let installed = Format.date(meta?.installedAt) { fact("Installed", installed) }
            fact("Folder", mod.id, mono: true)
        }
        .background(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).fill(Theme.panel))
        .overlay(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).strokeBorder(Theme.line, lineWidth: 1))
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

    private func fact(_ title: String, _ value: String, mono: Bool = false) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(title).font(Theme.small(16)).foregroundStyle(Theme.dim).frame(width: 130, alignment: .leading)
            Text(value).font(mono ? Theme.mono(15) : Theme.body(17)).foregroundStyle(Theme.text)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 18)
        .padding(.vertical, 11)
        .overlay(alignment: .bottom) { Rectangle().fill(Theme.line).frame(height: 1).padding(.leading, 18) }
    }

    @ViewBuilder
    private func conflictSection(_ mod: InstalledMod, entry: ConflictSummary.Entry) -> some View {
        if !mod.enabled {
            Banner(style: .info, title: "Conflicts are only counted between mods that are on")
        } else if entry.wins.isEmpty && entry.loses.isEmpty {
            Banner(style: .success, title: "No file conflicts", message: "No other enabled mod changes the same files.")
        } else {
            VStack(alignment: .leading, spacing: 10) {
                Text("FILE CONFLICTS").font(Theme.small(14)).tracking(2).foregroundStyle(Theme.dim)
                ForEach(entry.wins) { pair in conflictGroup(pair, wins: true) }
                ForEach(entry.loses) { pair in conflictGroup(pair, wins: false) }
            }
        }
    }

    private func conflictGroup(_ pair: ConflictSummary.Pair, wins: Bool) -> some View {
        let key = (wins ? "w:" : "l:") + pair.other
        let expanded = showAllPaths.contains(key)
        let shown = expanded ? pair.paths : Array(pair.paths.prefix(5))
        return VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: wins ? "arrow.up.right.circle.fill" : "arrow.down.right.circle.fill")
                    .foregroundStyle(wins ? Theme.good : Theme.warn)
                Text(wins ? "Overrides \(Format.count(pair.paths.count, "file")) of \(model.name(of: pair.other))"
                          : "\(Format.count(pair.paths.count, "file")) overridden by \(model.name(of: pair.other))")
                    .font(Theme.body(17)).foregroundStyle(Theme.text)
            }
            ForEach(shown, id: \.self) { path in
                Text(path).font(Theme.mono(12.5)).foregroundStyle(Theme.dim).lineLimit(1).truncationMode(.middle)
            }
            if pair.paths.count > 5 {
                Button(expanded ? "Show fewer" : "Show all \(pair.paths.count)") {
                    if expanded { showAllPaths.remove(key) } else { showAllPaths.insert(key) }
                }
                .buttonStyle(.plain).font(Theme.small(14)).foregroundStyle(Theme.accent)
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).fill(Theme.panel))
        .overlay(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).strokeBorder(Theme.line, lineWidth: 1))
    }

    private func actions(_ mod: InstalledMod) -> some View {
        VStack(spacing: 10) {
            ConsoleButton(id: "details.toggle", title: mod.enabled ? "Switch off" : "Switch on",
                          subtitle: mod.enabled ? "Its files stop loading at the next start" : "Its files load at the next start",
                          systemImage: mod.enabled ? "pause.circle.fill" : "play.circle.fill", isDefault: true) { model.toggle(mod.id) }
            ConsoleButton(id: "details.up", title: "Load later", subtitle: "Higher priority: wins over the mods above it",
                          systemImage: "arrow.down.circle", enabled: model.mods.last?.id != mod.id) { model.move(mod.id, by: 1) }
            ConsoleButton(id: "details.down", title: "Load earlier", subtitle: "Lower priority: the mods below it win",
                          systemImage: "arrow.up.circle", enabled: model.mods.first?.id != mod.id) { model.move(mod.id, by: -1) }
            ConsoleButton(id: "details.remove", title: "Remove", subtitle: "Deletes the mod's files from this device",
                          systemImage: "trash", destructive: true) { model.removalCandidate = mod.id }
        }
    }
}
