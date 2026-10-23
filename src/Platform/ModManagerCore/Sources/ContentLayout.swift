import Foundation

/// Where in a source the game files live, found from the entry list alone.
struct RootOption {
    /// Path of the root folder inside the source ("" = the source itself), no trailing slash.
    let prefix: String
    /// Folder below the mod's `content/` the root's files go to: "" when the root holds `gamedata/` or `db/`, "gamedata" when
    /// it holds `scripts/`, `configs/`, ... directly.
    let destination: String
    /// Name for the choice list when there are several roots.
    let displayName: String
    let fileCount: Int
    let sizeBytes: Int64
}

enum ContentLayout {
    /// A FOMOD installer: the folder that contains `fomod/ModuleConfig.xml` (a path inside the source, "" for the top).
    case fomod(root: String, config: String, info: String?)
    case single(RootOption)
    /// Several roots (numbered folders, "Main"/"Optional"): the user picks, through a synthetic installer.
    case multiple([RootOption])
    case none
}

enum ContentLayoutAnalyzer {
    static let maxRootDepth = 4

    static func analyze(_ entries: [SourceEntry]) -> ContentLayout {
        let files = entries.filter { !$0.isDirectory }
        if let fomod = findFomod(files) { return fomod }

        // Folder -> names of its child folders (lower-case), and every folder that exists.
        var children: [String: Set<String>] = ["": []]
        func registerFolder(_ path: String) {
            let parts = path.split(separator: "/").map(String.init)
            var parent = ""
            for part in parts {
                let current = parent.isEmpty ? part : parent + "/" + part
                children[parent, default: []].insert(part.lowercased())
                if children[current] == nil { children[current] = [] }
                parent = current
            }
        }
        for entry in entries {
            if entry.isDirectory { registerFolder(entry.path) } else if let slash = entry.path.lastIndex(of: "/") {
                registerFolder(String(entry.path[..<slash]))
            }
        }

        let marked = minimalRoots(in: children) { !$0.isDisjoint(with: ModPath.mountedRoots) }
        let bare = marked.isEmpty ? minimalRoots(in: children) { !$0.isDisjoint(with: ModPath.gamedataSubfolders) } : []
        let roots = marked.isEmpty ? bare : marked
        let destination = marked.isEmpty ? "gamedata" : ""
        if roots.isEmpty { return .none }

        let options = roots.map { root -> RootOption in
            let mine = files.filter { ModPath.isUnder($0.path, prefix: root) }
            return RootOption(prefix: root, destination: destination, displayName: root,
                              fileCount: mine.count, sizeBytes: mine.reduce(0) { $0 + $1.size })
        }
        if options.count == 1 { return .single(options[0]) }
        return .multiple(nameOptions(options))
    }

    /// Shallowest folders whose children satisfy `marker`, skipping folders that sit inside a game folder
    /// (`gamedata/…/db`) and any folder under an already-found root.
    private static func minimalRoots(in children: [String: Set<String>], marker: (Set<String>) -> Bool) -> [String] {
        var found: [String] = []
        let candidates = children.keys.sorted { a, b in
            let da = a.isEmpty ? 0 : a.split(separator: "/").count, db = b.isEmpty ? 0 : b.split(separator: "/").count
            return da != db ? da < db : ModNaming.naturalLess(a, b)
        }
        for path in candidates {
            let parts = path.split(separator: "/").map { $0.lowercased() }
            if parts.count > maxRootDepth { continue }
            // inside a gamedata/db/... folder, or inside another game-content folder: not a mod root
            if parts.contains(where: { ModPath.mountedRoots.contains($0) || ModPath.gamedataSubfolders.contains($0) }) { continue }
            if found.contains(where: { ModPath.isUnder(path, prefix: $0) }) { continue }
            if marker(children[path] ?? []) { found.append(path) }
        }
        return found
    }

    /// Names the options by the part of their path that differs (`Main`, `Optional/Patch`).
    private static func nameOptions(_ options: [RootOption]) -> [RootOption] {
        var common = options[0].prefix.split(separator: "/").map(String.init)
        for option in options.dropFirst() {
            let parts = option.prefix.split(separator: "/").map(String.init)
            var n = 0
            while n < common.count, n < parts.count, common[n].lowercased() == parts[n].lowercased() { n += 1 }
            common = Array(common.prefix(n))
        }
        // A root can't be both an option and the shared parent of another one.
        let drop = min(common.count, options.map { $0.prefix.split(separator: "/").count - 1 }.min() ?? 0)
        return options.map { o in
            let parts = o.prefix.split(separator: "/").map(String.init)
            let name = parts.dropFirst(drop).joined(separator: "/")
            return RootOption(prefix: o.prefix, destination: o.destination, displayName: name.isEmpty ? "(everything)" : name,
                              fileCount: o.fileCount, sizeBytes: o.sizeBytes)
        }.sorted { ModNaming.naturalLess($0.displayName, $1.displayName) }
    }

    private static func findFomod(_ files: [SourceEntry]) -> ContentLayout? {
        var best: (root: String, config: String, info: String?, depth: Int)?
        for file in files {
            let parts = file.path.split(separator: "/").map(String.init)
            guard parts.count >= 2, parts.count <= maxRootDepth + 2,
                  parts[parts.count - 1].lowercased() == "moduleconfig.xml",
                  parts[parts.count - 2].lowercased() == "fomod" else { continue }
            let root = parts.dropLast(2).joined(separator: "/")
            let depth = parts.count
            if let best, best.depth <= depth { continue }
            let infoPath = files.first {
                ModPath.key($0.path) == ModPath.key(ModPath.join(ModPath.join(root, parts[parts.count - 2]), "info.xml"))
            }?.path
            best = (root, file.path, infoPath, depth)
        }
        guard let best else { return nil }
        return .fomod(root: best.root, config: best.config, info: best.info)
    }

    // MARK: - Warnings

    /// What in the source cannot work on this port (executables, `bin/`) or will not be loaded (outside gamedata/db).
    static func warnings(for entries: [SourceEntry], root: String, destination: String) -> [String] {
        var warnings: [String] = []
        let files = entries.filter { !$0.isDirectory && ModPath.isUnder($0.path, prefix: root) }
        let programs = files.filter { ModPath.isProgram($0.path) }
        if !programs.isEmpty {
            let names = programs.prefix(3).map { ModPath.lastComponent($0.path) }.joined(separator: ", ")
            warnings.append("Contains \(programs.count) Windows program file\(programs.count == 1 ? "" : "s") (\(names)\(programs.count > 3 ? ", …" : "")). They cannot run here and are not installed; a mod that needs them may not work.")
        }
        let rel = files.compactMap { ModPath.relative($0.path, to: root) }
        let binCount = rel.filter { ["bin", "bin_x64", "bin_x86", "bin_arm64"].contains($0.split(separator: "/").first?.lowercased() ?? "") && !ModPath.isProgram($0) }.count
        if binCount > 0 {
            warnings.append("Contains \(binCount) file\(binCount == 1 ? "" : "s") in bin/, which this port does not use.")
        }
        if destination.isEmpty {
            let outside = rel.filter { !ModPath.mountedRoots.contains($0.split(separator: "/").first?.lowercased() ?? "") }
                .filter { !isDocumentation($0) && !isBin($0) && !ModPath.isProgram($0) }
            if !outside.isEmpty {
                warnings.append("\(outside.count) file\(outside.count == 1 ? " is" : "s are") outside gamedata/ and db/ and will not be loaded.")
            }
        }
        return warnings
    }

    private static func isBin(_ rel: String) -> Bool {
        ["bin", "bin_x64", "bin_x86", "bin_arm64"].contains(rel.split(separator: "/").first?.lowercased() ?? "")
    }

    static func isDocumentation(_ rel: String) -> Bool {
        let ext = ModPath.fileExtension(rel)
        return ["txt", "md", "rtf", "pdf", "doc", "docx", "url", "html", "htm", "jpg", "jpeg", "png", "gif", "webp"].contains(ext)
            && !rel.contains("/")
    }
}
