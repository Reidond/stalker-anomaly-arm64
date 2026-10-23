// Renders every launcher screen to a PNG with ImageRenderer, without a window (nothing appears on the screen):
//
//   launcher_snapshot <out-dir> [--logo <png>] [--only <name>]
//
// The views are the ones the app uses (this tool compiles the launcher's sources), on a made-up mods root. The focus ring, the held
// Square of the reorder mode and the modal cards are set up directly on the model, as a pad would.
import AppKit
import Foundation
import ImageIO
import SwiftUI
import UniformTypeIdentifiers
#if canImport(ModManagerCore)
import ModManagerCore
#endif

var arguments = Array(CommandLine.arguments.dropFirst())
var logoPath: String?
var only: String?
var outputDirectory: URL?
var selfTest = false
while !arguments.isEmpty {
    let argument = arguments.removeFirst()
    if argument == "--selftest" { selfTest = true }
    else if argument == "--logo", !arguments.isEmpty { logoPath = arguments.removeFirst() }
    else if argument == "--only", !arguments.isEmpty { only = arguments.removeFirst() }
    else { outputDirectory = URL(fileURLWithPath: argument) }
}
guard let outputDirectory else {
    FileHandle.standardError.write(Data("usage: launcher_snapshot <out-dir> [--logo <png>] [--only <name>] | --selftest <work-dir>\n".utf8))
    exit(2)
}

@MainActor
func settle() {
    RunLoop.main.run(until: Date().addingTimeInterval(0.25))
}

@MainActor
func writePNG(_ view: some View, size: CGSize, to url: URL) {
    let renderer = ImageRenderer(content: view.frame(width: size.width, height: size.height))
    renderer.scale = 1
    guard let image = renderer.cgImage,
          let destination = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) else {
        print("failed: \(url.lastPathComponent)")
        return
    }
    CGImageDestinationAddImage(destination, image, nil)
    CGImageDestinationFinalize(destination)
    print("wrote \(url.path)")
}

@MainActor
func model(root: URL, data: DataStatus = DataStatus(state: "ok", summary: "Game data OK"), platform: String = "ios") -> LauncherModel {
    let config = LauncherConfig(modsRoot: root.path, downloadsDir: root.appendingPathComponent(".downloads").path, version: "0.1",
                                platform: platform, allowQuit: platform == "macos", logoPath: logoPath,
                                deviceLine: platform == "ios" ? "iPad15,5 · iPadOS 27.0" : "Mac15,3 · macOS 27.0", notices: [])
    let model = LauncherModel(config: config, data: data)
    model.focus.synchronousUpdates = true
    model.focus.device = platform == "ios" ? .pad : .keyboard
    return model
}

struct Shot {
    var name: String
    var size = CGSize(width: 1366, height: 1024)
    var setup: (LauncherModel) -> Void
}

@MainActor
func run() throws {
    NSApplication.shared.setActivationPolicy(.prohibited)
    let work = FileManager.default.temporaryDirectory.appendingPathComponent("launcher-snapshot-\(ProcessInfo.processInfo.processIdentifier)")
    defer { try? FileManager.default.removeItem(at: work) }
    try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)

    let populated = work.appendingPathComponent("mods")
    try Fixtures.build(into: populated)
    // the weather pack and the HUD tweaks both carry ui_hud.xml: a conflict, the later one wins
    let empty = work.appendingPathComponent("empty")
    try FileManager.default.createDirectory(at: empty, withIntermediateDirectories: true)

    let incomplete = DataStatus(state: "incomplete", summary: "Incomplete: 3 of 67 archives missing",
                                help: "Copy the rest of the game data into the Anomaly folder of this app:\n  Files app > On My iPad > Anomaly\nor push it from the Mac with tools/ios/push_gamedata.sh.",
                                root: "/var/mobile/Containers/Data/Application/…/Documents/Anomaly", filesOk: 64, filesExpected: 67)
    let macSize = CGSize(width: 1180, height: 760)

    let shots: [Shot] = [
        Shot(name: "01-home") { _ in },
        Shot(name: "02-home-data-missing") { m in m.setData(incomplete) },
        Shot(name: "03-mods") { m in m.push(.mods); m.focus.focus("mod:sample-weather-pack") },
        Shot(name: "04-mods-reorder") { m in
            m.push(.mods); m.focus.focus("mod:sample-weather-pack"); m.focus.press(.square)
        },
        Shot(name: "05-mod-details") { m in m.push(.mods); m.push(.details("sample-weather-pack")) },
        Shot(name: "06-add-mod") { m in m.push(.add) },
        Shot(name: "07-import-reading") { m in m.push(.add); m.imports.preview(.working(title: "Reading sample-pack.zip", detail: "Checking what is inside", progress: nil)) },
        Shot(name: "08-import-warning") { m in
            m.push(.add)
            m.imports.preview(.review(ImportPlan(displayName: "Sample Total Overhaul", source: URL(fileURLWithPath: "/tmp/x"), fomod: nil,
                                                 warnings: ["Contains Windows DLLs (bin/xrAPI.dll): they do nothing here and are skipped.",
                                                            "Replaces 3 files of the game's own configs (gamedata/configs/system.ltx and 2 more)."])))
        },
        Shot(name: "09-import-installing") { m in m.push(.add); m.imports.preview(.working(title: "Installing", detail: "Sample Total Overhaul", progress: 0.62)) },
        Shot(name: "10-import-failed") { m in
            m.push(.add)
            m.imports.preview(.failed(title: "Could not add the mod", message: "sample-pack.rar is damaged or not a rar archive (unexpected end of data at 41 MiB)."))
        },
        Shot(name: "11-remove-confirm") { m in m.push(.mods); m.focus.focus("mod:sample-weather-pack"); m.removalCandidate = "sample-weather-pack" },
        Shot(name: "12-mods-empty", setup: { m in m.push(.mods) }),
        Shot(name: "13-home-mac", size: macSize) { _ in },
        Shot(name: "14-mods-mac", size: macSize) { m in m.push(.mods); m.focus.focus("mod:sample-hud-tweaks") },
    ]

    for shot in shots where only == nil || shot.name.contains(only!) {
        let empty_root = shot.name == "12-mods-empty"
        let mac = shot.name.hasSuffix("-mac")
        let m = model(root: empty_root ? empty : populated, platform: mac ? "macos" : "ios")
        m.dataProvider = nil
        shot.setup(m)
        m.focus.activeScope = (m.modalActive || shot.name.contains("import") || shot.name.contains("confirm")) ? "modal" : "main"
        settle()
        if m.focus.focusedID == nil || (m.focus.activeScope == "modal") { m.focus.focusDefault(scope: m.focus.activeScope) }
        let view = LauncherRootView(model: m).environment(\.launcherSnapshot, true)
        writePNG(view, size: shot.size, to: outputDirectory.appendingPathComponent("\(shot.name).png"))
        m.stop()
    }
}

MainActor.assumeIsolated {
    NSApplication.shared.setActivationPolicy(.prohibited)
    do {
        if selfTest {
            try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
            exit(try SelfTest.run(root: outputDirectory, logo: logoPath))
        }
        try run()
    } catch { print("error: \(error)"); exit(1) }
}
