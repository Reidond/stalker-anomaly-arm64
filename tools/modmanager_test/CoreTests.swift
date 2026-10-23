import Foundation
@testable import ModManagerCore

// Naming, path rules, layout detection and the library (modlist.json, mod.json, conflicts).

/// Writes `<root>/<id>/mod.json` and `content/...` the way the importer would.
@discardableResult
func makeInstalledMod(root: URL, id: String, name: String? = nil, files: [String: String] = [:], installedAt: Date = Date()) throws -> URL {
    let folder = root.appendingPathComponent(id, isDirectory: true)
    try FileManager.default.createDirectory(at: folder.appendingPathComponent("content"), withIntermediateDirectories: true)
    for (path, text) in files {
        let url = folder.appendingPathComponent("content").appendingPathComponent(path)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(text.utf8).write(to: url)
    }
    let manifest = ModManifest(id: id, name: name ?? id, version: nil, author: nil, description: nil, source: ModOrigin(),
                               installedAt: installedAt, fomodChoices: nil, fileCount: files.count, sizeBytes: 0)
    try ModManifest.encoder().encode(manifest).write(to: folder.appendingPathComponent("mod.json"))
    return folder
}

func entries(_ paths: [String]) -> [SourceEntry] {
    paths.enumerated().map { i, p in SourceEntry(index: i, path: p, size: 10, isDirectory: p.hasSuffix("/")) }
        .map { e in e.isDirectory ? SourceEntry(index: e.index, path: String(e.path.dropLast()), size: 0, isDirectory: true) : e }
}

func registerCoreTests() {
    test("naming: file name to name and version") {
        let cases: [(String, String, String?)] = [
            ("Arszi Reshade Preset v2.3.zip", "Arszi Reshade Preset", "2.3"),
            ("Weapon_Pack-1.4.2.7z", "Weapon Pack", "1.4.2"),
            ("Cool Mod v1.2.zip", "Cool Mod", "1.2"),
            ("Beefs NVG-1234-1-5-1678123456.zip", "Beefs NVG", "1.5"),
            ("Mod Name (1).rar", "Mod Name", nil),
            ("plain.zip", "plain", nil),
            ("Sunrise_v12.7z", "Sunrise", "12"),
            ("Better-Trade-Hotfix.zip", "Better Trade Hotfix", nil),
        ]
        for (file, name, version) in cases {
            let r = ModNaming.nameAndVersion(fromFileName: file)
            try expectEqual(r.name, name, file)
            try expectEqual(r.version, version, file + " version")
        }
    }

    test("naming: slugs are lower-case ascii and unique") {
        try expectEqual(ModNaming.slug("Cool Mod: Rebalance!"), "cool-mod-rebalance")
        try expectEqual(ModNaming.slug("Зона Отчуждения"), "zona-otcuzdenia")
        try expectEqual(ModNaming.slug("   "), "mod")
        try expectEqual(ModNaming.slug("Crème Brûlée"), "creme-brulee")
        try expect(ModNaming.slug(String(repeating: "abc ", count: 40)).count <= 48)
        let taken: Set<String> = ["a", "a-2"]
        try expectEqual(ModNaming.unique("a") { taken.contains($0) }, "a-3")
        try expectEqual(ModNaming.unique("b") { taken.contains($0) }, "b")
    }

    test("paths: sanitizing refuses anything that leaves the root") {
        try expectEqual(try ModPath.sanitize("a/b\\c/./d"), "a/b/c/d")
        try expectEqual(try ModPath.sanitize("./x"), "x")
        try expectEqual(try ModPath.sanitize("dir//file"), "dir/file")
        try expectEqual(try ModPath.sanitize("./"), nil)
        for bad in ["../x", "a/../../x", "/etc/passwd", "C:/Windows/x", "a\\..\\..\\x", "..", "a/b/..", "x\u{0}y", "x\ny"] {
            try await expectError(bad, matches: { if case ModImportError.unsafePath = $0 { return true } else { return false } }) {
                _ = try ModPath.sanitize(bad)
            }
        }
        try expect(ModPath.isUnder("Gamedata/Scripts/a.script", prefix: "gamedata"))
        try expect(!ModPath.isUnder("gamedata2/x", prefix: "gamedata"))
        try expectEqual(ModPath.relative("Mod/GameData/x.txt", to: "mod/gamedata"), "x.txt")
        try expect(ModPath.isJunk("__MACOSX/a/._b"))
        try expect(ModPath.isJunk("gamedata/.DS_Store"))
        try expect(!ModPath.isJunk("gamedata/scripts/a.script"))
    }

    test("layout: gamedata found under one or two wrapper folders") {
        for paths in [
            ["gamedata/scripts/a.script"],
            ["Mod/gamedata/scripts/a.script", "Mod/readme.txt"],
            ["Mod v1.2/Mod/gamedata/scripts/a.script", "Mod v1.2/readme.txt"],
            ["A/B/C/GameData/scripts/a.script"],
        ] {
            guard case .single(let option) = ContentLayoutAnalyzer.analyze(entries(paths)) else {
                throw TestFailure(message: "no single root for \(paths)")
            }
            try expectEqual(option.destination, "")
            let expected = paths[0].components(separatedBy: "/").dropLast(3).joined(separator: "/")
            try expectEqual(option.prefix.lowercased(), expected.lowercased(), "\(paths)")
        }
    }

    test("layout: db alone, scripts-only and unknown archives") {
        guard case .single(let db) = ContentLayoutAnalyzer.analyze(entries(["Mod/db/mods/x.db0"])) else { throw TestFailure(message: "db") }
        try expectEqual(db.prefix, "Mod")
        try expectEqual(db.destination, "")

        guard case .single(let bare) = ContentLayoutAnalyzer.analyze(entries(["Mod/scripts/a.script", "Mod/configs/x.ltx", "Mod/readme.txt"])) else {
            throw TestFailure(message: "bare")
        }
        try expectEqual(bare.prefix, "Mod")
        try expectEqual(bare.destination, "gamedata")

        guard case .none = ContentLayoutAnalyzer.analyze(entries(["readme.txt", "docs/manual.pdf"])) else { throw TestFailure(message: "none") }
    }

    test("layout: numbered option folders become several roots") {
        guard case .multiple(let options) = ContentLayoutAnalyzer.analyze(entries([
            "00 Main/gamedata/a.script", "01 Optional/gamedata/b.script", "10 Patch/gamedata/c.script", "readme.txt",
        ])) else { throw TestFailure(message: "multiple") }
        try expectEqual(options.map(\.displayName), ["00 Main", "01 Optional", "10 Patch"])

        guard case .multiple(let nested) = ContentLayoutAnalyzer.analyze(entries([
            "Mod/Main/gamedata/a.script", "Mod/Options/Alt/gamedata/b.script",
        ])) else { throw TestFailure(message: "nested") }
        try expectEqual(nested.map(\.displayName), ["Main", "Options/Alt"])
    }

    test("layout: a fomod folder wins and is located under wrappers") {
        guard case .fomod(let root, let config, let info) = ContentLayoutAnalyzer.analyze(entries([
            "Wrap/fomod/ModuleConfig.xml", "Wrap/fomod/info.xml", "Wrap/Core/gamedata/a.script",
        ])) else { throw TestFailure(message: "fomod") }
        try expectEqual(root, "Wrap")
        try expectEqual(config, "Wrap/fomod/ModuleConfig.xml")
        try expectEqual(info, "Wrap/fomod/info.xml")
    }

    test("layout: warnings for programs, bin and files outside gamedata") {
        let list = entries(["gamedata/a.script", "bin/xrEngine.dll", "bin/data.ltx", "tool/helper.exe", "extras/pic.dds", "readme.txt"])
        let warnings = ContentLayoutAnalyzer.warnings(for: list, root: "", destination: "")
        try expect(warnings.contains { $0.contains("2 Windows program files") }, "\(warnings)")
        try expect(warnings.contains { $0.contains("bin/") }, "\(warnings)")
        try expect(warnings.contains { $0.contains("outside gamedata/ and db/") }, "\(warnings)")
        try expectEqual(ContentLayoutAnalyzer.warnings(for: entries(["gamedata/a.script"]), root: "", destination: "").count, 0)
    }

    test("library: modlist order, unknown ids skipped, unlisted mods appended disabled") {
        try await withTempDir { root in
            try makeInstalledMod(root: root, id: "a", installedAt: Date(timeIntervalSince1970: 100))
            try makeInstalledMod(root: root, id: "b", installedAt: Date(timeIntervalSince1970: 200))
            try makeInstalledMod(root: root, id: "c", installedAt: Date(timeIntervalSince1970: 300))
            try makeInstalledMod(root: root, id: "d", installedAt: Date(timeIntervalSince1970: 50))
            let list = #"{"schema":1,"mods":[{"id":"c","enabled":true},{"id":"ghost","enabled":true},{"id":"a","enabled":false},{"id":"c","enabled":false}]}"#
            try Data(list.utf8).write(to: root.appendingPathComponent("modlist.json"))
            try FileManager.default.createDirectory(at: root.appendingPathComponent(".downloads"), withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: root.appendingPathComponent("not-a-mod"), withIntermediateDirectories: true)

            let library = try ModLibrary(root: root)
            try expectEqual(library.mods.map(\.id), ["c", "a", "d", "b"])
            try expectEqual(library.mods.map(\.enabled), [true, false, false, false])
            try expectEqual(library.mods[0].contentURL.lastPathComponent, "content")
        }
    }

    test("library: toggle, move, rename and remove persist atomically") {
        try await withTempDir { root in
            for id in ["a", "b", "c", "d"] { try makeInstalledMod(root: root, id: id, files: ["gamedata/x.txt": id]) }
            let library = try ModLibrary(root: root)
            try expectEqual(library.mods.count, 4)
            try expect(library.mods.allSatisfy { !$0.enabled })

            try library.setEnabled("b", true)
            try library.setEnabled("c", true)
            try library.move(fromOffsets: IndexSet(integer: 0), toOffset: 4)   // a to the end
            try expectEqual(library.mods.map(\.id), ["b", "c", "d", "a"])
            try library.move(fromOffsets: IndexSet(integer: 3), toOffset: 0)   // back to the front
            try expectEqual(library.mods.map(\.id), ["a", "b", "c", "d"])
            try library.move(fromOffsets: IndexSet([0, 1]), toOffset: 4)
            try expectEqual(library.mods.map(\.id), ["c", "d", "a", "b"])
            try library.move(fromOffsets: IndexSet(integer: 1), toOffset: 3)   // d between a and b
            try expectEqual(library.mods.map(\.id), ["c", "a", "d", "b"])
            try library.rename("a", to: "  Alpha  ")

            let reopened = try ModLibrary(root: root)
            try expectEqual(reopened.mods.map(\.id), ["c", "a", "d", "b"])
            try expectEqual(reopened.mods.first { $0.id == "b" }?.enabled, true)
            try expectEqual(reopened.mods.first { $0.id == "a" }?.name, "Alpha")

            let data = try Data(contentsOf: root.appendingPathComponent("modlist.json"))
            let json = try JSONSerialization.jsonObject(with: data) as? [String: Any]
            try expectEqual(json?["schema"] as? Int, 1)
            try expectEqual((json?["mods"] as? [[String: Any]])?.count, 4)

            try library.remove("c")
            try expectEqual(library.mods.map(\.id), ["a", "d", "b"])
            try expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent("c").path))
            try expectEqual(try ModLibrary(root: root).mods.map(\.id), ["a", "d", "b"])
            try await expectError(matches: { if case ModImportError.notFound = $0 { return true } else { return false } }) {
                try library.setEnabled("nope", true)
            }
        }
    }

    test("library: an unreadable modlist.json is kept aside and does not lose the mods") {
        try await withTempDir { root in
            try makeInstalledMod(root: root, id: "a")
            try Data("{ not json".utf8).write(to: root.appendingPathComponent("modlist.json"))
            let library = try ModLibrary(root: root)
            try expectEqual(library.mods.map(\.id), ["a"])
            try expect(FileManager.default.fileExists(atPath: root.appendingPathComponent("modlist.json.corrupt").path))
        }
    }

    test("library: stale staging folders are cleaned at startup") {
        try await withTempDir { root in
            let stale = root.appendingPathComponent(".staging/half-done/content")
            try FileManager.default.createDirectory(at: stale, withIntermediateDirectories: true)
            _ = try ModLibrary(root: root)
            try expect(!FileManager.default.fileExists(atPath: root.appendingPathComponent(".staging/half-done").path))
        }
    }

    test("library: conflicts compare paths case-insensitively and follow the load order") {
        try await withTempDir { root in
            try makeInstalledMod(root: root, id: "one", files: ["gamedata/Scripts/Foo.script": "1", "gamedata/configs/only1.ltx": "1"])
            try makeInstalledMod(root: root, id: "two", files: ["gamedata/scripts/foo.SCRIPT": "2", "gamedata/configs/only2.ltx": "2"])
            try makeInstalledMod(root: root, id: "three", files: ["GameData/scripts/FOO.script": "3", "db/mods/a.db0": "3"])
            try makeInstalledMod(root: root, id: "off", files: ["gamedata/scripts/foo.script": "x", "db/mods/a.db0": "x"])
            let library = try ModLibrary(root: root)
            for id in ["one", "two", "three"] { try library.setEnabled(id, true) }
            try library.move(fromOffsets: IndexSet(integer: library.mods.firstIndex { $0.id == "off" }!), toOffset: 0)
            // order now: off(disabled), one, two, three  -> position of "off" among the rest does not matter, it is disabled
            let order = library.mods.map(\.id).filter { $0 != "off" }
            let conflicts = library.conflicts()
            try expectEqual(conflicts.count, 1, "\(conflicts)")
            try expectEqual(conflicts[0].path.lowercased(), "gamedata/scripts/foo.script")
            try expectEqual(conflicts[0].winner, order.last!)
            try expectEqual(conflicts[0].overridden, Array(order.dropLast()))
            // enabling the fourth adds the db conflict and a new winner
            try library.setEnabled("off", true)
            let again = library.conflicts()
            try expectEqual(again.count, 2)
            try expect(again.contains { $0.path.lowercased() == "db/mods/a.db0" })
        }
    }
}
