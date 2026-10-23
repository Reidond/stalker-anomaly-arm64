// Mods: the load order on glass rows, each with a switch; drag to reorder (Edit on the iPad, direct on the Mac), swipe to remove,
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
                ForEach(Array(model.mods.enumerated()), id: \.element.id) { index, mod in
                    ModRow(mod: mod, index: index, count: model.mods.count)
                        .listRowInsets(EdgeInsets(top: 5, leading: 28, bottom: 5, trailing: 28))
                        .listRowSeparator(.hidden)
                        .listRowBackground(
                            GlassRowBackground(focused: focus.isFocused("mod:\(mod.id)"), dimmed: !mod.enabled)
                                .padding(.horizontal, 28).padding(.vertical, 5)
                        )
                }
                .onMove { source, destination in model.moveOffsets(source, toOffset: destination) }

                if !model.mods.isEmpty {
                    Text("Mods are loaded from the top down: when two mods change the same file, the one lower in the list wins.")
                        .font(.footnote)
                        .foregroundStyle(Theme.secondaryText)
                        .listRowInsets(EdgeInsets(top: 8, leading: 36, bottom: 8, trailing: 36))
                        .listRowSeparator(.hidden)
                        .listRowBackground(Color.clear)
                }
                MenuRow(title: "Add Mod", systemImage: "plus.circle.fill") { model.push(.add) }
                    .padFocus("mods.add")
                    .listRowInsets(EdgeInsets(top: 10, leading: 28, bottom: 90, trailing: 28))
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            #if os(iOS)
            .environment(\.editMode, $editMode)
            #endif
            .overlay {
                if model.mods.isEmpty {
                    VStack(spacing: 10) {
                        Image(systemName: "shippingbox").font(.system(size: 52, weight: .light)).foregroundStyle(Theme.secondaryText)
                        Text("No Mods").font(.title2.weight(.semibold)).foregroundStyle(Theme.primaryText)
                        Text("Mods are kept apart from the game files: nothing is overwritten, and switching one off puts everything back.")
                            .font(.callout).foregroundStyle(Theme.secondaryText).multilineTextAlignment(.center).frame(maxWidth: 420)
                    }
                    .padding(.bottom, 200)
                    .allowsHitTesting(false)
                }
            }
            .onChange(of: focus.focusedID) { _, id in
                guard let id else { return }
                scroller.scrollTo(id, anchor: .center)
            }
        }
        .frame(maxWidth: 820)
        .launcherScreen("Mods", subtitle: model.mods.isEmpty ? nil : "\(model.enabledCount) on · \(Format.count(model.mods.count, "mod")) installed") {
            #if os(iOS)
            if !model.mods.isEmpty {
                Button(editMode.isEditing ? "Done" : "Edit") {
                    withAnimation(LauncherMotion(reduced: false).content) { editMode = editMode.isEditing ? .inactive : .active }
                }
                .glassButton()
                .controlSize(.large)
            }
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
        HStack(spacing: 14) {
            Text("\(index + 1)")
                .font(.callout.weight(.semibold).monospacedDigit())
                .foregroundStyle(Theme.secondaryText)
                .frame(width: 28, height: 28)
                .background(Circle().fill(Color.white.opacity(0.10)))
            Button { model.push(.details(mod.id)) } label: {
                VStack(alignment: .leading, spacing: 3) {
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        Text(mod.name).font(.title3.weight(.semibold)).foregroundStyle(Theme.primaryText).lineLimit(1)
                        if let version = mod.version, !version.isEmpty {
                            Text(version).font(.footnote.monospacedDigit()).foregroundStyle(Theme.secondaryText)
                        }
                    }
                    Text("\(Format.bytes(mod.sizeBytes)) · \(Format.count(mod.fileCount, "file"))")
                        .font(.subheadline.monospacedDigit()).foregroundStyle(Theme.secondaryText)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if conflicts > 0 && mod.enabled {
                Label(Format.count(conflicts, "conflict"), systemImage: "arrow.triangle.merge")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(Theme.amber)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 5)
                    .background(Capsule().fill(Theme.rust.opacity(0.35)))
            }
            Toggle(mod.name, isOn: model.binding(for: mod))
                .labelsHidden()
                .toggleStyle(.switch)
                .tint(Theme.radiation)
        }
        .padding(.horizontal, 6)
        .frame(minHeight: Theme.minHit + 8)
        .opacity(mod.enabled ? 1 : 0.62)
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
