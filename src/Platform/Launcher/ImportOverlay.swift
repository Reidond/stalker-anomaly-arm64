// The cards that sit on top of the launcher: the import flow (reading, warnings, FOMOD wizard, installing, result) and the
// confirmation before a mod is removed.
import SwiftUI
#if canImport(ModManagerCore)
import ModManagerCore
#endif

struct ModalCard<Content: View>: View {
    var width: CGFloat = 640
    @ViewBuilder var content: () -> Content

    var body: some View {
        ZStack {
            Color.black.opacity(0.66)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture {}
            VStack(alignment: .leading, spacing: 18) { content() }
                .padding(28)
                .frame(maxWidth: width)
                .background(RoundedRectangle(cornerRadius: 22, style: .continuous).fill(Theme.panel))
                .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous).strokeBorder(Theme.line, lineWidth: 1))
                .shadow(color: .black.opacity(0.6), radius: 40, y: 12)
                .padding(24)
        }
        .environment(\.focusScope, "modal")
        .transition(.opacity)
    }
}

struct ImportOverlay: View {
    @EnvironmentObject private var model: LauncherModel
    @ObservedObject var imports: ImportCoordinator

    var body: some View {
        switch imports.stage {
        case .idle:
            EmptyView()
        case .working(let title, let detail, let progress):
            ModalCard {
                Text(title.uppercased()).font(Theme.heading(24)).tracking(2).foregroundStyle(Theme.text)
                Text(detail).font(Theme.body(17)).foregroundStyle(Theme.dim).lineLimit(2)
                ProgressBar(fraction: progress)
                HStack {
                    if let progress { Text("\(Int(progress * 100)) %").font(Theme.mono(14)).foregroundStyle(Theme.dim) }
                    Spacer()
                }
                if title != "Installing" {
                    ConsoleButton(id: "import.cancel", title: "Cancel", isDefault: true) { imports.cancel() }
                }
            }
        case .review(let plan):
            ModalCard {
                Text(plan.displayName.uppercased()).font(Theme.heading(24)).tracking(2).foregroundStyle(Theme.text).lineLimit(2)
                Text("Check this before it is added:").font(Theme.body(17)).foregroundStyle(Theme.dim)
                ForEach(plan.warnings, id: \.self) { warning in Banner(style: .warning, title: warning) }
                ConsoleButton(id: "import.install", title: "Add anyway", systemImage: "square.and.arrow.down.fill") {
                    imports.install(plan, choices: nil)
                }
                ConsoleButton(id: "import.cancel", title: "Cancel", isDefault: true) { imports.cancel() }
            }
        case .fomod(let plan):
            ModalCard(width: 900) {
                if let installer = plan.fomod {
                    FomodWizardView(installer: installer,
                                    onFinish: { choices in imports.install(plan, choices: choices) },
                                    onCancel: { imports.cancel() })
                        .frame(minHeight: 420)
                }
                if !plan.warnings.isEmpty {
                    ForEach(plan.warnings, id: \.self) { warning in Banner(style: .warning, title: warning) }
                }
            }
        case .done(let mod):
            ModalCard {
                Banner(style: .success, title: "\(mod.name) was added",
                       message: "\(Format.count(mod.fileCount, "file")), \(Format.bytes(mod.sizeBytes)). "
                              + (mod.enabled ? "It is on and loads at the next start." : "It is off: switch it on in Mods."))
                if !mod.enabled {
                    ConsoleButton(id: "import.enable", title: "Switch on", systemImage: "play.circle.fill", isDefault: true) {
                        model.toggle(mod.id)
                        imports.dismiss()
                        model.push(.mods)
                    }
                }
                ConsoleButton(id: "import.mods", title: "Open Mods", systemImage: "square.stack.3d.up.fill", isDefault: mod.enabled) {
                    imports.dismiss()
                    if model.screen != .mods { model.push(.mods) }
                }
                ConsoleButton(id: "import.done", title: "Done") { imports.dismiss() }
            }
        case .failed(let title, let message):
            ModalCard {
                Banner(style: .error, title: title, message: message)
                ConsoleButton(id: "import.close", title: "Close", isDefault: true) { imports.dismiss() }
            }
        }
    }
}

struct RemoveConfirmation: View {
    @EnvironmentObject private var model: LauncherModel
    let id: String

    var body: some View {
        ModalCard {
            Text("REMOVE \(model.name(of: id).uppercased())?").font(Theme.heading(24)).tracking(2).foregroundStyle(Theme.text).lineLimit(2)
            Text("The mod's files are deleted from this device. Add it again from its source if you want it back; your saves are not touched.")
                .font(Theme.body(17)).foregroundStyle(Theme.dim)
            ConsoleButton(id: "remove.confirm", title: "Remove", systemImage: "trash", destructive: true) { model.remove(id) }
            ConsoleButton(id: "remove.cancel", title: "Keep it", isDefault: true) { model.removalCandidate = nil }
        }
    }
}
