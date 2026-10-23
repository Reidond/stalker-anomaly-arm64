// Home: Play, the number of enabled mods, Mods, Add mod. When the game data is missing it says so and how to fix it.
import SwiftUI

struct HomeView: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        GeometryReader { proxy in
            let wide = proxy.size.width > 760
            Group {
                if wide {
                    HStack(alignment: .center, spacing: 56) {
                        identity.frame(maxWidth: 380)
                        menu.frame(maxWidth: 560)
                    }
                } else {
                    VStack(spacing: 28) {
                        identity
                        menu
                    }
                }
            }
            .padding(.horizontal, Theme.gutter)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    private var identity: some View {
        VStack(spacing: 22) {
            LogoView(path: model.config.logoPath, size: 220)
            VStack(spacing: 6) {
                Text("ANOMALY")
                    .font(Theme.title(46))
                    .tracking(8)
                    .foregroundStyle(Theme.text)
                Text("S.T.A.L.K.E.R. · Mod launcher")
                    .font(Theme.small(16))
                    .tracking(1.5)
                    .foregroundStyle(Theme.dim)
                if let device = model.config.deviceLine, !device.isEmpty {
                    Text(device).font(Theme.mono(12)).foregroundStyle(Theme.dim.opacity(0.7)).padding(.top, 2)
                }
            }
        }
    }

    private var menu: some View {
        VStack(spacing: 14) {
            if !model.data.isOK { DataBanner() }
            ForEach(model.config.notices, id: \.self) { notice in
                Banner(style: .warning, title: notice)
            }
            if let error = model.libraryError {
                Banner(style: .error, title: "The mod library could not be opened", message: error)
            }
            ConsoleButton(id: "home.play", title: "Play", subtitle: playSubtitle, systemImage: "play.fill", prominent: true,
                          enabled: model.canPlay, isDefault: model.data.isOK) { model.play() }
            ConsoleButton(id: "home.mods", title: "Mods", subtitle: modsSubtitle, systemImage: "square.stack.3d.up.fill",
                          enabled: model.library != nil) { model.push(.mods) }
            ConsoleButton(id: "home.add", title: "Add mod", subtitle: "From Files, the browser or a link", systemImage: "plus.circle.fill",
                          enabled: model.library != nil) { model.push(.add) }
            if model.config.allowQuit {
                ConsoleButton(id: "home.quit", title: "Quit", systemImage: "power") { model.quit() }
            }
        }
    }

    private var playSubtitle: String {
        let enabled = model.enabledCount
        if !model.data.isOK { return "Needs the game data first" }
        return enabled == 0 ? "No mods enabled" : "\(Format.count(enabled, "mod")) enabled"
    }

    private var modsSubtitle: String {
        let total = model.mods.count
        if total == 0 { return "Nothing installed yet" }
        return "\(model.enabledCount) of \(total) enabled · load order, details"
    }
}

/// The shell's check of the game data: what is wrong and how to copy it in, with a Rescan.
struct DataBanner: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        VStack(spacing: 10) {
            Banner(style: model.data.state == "incomplete" ? .warning : .error,
                   title: model.data.summary.isEmpty ? "Game data not found" : model.data.summary,
                   message: model.data.help.isEmpty ? model.data.root : model.data.help,
                   monospaced: true)
            ConsoleButton(id: "home.rescan", title: "Rescan", subtitle: "Looks again; this also happens by itself every 2 s",
                          systemImage: "arrow.clockwise", isDefault: true) { model.rescan() }
        }
    }
}
