// Home: Play, the number of enabled mods, Mods, Add Mod. Glass over the Zone; when the game data is missing it says so and how to fix it.
import SwiftUI

struct HomeView: View {
    @EnvironmentObject private var model: LauncherModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.launcherSnapshot) private var snapshot
    @Namespace private var glass
    @State private var entered = false

    private var shown: Bool { entered || model.homeShown || snapshot }

    var body: some View {
        GeometryReader { proxy in
            let wide = proxy.size.width > 820
            ScrollView {
                Group {
                    if wide {
                        HStack(alignment: .center, spacing: 72) {
                            identity.frame(maxWidth: 360)
                            menu.frame(maxWidth: 520)
                        }
                    } else {
                        VStack(spacing: 36) {
                            identity
                            menu.frame(maxWidth: 520)
                        }
                    }
                }
                .padding(.horizontal, 40)
                .padding(.vertical, 32)
                .frame(maxWidth: .infinity, minHeight: proxy.size.height)
            }
            .scrollBounceBehavior(.basedOnSize)
        }
        .onAppear {
            entered = true
            model.homeShown = true
        }
    }

    // MARK: identity

    private var identity: some View {
        VStack(spacing: 20) {
            LogoView(path: model.config.logoPath, size: 148)
            VStack(spacing: 6) {
                Text("ANOMALY").font(.system(size: 44, weight: .heavy)).tracking(7).foregroundStyle(Theme.primaryText)
                    .shadow(color: .black.opacity(0.4), radius: 8, y: 2)
                Text("S.T.A.L.K.E.R. · Mod launcher").font(.callout.weight(.medium)).tracking(0.8).foregroundStyle(Theme.secondaryText)
            }
        }
        .modifier(Enter(index: 0, shown: shown))
    }

    // MARK: menu

    private var menu: some View {
        GlassGroup(spacing: 14) {
            VStack(spacing: 14) {
                if !model.data.isOK { dataCard.modifier(Enter(index: 1, shown: shown)) }
                ForEach(model.config.notices, id: \.self) { notice in
                    Label(notice, systemImage: "exclamationmark.triangle.fill")
                        .symbolRenderingMode(.palette)
                        .foregroundStyle(Theme.primaryText, Theme.amber)
                        .font(.callout.weight(.medium))
                        .padding(Theme.rowPadding + 2)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .glassSurface(radius: Theme.rowRadius, tint: .rust)
                }
                if let error = model.libraryError {
                    Label("The mod library could not be opened: \(error)", systemImage: "xmark.octagon.fill")
                        .font(.callout)
                        .foregroundStyle(Theme.primaryText)
                        .padding(Theme.rowPadding + 2)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .glassSurface(radius: Theme.rowRadius, tint: .rust)
                }
                playButton.modifier(Enter(index: 1, shown: shown))
                MenuRow(title: "Mods", subtitle: modsSubtitle, systemImage: "square.stack.3d.up.fill", value: modsValue) { model.push(.mods) }
                    .disabled(model.library == nil)
                    .padFocus("home.mods")
                    .modifier(Enter(index: 2, shown: shown))
                MenuRow(title: "Add Mod", subtitle: "From Files, the browser or a link", systemImage: "plus.circle.fill") { model.push(.add) }
                    .disabled(model.library == nil)
                    .padFocus("home.add")
                    .modifier(Enter(index: 3, shown: shown))
                if model.config.allowQuit {
                    MenuRow(title: "Quit", systemImage: "power", chevron: false, tint: Theme.rust) { model.quit() }
                        .padFocus("home.quit")
                        .modifier(Enter(index: 4, shown: shown))
                }
            }
            .animation(LauncherMotion(reduced: reduceMotion).content, value: model.launching)
        }
    }

    /// Play: the one prominent control. Pressed, it morphs into "Starting" in the same place, so that the player sees the game coming.
    @ViewBuilder
    private var playButton: some View {
        VStack(spacing: 8) {
            if model.launching {
                HStack(spacing: 12) {
                    ProgressView().controlSize(.regular)
                    Text("Starting").font(.title2.weight(.bold))
                }
                .foregroundStyle(Theme.primaryText)
                .frame(maxWidth: .infinity, minHeight: 76)
                .glassCapsule(tint: .amber)
                .glassMorph("play", in: glass)
            } else {
                Button { model.play() } label: {
                    Label("Play", systemImage: "play.fill")
                        .font(.title2.weight(.bold))
                        .frame(maxWidth: .infinity, minHeight: 76)
                }
                .prominentButton()
                .tint(Theme.amber)
                .disabled(!model.canPlay)
                .glassMorph("play", in: glass)
                .padFocus("home.play", radius: 38, ring: .white)
            }
            Text(playCaption).font(.callout).foregroundStyle(Theme.secondaryText).monospacedDigit()
        }
    }

    private var dataCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(model.data.summary.isEmpty ? "Game data not found" : model.data.summary,
                  systemImage: model.data.state == "incomplete" ? "exclamationmark.triangle.fill" : "xmark.octagon.fill")
                .symbolRenderingMode(.palette)
                .foregroundStyle(Theme.primaryText, Theme.amber)
                .font(.headline)
            Text(model.data.help.isEmpty ? model.data.root : model.data.help + "\n\nIt looks again by itself every 2 seconds.")
                .font(.system(.footnote, design: .monospaced))
                .foregroundStyle(Theme.secondaryText)
                .fixedSize(horizontal: false, vertical: true)
            Button { model.rescan() } label: { Label("Rescan", systemImage: "arrow.clockwise").frame(minHeight: 40) }
                .glassButton()
                .padFocus("home.rescan", radius: 22)
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .glassSurface(radius: Theme.cardRadius, tint: .rust)
    }

    private var playCaption: String {
        if !model.data.isOK { return "Play needs the game data first." }
        let enabled = model.enabledCount
        return enabled == 0 ? "No mods are on." : "\(Format.count(enabled, "mod")) on."
    }

    private var modsSubtitle: String {
        model.mods.isEmpty ? "Nothing installed yet" : "Load order, details, conflicts"
    }

    private var modsValue: String? {
        model.mods.isEmpty ? nil : "\(model.enabledCount) of \(model.mods.count) on"
    }
}

/// The staggered entrance of Home, played once per launch: each piece fades in and settles from 12 pt below, 50 ms after the one before.
struct Enter: ViewModifier {
    let index: Int
    let shown: Bool
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    func body(content: Content) -> some View {
        content
            .opacity(shown ? 1 : 0)
            .offset(y: shown || reduceMotion ? 0 : 12)
            .animation(reduceMotion ? .easeOut(duration: 0.12) : .spring(duration: 0.4, bounce: 0).delay(Double(index) * 0.05), value: shown)
    }
}
