// Renders every launcher screen to a PNG without showing a window:
//
//   launcher_snapshot <out-dir> [--logo <png>] [--only <name>] [--dark]
//   launcher_snapshot --selftest <work-dir>     the pad logic: focus, reordering, sheets, what Play writes
//   launcher_snapshot --fixtures <mods-root>    four invented mods, for ANOMALY_MODS_ROOT
//
// The views are the ones the app uses (this tool compiles the launcher's sources), hosted in a window that is never shown and drawn
// into a bitmap, so the stock controls (lists, toggles, navigation) are the real macOS ones. On a made-up mods root.
import AppKit
import Foundation
import SwiftUI
import ModManagerCore

var arguments = Array(CommandLine.arguments.dropFirst())
var logoPath: String?
var only: String?
var outputDirectory: URL?
var selfTest = false
var makeFixtures = false
var dark = false
while !arguments.isEmpty {
    let argument = arguments.removeFirst()
    if argument == "--selftest" { selfTest = true }
    else if argument == "--fixtures" { makeFixtures = true }
    else if argument == "--dark" { dark = true }
    else if argument == "--logo", !arguments.isEmpty { logoPath = arguments.removeFirst() }
    else if argument == "--only", !arguments.isEmpty { only = arguments.removeFirst() }
    else { outputDirectory = URL(fileURLWithPath: argument) }
}
guard let outputDirectory else {
    FileHandle.standardError.write(Data("usage: launcher_snapshot <out-dir> [--logo <png>] [--only <name>] [--dark] | --selftest <work-dir> | --fixtures <mods-root>\n".utf8))
    exit(2)
}

@MainActor
func settle(_ seconds: Double = 0.35) {
    RunLoop.main.run(until: Date().addingTimeInterval(seconds))
}

/// Draws a view the way a window would, without ordering the window in.
@MainActor
func writePNG<V: View>(_ view: V, size: CGSize, to url: URL) {
    let host = NSHostingView(rootView: view.environment(\.launcherSnapshot, true))
    let window = NSWindow(contentRect: CGRect(origin: .zero, size: size), styleMask: [.titled], backing: .buffered, defer: false)
    window.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
    window.contentView = host
    host.frame = CGRect(origin: .zero, size: size)
    for _ in 0..<4 {
        settle(0.2)
        host.layoutSubtreeIfNeeded()
        host.displayIfNeeded()
    }
    guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { print("failed: \(url.lastPathComponent)"); return }
    host.cacheDisplay(in: host.bounds, to: rep)
    guard let data = rep.representation(using: .png, properties: [:]) else { print("failed: \(url.lastPathComponent)"); return }
    try? data.write(to: url)
    print("wrote \(url.path)")
}

@MainActor
func model(root: URL, data: DataStatus = DataStatus(state: "ok", summary: "Game data OK"), platform: String = "macos") -> LauncherModel {
    let config = LauncherConfig(modsRoot: root.path, downloadsDir: root.appendingPathComponent(".downloads").path, version: "0.1",
                                platform: platform, allowQuit: platform == "macos", logoPath: logoPath,
                                deviceLine: platform == "ios" ? "iPad15,5 · iPadOS 27.0" : "Mac15,3 · macOS 27.0", notices: [])
    return LauncherModel(config: config, data: data)
}

enum Frame {
    case window            // the whole launcher
    case sheet(AnyView)    // a sheet's content on its own
}

struct Shot {
    var name: String
    var size = CGSize(width: 1100, height: 760)
    var empty = false
    var pad = true
    var setup: (LauncherModel) -> Frame
}

/// A sheet's content on its own (the system draws the real sheet around it).
@MainActor
func sheet<V: View>(_ view: V, _ model: LauncherModel) -> Frame {
    .sheet(AnyView(view.environmentObject(model).environmentObject(model.focus)))
}

/// A fresh folder with a name nobody has used (mkdtemp). Nothing in this tool deletes a folder: they are in the temporary directory.
func makeTemporaryFolder(_ prefix: String, in parent: URL = FileManager.default.temporaryDirectory) throws -> URL {
    try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
    var template = Array((parent.path + "/" + prefix + "-XXXXXX").utf8CString)
    guard let made = mkdtemp(&template) else { throw CocoaError(.fileWriteUnknown) }
    return URL(fileURLWithPath: String(cString: made))
}

@MainActor
func run() throws {
    NSApplication.shared.setActivationPolicy(.prohibited)
    let work = try makeTemporaryFolder("launcher-snapshot")
    try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)

    let populated = work.appendingPathComponent("mods")
    try Fixtures.build(into: populated)
    let empty = work.appendingPathComponent("empty")
    try FileManager.default.createDirectory(at: empty, withIntermediateDirectories: true)

    let incomplete = DataStatus(state: "incomplete", summary: "Incomplete: 3 of 67 archives missing",
                                help: "Copy the rest of the game data into the Anomaly folder of this app:\n  Files app > On My iPad > Anomaly\nor push it from the Mac with tools/ios/push_gamedata.sh.",
                                root: "/var/mobile/Containers/Data/Application/…/Documents/Anomaly", filesOk: 64, filesExpected: 67)
    let sheetSize = CGSize(width: 640, height: 560)

    let shots: [Shot] = [
        Shot(name: "01-home", pad: false) { _ in .window },
        Shot(name: "02-home-pad") { _ in .window },
        Shot(name: "03-home-data-missing", pad: false) { m in m.setData(incomplete); return .window },
        Shot(name: "04-mods", pad: false) { m in m.push(.mods); return .window },
        Shot(name: "05-mods-pad-focus") { m in m.push(.mods); m.refreshFocus(); m.focus.focus("mod:sample-weather-pack"); return .window },
        Shot(name: "06-mods-reorder-held") { m in
            m.push(.mods); m.refreshFocus(); m.focus.focus("mod:sample-weather-pack"); m.focus.press(.square); return .window
        },
        Shot(name: "07-mod-details", pad: false) { m in m.push(.mods); m.push(.details("sample-weather-pack")); return .window },
        Shot(name: "08-add-mod", pad: false) { m in m.push(.add); return .window },
        Shot(name: "09-mods-empty", empty: true, pad: false) { m in m.push(.mods); return .window },
        Shot(name: "10-import-reading", size: sheetSize, pad: false) { m in
            m.imports.preview(.working(title: "Reading sample-pack.zip", detail: "Checking what is inside", progress: nil))
            return sheet(ImportSheet(imports: m.imports), m)
        },
        Shot(name: "11-import-warning", size: sheetSize, pad: false) { m in
            m.imports.preview(.review(ImportPlan(displayName: "Sample Total Overhaul", source: URL(fileURLWithPath: "/tmp/x"), fomod: nil,
                                                 warnings: ["Contains Windows DLLs (bin/xrAPI.dll): they do nothing here and are skipped.",
                                                            "Replaces 3 files of the game's own configs (gamedata/configs/system.ltx and 2 more)."])))
            return sheet(ImportSheet(imports: m.imports), m)
        },
        Shot(name: "12-import-installing", size: sheetSize, pad: false) { m in
            m.imports.preview(.working(title: "Installing", detail: "Sample Total Overhaul", progress: 0.62))
            return sheet(ImportSheet(imports: m.imports), m)
        },
        Shot(name: "13-import-failed", size: sheetSize, pad: false) { m in
            m.imports.preview(.failed(title: "Could not add the mod", message: "sample-pack.rar is damaged or not a rar archive (unexpected end of data at 41 MiB)."))
            return sheet(ImportSheet(imports: m.imports), m)
        },
        Shot(name: "14-remove-confirm", size: sheetSize, pad: false) { m in sheet(RemoveSheet(id: "sample-weather-pack"), m) },
    ]

    for shot in shots where only == nil || shot.name.contains(only!) {
        let m = model(root: shot.empty ? empty : populated)
        m.dataProvider = nil
        let frame = shot.setup(m)
        m.focus.device = shot.pad ? .pad : .pointer
        m.refreshFocus()
        settle(0.3)
        let url = outputDirectory.appendingPathComponent("\(shot.name).png")
        switch frame {
        case .window:
            writePNG(LauncherRootView(model: m), size: shot.size, to: url)
        case .sheet(let content):
            writePNG(content, size: shot.size, to: url)
        }
        m.stop()
    }
}

MainActor.assumeIsolated {
    NSApplication.shared.setActivationPolicy(.prohibited)
    do {
        if makeFixtures {
            // a made-up mods root (four invented mods, one of them off) for runs of the app: ANOMALY_MODS_ROOT=<dir>
            try Fixtures.build(into: outputDirectory)
            print("fixtures in \(outputDirectory.path)")
            exit(0)
        }
        if selfTest {
            try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
            exit(try SelfTest.run(root: outputDirectory, logo: logoPath))
        }
        try run()
    } catch { print("error: \(error)"); exit(1) }
}
