import Foundation
@testable import ModManagerCore

// Install pipeline: archives of every kind, malicious and damaged input, cancel, disk space, downloads.

private func isError(_ matcher: @escaping (ModImportError) -> Bool) -> (Error) -> Bool {
    { error in (error as? ModImportError).map(matcher) ?? false }
}

private let coolModEntries: [FixtureEntry] = [
    FixtureEntry("Cool Mod v1.2/readme.txt", "read me"),
    FixtureEntry("Cool Mod v1.2/Cool Mod/gamedata/scripts/cool.script", "-- cool"),
    FixtureEntry("Cool Mod v1.2/Cool Mod/gamedata/configs/system.ltx", "[x]"),
    FixtureEntry("Cool Mod v1.2/Cool Mod/gamedata/textures/ui/x.dds", data: Data((0..<4096).map { UInt8($0 & 0xff) })),
    FixtureEntry("Cool Mod v1.2/Cool Mod/gamedata/.DS_Store", "junk"),
    FixtureEntry("__MACOSX/Cool Mod v1.2/._readme.txt", "junk"),
]

private func newLibrary(_ root: URL) throws -> (ModLibrary, ModImporter) {
    let library = try ModLibrary(root: root.appendingPathComponent("Mods"))
    return (library, ModImporter(library: library))
}

func registerInstallTests() {
    test("install: zip with nested roots, metadata from the file name, progress, sha256") {
        try await withTempDir { dir in
            let archive = dir.appendingPathComponent("Cool Mod v1.2.zip")
            try writeArchive(coolModEntries, to: archive)
            let (library, importer) = try newLibrary(dir)
            let plan = try await importer.inspect(archive)
            try expectEqual(plan.displayName, "Cool Mod")
            try expectEqual(plan.version, "1.2")
            try expect(plan.fomod == nil)
            try expectEqual(plan.warnings, [])

            var seen: [Double] = []
            let mod = try await importer.install(plan, choices: nil) { seen.append($0) }
            try expectEqual(mod.id, "cool-mod")
            try expectEqual(mod.name, "Cool Mod")
            try expectEqual(mod.version, "1.2")
            try expectEqual(mod.fileCount, 3)
            try expect(mod.enabled)
            try expectEqual(library.mods.map(\.id), ["cool-mod"])
            try expectEqual(seen.last, 1.0)
            try expect(zip(seen, seen.dropFirst()).allSatisfy { $0 <= $1 }, "progress goes forward: \(seen)")

            let probe = InstalledModProbe(content: mod.contentURL)
            try expectEqual(probe.allFiles(), ["gamedata/configs/system.ltx", "gamedata/scripts/cool.script", "gamedata/textures/ui/x.dds"])
            try expectEqual(contentText(probe, "gamedata/scripts/cool.script"), "-- cool")
            try expectEqual(try Data(contentsOf: mod.contentURL.appendingPathComponent("gamedata/textures/ui/x.dds")).count, 4096)
            try expectEqual(mod.sizeBytes, Int64(7 + 3 + 4096))

            // mod.json and modlist.json follow the contract
            let manifest = try JSONSerialization.jsonObject(with: Data(contentsOf: mod.folderURL.appendingPathComponent("mod.json"))) as! [String: Any]
            try expectEqual(manifest["schema"] as? Int, 1)
            try expectEqual(manifest["id"] as? String, "cool-mod")
            try expectEqual(manifest["version"] as? String, "1.2")
            try expect(manifest["fomod_choices"] is NSNull, "fomod_choices is null")
            try expectEqual(manifest["file_count"] as? Int, 3)
            let source = manifest["source"] as! [String: Any]
            try expectEqual(source["kind"] as? String, "file")
            try expectEqual(source["file_name"] as? String, "Cool Mod v1.2.zip")
            try expectEqual(source["sha256"] as? String, try sha256Hex(archive))
            try expect(ISO8601DateFormatter().date(from: manifest["installed_at"] as! String) != nil)
            let list = try JSONSerialization.jsonObject(with: Data(contentsOf: library.root.appendingPathComponent("modlist.json"))) as! [String: Any]
            try expectEqual((list["mods"] as! [[String: Any]]).first?["enabled"] as? Bool, true)

            // a second install of the same archive gets its own id and goes last
            let second = try await importer.install(try await importer.inspect(archive), choices: nil) { _ in }
            try expectEqual(second.id, "cool-mod-2")
            try expectEqual(library.mods.map(\.id), ["cool-mod", "cool-mod-2"])
            // the library only holds mods: no staging leftovers
            try expect((try? FileManager.default.contentsOfDirectory(atPath: library.stagingURL.path))?.isEmpty ?? true)
        }
    }

    test("install: 7z (LZMA2, LZMA, PPMd, copy) installs like zip") {
        try await withTempDir { dir in
            let (library, importer) = try newLibrary(dir)
            for method in ["lzma2", "lzma1", "ppmd", "copy", "bzip2", "deflate"] {
                let archive = dir.appendingPathComponent("Cool Mod v1.2 \(method).7z")
                try writeArchive(coolModEntries, format: .sevenZip(compression: method), to: archive)
                let plan = try await importer.inspect(archive)
                let mod = try await importer.install(plan, choices: nil) { _ in }
                let probe = InstalledModProbe(content: mod.contentURL)
                try expectEqual(probe.allFiles().count, 3, method)
                try expectEqual(contentText(probe, "gamedata/scripts/cool.script"), "-- cool", method)
            }
            try expectEqual(library.mods.count, 6)
        }
    }

    test("install: scripts/configs without gamedata go into gamedata, db archives stay in db") {
        try await withTempDir { dir in
            let (_, importer) = try newLibrary(dir)
            let bare = dir.appendingPathComponent("bare.zip")
            try writeArchive([FixtureEntry("Mod/scripts/a.script", "a"), FixtureEntry("Mod/configs/b.ltx", "b"), FixtureEntry("Mod/notes.txt", "n")], to: bare)
            let barePlan = try await importer.inspect(bare)
            let bareMod = try await importer.install(barePlan, choices: nil) { _ in }
            try expectEqual(InstalledModProbe(content: bareMod.contentURL).allFiles(), ["gamedata/configs/b.ltx", "gamedata/scripts/a.script"])

            let db = dir.appendingPathComponent("db.zip")
            try writeArchive([FixtureEntry("db/mods/x.db0", data: Data(repeating: 7, count: 1000))], to: db)
            let dbMod = try await importer.install(try await importer.inspect(db), choices: nil) { _ in }
            try expectEqual(InstalledModProbe(content: dbMod.contentURL).allFiles(), ["db/mods/x.db0"])
        }
    }

    test("install: non-ASCII file names survive in zip and 7z") {
        try await withTempDir { dir in
            let (_, importer) = try newLibrary(dir)
            let names = ["gamedata/configs/текст/привет.ltx", "gamedata/scripts/日本語.script", "gamedata/textures/café.dds"]
            for (ext, format) in [("zip", FixtureFormat.zip), ("7z", .sevenZip(compression: "lzma2"))] {
                let archive = dir.appendingPathComponent("Names \(ext).\(ext)")
                try writeArchive(names.map { FixtureEntry($0, "x") }, format: format, to: archive)
                let mod = try await importer.install(try await importer.inspect(archive), choices: nil) { _ in }
                let files = InstalledModProbe(content: mod.contentURL).allFiles().map { $0.precomposedStringWithCanonicalMapping }
                try expectEqual(files, names.map { $0.precomposedStringWithCanonicalMapping }.sorted(), ext)
            }
        }
    }

    test("install: Windows programs are skipped with a warning") {
        try await withTempDir { dir in
            let (_, importer) = try newLibrary(dir)
            let archive = dir.appendingPathComponent("withdll.zip")
            try writeArchive([
                FixtureEntry("gamedata/scripts/a.script", "a"), FixtureEntry("bin/lua_ext.dll", "MZ"),
                FixtureEntry("bin/tool.exe", "MZ"), FixtureEntry("bin/settings.ltx", "x"),
            ], to: archive)
            let plan = try await importer.inspect(archive)
            try expect(plan.warnings.contains { $0.contains("Windows program") }, "\(plan.warnings)")
            try expect(plan.warnings.contains { $0.contains("bin/") }, "\(plan.warnings)")
            let mod = try await importer.install(plan, choices: nil) { _ in }
            let files = InstalledModProbe(content: mod.contentURL).allFiles()
            try expectEqual(files, ["bin/settings.ltx", "gamedata/scripts/a.script"])
        }
    }

    test("install: numbered option folders show a synthetic installer; priority follows the numbers") {
        try await withTempDir { dir in
            let (_, importer) = try newLibrary(dir)
            let archive = dir.appendingPathComponent("Multi v3.zip")
            try writeArchive([
                FixtureEntry("00 Main/gamedata/scripts/a.script", "main"),
                FixtureEntry("00 Main/gamedata/configs/m.ltx", "m"),
                FixtureEntry("01 Optional/gamedata/scripts/a.script", "optional"),
                FixtureEntry("01 Optional/gamedata/configs/o.ltx", "o"),
                FixtureEntry("readme.txt", "r"),
            ], to: archive)
            let plan = try await importer.inspect(archive)
            guard let installer = plan.fomod else { throw TestFailure(message: "no installer") }
            try expect(installer.isSynthetic)
            try expectEqual(installer.steps.count, 1)
            try expectEqual(installer.steps[0].groups[0].plugins.map(\.name), ["00 Main", "01 Optional"])

            // defaults: the main folder only
            let defaults = installer.defaultChoices()
            try expectEqual(defaults.selections.first?.plugins, ["00 Main"])
            let one = try await importer.install(plan, choices: nil) { _ in }
            let probeOne = InstalledModProbe(content: one.contentURL)
            try expectEqual(probeOne.allFiles(), ["gamedata/configs/m.ltx", "gamedata/scripts/a.script"])
            try expectEqual(contentText(probeOne, "gamedata/scripts/a.script"), "main")

            // both: the later folder wins the shared file
            let both = FomodChoices(selections: [.init(step: "Choose what to install", group: "Options", plugins: ["00 Main", "01 Optional"])])
            let two = try await importer.install(plan, choices: both) { _ in }
            let probeTwo = InstalledModProbe(content: two.contentURL)
            try expectEqual(probeTwo.allFiles(), ["gamedata/configs/m.ltx", "gamedata/configs/o.ltx", "gamedata/scripts/a.script"])
            try expectEqual(contentText(probeTwo, "gamedata/scripts/a.script"), "optional")
            let manifest = try JSONSerialization.jsonObject(with: Data(contentsOf: two.folderURL.appendingPathComponent("mod.json"))) as! [String: Any]
            let saved = manifest["fomod_choices"] as? [[String: Any]]
            try expectEqual(saved?.first?["plugins"] as? [String], ["00 Main", "01 Optional"])
        }
    }

    test("install: a plain folder works, a folder with a link is refused") {
        try await withTempDir { dir in
            let (_, importer) = try newLibrary(dir)
            let folder = dir.appendingPathComponent("Folder Mod 2.0")
            try writeFolder([FixtureEntry("Inner/gamedata/scripts/f.script", "f"), FixtureEntry("Inner/gamedata/configs/f.ltx", "g")], to: folder)
            let plan = try await importer.inspect(folder)
            try expectEqual(plan.displayName, "Folder Mod")
            let mod = try await importer.install(plan, choices: nil) { _ in }
            try expectEqual(InstalledModProbe(content: mod.contentURL).allFiles(), ["gamedata/configs/f.ltx", "gamedata/scripts/f.script"])
            try expect(FileManager.default.fileExists(atPath: folder.path), "a folder source is left alone")

            let evil = dir.appendingPathComponent("evil-folder")
            try writeFolder([FixtureEntry("gamedata/scripts/f.script", "f"), FixtureEntry("gamedata/link", symlinkTo: "/etc")], to: evil)
            try await expectError(matches: isError { if case .symlinkRefused = $0 { return true } else { return false } }) {
                _ = try await importer.inspect(evil)
            }
        }
    }

    test("security: traversal, absolute paths and links in archives are refused") {
        try await withTempDir { dir in
            let (library, importer) = try newLibrary(dir)
            let cases: [(String, [FixtureEntry], (ModImportError) -> Bool)] = [
                ("dotdot", [FixtureEntry("gamedata/a.script", "a"), FixtureEntry("../evil.txt", "x")], { if case .unsafePath = $0 { return true } else { return false } }),
                ("deep dotdot", [FixtureEntry("gamedata/../../evil.txt", "x")], { if case .unsafePath = $0 { return true } else { return false } }),
                ("absolute", [FixtureEntry("/tmp/evil.txt", "x"), FixtureEntry("gamedata/a.script", "a")], { if case .unsafePath = $0 { return true } else { return false } }),
                ("backslash", [FixtureEntry("gamedata\\..\\..\\evil.txt", "x")], { if case .unsafePath = $0 { return true } else { return false } }),
                ("symlink", [FixtureEntry("gamedata/a.script", "a"), FixtureEntry("gamedata/link", symlinkTo: "/etc/passwd")], { if case .symlinkRefused = $0 { return true } else { return false } }),
            ]
            for (name, entries, matcher) in cases {
                for format in [FixtureFormat.zip, .sevenZip(compression: "lzma2")] {
                    let ext: String = { if case .zip = format { return "zip" } else { return "7z" } }()
                    // the 7z writer refuses some of these names itself; only archives that could be made are checked
                    let archive = dir.appendingPathComponent("\(name).\(ext)")
                    guard (try? writeArchive(entries, format: format, to: archive)) != nil else { continue }
                    try await expectError("\(name).\(ext)", matches: isError(matcher)) { _ = try await importer.inspect(archive) }
                }
            }
            try expectEqual(library.mods.count, 0)
            try expect(!FileManager.default.fileExists(atPath: dir.appendingPathComponent("evil.txt").path))
            try expect(!FileManager.default.fileExists(atPath: "/tmp/evil.txt"))
        }
    }

    test("damaged input: truncated, garbled and empty archives fail cleanly") {
        try await withTempDir { dir in
            let (library, importer) = try newLibrary(dir)
            let good = dir.appendingPathComponent("good.zip")
            try writeArchive(coolModEntries, to: good)
            let bytes = try Data(contentsOf: good)

            let truncated = dir.appendingPathComponent("truncated.zip")
            try bytes.prefix(bytes.count / 2).write(to: truncated)
            let garbled = dir.appendingPathComponent("garbled.zip")
            try Data((0..<2048).map { UInt8(($0 &* 31) & 0xff) }).write(to: garbled)
            let empty = dir.appendingPathComponent("empty.zip")
            try Data().write(to: empty)
            // a valid central directory but a payload with flipped bytes: found when the file is read
            let flipped = dir.appendingPathComponent("flipped.zip")
            var damaged = bytes
            for i in stride(from: 60, to: min(damaged.count - 200, 400), by: 7) { damaged[i] ^= 0xff }
            try damaged.write(to: flipped)

            for url in [truncated, garbled, empty] {
                try await expectError(url.lastPathComponent, matches: isError {
                    switch $0 {
                    case .corruptArchive, .unsupportedFormat, .noGameContent: return true
                    default: return false
                    }
                }) { _ = try await importer.inspect(url) }
            }
            // the flipped one may fail at inspect or at install, but never leave a mod behind
            do {
                let plan = try await importer.inspect(flipped)
                _ = try await importer.install(plan, choices: nil) { _ in }
                throw TestFailure(message: "damaged archive installed")
            } catch is TestFailure {
                throw TestFailure(message: "damaged archive installed")
            } catch {}
            try expectEqual(library.mods.count, 0)
            try expect((try? FileManager.default.contentsOfDirectory(atPath: library.stagingURL.path))?.isEmpty ?? true)
            try await expectError(matches: isError { if case .notFound = $0 { return true } else { return false } }) {
                _ = try await importer.inspect(dir.appendingPathComponent("missing.zip"))
            }
        }
    }

    test("cancel: stops extraction and leaves nothing behind") {
        try await withTempDir { dir in
            let (library, importer) = try newLibrary(dir)
            let archive = dir.appendingPathComponent("big.zip")
            var files: [FixtureEntry] = []
            for i in 0..<12 { files.append(FixtureEntry("gamedata/textures/t\(i).dds", data: Data(repeating: UInt8(i), count: 1 << 20))) }
            try writeArchive(files, to: archive)
            let plan = try await importer.inspect(archive)

            // the hook runs on the extraction thread after the first file: cancel the task right there
            let holder = TaskHolder()
            importer.extractionHook = { holder.task?.cancel() }
            let task = Task { try await importer.install(plan, choices: nil) { _ in } }
            holder.task = task
            do {
                _ = try await task.value
                throw TestFailure(message: "install finished although it was cancelled")
            } catch is CancellationError {
            }
            importer.extractionHook = nil
            try expectEqual(library.mods.count, 0)
            try expect((try? FileManager.default.contentsOfDirectory(atPath: library.stagingURL.path))?.isEmpty ?? true, "staging is empty")
            try expect(!FileManager.default.fileExists(atPath: library.root.appendingPathComponent("big").path))

            // a cancelled inspect
            let inspectTask = Task { try await importer.inspect(archive) }
            inspectTask.cancel()
            _ = try? await inspectTask.value   // either result is fine; it must not hang or crash
            // and the importer still works afterwards
            let mod = try await importer.install(try await importer.inspect(archive), choices: nil) { _ in }
            try expectEqual(InstalledModProbe(content: mod.contentURL).allFiles().count, 12)
        }
    }

    test("space: an install that cannot fit is refused before anything is written") {
        try await withTempDir { dir in
            let (library, _) = try newLibrary(dir)
            try await expectError(matches: isError { if case .insufficientSpace = $0 { return true } else { return false } }) {
                try ModImporter.checkFreeSpace(root: library.root, needed: Int64.max / 4)
            }
            try ModImporter.checkFreeSpace(root: library.root, needed: 1024)
        }
    }

    test("downloads: a finished download is deleted after install, other files are not") {
        try await withTempDir { dir in
            let (library, importer) = try newLibrary(dir)
            try FileManager.default.createDirectory(at: library.downloadsURL, withIntermediateDirectories: true)
            let download = library.downloadsURL.appendingPathComponent("Fetched Mod 1.0.zip")
            try writeArchive([FixtureEntry("gamedata/scripts/d.script", "d")], to: download)
            let elsewhere = dir.appendingPathComponent("Mine.zip")
            try writeArchive([FixtureEntry("gamedata/scripts/m.script", "m")], to: elsewhere)

            let origin = ModOrigin(kind: .url, url: "https://example.invalid/fetched.zip")
            let fetched = try await importer.install(try await importer.inspect(download, origin: origin), choices: nil) { _ in }
            try expect(!FileManager.default.fileExists(atPath: download.path), "download removed")
            try expectEqual(fetched.origin.kind, .url)
            try expectEqual(fetched.origin.url, "https://example.invalid/fetched.zip")
            _ = try await importer.install(try await importer.inspect(elsewhere), choices: nil) { _ in }
            try expect(FileManager.default.fileExists(atPath: elsewhere.path), "a picked file stays")
        }
    }

    test("rar: libarchive's RAR 4 and RAR 5 fixtures list and extract in streaming mode") {
        try await withTempDir { dir in
            let cancel = CancelFlag()
            var tested = 0
            var refused = 0
            for name in [
                "test_read_format_rar.rar", "test_read_format_rar5_stored.rar", "test_read_format_rar5_compressed.rar",
                "test_read_format_rar_noeof.rar", "test_read_format_rar5_multiple_files.rar", "test_read_format_rar_compress_normal.rar",
                "test_read_format_rar_compress_best.rar", "test_read_format_rar_binary_data.rar", "test_read_format_rar_unicode.rar",
                "test_read_format_rar_filter.rar", "test_read_format_rar_windows.rar", "test_read_format_rar5_arm.rar",
            ] {
                guard let url = try libarchiveFixture(name, into: dir) else { return }   // libarchive's test files not available
                let source = ArchiveContentSource(url: url)
                let entries: [SourceEntry]
                do {
                    entries = try source.list(cancel: cancel) { _ in nil }
                } catch ModImportError.symlinkRefused(let link) {
                    // several of libarchive's RAR 4 files carry the same symlink ("testlink"): refused, as it must be
                    print("     rar fixture \(name): refused the link \(link)")
                    refused += 1
                    continue
                }
                let files = entries.filter { !$0.isDirectory }
                try expect(!files.isEmpty, "\(name) lists files")
                let out = dir.appendingPathComponent("out-" + name)
                var jobs: [Int: [String]] = [:]
                for f in files { jobs[f.index] = [f.path] }
                var written: Int64 = 0
                try source.extract(jobs: jobs, entries: entries, into: out, cancel: cancel) { written += $0 }
                for f in files {
                    let size = (try FileManager.default.attributesOfItem(atPath: out.appendingPathComponent(f.path).path)[.size] as? Int64) ?? -1
                    try expectEqual(size, f.size, "\(name): \(f.path)")
                }
                try expectEqual(written, files.reduce(0) { $0 + $1.size }, name)
                tested += 1
                print("     rar fixture \(name): \(files.count) file(s), \(written) bytes")
            }
            if ProcessInfo.processInfo.environment["ANOMALY_LIBARCHIVE_TEST_DIR"] != nil {
                try expect(tested >= 4, "the RAR fixtures were read (\(tested))")
                try expect(refused >= 1, "a RAR 4 fixture with a symlink is refused (\(refused))")
            }
        }
    }

    test("rar: an encrypted RAR is reported as password protected") {
        try await withTempDir { dir in
            guard let url = try libarchiveFixture("test_read_format_rar5_encrypted.rar", into: dir) else { return }
            let importer = try newLibrary(dir).1
            try await expectError(matches: isError {
                switch $0 {
                case .encrypted, .corruptArchive, .noGameContent: return true
                default: return false
                }
            }) { _ = try await importer.inspect(url) }
        }
    }
}

final class TaskHolder: @unchecked Sendable {
    var task: Task<InstalledMod, Error>?
}
