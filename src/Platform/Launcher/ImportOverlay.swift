// The sheets that sit on top of the launcher: the import flow (reading, warnings, FOMOD wizard, installing, result) and the
// confirmation before a mod is removed. System sheets, so they get the system's glass presentation on iPadOS and macOS 26.
import SwiftUI
import ModManagerCore

/// An icon, a title and a sentence: the result states of a sheet.
struct SheetMessage: View {
    let systemImage: String
    var tint: Color = Theme.amber
    let title: String
    var message: String?

    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: systemImage).font(.system(size: 54, weight: .medium)).foregroundStyle(tint).padding(.bottom, 4)
            Text(title).font(.title2.weight(.bold)).foregroundStyle(Theme.primaryText).multilineTextAlignment(.center)
            if let message {
                Text(message).font(.body).foregroundStyle(Theme.secondaryText).multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: 440)
    }
}

/// The buttons at the foot of a sheet, full width, at a size a finger and a pad can hit.
private struct SheetButton: View {
    let title: String
    var systemImage: String?
    var style: Style = .glass
    let action: () -> Void
    enum Style { case prominent, glass, destructive }
    /// the same radius as the warnings above the buttons, so that the corners of a sheet's content agree
    static let radius: CGFloat = 14

    var body: some View {
        Button(action: action) {
            Group {
                if let systemImage { Label(title, systemImage: systemImage) } else { Text(title) }
            }
            .font(.headline)
            .frame(maxWidth: .infinity, minHeight: 52)
        }
        .modifier(SheetButtonStyle(style: style))
        .buttonBorderShape(.roundedRectangle(radius: SheetButton.radius))
        .controlSize(.large)
    }
}

enum SheetButtonRadius { static let value: CGFloat = 14 }

private struct SheetButtonStyle: ViewModifier {
    let style: SheetButton.Style

    func body(content: Content) -> some View {
        switch style {
        case .prominent: content.prominentButton(radius: SheetButtonRadius.value).tint(Theme.amber)
        case .glass: content.glassButton()
        case .destructive: content.prominentButton(radius: SheetButtonRadius.value).tint(Theme.rust)
        }
    }
}

struct ImportSheet: View {
    @EnvironmentObject private var model: LauncherModel
    @ObservedObject var imports: ImportCoordinator

    var body: some View {
        NavigationStack {
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .navigationTitle(title)
                #if os(iOS)
                .navigationBarTitleDisplayMode(.inline)
                #endif
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        if showsCancel { Button("Cancel") { imports.cancel() } }
                    }
                }
        }
        .preferredColorScheme(.dark)
        .tint(Theme.amber)
        #if os(macOS)
        .frame(minWidth: 560, minHeight: 420)
        #endif
        .presentationDetents(isFomod ? [.large] : [.medium, .large])
        .interactiveDismissDisabled(imports.isInstalling)
    }

    private var isFomod: Bool {
        if case .fomod = imports.stage { return true }
        return false
    }

    private var title: String {
        switch imports.stage {
        case .fomod(let plan): plan.displayName
        case .review(let plan): plan.displayName
        default: "Add Mod"
        }
    }

    private var showsCancel: Bool {
        switch imports.stage {
        case .working, .review, .fomod: !imports.isInstalling
        default: false
        }
    }

    @ViewBuilder
    private var content: some View {
        switch imports.stage {
        case .idle:
            EmptyView()
        case .working(let title, let detail, let progress):
            VStack(spacing: 18) {
                if let progress {
                    ProgressView(value: progress) {
                        Text(title).font(.title3.weight(.semibold)).foregroundStyle(Theme.primaryText)
                    } currentValueLabel: {
                        Text("\(Int(progress * 100)) %").font(.callout.monospacedDigit()).foregroundStyle(Theme.secondaryText)
                    }
                    .tint(Theme.amber)
                    .frame(maxWidth: 380)
                } else {
                    ProgressView { Text(title).font(.title3.weight(.semibold)).foregroundStyle(Theme.primaryText) }
                        .controlSize(.large)
                }
                Text(detail).font(.callout).foregroundStyle(Theme.secondaryText).lineLimit(2).multilineTextAlignment(.center)
            }
            .padding(32)
        case .review(let plan):
            VStack(spacing: 20) {
                Spacer(minLength: 0)
                SheetMessage(systemImage: "exclamationmark.triangle.fill", title: "Check before adding")
                VStack(spacing: 8) {
                    ForEach(plan.warnings, id: \.self) { warning in
                        Text(warning).font(.callout).foregroundStyle(Theme.primaryText)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(14)
                            .background(RoundedRectangle(cornerRadius: SheetButtonRadius.value, style: .continuous).fill(Theme.rust.opacity(0.30)))
                    }
                }
                .frame(maxWidth: 480)
                VStack(spacing: 10) {
                    SheetButton(title: "Add Anyway", systemImage: "square.and.arrow.down", style: .prominent) { imports.install(plan, choices: nil) }
                        .padFocus("import.install", radius: SheetButtonRadius.value, ring: .white)
                    SheetButton(title: "Cancel") { imports.cancel() }
                        .padFocus("import.cancel", radius: SheetButtonRadius.value)
                }
                .frame(maxWidth: 480)
                Spacer(minLength: 0)
            }
            .padding(24)
        case .fomod(let plan):
            if let session = imports.fomodSession {
                FomodWizardView(session: session,
                                onFinish: { choices in imports.install(plan, choices: choices) },
                                onCancel: { imports.cancel() })
            }
        case .done(let mod):
            VStack(spacing: 24) {
                Spacer(minLength: 0)
                SheetMessage(systemImage: "checkmark.circle.fill", tint: Theme.radiation, title: "\(mod.name) was added",
                             message: "\(Format.count(mod.fileCount, "file")), \(Format.bytes(mod.sizeBytes)). "
                                    + (mod.enabled ? "It is on and loads the next time the game starts." : "It is off: switch it on to use it."))
                VStack(spacing: 10) {
                    if !mod.enabled {
                        SheetButton(title: "Switch On", systemImage: "power", style: .prominent) { model.enableAfterImport(mod) }
                            .padFocus("import.enable", radius: SheetButtonRadius.value, ring: .white)
                    }
                    SheetButton(title: "Open Mods", style: mod.enabled ? .prominent : .glass) { model.openModsAfterImport() }
                        .padFocus("import.mods", radius: SheetButtonRadius.value)
                    SheetButton(title: "Done") { imports.dismiss() }
                        .padFocus("import.done", radius: SheetButtonRadius.value)
                }
                .frame(maxWidth: 480)
                Spacer(minLength: 0)
            }
            .padding(24)
        case .failed(let title, let message):
            VStack(spacing: 24) {
                Spacer(minLength: 0)
                SheetMessage(systemImage: "exclamationmark.triangle.fill", tint: Theme.rust, title: title, message: message)
                SheetButton(title: "Close", style: .prominent) { imports.dismiss() }
                    .padFocus("import.close", radius: SheetButtonRadius.value, ring: .white)
                    .frame(maxWidth: 480)
                Spacer(minLength: 0)
            }
            .padding(24)
        }
    }
}

struct RemoveSheet: View {
    @EnvironmentObject private var model: LauncherModel
    let id: String

    var body: some View {
        VStack(spacing: 24) {
            Spacer(minLength: 0)
            SheetMessage(systemImage: "trash", tint: Theme.rust, title: "Remove \(model.name(of: id))?",
                         message: "The mod's files are deleted from this device. Add it again from its source if you want it back; your saves are not touched.")
            VStack(spacing: 10) {
                SheetButton(title: "Remove", style: .destructive) { model.remove(id) }
                    .padFocus("remove.confirm", radius: SheetButtonRadius.value, ring: .white)
                SheetButton(title: "Keep It") { model.removalCandidate = nil }
                    .padFocus("remove.cancel", radius: SheetButtonRadius.value)
            }
            .frame(maxWidth: 480)
            Spacer(minLength: 0)
        }
        .padding(24)
        .preferredColorScheme(.dark)
        .tint(Theme.amber)
        #if os(macOS)
        .frame(minWidth: 480, minHeight: 360)
        #endif
        .presentationDetents([.medium])
    }
}
