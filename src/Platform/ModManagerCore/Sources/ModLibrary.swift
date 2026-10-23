import Combine
import Foundation

public struct InstalledMod: Identifiable, Hashable {
    public let id: String
    public var name: String
    public var version: String?
    public var enabled: Bool
    public let sizeBytes: Int64
    public let fileCount: Int
    public let folderURL: URL

    // Beyond the shared contract: more of mod.json for a detail screen.
    public var author: String?
    public var summary: String?
    public var origin: ModOrigin
    public var installedAt: Date?
    /// True when the install went through a FOMOD wizard (or the synthetic one for numbered option folders).
    public var usedInstaller: Bool

    public var contentURL: URL { folderURL.appendingPathComponent("content", isDirectory: true) }

    public init(id: String, name: String, version: String?, enabled: Bool, sizeBytes: Int64, fileCount: Int, folderURL: URL,
                author: String? = nil, summary: String? = nil, origin: ModOrigin = ModOrigin(), installedAt: Date? = nil,
                usedInstaller: Bool = false) {
        self.id = id
        self.name = name
        self.version = version
        self.enabled = enabled
        self.sizeBytes = sizeBytes
        self.fileCount = fileCount
        self.folderURL = folderURL
        self.author = author
        self.summary = summary
        self.origin = origin
        self.installedAt = installedAt
        self.usedInstaller = usedInstaller
    }
}

/// A file that several enabled mods ship. Paths are relative to the game folder and compared case-insensitively.
public struct ModConflict: Hashable {
    public let path: String
    public let winner: String
    /// Mods whose copy is hidden by `winner`, lowest priority first.
    public let overridden: [String]

    public init(path: String, winner: String, overridden: [String]) {
        self.path = path
        self.winner = winner
        self.overridden = overridden
    }
}

/// The installed mods under the mods root, in load order (lowest priority first; the later mod wins a conflict).
///
/// Files: `<root>/<id>/mod.json`, `<root>/<id>/content/...`, `<root>/modlist.json`. The library is not bound to an actor:
/// a lock keeps its state consistent and the views should call it from the main thread, where `mods` is published.
public final class ModLibrary: ObservableObject {
    public let root: URL
    @Published public private(set) var mods: [InstalledMod] = []

    /// Downloads wait here until the importer has installed them (contract point 7).
    public var downloadsURL: URL { root.appendingPathComponent(".downloads", isDirectory: true) }
    /// Half-installed and half-removed mods; emptied at startup.
    var stagingURL: URL { root.appendingPathComponent(".staging", isDirectory: true) }
    var modListURL: URL { root.appendingPathComponent("modlist.json") }

    private let lock = NSRecursiveLock()
    private var manifests: [String: ModManifest] = [:]
    private var fileIndexCache: [String: (stamp: Date?, files: [String: String])] = [:]

    public init(root: URL) throws {
        self.root = root.standardizedFileURL
        let fm = FileManager.default
        do {
            try fm.createDirectory(at: self.root, withIntermediateDirectories: true)
        } catch {
            throw ModImportError.io("The mods folder cannot be created: \(error.localizedDescription)")
        }
        // A crash or a cancel can leave staging folders behind; nothing in there is a mod yet.
        if let leftovers = try? fm.contentsOfDirectory(atPath: stagingURL.path) {
            for name in leftovers { try? fm.removeItem(at: stagingURL.appendingPathComponent(name)) }
        }
        try reload()
    }

    // MARK: - Reading

    public func reload() throws {
        lock.lock(); defer { lock.unlock() }
        let found = scanManifests()
        let list = readModList()
        var ordered: [InstalledMod] = []
        var seen = Set<String>()
        for entry in list.mods where !seen.contains(entry.id) {
            guard let manifest = found[entry.id] else { continue }   // unknown ids are skipped
            seen.insert(entry.id)
            ordered.append(makeMod(manifest, enabled: entry.enabled))
        }
        // Folders the list does not mention count as appended and disabled.
        let unlisted = found.values.filter { !seen.contains($0.id) }.sorted { (a: ModManifest, b: ModManifest) -> Bool in
            let da = a.installedDate ?? Date.distantPast
            let db = b.installedDate ?? Date.distantPast
            return da != db ? da < db : a.id < b.id
        }
        for manifest in unlisted { ordered.append(makeMod(manifest, enabled: false)) }
        manifests = found
        fileIndexCache.removeAll()
        mods = ordered
    }

    private func makeMod(_ m: ModManifest, enabled: Bool) -> InstalledMod {
        InstalledMod(id: m.id, name: m.name, version: m.version, enabled: enabled, sizeBytes: m.sizeBytes,
                     fileCount: m.fileCount, folderURL: root.appendingPathComponent(m.id, isDirectory: true),
                     author: m.author, summary: m.description, origin: m.source, installedAt: m.installedDate,
                     usedInstaller: m.fomodChoices != nil)
    }

    /// Every `<root>/<folder>/mod.json` that parses. The folder name is the id (mod.json's own id is ignored when it differs).
    private func scanManifests() -> [String: ModManifest] {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: root.path) else { return [:] }
        var out: [String: ModManifest] = [:]
        for name in names where !name.hasPrefix(".") {
            let dir = root.appendingPathComponent(name, isDirectory: true)
            guard let data = try? Data(contentsOf: dir.appendingPathComponent("mod.json")),
                  var manifest = try? JSONDecoder().decode(ModManifest.self, from: data) else { continue }
            manifest.id = name
            out[name] = manifest
        }
        return out
    }

    private func readModList() -> ModListFile {
        guard let data = try? Data(contentsOf: modListURL) else { return ModListFile() }
        if let list = try? JSONDecoder().decode(ModListFile.self, from: data) { return list }
        // Unreadable (edited by hand?): keep the bytes for the user and start from the folders.
        let aside = root.appendingPathComponent("modlist.json.corrupt")
        try? FileManager.default.removeItem(at: aside)
        try? FileManager.default.copyItem(at: modListURL, to: aside)
        return ModListFile()
    }

    // MARK: - Changing

    public func setEnabled(_ id: String, _ enabled: Bool) throws {
        lock.lock(); defer { lock.unlock() }
        guard let index = mods.firstIndex(where: { $0.id == id }) else { throw ModImportError.notFound("The mod “\(id)”") }
        guard mods[index].enabled != enabled else { return }
        var next = mods
        next[index].enabled = enabled
        try commit(next)
    }

    /// Same semantics as SwiftUI's `onMove`: the moved mods end up before the element that was at `toOffset`.
    public func move(fromOffsets source: IndexSet, toOffset destination: Int) throws {
        lock.lock(); defer { lock.unlock() }
        let indices = source.sorted().filter { $0 >= 0 && $0 < mods.count }
        guard !indices.isEmpty, destination >= 0, destination <= mods.count else { return }
        var next = mods
        let moving = indices.map { next[$0] }
        let before = indices.filter { $0 < destination }.count
        for i in indices.reversed() { next.remove(at: i) }
        next.insert(contentsOf: moving, at: destination - before)
        guard next.map(\.id) != mods.map(\.id) else { return }
        try commit(next)
    }

    public func rename(_ id: String, to name: String) throws {
        lock.lock(); defer { lock.unlock() }
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, var manifest = manifests[id], let index = mods.firstIndex(where: { $0.id == id }) else { return }
        manifest.name = trimmed
        try writeManifest(manifest, in: root.appendingPathComponent(id, isDirectory: true))
        manifests[id] = manifest
        var next = mods
        next[index].name = trimmed
        mods = next
    }

    public func remove(_ id: String) throws {
        lock.lock(); defer { lock.unlock() }
        guard mods.contains(where: { $0.id == id }) else { throw ModImportError.notFound("The mod “\(id)”") }
        let fm = FileManager.default
        let folder = root.appendingPathComponent(id, isDirectory: true)
        // Move the folder out first: from then on the engine can no longer see half a mod.
        try fm.createDirectory(at: stagingURL, withIntermediateDirectories: true)
        let trash = stagingURL.appendingPathComponent("removed-\(UUID().uuidString)", isDirectory: true)
        if fm.fileExists(atPath: folder.path) {
            do { try fm.moveItem(at: folder, to: trash) } catch { throw ModImportError.io("The mod cannot be removed: \(error.localizedDescription)") }
        }
        do {
            try commit(mods.filter { $0.id != id })
        } catch {
            try? fm.moveItem(at: trash, to: folder)
            throw error
        }
        manifests[id] = nil
        fileIndexCache[id] = nil
        try? fm.removeItem(at: trash)
    }

    /// Persists the list first and publishes it only when that worked.
    private func commit(_ next: [InstalledMod]) throws {
        let file = ModListFile(schema: 1, mods: next.map { .init(id: $0.id, enabled: $0.enabled) })
        let data = try ModManifest.encoder().encode(file)
        do { try data.write(to: modListURL, options: .atomic) } catch {
            throw ModImportError.io("modlist.json cannot be written: \(error.localizedDescription)")
        }
        fileIndexCache = fileIndexCache.filter { key, _ in next.contains { $0.id == key } }
        mods = next
    }

    func writeManifest(_ manifest: ModManifest, in folder: URL) throws {
        let data = try ModManifest.encoder().encode(manifest)
        try data.write(to: folder.appendingPathComponent("mod.json"), options: .atomic)
    }

    /// Takes a fully written staging folder (`mod.json` + `content/`), gives it its final unique id and adds it last in the
    /// load order, which is the highest priority.
    func adopt(staging: URL, manifest: ModManifest, enabled: Bool) throws -> InstalledMod {
        lock.lock(); defer { lock.unlock() }
        let fm = FileManager.default
        var final = manifest
        final.id = ModNaming.unique(ModNaming.slug(manifest.id)) { candidate in
            manifests[candidate] != nil || mods.contains { $0.id == candidate } || fm.fileExists(atPath: root.appendingPathComponent(candidate).path)
        }
        try writeManifest(final, in: staging)
        let destination = root.appendingPathComponent(final.id, isDirectory: true)
        do { try fm.moveItem(at: staging, to: destination) } catch {
            throw ModImportError.io("The mod cannot be moved into the library: \(error.localizedDescription)")
        }
        manifests[final.id] = final
        let mod = makeMod(final, enabled: enabled)
        try commit(mods + [mod])
        return mod
    }

    func isTaken(id: String) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return manifests[id] != nil || FileManager.default.fileExists(atPath: root.appendingPathComponent(id).path)
    }

    // MARK: - Conflicts

    /// Files that two or more enabled mods ship under `content/`, compared case-insensitively with `/` separators, the way
    /// the engine resolves them (`xr_strlwr`). The winner is the enabled mod last in the list. `db/` archives conflict by
    /// file name only; what is inside them is not looked at.
    public func conflicts() -> [ModConflict] {
        lock.lock(); defer { lock.unlock() }
        var owners: [String: (path: String, ids: [String])] = [:]
        for mod in mods where mod.enabled {
            for (key, original) in fileIndex(for: mod) {
                var entry = owners[key] ?? (original, [])
                entry.path = original
                entry.ids.append(mod.id)
                owners[key] = entry
            }
        }
        return owners.values
            .filter { $0.ids.count > 1 }
            .map { ModConflict(path: $0.path, winner: $0.ids[$0.ids.count - 1], overridden: Array($0.ids.dropLast())) }
            .sorted { $0.path.lowercased() < $1.path.lowercased() }
    }

    /// lower-case relative path -> path as stored, for one mod's content. Cached until the mod changes.
    private func fileIndex(for mod: InstalledMod) -> [String: String] {
        let stamp = (try? FileManager.default.attributesOfItem(atPath: mod.folderURL.appendingPathComponent("mod.json").path))?[.modificationDate] as? Date
        if let cached = fileIndexCache[mod.id], cached.stamp == stamp { return cached.files }
        var files: [String: String] = [:]
        Self.walk(mod.contentURL, relative: "") { relative in
            files[ModPath.key(relative)] = relative
        }
        fileIndexCache[mod.id] = (stamp, files)
        return files
    }

    private static func walk(_ dir: URL, relative: String, visit: (String) -> Void) {
        let keys: [URLResourceKey] = [.isDirectoryKey]
        guard let children = try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: keys, options: []) else { return }
        for child in children {
            let name = child.lastPathComponent
            if ModPath.junkNames.contains(name.lowercased()) { continue }
            let rel = relative.isEmpty ? name : relative + "/" + name
            if (try? child.resourceValues(forKeys: Set(keys)).isDirectory) == true {
                walk(child, relative: rel, visit: visit)
            } else {
                visit(rel)
            }
        }
    }
}
