// Temporary stand-ins for ModManagerCore (mod-core) and ModDownloads (mod-downloads), written against the shared contract
// (docs/ios-port/MOD-MANAGER.md). Each block compiles only while the real module is not on the build's module path, and
// the file is deleted once both modules are on main. Nothing here is a design: it is just enough to run the launcher.
#if !canImport(ModManagerCore)
import Foundation
import SwiftUI

public struct InstalledMod: Identifiable, Hashable {
    public let id: String
    public var name: String
    public var version: String?
    public var enabled: Bool
    public let sizeBytes: Int64
    public let fileCount: Int
    public let folderURL: URL

    public init(id: String, name: String, version: String?, enabled: Bool, sizeBytes: Int64, fileCount: Int, folderURL: URL) {
        self.id = id
        self.name = name
        self.version = version
        self.enabled = enabled
        self.sizeBytes = sizeBytes
        self.fileCount = fileCount
        self.folderURL = folderURL
    }
}

public struct ModConflict {
    public let path: String
    public let winner: String
    public let overridden: [String]

    public init(path: String, winner: String, overridden: [String]) {
        self.path = path
        self.winner = winner
        self.overridden = overridden
    }
}

public final class ModLibrary: ObservableObject {
    public let root: URL
    @Published public private(set) var mods: [InstalledMod] = []

    public init(root: URL) throws {
        self.root = root
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        try reload()
    }

    private struct ListEntry: Codable { var id: String; var enabled: Bool }
    private struct ListFile: Codable { var schema = 1; var mods: [ListEntry] }
    private struct ModFile: Codable { var name: String?; var version: String?; var file_count: Int?; var size_bytes: Int64? }

    private var listURL: URL { root.appendingPathComponent("modlist.json") }

    public func reload() throws {
        var listed: [ListEntry] = []
        if let data = try? Data(contentsOf: listURL), let file = try? JSONDecoder().decode(ListFile.self, from: data) {
            listed = file.mods
        }
        let folders = (try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isDirectoryKey])) ?? []
        var found: [String: URL] = [:]
        for url in folders where !url.lastPathComponent.hasPrefix(".") {
            if (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                found[url.lastPathComponent] = url
            }
        }
        var result: [InstalledMod] = []
        var seen = Set<String>()
        func add(_ id: String, enabled: Bool) {
            guard let url = found[id], seen.insert(id).inserted else { return }
            let meta = (try? Data(contentsOf: url.appendingPathComponent("mod.json"))).flatMap { try? JSONDecoder().decode(ModFile.self, from: $0) }
            result.append(InstalledMod(id: id, name: meta?.name ?? id, version: meta?.version, enabled: enabled,
                                       sizeBytes: meta?.size_bytes ?? 0, fileCount: meta?.file_count ?? 0, folderURL: url))
        }
        for entry in listed { add(entry.id, enabled: entry.enabled) }
        for id in found.keys.sorted() { add(id, enabled: false) }
        mods = result
    }

    private func save() throws {
        let file = ListFile(mods: mods.map { ListEntry(id: $0.id, enabled: $0.enabled) })
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(file).write(to: listURL, options: .atomic)
    }

    public func setEnabled(_ id: String, _ enabled: Bool) throws {
        guard let index = mods.firstIndex(where: { $0.id == id }) else { return }
        mods[index].enabled = enabled
        try save()
    }

    public func move(fromOffsets: IndexSet, toOffset: Int) throws {
        mods.move(fromOffsets: fromOffsets, toOffset: toOffset)
        try save()
    }

    public func remove(_ id: String) throws {
        guard let index = mods.firstIndex(where: { $0.id == id }) else { return }
        try FileManager.default.removeItem(at: mods[index].folderURL)
        mods.remove(at: index)
        try save()
    }

    public func conflicts() -> [ModConflict] {
        var owners: [String: [String]] = [:] // lower-cased relative path -> mod ids, lowest priority first
        var spelled: [String: String] = [:]
        for mod in mods where mod.enabled {
            let content = mod.folderURL.appendingPathComponent("content")
            guard let walker = FileManager.default.enumerator(at: content, includingPropertiesForKeys: [.isRegularFileKey]) else { continue }
            for case let url as URL in walker where (try? url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true {
                let relative = String(url.path.dropFirst(content.path.count + 1))
                let key = relative.lowercased()
                owners[key, default: []].append(mod.id)
                spelled[key] = relative
            }
        }
        return owners.compactMap { key, ids in
            ids.count > 1 ? ModConflict(path: spelled[key] ?? key, winner: ids[ids.count - 1], overridden: Array(ids.dropLast())) : nil
        }.sorted { $0.path < $1.path }
    }

    // Only for the stub importer.
    func adopt(_ id: String) throws {
        try reload()
        if let index = mods.firstIndex(where: { $0.id == id }) {
            mods[index].enabled = true
            try save()
        }
    }
}

public struct FomodInstaller {}
public struct FomodChoices {}

public struct ImportPlan {
    public let displayName: String
    public let source: URL
    public let fomod: FomodInstaller?
    public let warnings: [String]

    public init(displayName: String, source: URL, fomod: FomodInstaller?, warnings: [String]) {
        self.displayName = displayName
        self.source = source
        self.fomod = fomod
        self.warnings = warnings
    }
}

public struct StubImportError: LocalizedError {
    public let errorDescription: String?
}

/// Folders only: copies them as they are. Archives need the real ModManagerCore.
public final class ModImporter {
    private let library: ModLibrary

    public init(library: ModLibrary) { self.library = library }

    public func inspect(_ url: URL) async throws -> ImportPlan {
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory) else {
            throw StubImportError(errorDescription: "The file is gone: \(url.lastPathComponent)")
        }
        guard isDirectory.boolValue else {
            throw StubImportError(errorDescription: "Archives (\(url.pathExtension)) need the mod manager core, which is not in this build.")
        }
        return ImportPlan(displayName: url.lastPathComponent, source: url, fomod: nil, warnings: Self.warnings(in: url))
    }

    private static func warnings(in url: URL) -> [String] {
        guard let walker = FileManager.default.enumerator(atPath: url.path) else { return [] }
        for case let path as String in walker where path.lowercased().hasSuffix(".dll") || path.lowercased().hasSuffix(".exe") {
            return ["Contains Windows binaries (\(path)); they do nothing here."]
        }
        return []
    }

    private static func measure(_ content: URL) -> (count: Int, size: Int64) {
        var count = 0
        var size: Int64 = 0
        if let walker = FileManager.default.enumerator(at: content, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey]) {
            for case let url as URL in walker {
                let values = try? url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
                if values?.isRegularFile == true {
                    count += 1
                    size += Int64(values?.fileSize ?? 0)
                }
            }
        }
        return (count, size)
    }

    public func install(_ plan: ImportPlan, choices: FomodChoices?, progress: @escaping (Double) -> Void) async throws -> InstalledMod {
        let slug = plan.displayName.lowercased().map { $0.isLetter || $0.isNumber ? String($0) : "-" }.joined()
        let target = library.root.appendingPathComponent(slug)
        let content = target.appendingPathComponent("content")
        try FileManager.default.createDirectory(at: target, withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: content)
        try FileManager.default.copyItem(at: plan.source, to: content)
        progress(0.5)
        let (count, size) = Self.measure(content)
        let json: [String: Any] = ["schema": 1, "id": slug, "name": plan.displayName, "source": ["kind": "file", "file_name": plan.source.lastPathComponent],
                                   "installed_at": ISO8601DateFormatter().string(from: Date()), "file_count": count, "size_bytes": size]
        try JSONSerialization.data(withJSONObject: json, options: [.prettyPrinted, .sortedKeys]).write(to: target.appendingPathComponent("mod.json"))
        try await MainActor.run { try library.adopt(slug) }
        progress(1)
        return await MainActor.run { library.mods.first { $0.id == slug }! }
    }
}

public struct FomodWizardView: View {
    let onFinish: (FomodChoices) -> Void
    let onCancel: () -> Void

    public init(installer: FomodInstaller, onFinish: @escaping (FomodChoices) -> Void, onCancel: @escaping () -> Void) {
        self.onFinish = onFinish
        self.onCancel = onCancel
    }

    public var body: some View {
        VStack(spacing: 16) {
            Text("FOMOD installer (stand-in)").font(.title2)
            HStack {
                Button("Cancel", action: onCancel)
                Button("Install") { onFinish(FomodChoices()) }
            }
        }
        .padding(40)
    }
}
#endif

#if !canImport(ModDownloads)
import Foundation
import SwiftUI

public struct DownloadsView: View {
    let downloadsDir: URL
    let onDownloaded: (URL) -> Void

    public init(downloadsDir: URL, onDownloaded: @escaping (URL) -> Void) {
        self.downloadsDir = downloadsDir
        self.onDownloaded = onDownloaded
    }

    public var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "globe").font(.system(size: 48))
            Text("The in-app browser (ModDB, GitHub) is not in this build.")
        }
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

public final class LinkDownloader {
    public init() {}

    public func download(_ url: URL, to dir: URL, progress: @escaping (Double) -> Void) async throws -> URL {
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let (temporary, response) = try await URLSession.shared.download(from: url)
        let suggested = response.suggestedFilename ?? url.lastPathComponent
        let target = dir.appendingPathComponent(suggested.isEmpty ? "download" : suggested)
        try? FileManager.default.removeItem(at: target)
        try FileManager.default.moveItem(at: temporary, to: target)
        progress(1)
        return target
    }
}
#endif
