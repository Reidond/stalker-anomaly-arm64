// Mods: the load order with a switch per mod, reordering (drag in edit mode on the iPad, drag on the Mac), swipe to remove,
// details. The last mod in the list wins a file conflict.
import SwiftUI
import ModManagerCore

struct ModsView: View {
    @EnvironmentObject private var model: LauncherModel
    @EnvironmentObject private var focus: FocusStore
    #if os(iOS)
    @State private var editMode: EditMode = .inactive
    #endif

    var body: some View {
        ScrollViewReader { scroller in
            List {
                Section {
                    ForEach(Array(model.mods.enumerated()), id: \.element.id) { index, mod in
                        ModRow(mod: mod, index: index, count: model.mods.count)
                    }
                    .onMove { source, destination in model.moveOffsets(source, toOffset: destination) }
                } footer: {
                    if !model.mods.isEmpty {
                        Text("Mods are loaded from the top down: when two mods change the same file, the one lower in the list wins.")
                    }
                }
                Section {
                    NavigationLink(value: Screen.add) { Label("Add Mod", systemImage: "plus.circle.fill") }
                        .padFocus("mods.add", inList: true)
                }
            }
            .launcherListStyle()
            .scrollContentBackground(.hidden)
            .readableColumn()
            #if os(iOS)
            .environment(\.editMode, $editMode)
            #endif
            .overlay {
                if model.mods.isEmpty {
                    ContentUnavailableView {
                        Label("No Mods", systemImage: "shippingbox")
                    } description: {
                        Text("Mods are kept apart from the game files: nothing is overwritten, and switching one off puts everything back.")
                    }
                    .allowsHitTesting(false)
                    .padding(.bottom, 80)
                }
            }
            .onChange(of: focus.focusedID) { _, id in
                guard let id else { return }
                withAnimation { scroller.scrollTo(id, anchor: .center) }
            }
        }
        .launcherScreen("Mods") {
            #if os(iOS)
            if !model.mods.isEmpty { EditButton() }
            #endif
        }
    }
}

struct ModRow: View {
    @EnvironmentObject private var model: LauncherModel
    let mod: InstalledMod
    let index: Int
    let count: Int

    var body: some View {
        let conflicts = model.conflictCount(of: mod.id)
        HStack(spacing: 12) {
            Text("\(index + 1)")
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
                .frame(width: 22, alignment: .trailing)
            Button { model.push(.details(mod.id)) } label: {
                VStack(alignment: .leading, spacing: 2) {
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        Text(mod.name).font(.headline).lineLimit(1)
                        if let version = mod.version, !version.isEmpty {
                            Text(version).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    Text("\(Format.bytes(mod.sizeBytes)) · \(Format.count(mod.fileCount, "file"))")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if conflicts > 0 && mod.enabled {
                Label(Format.count(conflicts, "conflict"), systemImage: "arrow.triangle.merge")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }
            Toggle(mod.name, isOn: model.binding(for: mod)).labelsHidden().toggleStyle(.switch)
        }
        .opacity(mod.enabled ? 1 : 0.6)
        .padFocus("mod:\(mod.id)", inList: true)
        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
            Button(role: .destructive) { model.removalCandidate = mod.id } label: { Label("Remove", systemImage: "trash") }
        }
        .contextMenu {
            Button(mod.enabled ? "Switch Off" : "Switch On") { model.toggle(mod.id) }
            Button("Details") { model.push(.details(mod.id)) }
            Divider()
            Button("Move Up") { model.move(mod.id, by: -1) }.disabled(index == 0)
            Button("Move Down") { model.move(mod.id, by: 1) }.disabled(index == count - 1)
            Divider()
            Button("Remove…", role: .destructive) { model.removalCandidate = mod.id }
        }
    }
}
