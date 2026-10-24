import Foundation

/// File names for downloads: Content-Disposition parsing, sanitising and collision-free placement.
enum FileNaming {
    struct Disposition: Equatable {
        var isAttachment: Bool
        var fileName: String?
    }

    /// Parses a Content-Disposition header (RFC 6266): `filename*` (RFC 5987) wins over `filename`.
    static func parseContentDisposition(_ header: String?) -> Disposition {
        guard let header, !header.isEmpty else { return Disposition(isAttachment: false, fileName: nil) }
        let parts = splitOutsideQuotes(header, separator: ";")
        let type = parts.first?.trimmingCharacters(in: .whitespaces).lowercased() ?? ""
        var plain: String?
        var extended: String?
        for part in parts.dropFirst() {
            guard let eq = part.firstIndex(of: "=") else { continue }
            let key = part[..<eq].trimmingCharacters(in: .whitespaces).lowercased()
            var value = part[part.index(after: eq)...].trimmingCharacters(in: .whitespaces)
            if key == "filename*" {
                extended = decodeExtendedValue(value)
            } else if key == "filename" {
                if value.hasPrefix("\"") {
                    value = unquote(value)
                }
                plain = repairMojibake(value)
            }
        }
        return Disposition(isAttachment: type == "attachment", fileName: extended ?? plain)
    }

    private static func splitOutsideQuotes(_ s: String, separator: Character) -> [String] {
        var result: [String] = []
        var current = ""
        var inQuotes = false
        var escaped = false
        for ch in s {
            if escaped {
                current.append(ch)
                escaped = false
            } else if ch == "\\" && inQuotes {
                current.append(ch)
                escaped = true
            } else if ch == "\"" {
                inQuotes.toggle()
                current.append(ch)
            } else if ch == separator && !inQuotes {
                result.append(current)
                current = ""
            } else {
                current.append(ch)
            }
        }
        result.append(current)
        return result
    }

    /// "..." with \" and \\ escapes. A missing closing quote is tolerated.
    private static func unquote(_ value: String) -> String {
        var out = ""
        var escaped = false
        for ch in value.dropFirst() {
            if escaped {
                out.append(ch)
                escaped = false
            } else if ch == "\\" {
                escaped = true
            } else if ch == "\"" {
                break
            } else {
                out.append(ch)
            }
        }
        return out
    }

    /// `UTF-8''na%C3%AFve.zip` (charset, optional language, percent-encoded bytes).
    private static func decodeExtendedValue(_ value: String) -> String? {
        let pieces = value.split(separator: "'", maxSplits: 2, omittingEmptySubsequences: false)
        guard pieces.count == 3 else { return nil }
        let charset = pieces[0].lowercased()
        let encoding: String.Encoding
        switch charset {
        case "utf-8", "utf8", "": encoding = .utf8
        case "iso-8859-1", "latin1": encoding = .isoLatin1
        default: return nil
        }
        var bytes: [UInt8] = []
        let raw = Array(pieces[2].utf8)
        var i = 0
        while i < raw.count {
            if raw[i] == UInt8(ascii: "%"), i + 2 < raw.count,
               let hi = hexValue(raw[i + 1]), let lo = hexValue(raw[i + 2]) {
                bytes.append(UInt8(hi << 4 | lo))
                i += 3
                continue
            }
            bytes.append(raw[i])
            i += 1
        }
        return String(bytes: bytes, encoding: encoding)
    }

    private static func hexValue(_ c: UInt8) -> Int? {
        switch c {
        case UInt8(ascii: "0")...UInt8(ascii: "9"): return Int(c) - Int(UInt8(ascii: "0"))
        case UInt8(ascii: "a")...UInt8(ascii: "f"): return Int(c) - Int(UInt8(ascii: "a")) + 10
        case UInt8(ascii: "A")...UInt8(ascii: "F"): return Int(c) - Int(UInt8(ascii: "A")) + 10
        default: return nil
        }
    }

    /// Header bytes that are really UTF-8 arrive decoded as Latin-1 ("naÃ¯ve"); turn them back.
    static func repairMojibake(_ s: String) -> String {
        guard s.unicodeScalars.contains(where: { $0.value >= 0x80 }),
              s.unicodeScalars.allSatisfy({ $0.value <= 0xFF }),
              let latin1 = s.data(using: .isoLatin1),
              let utf8 = String(data: latin1, encoding: .utf8) else { return s }
        return utf8
    }

    // MARK: Sanitising

    /// A single path component that is safe to create in the downloads folder: no separators, no control characters,
    /// no leading dot (the partial files and the folder itself are dot files), at most 200 UTF-8 bytes.
    static func sanitize(_ name: String) -> String {
        var s = name.replacingOccurrences(of: "\\", with: "/")
        if let last = s.split(separator: "/", omittingEmptySubsequences: true).last { s = String(last) }
        s = String(String.UnicodeScalarView(s.unicodeScalars.filter { scalar in
            !(scalar.value < 0x20 || scalar.value == 0x7F || scalar == ":" || (0x80...0x9F).contains(scalar.value)
              || scalar == "\u{202E}" || scalar == "\u{200E}" || scalar == "\u{200F}")
        }))
        s = s.trimmingCharacters(in: .whitespacesAndNewlines)
        while s.hasPrefix(".") { s.removeFirst() }
        s = s.trimmingCharacters(in: .whitespaces)
        if s.isEmpty { return "download" }
        return truncate(s, maxBytes: 200)
    }

    private static func truncate(_ s: String, maxBytes: Int) -> String {
        guard s.utf8.count > maxBytes else { return s }
        let ns = s as NSString
        let ext = ns.pathExtension
        let extPart = ext.isEmpty || ext.utf8.count > 16 ? "" : "." + ext
        let stem = extPart.isEmpty ? s : ns.deletingPathExtension
        var out = ""
        for ch in stem {
            if out.utf8.count + String(ch).utf8.count + extPart.utf8.count > maxBytes { break }
            out.append(ch)
        }
        return out + extPart
    }

    /// The name for a response: Content-Disposition, else the last path component of the final URL, else "download";
    /// a name without an extension gets one from a few archive MIME types.
    static func fileName(contentDisposition: String?, mimeType: String?, finalURL: URL) -> String {
        var name = parseContentDisposition(contentDisposition).fileName
        if name?.trimmingCharacters(in: .whitespaces).isEmpty ?? true {
            let last = finalURL.lastPathComponent.removingPercentEncoding ?? finalURL.lastPathComponent
            name = last == "/" ? nil : last
        }
        var result = sanitize(name ?? "")
        if (result as NSString).pathExtension.isEmpty, let ext = pathExtension(forMIME: mimeType) {
            result += "." + ext
        }
        return result
    }

    static func pathExtension(forMIME mime: String?) -> String? {
        switch mime?.lowercased() {
        case "application/zip", "application/x-zip-compressed": return "zip"
        case "application/x-7z-compressed": return "7z"
        case "application/vnd.rar", "application/x-rar-compressed", "application/x-rar": return "rar"
        default: return nil
        }
    }

    // MARK: Placement

    /// `dir/name`, or `dir/stem (1).ext`, `(2)` ... when the name is taken. The returned URL does not exist (at the time of the call).
    static func uniqueURL(in dir: URL, name: String) -> URL {
        let fm = FileManager.default
        var candidate = dir.appendingPathComponent(name)
        guard fm.fileExists(atPath: candidate.path) else { return candidate }
        let ns = name as NSString
        let ext = ns.pathExtension
        let stem = ext.isEmpty ? name : ns.deletingPathExtension
        var n = 1
        repeat {
            let numbered = ext.isEmpty ? "\(stem) (\(n))" : "\(stem) (\(n)).\(ext)"
            candidate = dir.appendingPathComponent(numbered)
            n += 1
        } while fm.fileExists(atPath: candidate.path)
        return candidate
    }

    /// Moves `source` to `dir/name` (made unique) and returns where it went. Retries when another download took the name in between.
    static func moveUnique(_ source: URL, to dir: URL, name: String) throws -> URL {
        let fm = FileManager.default
        var lastError: Error?
        for _ in 0..<50 {
            let target = uniqueURL(in: dir, name: name)
            do {
                try fm.moveItem(at: source, to: target)
                return target
            } catch let error as CocoaError where error.code == .fileWriteFileExists {
                lastError = error
            }
        }
        throw lastError ?? CocoaError(.fileWriteFileExists)
    }
}
