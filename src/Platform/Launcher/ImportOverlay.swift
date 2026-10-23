// The sheets that sit on top of the launcher: the import flow (reading, warnings, FOMOD wizard, installing, result) and the
// confirmation before a mod is removed. System sheets, so they get the system's presentation and glass on iPadOS and macOS.
import SwiftUI
import ModManagerCore

struct ImportSheet: View {
    @EnvironmentObject private var model: LauncherModel
    @ObservedObject var imports: ImportCoordinator

    var body: some View {
        NavigationStack {
            content
                .navigationTitle(title)
                #if os(iOS)
                .navigationBarTitleDisplayMode(.inline)
                #endif
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        if showsCancel { Button("Cancel") { imports.cancel() }.keyboardShortcut(.cancelAction) }
                    }
                }
        }
        #if os(macOS)
        .frame(minWidth: 520, minHeight: 360)
        #endif
        .interactiveDismissDisabled(imports.isInstalling)
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
                Spacer()
                if let progress {
                    ProgressView(value: progress) { Text(title).font(.headline) } currentValueLabel: { Text("\(Int(progress * 100)) %") }
                        .frame(maxWidth: 360)
                } else {
                    ProgressView { Text(title).font(.headline) }.controlSize(.large)
                }
                Text(detail).font(.subheadline).foregroundStyle(.secondary).lineLimit(2).multilineTextAlignment(.center)
                Spacer()
            }
            .padding(32)
            .frame(maxWidth: .infinity)
        case .review(let plan):
            List {
                Section {
                    ForEach(plan.warnings, id: \.self) { warning in
                        Label(warning, systemImage: "exclamationmark.triangle.fill").symbolRenderingMode(.multicolor)
                    }
                } header: {
                    Text("Check before adding")
                }
                Section {
                    Button { imports.install(plan, choices: nil) } label: { Label("Add Anyway", systemImage: "square.and.arrow.down") }
                        .padFocus("import.install")
                    Button { imports.cancel() } label: { Label("Cancel", systemImage: "xmark") }
                        .padFocus("import.cancel")
                }
            }
        case .fomod(let plan):
            if let session = imports.fomodSession {
                FomodWizardView(session: session,
                                onFinish: { choices in imports.install(plan, choices: choices) },
                                onCancel: { imports.cancel() })
            }
        case .done(let mod):
            VStack(spacing: 0) {
                ContentUnavailableView {
                    Label("\(mod.name) was added", systemImage: "checkmark.circle.fill").symbolRenderingMode(.multicolor)
                } description: {
                    Text("\(Format.count(mod.fileCount, "file")), \(Format.bytes(mod.sizeBytes)). "
                         + (mod.enabled ? "It is on and loads the next time the game starts." : "It is off: switch it on to use it."))
                }
                VStack(spacing: 10) {
                    if !mod.enabled {
                        Button { model.enableAfterImport(mod) } label: { Text("Switch On").frame(maxWidth: .infinity) }
                            .prominentButton().controlSize(.large).padFocus("import.enable")
                    }
                    Button { model.openModsAfterImport() } label: { Text("Open Mods").frame(maxWidth: .infinity) }
                        .modifier(ConditionalProminent(prominent: mod.enabled)).controlSize(.large).padFocus("import.mods")
                    Button { imports.dismiss() } label: { Text("Done").frame(maxWidth: .infinity) }
                        .buttonStyle(.plain).foregroundStyle(Color.accentColor).padding(.top, 4).padFocus("import.done")
                }
                .padding(.horizontal, 32)
                .padding(.bottom, 28)
                .frame(maxWidth: 480)
            }
        case .failed(let title, let message):
            VStack(spacing: 0) {
                ContentUnavailableView {
                    Label(title, systemImage: "exclamationmark.triangle.fill").symbolRenderingMode(.multicolor)
                } description: {
                    Text(message)
                }
                Button { imports.dismiss() } label: { Text("Close").frame(maxWidth: .infinity) }
                    .prominentButton().controlSize(.large).padFocus("import.close")
                    .padding(.horizontal, 32).padding(.bottom, 28).frame(maxWidth: 480)
            }
        }
    }
}

private struct ConditionalProminent: ViewModifier {
    let prominent: Bool

    func body(content: Content) -> some View {
        if prominent {
            content.prominentButton()
        } else if #available(iOS 26.0, macOS 26.0, *) {
            content.buttonStyle(.glass)
        } else {
            content.buttonStyle(.bordered)
        }
    }
}

struct RemoveSheet: View {
    @EnvironmentObject private var model: LauncherModel
    let id: String

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                ContentUnavailableView {
                    Label("Remove \(model.name(of: id))?", systemImage: "trash")
                } description: {
                    Text("The mod's files are deleted from this device. Add it again from its source if you want it back; your saves are not touched.")
                }
                VStack(spacing: 10) {
                    Button(role: .destructive) { model.remove(id) } label: { Text("Remove").frame(maxWidth: .infinity) }
                        .buttonStyle(.borderedProminent).tint(.red).controlSize(.large).padFocus("remove.confirm")
                    Button { model.removalCandidate = nil } label: { Text("Keep It").frame(maxWidth: .infinity) }
                        .buttonStyle(.bordered).controlSize(.large).padFocus("remove.cancel")
                }
                .padding(.horizontal, 32)
                .padding(.bottom, 28)
                .frame(maxWidth: 480)
            }
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            #endif
        }
        #if os(macOS)
        .frame(minWidth: 460, minHeight: 320)
        #endif
        .presentationDetents([.medium])
    }
}
