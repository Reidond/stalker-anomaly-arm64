import Foundation

// The FOMOD model (installer schema 4.0/5.0): what the config describes, with the order of steps, groups and plugins already
// applied. `FomodSession` evaluates it against the user's choices.

public enum FomodGroupType: String {
    case selectAtLeastOne = "SelectAtLeastOne"
    case selectAtMostOne = "SelectAtMostOne"
    case selectExactlyOne = "SelectExactlyOne"
    case selectAll = "SelectAll"
    case selectAny = "SelectAny"

    init(attribute: String?) {
        let wanted = (attribute ?? "").trimmingCharacters(in: .whitespaces).lowercased()
        self = [FomodGroupType.selectAtLeastOne, .selectAtMostOne, .selectExactlyOne, .selectAll, .selectAny]
            .first { $0.rawValue.lowercased() == wanted } ?? .selectAny
    }

    /// At most one plugin may be on (radio buttons).
    public var isSingleChoice: Bool { self == .selectExactlyOne || self == .selectAtMostOne }
}

public enum FomodPluginType: String {
    case required = "Required"
    case optional = "Optional"
    case recommended = "Recommended"
    case notUsable = "NotUsable"
    case couldBeUsable = "CouldBeUsable"

    init(attribute: String?) {
        let wanted = (attribute ?? "").trimmingCharacters(in: .whitespaces).lowercased()
        self = [FomodPluginType.required, .optional, .recommended, .notUsable, .couldBeUsable]
            .first { $0.rawValue.lowercased() == wanted } ?? .optional
    }
}

public enum FomodFileState { case active, inactive, missing }

/// What the installer may ask about the outside world: whether a file is present in the game folder or an enabled mod.
public struct FomodEnvironment {
    public var fileState: (String) -> FomodFileState
    public init(fileState: @escaping (String) -> FomodFileState = { _ in .missing }) { self.fileState = fileState }
}

/// A dependency tree: flags set by earlier choices, files, nested groups with And/Or.
indirect enum FomodCondition {
    case all([FomodCondition])
    case any([FomodCondition])
    case flag(name: String, value: String)
    case file(path: String, state: FomodFileState)
    /// game, script-extender and other version checks: always satisfied (there is no such thing to check here)
    case satisfied

    func evaluate(flags: [String: String], environment: FomodEnvironment) -> Bool {
        switch self {
        case .all(let items): return items.allSatisfy { $0.evaluate(flags: flags, environment: environment) }
        case .any(let items): return items.isEmpty || items.contains { $0.evaluate(flags: flags, environment: environment) }
        case .flag(let name, let value): return (flags[name.lowercased()] ?? "").lowercased() == value.lowercased()
        case .file(let path, let state): return environment.fileState(path) == state
        case .satisfied: return true
        }
    }
}

enum FomodTypeDescriptor {
    case fixed(FomodPluginType)
    case dependent(defaultType: FomodPluginType, patterns: [(condition: FomodCondition, type: FomodPluginType)])
}

/// A `<file>` or `<folder>` entry.
struct FomodFileEntry {
    let source: String            // relative to the installer root, `/` separators, case as written
    let destination: String?      // nil: omitted; "": explicitly empty
    /// The destination was written with a trailing slash: for a `<file>` it names a folder, not the new file name.
    let destinationIsDirectory: Bool
    let isFolder: Bool
    let alwaysInstall: Bool
    let installIfUsable: Bool
    let priority: Int
}

public struct FomodPlugin: Identifiable {
    public var id: String { name }
    public let name: String
    public let description: String
    public let imagePath: String?
    let flags: [(name: String, value: String)]
    let files: [FomodFileEntry]
    let typeDescriptor: FomodTypeDescriptor
}

public struct FomodGroup: Identifiable {
    public var id: String { name }
    public let name: String
    public let type: FomodGroupType
    public let plugins: [FomodPlugin]
}

public struct FomodStep: Identifiable {
    public var id: String { name }
    public let name: String
    let visible: FomodCondition?
    public let groups: [FomodGroup]
}

struct FomodConditionalInstall {
    let condition: FomodCondition
    let files: [FomodFileEntry]
}

/// What `fomod/info.xml` says about the mod.
public struct FomodInfo {
    public var name: String?
    public var author: String?
    public var version: String?
    public var description: String?
    public var website: String?
}

/// A parsed installer, or the synthetic one built for an archive with several option folders.
public final class FomodInstaller {
    public let moduleName: String
    public let info: FomodInfo
    public let moduleImagePath: String?
    public let steps: [FomodStep]
    /// True for the installer made up from option folders (no ModuleConfig.xml behind it).
    public let isSynthetic: Bool
    let requiredFiles: [FomodFileEntry]
    let conditionalInstalls: [FomodConditionalInstall]
    /// Where the installer's paths start inside the source (the folder holding `fomod/`).
    let rootPrefix: String
    let assets: FomodAssets

    init(moduleName: String, info: FomodInfo, moduleImagePath: String?, steps: [FomodStep], requiredFiles: [FomodFileEntry],
         conditionalInstalls: [FomodConditionalInstall], rootPrefix: String, isSynthetic: Bool, assets: FomodAssets) {
        self.moduleName = moduleName
        self.info = info
        self.moduleImagePath = moduleImagePath
        self.steps = steps
        self.requiredFiles = requiredFiles
        self.conditionalInstalls = conditionalInstalls
        self.rootPrefix = rootPrefix
        self.isSynthetic = isSynthetic
        self.assets = assets
    }

    /// File on disk for an image path written in the config, if the archive had it.
    public func imageURL(for path: String?) -> URL? {
        guard let path else { return nil }
        return assets.url(forInstallerPath: path)
    }

    /// The choices a user gets by pressing Next all the way: the defaults of every visible step.
    public func defaultChoices(environment: FomodEnvironment = FomodEnvironment()) -> FomodChoices {
        let session = FomodSession(installer: self, environment: environment)
        while session.canGoNext { session.next() }
        return session.choices()
    }
}

/// The user's selections, by name: for each step shown, for each group, the plugins that are on. Stored in mod.json.
public struct FomodChoices: Codable, Hashable {
    public struct Selection: Codable, Hashable {
        public var step: String
        public var group: String
        public var plugins: [String]
        public init(step: String, group: String, plugins: [String]) {
            self.step = step
            self.group = group
            self.plugins = plugins
        }
    }

    public var selections: [Selection]
    public init(selections: [Selection] = []) { self.selections = selections }
}

/// Files of the installer (config, images) copied out of the source for the wizard; removed with the object.
final class FomodAssets {
    let directory: URL
    /// Where the installer's relative paths start inside the source.
    var rootPrefix = ""
    /// archive path (lower-case) -> file name in `directory`
    private var files: [String: String] = [:]
    private var counter = 0
    private(set) var capturedBytes: Int64 = 0

    init() {
        directory = FileManager.default.temporaryDirectory.appendingPathComponent("ModManagerFomod-\(UUID().uuidString)", isDirectory: true)
    }

    deinit { try? FileManager.default.removeItem(at: directory) }

    /// Reserves a file for the source entry at `archivePath` and returns where to write it.
    func reserve(archivePath: String, size: Int64) -> URL {
        counter += 1
        capturedBytes += size
        let ext = ModPath.fileExtension(archivePath)
        let name = "\(counter)." + (ext.isEmpty ? "dat" : ext)
        files[ModPath.key(archivePath)] = name
        return directory.appendingPathComponent(name)
    }

    func url(forArchivePath path: String) -> URL? {
        guard let name = files[ModPath.key(path)] else { return nil }
        let url = directory.appendingPathComponent(name)
        return FileManager.default.fileExists(atPath: url.path) ? url : nil
    }

    /// A path as written in the config (relative to the installer root, either slash).
    func url(forInstallerPath path: String) -> URL? {
        guard let clean = try? ModPath.sanitize(path) else { return nil }
        return url(forArchivePath: ModPath.join(rootPrefix, clean))
    }
}
