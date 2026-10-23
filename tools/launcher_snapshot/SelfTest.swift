// `launcher_snapshot --selftest`: drives the launcher with pad presses (the same calls the app shell's SDL events end up in), renders
// between the steps so the views report their frames, and checks where the focus went, what the mod list looks like and what Play
// wrote. No window is created.
import AppKit
import Foundation
import SwiftUI
#if canImport(ModManagerCore)
import ModManagerCore
#endif

@MainActor
enum SelfTest {
    static var failures = 0

    static func check(_ condition: Bool, _ message: String) {
        print("\(condition ? "ok  " : "FAIL") \(message)")
        if !condition { failures += 1 }
    }

    static func render(_ model: LauncherModel) {
        RunLoop.main.run(until: Date().addingTimeInterval(0.12))
        let renderer = ImageRenderer(content: LauncherRootView(model: model).environment(\.launcherSnapshot, true).frame(width: 1366, height: 1024))
        renderer.scale = 1
        _ = renderer.cgImage
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        _ = renderer.cgImage
    }

    static func press(_ model: LauncherModel, _ button: PadButton, hold: Bool = false) {
        model.focus.press(button)
        render(model)
        if !hold { model.focus.release(button) }
    }

    static func ids(_ model: LauncherModel) -> [String] { model.mods.map(\.id) }

    static func run(root: URL, logo: String?) throws -> Int32 {
        let mods = root.appendingPathComponent("mods")
        try Fixtures.build(into: mods)
        let config = LauncherConfig(modsRoot: mods.path, downloadsDir: nil, version: "test", platform: "ios", allowQuit: false, logoPath: logo,
                                    deviceLine: nil, notices: [])
        let model = LauncherModel(config: config, data: DataStatus(state: "ok"))
        model.focus.synchronousUpdates = true
        model.focus.device = .pad
        render(model)

        check(model.focus.focusedID == "home.play", "home: Play has the focus at start (\(model.focus.focusedID ?? "nil"))")
        press(model, .down)
        check(model.focus.focusedID == "home.mods", "home: D-pad down goes to Mods (\(model.focus.focusedID ?? "nil"))")
        press(model, .down)
        check(model.focus.focusedID == "home.add", "home: down again goes to Add mod (\(model.focus.focusedID ?? "nil"))")
        press(model, .up)
        press(model, .confirm)
        render(model)
        check(model.screen == .mods, "Cross on Mods opens the list (\(model.screen))")
        check(model.focus.focusedID == "mod:sample-hud-tweaks", "mods: the first mod has the focus (\(model.focus.focusedID ?? "nil"))")

        press(model, .down)
        check(model.focus.focusedID == "mod:sample-weather-pack", "mods: down goes to the second mod (\(model.focus.focusedID ?? "nil"))")
        let was = model.mods.first { $0.id == "sample-weather-pack" }!.enabled
        press(model, .confirm)
        check(model.mods.first { $0.id == "sample-weather-pack" }!.enabled != was, "mods: Cross switches the mod")
        press(model, .confirm)
        check(model.mods.first { $0.id == "sample-weather-pack" }!.enabled == was, "mods: Cross again switches it back")

        press(model, .square, hold: true)
        press(model, .down)
        press(model, .down)
        model.focus.release(.square)
        check(ids(model) == ["sample-hud-tweaks", "sample-reload-sounds", "sample-trader-prices", "sample-weather-pack"],
              "mods: Square held + down twice moves the mod to the end (\(ids(model)))")
        check(model.focus.focusedID == "mod:sample-weather-pack", "mods: the focus follows the moved mod")
        press(model, .square, hold: true)
        press(model, .up)
        model.focus.release(.square)
        check(ids(model) == ["sample-hud-tweaks", "sample-reload-sounds", "sample-weather-pack", "sample-trader-prices"],
              "mods: Square held + up moves it back one (\(ids(model)))")
        let modlist = try Data(contentsOf: mods.appendingPathComponent("modlist.json"))
        check(String(decoding: modlist, as: UTF8.self).contains("sample-weather-pack"), "mods: modlist.json was written by the move")

        press(model, .triangle)
        render(model)
        check(model.screen == .details("sample-weather-pack"), "Triangle opens the details (\(model.screen))")
        press(model, .back)
        render(model)
        check(model.screen == .mods, "Circle goes back to the list")
        check(model.focus.focusedID == "mod:sample-weather-pack", "mods: the focus is back on the mod that was open (\(model.focus.focusedID ?? "nil"))")

        // remove, through the confirmation card
        press(model, .triangle)
        render(model)
        model.focus.focus("details.remove")
        press(model, .confirm)
        render(model)
        check(model.removalCandidate == "sample-weather-pack", "details: Remove asks first")
        check(model.focus.focusedID == "remove.cancel", "remove card: the safe button has the focus (\(model.focus.focusedID ?? "nil"))")
        press(model, .back)
        render(model)
        check(model.removalCandidate == nil && !ids(model).isEmpty, "remove card: Circle cancels")
        press(model, .confirm) // focus is back on the Remove button
        render(model)
        press(model, .up)
        check(model.focus.focusedID == "remove.confirm", "remove card: up goes to Remove (\(model.focus.focusedID ?? "nil"))")
        press(model, .confirm)
        render(model)
        check(!ids(model).contains("sample-weather-pack"), "remove card: Cross removes the mod (\(ids(model)))")
        check(model.screen == .mods, "after removing from the details the list is shown (\(model.screen))")

        press(model, .back)
        render(model)
        check(model.screen == .home, "Circle goes back to home")
        check(model.focus.focusedID == "home.mods", "home: the focus is back on Mods (\(model.focus.focusedID ?? "nil"))")

        // an import with a warning: Cancel has the focus, Circle cancels, nothing is added
        let risky = root.appendingPathComponent("risky-mod")
        try FileManager.default.createDirectory(at: risky.appendingPathComponent("bin"), withIntermediateDirectories: true)
        try Data("x".utf8).write(to: risky.appendingPathComponent("bin/helper.dll"))
        let before = ids(model)
        model.imports.begin(url: risky)
        RunLoop.main.run(until: Date().addingTimeInterval(0.4))
        render(model)
        var isReview = false
        if case .review = model.imports.stage { isReview = true }
        check(isReview, "import: a folder with a DLL stops at the warning")
        check(model.focus.activeScope == "modal" && model.focus.focusedID == "import.cancel", "import: the card has the focus on Cancel (\(model.focus.focusedID ?? "nil"))")
        press(model, .back)
        render(model)
        check(!model.imports.isActive && ids(model) == before, "import: Circle cancels and adds nothing")
        check(model.focus.activeScope == "main" && model.focus.focusedID == "home.mods", "import: the focus is back on Mods (\(model.focus.focusedID ?? "nil"))")

        press(model, .options)
        render(model)
        check(model.result == .play, "Options plays")
        let final = try JSONSerialization.jsonObject(with: Data(contentsOf: mods.appendingPathComponent("modlist.json"))) as? [String: Any]
        let listed = (final?["mods"] as? [[String: Any]])?.compactMap { $0["id"] as? String } ?? []
        check(listed == ids(model) && !listed.isEmpty, "Play: modlist.json lists the mods in the shown order (\(listed))")

        model.stop()
        print(failures == 0 ? "selftest passed" : "selftest: \(failures) failed")
        return failures == 0 ? 0 : 1
    }
}
