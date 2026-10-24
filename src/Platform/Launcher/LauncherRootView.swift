// The launcher's root: a NavigationStack over home, the hint bar for pad and keyboard, the sheets, and the wiring of the file
// picker and toasts.
import SwiftUI
import UniformTypeIdentifiers

struct LauncherRootView: View {
    @ObservedObject var model: LauncherModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        NavigationStack(path: $model.path) {
            HomeView()
                .navigationDestination(for: Screen.self) { screen in
                    switch screen {
                    case .home: HomeView()
                    case .mods: ModsView()
                    case .details(let id): DetailsView(id: id)
                    case .add: AddModView()
                    case .browse: BrowseView()
                    }
                }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) { HintBar(hints: hints) }
        .overlay(alignment: .bottom) {
            if let toast = model.toast {
                ToastView(toast: toast)
                    .padding(.bottom, 72)
                    .transition(reduceMotion ? .opacity : .move(edge: .bottom).combined(with: .opacity))
                    .allowsHitTesting(false)
            }
        }
        .overlay {
            if model.launching {
                ZStack {
                    Color.black.opacity(0.25)
                    ProgressView().controlSize(.large).padding(24).background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
                }
                .ignoresSafeArea()
            }
        }
        .sheet(isPresented: Binding(get: { model.imports.isActive }, set: { if !$0 { model.imports.back() } })) {
            ImportSheet(imports: model.imports)
                .environmentObject(model)
                .environmentObject(model.focus)
        }
        .sheet(isPresented: Binding(get: { model.removalCandidate != nil }, set: { if !$0 { model.removalCandidate = nil } })) {
            if let id = model.removalCandidate {
                RemoveSheet(id: id)
                    .environmentObject(model)
                    .environmentObject(model.focus)
            }
        }
        .fileImporter(isPresented: $model.showFilePicker, allowedContentTypes: ModFileTypes.importable, allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                if let url = urls.first { model.imports.begin(url: url, scoped: true) }
            case .failure(let error):
                model.show("Could not open the file: \(error.localizedDescription)", error: true)
            }
        }
        .environmentObject(model)
        .environmentObject(model.focus)
        .animation(.easeOut(duration: 0.2), value: model.toast)
        .animation(.easeOut(duration: 0.2), value: model.focus.device)
        // a touch or a click is the pointer again: the pad's focus ring and the hints go away
        .simultaneousGesture(TapGesture().onEnded { model.focus.device = .pointer })
        .tint(Theme.accent)
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
            return [Hint(glyph: .dpad, key: "↑↓", text: "Move"), Hint(glyph: .cross, key: "↩", text: "Switch On/Off"),
                    Hint(glyph: .triangle, key: "I", text: "Details"), Hint(glyph: .square, key: "⌥↑↓", text: "Hold + D-pad: Reorder"),
                    Hint(glyph: .circle, key: "esc", text: "Back")]
        case .browse:
            return [Hint(glyph: .circle, key: "esc", text: "Back")]
        default:
            return [Hint(glyph: .dpad, key: "↑↓", text: "Move"), Hint(glyph: .cross, key: "↩", text: "Select"),
                    Hint(glyph: .circle, key: "esc", text: "Back")]
        }
    }
}
