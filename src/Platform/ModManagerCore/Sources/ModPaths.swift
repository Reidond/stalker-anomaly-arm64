import Foundation

/// Path rules shared by the importer, the FOMOD resolver and the conflict scan.
enum ModPath {
    /// Folders under `gamedata/` that a mod archive may ship without the `gamedata/` folder itself.
    static let gamedataSubfolders: Set<String> = [
        "anims", "configs", "levels", "meshes", "scripts", "shaders", "sounds", "spawns", "textures",
    ]
    /// Folders the engine mounts from a mod (contract point 4).
    static let mountedRoots: Set<String> = ["gamedata", "db"]
    /// Windows programs: never installed (they cannot run here), reported as a warning.
    static let programExtensions: Set<String> = ["exe", "dll", "msi", "bat", "cmd", "com", "scr", "ps1", "vbs", "dylib", "so"]
    /// Files archives carry that are not part of the mod.
    static let junkNames: Set<String> = [".ds_store", "thumbs.db", "desktop.ini"]

    /// Lower-case comparison key, as the engine resolves paths.
    static func key(_ path: String) -> String { path.lowercased() }

    /// Normalizes an archive or FOMOD path: `/` separators, no empty or `.` components. Returns nil for an empty
    /// path and throws for a path that leaves its root (absolute, drive letter, `..`, control characters).
    static func sanitize(_ raw: String) throws -> String? {
        if raw.unicodeScalars.contains(where: { $0.value < 0x20 || $0.value == 0x7f }) {
            throw ModImportError.unsafePath(raw)
        }
        let slashed = raw.replacingOccurrences(of: "\\", with: "/")
        if slashed.hasPrefix("/") { throw ModImportError.unsafePath(raw) }
        var parts: [Substring] = []
        for part in slashed.split(separator: "/", omittingEmptySubsequences: true) {
            if part == "." { continue }
            if part == ".." { throw ModImportError.unsafePath(raw) }
            parts.append(part)
        }
        if let first = parts.first, first.count == 2, first.hasSuffix(":") { throw ModImportError.unsafePath(raw) }
        return parts.isEmpty ? nil : parts.joined(separator: "/")
    }

    /// Like `sanitize` for the paths a FOMOD config names (they may be empty, `..` is an error).
    static func sanitizeLoose(_ raw: String) throws -> String {
        try sanitize(raw.trimmingCharacters(in: .whitespacesAndNewlines)) ?? ""
    }

    static func components(_ path: String) -> [Substring] { path.split(separator: "/") }

    static func lastComponent(_ path: String) -> String {
        path.split(separator: "/").last.map(String.init) ?? path
    }

    static func fileExtension(_ path: String) -> String {
        let name = lastComponent(path)
        guard let dot = name.lastIndex(of: "."), dot != name.startIndex else { return "" }
        return String(name[name.index(after: dot)...]).lowercased()
    }

    static func join(_ a: String, _ b: String) -> String {
        if a.isEmpty { return b }
        if b.isEmpty { return a }
        return a + "/" + b
    }

    static func isJunk(_ path: String) -> Bool {
        let parts = components(path)
        if parts.contains(where: { $0.lowercased() == "__macosx" }) { return true }
        if let last = parts.last, junkNames.contains(last.lowercased()) { return true }
        // AppleDouble forks next to the real file
        if let last = parts.last, last.hasPrefix("._") { return true }
        return false
    }

    static func isProgram(_ path: String) -> Bool { programExtensions.contains(fileExtension(path)) }

    /// True when `path` is `prefix` or lies under it (case-insensitive; `prefix` has no trailing slash, "" is everything).
    static func isUnder(_ path: String, prefix: String) -> Bool {
        if prefix.isEmpty { return true }
        let p = path.lowercased(), q = prefix.lowercased()
        return p == q || p.hasPrefix(q + "/")
    }

    /// `path` below `prefix`, or nil when it is not under it.
    static func relative(_ path: String, to prefix: String) -> String? {
        if prefix.isEmpty { return path }
        let p = path.lowercased(), q = prefix.lowercased()
        if p == q { return "" }
        guard p.hasPrefix(q + "/") else { return nil }
        return String(path.dropFirst(prefix.count + 1))
    }
}

/// Naming helpers: ids, display names and versions taken from archive file names.
enum ModNaming {
    /// Lower-case ASCII slug, `-` separated, at most 48 characters; never empty.
    static func slug(_ text: String) -> String {
        var s = text
        if let latin = s.applyingTransform(.toLatin, reverse: false) { s = latin }
        s = s.folding(options: [.diacriticInsensitive, .caseInsensitive, .widthInsensitive], locale: nil).lowercased()
        var out = ""
        var lastDash = true
        for scalar in s.unicodeScalars {
            if (scalar.value >= 0x61 && scalar.value <= 0x7a) || (scalar.value >= 0x30 && scalar.value <= 0x39) {
                out.unicodeScalars.append(scalar)
                lastDash = false
            } else if !lastDash {
                out.append("-")
                lastDash = true
            }
        }
        while out.hasSuffix("-") { out.removeLast() }
        if out.count > 48 {
            out = String(out.prefix(48))
            while out.hasSuffix("-") { out.removeLast() }
        }
        return out.isEmpty ? "mod" : out
    }

    /// `base`, then `base-2`, `base-3`, ... until `taken` no longer holds the id.
    static func unique(_ base: String, taken: (String) -> Bool) -> String {
        if !taken(base) { return base }
        var n = 2
        while taken("\(base)-\(n)") { n += 1 }
        return "\(base)-\(n)"
    }

    /// File extensions that are an archive's, not part of a mod's name ("Mod 1.2" has no extension, "Mod 1.2.zip" has).
    private static let archiveExtensions: Set<String> = ["zip", "zipx", "7z", "rar", "tar", "gz", "tgz", "bz2", "xz", "exe"]

    /// "Weapon_Pack-v1.4.2.7z" -> ("Weapon Pack", "1.4.2"); "Cool Mod-1234-1-5-1678.zip" (a Nexus name) -> ("Cool Mod", "1.5").
    static func nameAndVersion(fromFileName fileName: String, isFolder: Bool = false) -> (name: String, version: String?) {
        var base = fileName
        if !isFolder {
            let ext = (fileName as NSString).pathExtension.lowercased()
            if archiveExtensions.contains(ext) { base = (fileName as NSString).deletingPathExtension }
            // "name.part1.rar" and "name.tar.gz"
            base = base.replacingOccurrences(of: #"\.(?:part\d+|tar)$"#, with: "", options: [.regularExpression, .caseInsensitive])
        }
        // "Mod (1)" copies made by browsers
        base = base.replacingOccurrences(of: #"\s*\(\d+\)$"#, with: "", options: .regularExpression)
        var version: String?

        // Nexus: Name-<mod id>-<version parts>-<unix time>
        if let m = firstMatch(#"^(.+?)-(\d{2,6})-((?:\d+-)*\d+)-(\d{9,11})$"#, in: base), m.count == 5 {
            version = m[3].replacingOccurrences(of: "-", with: ".")
            base = m[1]
        } else if let m = firstMatch(#"^(.*?)[\s_\-\.]+[vV]?(\d+(?:[._]\d+)+[a-zA-Z]?)(?:[\s_\-](?:hotfix|fix|patch|final|beta|alpha|rc|release)\d*)?$"#, in: base), m.count == 3, !m[1].isEmpty {
            version = m[2].replacingOccurrences(of: "_", with: ".")
            base = m[1]
        } else if let m = firstMatch(#"^(.*?)[\s_\-\.]+[vV](\d+[a-zA-Z]?)$"#, in: base), m.count == 3, !m[1].isEmpty {
            version = m[2]
            base = m[1]
        }
        base = base.replacingOccurrences(of: "_", with: " ")
        // "a-b-c" with no spaces reads better with spaces
        if !base.contains(" ") { base = base.replacingOccurrences(of: "-", with: " ") }
        let name = base.split(separator: " ", omittingEmptySubsequences: true).joined(separator: " ")
        return (name.isEmpty ? fileName : name, version)
    }

    private static func firstMatch(_ pattern: String, in text: String) -> [String]? {
        guard let re = try? NSRegularExpression(pattern: pattern) else { return nil }
        let ns = text as NSString
        guard let m = re.firstMatch(in: text, range: NSRange(location: 0, length: ns.length)) else { return nil }
        return (0 ..< m.numberOfRanges).map { i in
            let r = m.range(at: i)
            return r.location == NSNotFound ? "" : ns.substring(with: r)
        }
    }

    /// Natural order ("2 Foo" before "10 Bar") used for option folders.
    static func naturalLess(_ a: String, _ b: String) -> Bool {
        a.compare(b, options: [.caseInsensitive, .numeric]) == .orderedAscending
    }
}
