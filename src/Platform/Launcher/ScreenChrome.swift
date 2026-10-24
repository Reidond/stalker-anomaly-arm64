// The title of a pushed screen: the navigation bar on iPadOS (large title, back button, toolbar items), and a header with a back
// button on the Mac, where the launcher lives in a plain window with no toolbar of its own.
import SwiftUI

struct ScreenChrome<Trailing: View>: ViewModifier {
    @EnvironmentObject private var model: LauncherModel
    let title: String
    var inline = false
    @ViewBuilder var trailing: () -> Trailing

    func body(content: Content) -> some View {
        #if os(macOS)
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Button { model.pop() } label: { Label("Back", systemImage: "chevron.backward") }
                    .keyboardShortcut(.cancelAction)
                Text(title).font(.title2.weight(.bold)).lineLimit(1)
                Spacer()
                trailing()
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 12)
            Divider()
            content
        }
        #else
        content
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(inline ? .inline : .large)
            .toolbar { ToolbarItem(placement: .primaryAction) { trailing() } }
        #endif
    }
}

extension View {
    func launcherScreen<Trailing: View>(_ title: String, inline: Bool = false, @ViewBuilder trailing: @escaping () -> Trailing) -> some View {
        modifier(ScreenChrome(title: title, inline: inline, trailing: trailing))
    }

    func launcherScreen(_ title: String, inline: Bool = false) -> some View {
        modifier(ScreenChrome(title: title, inline: inline, trailing: { EmptyView() }))
    }
}
