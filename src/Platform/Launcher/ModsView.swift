// Mods: the load order with an on/off switch, drag to reorder, delete, details. The last mod in the list wins a file conflict.
import SwiftUI
#if canImport(ModManagerCore)
import ModManagerCore
#endif

struct ScreenHeader<Trailing: View>: View {
    @EnvironmentObject private var model: LauncherModel
    let title: String
    var subtitle: String?
    var showBack = true
    @ViewBuilder var trailing: () -> Trailing

    var body: some View {
        HStack(spacing: 16) {
            if showBack {
                Button { model.back() } label: {
                    Image(systemName: "chevron.left").font(.system(size: 20, weight: .bold)).foregroundStyle(Theme.accent)
                        .frame(width: 44, height: 44)
                        .background(Circle().fill(Theme.panel))
                }
                .buttonStyle(.plain)
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(title.uppercased()).font(Theme.title(32)).tracking(4).foregroundStyle(Theme.text)
                if let subtitle { Text(subtitle).font(Theme.small(15)).foregroundStyle(Theme.dim) }
            }
            Spacer(minLength: 0)
            trailing()
        }
        .padding(.horizontal, Theme.gutter)
        .padding(.top, 14)
        .padding(.bottom, 12)
    }
}

struct ModsView: View {
    @EnvironmentObject private var model: LauncherModel
    @EnvironmentObject private var focus: FocusStore
    @State private var dragging: String?
    @State private var dragStartIndex = 0

    var body: some View {
        VStack(spacing: 0) {
            ScreenHeader(title: "Mods", subtitle: subtitle) {
                Chip(text: "Last one wins a conflict", color: Theme.dim, systemImage: "arrow.down.to.line")
            }
            if model.mods.isEmpty {
                empty
            } else {
                list
            }
        }
    }

    private var subtitle: String {
        "\(model.enabledCount) enabled · \(Format.count(model.mods.count, "mod")) installed"
    }

    private var empty: some View {
        VStack(spacing: 18) {
            Image(systemName: "shippingbox").font(.system(size: 56)).foregroundStyle(Theme.dim)
            Text("No mods yet").font(Theme.heading(26)).foregroundStyle(Theme.text)
            Text("Mods are kept apart from the game files: nothing is overwritten, and switching one off puts everything back.")
                .font(Theme.body(17)).foregroundStyle(Theme.dim).multilineTextAlignment(.center).frame(maxWidth: 520)
            ConsoleButton(id: "mods.add", title: "Add mod", systemImage: "plus.circle.fill", isDefault: true) { model.push(.add) }
                .frame(maxWidth: 320)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var list: some View {
        ScrollViewReader { scroller in
            LauncherScroll {
                VStack(spacing: 10) {
                    ForEach(Array(model.mods.enumerated()), id: \.element.id) { index, mod in
                        ModRow(mod: mod, index: index, count: model.mods.count, dragging: dragging == mod.id,
                               onDragChanged: { dy in dragChanged(mod.id, index: index, dy: dy) },
                               onDragEnded: { dragging = nil })
                    }
                    ConsoleButton(id: "mods.add", title: "Add mod", systemImage: "plus.circle.fill") { model.push(.add) }
                        .padding(.top, 8)
                }
                .padding(.horizontal, Theme.gutter)
                .padding(.vertical, 8)
            }
            .onChange(of: focus.focusedID) { _, id in
                guard let id else { return }
                withAnimation(.easeOut(duration: 0.15)) { scroller.scrollTo(id, anchor: .center) }
            }
        }
    }

    private func dragChanged(_ id: String, index: Int, dy: CGFloat) {
        if dragging != id {
            dragging = id
            dragStartIndex = index
        }
        let step = Theme.rowHeight + 10
        let target = min(max(dragStartIndex + Int((dy / step).rounded()), 0), model.mods.count - 1)
        if let current = model.mods.firstIndex(where: { $0.id == id }), current != target {
            model.move(from: current, to: target)
        }
    }
}

struct ModRow: View {
    @EnvironmentObject private var model: LauncherModel
    let mod: InstalledMod
    let index: Int
    let count: Int
    let dragging: Bool
    let onDragChanged: (CGFloat) -> Void
    let onDragEnded: () -> Void

    var body: some View {
        let id = "mod:\(mod.id)"
        let conflicts = model.conflictCount(of: mod.id)
        LauncherFocusable(id: id, isDefault: index == 0, onActivate: { model.toggle(mod.id) },
                          onSecondary: { model.push(.details(mod.id)) }) { focused in
            HStack(spacing: 14) {
                handle
                Text(String(format: "%02d", index + 1))
                    .font(Theme.mono(15)).foregroundStyle(Theme.dim).frame(width: 28)
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 10) {
                        Text(mod.name).font(Theme.heading(21)).foregroundStyle(mod.enabled ? Theme.text : Theme.dim).lineLimit(1)
                        if let version = mod.version, !version.isEmpty {
                            Text(version).font(Theme.mono(13)).foregroundStyle(Theme.dim)
                        }
                    }
                    Text("\(Format.bytes(mod.sizeBytes)) · \(Format.count(mod.fileCount, "file"))")
                        .font(Theme.small(14)).foregroundStyle(Theme.dim)
                }
                Spacer(minLength: 8)
                if conflicts > 0 && mod.enabled {
                    Chip(text: Format.count(conflicts, "conflict"), color: Theme.warn, systemImage: "arrow.triangle.merge")
                }
                Button { model.push(.details(mod.id)) } label: {
                    Image(systemName: "info.circle").font(.system(size: 24)).foregroundStyle(Theme.dim).frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                ConsoleSwitch(isOn: mod.enabled)
            }
            .padding(.horizontal, 16)
            .frame(height: Theme.rowHeight)
            .consoleCard(focused: focused || dragging, dimmed: !mod.enabled && !focused)
            .overlay(alignment: .leading) {
                if focused && model.focus.held.contains(.square) {
                    Image(systemName: "arrow.up.arrow.down").font(.system(size: 14, weight: .bold)).foregroundStyle(Theme.accent)
                        .offset(x: -22)
                }
            }
            .contextMenu {
                Button(mod.enabled ? "Switch off" : "Switch on") { model.toggle(mod.id) }
                Button("Details") { model.push(.details(mod.id)) }
                Button("Move up") { model.move(mod.id, by: -1) }.disabled(index == 0)
                Button("Move down") { model.move(mod.id, by: 1) }.disabled(index == count - 1)
                Button("Remove…", role: .destructive) { model.removalCandidate = mod.id }
            }
        }
    }

    /// Touch / mouse reordering: drag the handle; the row follows by swapping places as it crosses its neighbours.
    private var handle: some View {
        Image(systemName: "line.3.horizontal")
            .font(.system(size: 18, weight: .semibold))
            .foregroundStyle(Theme.dim)
            .frame(width: 34, height: Theme.rowHeight)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 4, coordinateSpace: .named("launcher"))
                    .onChanged { value in onDragChanged(value.translation.height) }
                    .onEnded { _ in onDragEnded() }
            )
    }
}
