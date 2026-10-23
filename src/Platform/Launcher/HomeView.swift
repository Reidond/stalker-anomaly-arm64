// Home: Play, the number of enabled mods, Mods, Add mod. When the game data is missing it says so and how to fix it.
import SwiftUI

struct HomeView: View {
    @EnvironmentObject private var model: LauncherModel

    var body: some View {
        LauncherForm {
            Section {
                hero
                    .frame(maxWidth: .infinity)
                    .listRowBackground(Color.clear)
                    .listRowSeparator(.hidden)
            }

            if !model.data.isOK { dataSection }
            ForEach(model.config.notices, id: \.self) { notice in
                Section { Label(notice, systemImage: "exclamationmark.triangle.fill").symbolRenderingMode(.multicolor) }
            }
            if let error = model.libraryError {
                Section {
                    Label("The mod library could not be opened", systemImage: "xmark.octagon.fill").foregroundStyle(.red)
                } footer: { Text(error) }
            }

            Section {
                Button { model.play() } label: {
                    Label("Play", systemImage: "play.fill")
                        .font(.title3.weight(.semibold))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 8)
                }
                .prominentButton()
                .controlSize(.large)
                .disabled(!model.canPlay)
                .padFocus("home.play")
                .listRowBackground(Color.clear)
                .listRowInsets(EdgeInsets())
            } footer: {
                Text(playCaption).frame(maxWidth: .infinity)
            }

            Section {
                NavigationLink(value: Screen.mods) {
                    HStack {
                        Label("Mods", systemImage: "square.stack.3d.up.fill")
                        Spacer()
                        Text(modsDetail).foregroundStyle(.secondary)
                    }
                }
                .disabled(model.library == nil)
                .padFocus("home.mods")
                NavigationLink(value: Screen.add) {
                    Label("Add Mod", systemImage: "plus.circle.fill")
                }
                .disabled(model.library == nil)
                .padFocus("home.add")
            }

            if model.config.allowQuit {
                Section {
                    Button(role: .destructive) { model.quit() } label: { Label("Quit", systemImage: "power") }
                        .padFocus("home.quit")
                }
            }
        }
        #if os(iOS)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar(.hidden, for: .navigationBar)
        #endif
    }

    private var hero: some View {
        VStack(spacing: 14) {
            LogoView(path: model.config.logoPath, size: 112)
            VStack(spacing: 4) {
                Text("Anomaly").font(.largeTitle.weight(.bold))
                Text("S.T.A.L.K.E.R. · Mod launcher").font(.subheadline).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 12)
    }

    private var dataSection: some View {
        Section {
            Label(model.data.summary.isEmpty ? "Game data not found" : model.data.summary,
                  systemImage: model.data.state == "incomplete" ? "exclamationmark.triangle.fill" : "xmark.octagon.fill")
                .symbolRenderingMode(.multicolor)
                .font(.headline)
            Button { model.rescan() } label: { Label("Rescan", systemImage: "arrow.clockwise") }
                .padFocus("home.rescan")
        } header: {
            Text("Game data")
        } footer: {
            Text(model.data.help.isEmpty ? model.data.root : model.data.help + "\n\nIt looks again by itself every 2 seconds.")
        }
    }

    private var playCaption: String {
        if !model.data.isOK { return "Play needs the game data first." }
        let enabled = model.enabledCount
        return enabled == 0 ? "No mods are on." : "\(Format.count(enabled, "mod")) on."
    }

    private var modsDetail: String {
        let total = model.mods.count
        return total == 0 ? "None" : "\(model.enabledCount) of \(total) on"
    }
}
