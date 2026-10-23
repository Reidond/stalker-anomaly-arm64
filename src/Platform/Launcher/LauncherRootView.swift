// The launcher's root: the Zone's backdrop, the current screen on it, the hint bar for pad and keyboard, the sheets, and the wiring
// of the file picker and toasts. Screens change with a soft spring (a fade and a settle of 10 pt: nothing slides across the glass);
// Reduce Motion keeps the fade.
import SwiftUI
import UniformTypeIdentifiers

struct LauncherRootView: View {
    @ObservedObject var model: LauncherModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        let motion = LauncherMotion(reduced: reduceMotion)
        ZStack {
            LauncherBackdrop(path: model.config.backdropPath)

            ZStack {
                screen
                    .id(model.screen)
                    .transition(.asymmetric(
                        insertion: reduceMotion ? .opacity : .opacity.combined(with: .offset(y: 10)),
                        removal: .opacity))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            .animation(motion.screen, value: model.screen)

            VStack(spacing: 10) {
                Spacer()
                if let toast = model.toast {
                    ToastView(toast: toast)
                        .transition(reduceMotion ? .opacity : .move(edge: .bottom).combined(with: .opacity))
                }
                HintBar(hints: hints)
            }
            .animation(motion.content, value: model.toast)
            .animation(motion.content, value: model.focus.device)
            .allowsHitTesting(false)
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
        // a touch or a click is the pointer again: the pad's focus ring and the hints go away
        .simultaneousGesture(TapGesture().onEnded { model.focus.device = .pointer })
        .preferredColorScheme(.dark)
        .tint(Theme.amber)
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
