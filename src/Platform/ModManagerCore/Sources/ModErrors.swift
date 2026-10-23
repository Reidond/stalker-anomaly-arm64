import Foundation

/// Why an import or a library change failed. Every case has a message fit to show to the user.
public enum ModImportError: Error, LocalizedError, Equatable {
    case notFound(String)
    case unsupportedFormat(String)
    case corruptArchive(String)
    case encrypted
    case unsafePath(String)
    case symlinkRefused(String)
    case unsupportedEntry(String)
    case tooManyEntries(Int)
    case noGameContent
    case nothingSelected
    case insufficientSpace(needed: Int64, available: Int64)
    case diskFull
    case invalidFomod(String)
    case archiveChanged
    case io(String)

    public var errorDescription: String? {
        switch self {
        case .notFound(let what): return "\(what) was not found."
        case .unsupportedFormat(let why): return "This file is not a supported archive (zip, 7z, rar) or folder: \(why)"
        case .corruptArchive(let why): return "The archive is damaged or incomplete: \(why)"
        case .encrypted: return "The archive is password protected. Remove the password and add it again."
        case .unsafePath(let p): return "The archive contains a path that leaves the mod folder (\(p)); it was not installed."
        case .symlinkRefused(let p): return "The archive contains a link (\(p)); links are not allowed in mods."
        case .unsupportedEntry(let p): return "The archive contains a special file (\(p)) that cannot be installed."
        case .tooManyEntries(let n): return "The archive lists \(n) entries, more than a mod can have."
        case .noGameContent:
            return "No game files found: the archive has no gamedata or db folder (and no scripts, configs, textures, meshes or sounds folders)."
        case .nothingSelected: return "The options chosen install no files."
        case .insufficientSpace(let needed, let available):
            let f = ByteCountFormatter()
            return "Not enough free space: the mod needs \(f.string(fromByteCount: needed)), \(f.string(fromByteCount: available)) is free."
        case .diskFull: return "The device ran out of free space while installing."
        case .invalidFomod(let why): return "The FOMOD installer is not valid: \(why)"
        case .archiveChanged: return "The archive changed while it was being installed."
        case .io(let why): return why
        }
    }
}
