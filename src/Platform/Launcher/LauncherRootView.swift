// The launcher's root: the current screen, the hint bar, the cards on top of it, and the wiring of focus, file picker and toasts.
import SwiftUI
import UniformTypeIdentifiers

struct LauncherRootView: View {
    @ObservedObject var model: LauncherModel

    var body: some View {
        ZStack {
            LauncherBackground()
            VStack(spacing: 0) {
                screen
                    .id(model.screen)
                    .transition(.opacity)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                HintBar(hints: hints)
            }
            .allowsHitTesting(!model.modalActive)

            ImportOverlay(imports: model.imports)
            if let id = model.removalCandidate { RemoveConfirmation(id: id) }

            if let toast = model.toast {
                VStack {
                    Spacer()
                    ToastView(toast: toast).padding(.bottom, 64)
                }
                .transition(.move(edge: .bottom).combined(with: .opacity))
                .allowsHitTesting(false)
            }
            if model.launching {
                Color.black.opacity(0.55).ignoresSafeArea()
                    .overlay(ProgressView().controlSize(.large).tint(Theme.accent))
            }
        }
        .coordinateSpace(name: "launcher")
        .onPreferenceChange(FocusFramesKey.self) { frames in model.focus.update(frames: frames) }
        .environmentObject(model)
        .environmentObject(model.focus)
        .animation(.easeOut(duration: 0.2), value: model.imports.isActive)
        .animation(.easeOut(duration: 0.2), value: model.removalCandidate)
        .animation(.easeOut(duration: 0.2), value: model.toast)
        .fileImporter(isPresented: $model.showFilePicker, allowedContentTypes: ModFileTypes.importable, allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                if let url = urls.first { model.imports.begin(url: url, scoped: true) }
            case .failure(let error):
                model.show("Could not open the file: \(error.localizedDescription)", error: true)
            }
        }
        .preferredColorScheme(.dark)
        .tint(Theme.accent)
    }

    @ViewBuilder
    private var screen: some View {
        switch model.screen {
        case .home: HomeView()
        case .mods: ModsView()
        case .details(let id): DetailsView(id: id)
        case .add: AddModView()
        case .browse: BrowseView()
        }
    }

    private var hints: [Hint] {
        if model.modalActive {
            return [Hint(glyph: .cross, key: "↩", text: "Select"), Hint(glyph: .circle, key: "esc", text: "Back")]
        }
        switch model.screen {
        case .home:
            return [Hint(glyph: .dpad, key: "↑↓", text: "Move"), Hint(glyph: .cross, key: "↩", text: "Select"),
                    Hint(glyph: .options, key: "P", text: "Play")]
        case .mods:
            return [Hint(glyph: .dpad, key: "↑↓", text: "Move"), Hint(glyph: .cross, key: "↩", text: "Switch on/off"),
                    Hint(glyph: .triangle, key: "I", text: "Details"), Hint(glyph: .square, key: "⌥↑↓", text: "Hold and ↑↓: reorder"),
                    Hint(glyph: .circle, key: "esc", text: "Back")]
        case .browse:
            return [Hint(glyph: .circle, key: "esc", text: "Back")]
        default:
            return [Hint(glyph: .dpad, key: "↑↓", text: "Move"), Hint(glyph: .cross, key: "↩", text: "Select"),
                    Hint(glyph: .circle, key: "esc", text: "Back")]
        }
    }
}
