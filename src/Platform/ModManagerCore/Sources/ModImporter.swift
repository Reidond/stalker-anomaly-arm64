import CryptoKit
import Foundation

/// What `ModImporter.inspect` found out about a file or folder, ready to install.
public struct ImportPlan {
    public let displayName: String
    public let source: URL
    /// The installer to show before installing: a real FOMOD, or the synthetic one for an archive with option folders.
    public let fomod: FomodInstaller?
    public let warnings: [String]

    // Beyond the shared contract.
    public let version: String?
    public let author: String?
    public let details: String?
    /// Files and bytes in the source that belong to the mod (all options counted).
    public let fileCount: Int
    public let sizeBytes: Int64

    let internals: PlanInternals

    /// A plan that is only shown (previews, tests of a UI). `ModImporter.install` refuses it: use `inspect` for a real one.
    public init(displayName: String, source: URL, fomod: FomodInstaller?, warnings: [String]) {
        self.displayName = displayName
        self.source = source
        self.fomod = fomod
        self.warnings = warnings
        version = nil
        author = nil
        details = nil
        fileCount = 0
        sizeBytes = 0
        internals = PlanInternals(contentSource: FolderContentSource(url: source), entries: [], layout: .none,
                                  origin: ModOrigin(), scope: SecurityScope(source), installerRoot: "", isPreview: true)
    }

    init(displayName: String, source: URL, fomod: FomodInstaller?, warnings: [String], version: String?, author: String?,
         details: String?, fileCount: Int, sizeBytes: Int64, internals: PlanInternals) {
        self.displayName = displayName
        self.source = source
        self.fomod = fomod
        self.warnings = warnings
        self.version = version
        self.author = author
        self.details = details
        self.fileCount = fileCount
        self.sizeBytes = sizeBytes
        self.internals = internals
    }
}

final class PlanInternals {
    let contentSource: ContentSource
    let entries: [SourceEntry]
    let layout: ContentLayout
    let origin: ModOrigin
    let scope: SecurityScope
    let installerRoot: String
    let isPreview: Bool

    init(contentSource: ContentSource, entries: [SourceEntry], layout: ContentLayout, origin: ModOrigin, scope: SecurityScope,
         installerRoot: String, isPreview: Bool = false) {
        self.isPreview = isPreview
        self.contentSource = contentSource
        self.entries = entries
        self.layout = layout
        self.origin = origin
        self.scope = scope
        self.installerRoot = installerRoot
    }
}

/// Keeps a security-scoped URL (a file picked in the Files app) open for as long as the plan lives.
final class SecurityScope {
    private let url: URL
    private let started: Bool
    init(_ url: URL) {
        self.url = url
        started = url.startAccessingSecurityScopedResource()
    }

    deinit { if started { url.stopAccessingSecurityScopedResource() } }
}

/// Adds mods to a `ModLibrary` from archives (.zip, .7z, .rar) and folders.
///
/// `inspect` reads the archive's table of contents (and the FOMOD files), finds the mod root and builds a plan. `install`
/// extracts into a staging folder inside the mods root and moves it into place only when everything worked, so a cancel or a
/// failure never leaves half a mod in the library. Both run off the calling thread and honor Task cancellation.
public final class ModImporter {
    private let library: ModLibrary
    /// How the installer asks about files named in `fileDependency` (e.g. "is gamedata/scripts/x.script there?").
    /// The default looks into the enabled mods of the library.
    public var environment: FomodEnvironment
    /// Test seam: called on the extraction thread whenever a chunk has been written.
    var extractionHook: (() -> Void)?

    public init(library: ModLibrary) {
        self.library = library
        environment = FomodEnvironment()
        environment = FomodEnvironment(fileState: { [weak library] path in
            guard let library else { return .missing }
            let clean = (try? ModPath.sanitize(path)) ?? nil
            guard let rel = clean else { return .missing }
            for mod in library.mods where mod.enabled {
                if FileManager.default.fileExists(atPath: mod.contentURL.appendingPathComponent(rel).path) { return .active }
            }
            return .missing
        })
    }

    // MARK: - Inspect

    public func inspect(_ url: URL) async throws -> ImportPlan {
        try await inspect(url, origin: nil)
    }

    /// `origin` says where the file came from (a download); by default it is a local file.
    public func inspect(_ url: URL, origin: ModOrigin?) async throws -> ImportPlan {
        try await Self.runCancellable { cancel in try Self.buildPlan(url: url, origin: origin, cancel: cancel) }
    }

    private static func buildPlan(url: URL, origin: ModOrigin?, cancel: CancelFlag) throws -> ImportPlan {
        let scope = SecurityScope(url)
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory) else {
            throw ModImportError.notFound(url.lastPathComponent)
        }
        let source: ContentSource = isDirectory.boolValue ? FolderContentSource(url: url) : ArchiveContentSource(url: url)
        let assets = FomodAssets()
        var origin = origin ?? ModOrigin(kind: .file)
        if origin.fileName == nil { origin.fileName = url.lastPathComponent }

        let entries = try source.list(cancel: cancel) { entry in
            guard assets.capturedBytes < 96 << 20 else { return nil }
            let parts = entry.path.lowercased().split(separator: "/")
            let inFomodFolder = parts.dropLast().contains("fomod")
            let isImage = ["png", "jpg", "jpeg", "bmp", "gif", "webp"].contains(ModPath.fileExtension(entry.path))
            if (inFomodFolder && entry.size <= 32 << 20) || (isImage && entry.size <= 4 << 20) {
                return assets.reserve(archivePath: entry.path, size: entry.size)
            }
            return nil
        }
        try cancel.check()

        let fromFile = ModNaming.nameAndVersion(fromFileName: url.lastPathComponent, isFolder: isDirectory.boolValue)
        var layout = ContentLayoutAnalyzer.analyze(entries)
        var warnings: [String] = []
        var installer: FomodInstaller?
        var installerRoot = ""

        if case .fomod(let root, let configPath, let infoPath) = layout {
            do {
                guard let configURL = assets.url(forArchivePath: configPath) else {
                    throw ModImportError.invalidFomod("ModuleConfig.xml could not be read")
                }
                assets.rootPrefix = root
                let info = infoPath.flatMap { assets.url(forArchivePath: $0) }.flatMap { try? Data(contentsOf: $0) }
                installer = try FomodParser.parse(config: Data(contentsOf: configURL), info: info, rootPrefix: root, assets: assets)
                installerRoot = root
            } catch let error as ModImportError where { if case .unsafePath = error { return true } else { return false } }() {
                throw error   // an installer that names a path outside the mod is hostile, not merely broken
            } catch {
                warnings.append("The FOMOD installer could not be read (\(error.localizedDescription)); installing the files without it.")
                layout = ContentLayoutAnalyzer.analyze(entries.filter { !ModPath.components($0.path).contains { $0.lowercased() == "fomod" } })
            }
        }

        var displayName = fromFile.name
        var version = fromFile.version
        var author: String?
        var details: String?
        if let installer {
            let name = installer.info.name ?? (installer.moduleName.isEmpty ? nil : installer.moduleName)
            displayName = name ?? displayName
            version = installer.info.version ?? version
            author = installer.info.author
            details = installer.info.description
        }

        var plannedInstaller = installer
        var fileCount = entries.filter { !$0.isDirectory }.count
        var sizeBytes = entries.reduce(0) { $0 + $1.size }
        switch layout {
        case .fomod(let root, _, _):
            warnings += ContentLayoutAnalyzer.warnings(for: entries, root: root, destination: "gamedata")
        case .single(let option):
            warnings += ContentLayoutAnalyzer.warnings(for: entries, root: option.prefix, destination: option.destination)
            warnings += ignoredContentWarning(entries, root: option.prefix)
            fileCount = option.fileCount
            sizeBytes = option.sizeBytes
        case .multiple(let options):
            for option in options {
                warnings += ContentLayoutAnalyzer.warnings(for: entries, root: option.prefix, destination: option.destination)
                    .map { "\(option.displayName): \($0)" }
            }
            plannedInstaller = SyntheticInstaller.make(name: displayName, options: options, assets: assets)
            installerRoot = ""
        case .none:
            throw ModImportError.noGameContent
        }

        return ImportPlan(displayName: displayName, source: url, fomod: plannedInstaller, warnings: warnings, version: version,
                          author: author, details: details, fileCount: fileCount, sizeBytes: sizeBytes,
                          internals: PlanInternals(contentSource: source, entries: entries, layout: layout, origin: origin,
                                                   scope: scope, installerRoot: installerRoot))
    }

    /// Files beside the chosen root that look like game content: say that they are left out.
    private static func ignoredContentWarning(_ entries: [SourceEntry], root: String) -> [String] {
        guard !root.isEmpty else { return [] }
        let outside = entries.filter { !$0.isDirectory && !ModPath.isUnder($0.path, prefix: root) }
        let gameLike = outside.filter { entry in
            ModPath.components(entry.path).contains { part in
                let p = part.lowercased()
                return ModPath.mountedRoots.contains(p) || ModPath.gamedataSubfolders.contains(p)
            }
        }
        guard !gameLike.isEmpty else { return [] }
        return ["\(gameLike.count) more game file\(gameLike.count == 1 ? "" : "s") outside “\(root)” \(gameLike.count == 1 ? "is" : "are") not part of this install."]
    }

    // MARK: - Install

    public func install(_ plan: ImportPlan, choices: FomodChoices?, progress: @escaping (Double) -> Void) async throws -> InstalledMod {
        guard !plan.internals.isPreview else { throw ModImportError.io("This plan was not made by ModImporter.inspect and cannot be installed.") }
        let library = self.library
        let environment = self.environment
        let hook = extractionHook
        let job = try Self.makeJob(plan: plan, choices: choices, environment: environment)
        try Self.checkFreeSpace(root: library.root, needed: job.bytes)

        let staging = library.stagingURL.appendingPathComponent(UUID().uuidString, isDirectory: true)
        let reporter = ProgressReporter(callback: progress)
        let wantsChoices = plan.fomod != nil
        let usedChoices: FomodChoices? = wantsChoices ? (choices ?? plan.fomod?.defaultChoices(environment: environment)) : nil

        let manifest: ModManifest
        do {
            manifest = try await Self.runCancellable { cancel in
                try FileManager.default.createDirectory(at: staging.appendingPathComponent("content", isDirectory: true),
                                                        withIntermediateDirectories: true)
                let total = max(job.bytes, 1)
                var done: Int64 = 0
                try plan.internals.contentSource.extract(jobs: job.destinations, entries: plan.internals.entries,
                                                         into: staging.appendingPathComponent("content", isDirectory: true),
                                                         cancel: cancel) { delta in
                    done += delta
                    hook?()
                    reporter.report(min(Double(done) / Double(total), 1) * 0.97)
                }
                var origin = plan.internals.origin
                if plan.internals.contentSource.isArchive, origin.sha256 == nil {
                    origin.sha256 = try Self.sha256(of: plan.source, cancel: cancel)
                }
                try cancel.check()
                return ModManifest(id: ModNaming.slug(plan.displayName), name: plan.displayName, version: plan.version,
                                   author: plan.author, description: plan.details, source: origin, installedAt: Date(),
                                   fomodChoices: usedChoices?.selections, fileCount: job.destinations.values.reduce(0) { $0 + $1.count },
                                   sizeBytes: job.bytes)
            }
            try Task.checkCancellation()
        } catch {
            try? FileManager.default.removeItem(at: staging)
            throw error
        }

        let mod: InstalledMod
        do {
            mod = try await MainActor.run { try library.adopt(staging: staging, manifest: manifest, enabled: true) }
        } catch {
            try? FileManager.default.removeItem(at: staging)
            throw error
        }
        Self.deleteIfDownload(plan.source, downloads: library.downloadsURL)
        await MainActor.run { reporter.finish() }
        return mod
    }

    // MARK: Job

    struct Job {
        /// source entry index -> destinations below `content/`
        var destinations: [Int: [String]]
        var bytes: Int64
        var missing: [String]
    }

    static func makeJob(plan: ImportPlan, choices: FomodChoices?, environment: FomodEnvironment) throws -> Job {
        let internals = plan.internals
        var operations: [FomodFileEntry]
        var base = internals.installerRoot
        var singleOption: RootOption?
        switch internals.layout {
        case .single(let option):
            singleOption = option
            operations = []
        default:
            guard let installer = plan.fomod else { throw ModImportError.noGameContent }
            operations = installer.resolvedFiles(choices: choices, environment: environment)
            base = installer.rootPrefix
        }

        let files = internals.entries.filter { !$0.isDirectory }
        let index = PathIndex(files)
        var winners: [String: (path: String, entry: SourceEntry)] = [:]
        var missing: [String] = []

        func assign(_ rawDestination: String, _ entry: SourceEntry) throws {
            guard let destination = try ModPath.sanitize(rawDestination) else { return }
            if ModPath.isProgram(destination) || ModPath.isJunk(destination) { return }
            winners[ModPath.key(destination)] = (destination, entry)
        }

        if let option = singleOption {
            // One root: everything below it, except the readme beside the game folders, and for a root that holds `scripts/`
            // and `configs/` directly only those folders.
            for entry in index.files(under: option.prefix) {
                guard let rel = ModPath.relative(entry.path, to: option.prefix) else { continue }
                if option.destination == "gamedata" {
                    let first = rel.split(separator: "/").first.map { $0.lowercased() } ?? ""
                    if !rel.contains("/") || !ModPath.gamedataSubfolders.contains(first) { continue }
                } else if ContentLayoutAnalyzer.isDocumentation(rel) || rel.split(separator: "/").first?.lowercased() == "fomod" {
                    continue
                }
                try assign(ModPath.join(option.destination, rel), entry)
            }
        }
        for op in operations {
            let source = ModPath.join(base, op.source)
            if !op.isFolder, let entry = index.file(at: source) {
                let name = ModPath.lastComponent(entry.path)
                let destination: String
                switch op.destination {
                case nil: destination = op.source
                case "": destination = name
                case let d?: destination = op.destinationIsDirectory ? ModPath.join(d, name) : d
                }
                try assign(destination, entry)
                continue
            }
            let inside = index.files(under: source)
            if inside.isEmpty, let entry = index.file(at: source) {   // <folder> naming a file
                try assign(op.destination ?? op.source, entry)
                continue
            }
            if inside.isEmpty {
                missing.append(op.source)
                continue
            }
            let destinationDirectory = op.destination ?? op.source
            for entry in inside {
                guard let rel = ModPath.relative(entry.path, to: source) else { continue }
                try assign(ModPath.join(destinationDirectory, rel), entry)
            }
        }
        if winners.isEmpty { throw ModImportError.nothingSelected }

        // A FOMOD written for a Data folder installs `scripts/` and `configs/` bare: put them where the game looks.
        var prefix = ""
        if case .single = internals.layout {} else {
            let tops = Set(winners.values.map { ($0.path.split(separator: "/").first.map(String.init) ?? "").lowercased() })
            if tops.isDisjoint(with: ModPath.mountedRoots), tops.isSubset(of: ModPath.gamedataSubfolders) { prefix = "gamedata" }
        }

        var destinations: [Int: [String]] = [:]
        var bytes: Int64 = 0
        for (_, winner) in winners.sorted(by: { $0.key < $1.key }) {
            destinations[winner.entry.index, default: []].append(ModPath.join(prefix, winner.path))
            bytes += winner.entry.size
        }
        return Job(destinations: destinations, bytes: bytes, missing: missing)
    }

    static func checkFreeSpace(root: URL, needed: Int64) throws {
        let margin = max(Int64(32 << 20), needed / 50)
        let values = try? root.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeAvailableCapacityKey])
        let available: Int64? = values?.volumeAvailableCapacityForImportantUsage ?? values?.volumeAvailableCapacity.map(Int64.init)
        if let available, available < needed + margin {
            throw ModImportError.insufficientSpace(needed: needed + margin, available: available)
        }
    }

    private static func deleteIfDownload(_ source: URL, downloads: URL) {
        let s = source.resolvingSymlinksInPath().path
        let d = downloads.resolvingSymlinksInPath().path
        if s.hasPrefix(d + "/") { try? FileManager.default.removeItem(at: source) }
    }

    private static func sha256(of url: URL, cancel: CancelFlag) throws -> String {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return "" }
        defer { try? handle.close() }
        var hasher = SHA256()
        while true {
            try cancel.check()
            let chunk = try autoreleasepool { try handle.read(upToCount: 4 << 20) }
            guard let chunk, !chunk.isEmpty else { break }
            hasher.update(data: chunk)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    // MARK: Plumbing

    /// Runs blocking work off the cooperative pool; cancelling the Task raises the flag the work polls.
    static func runCancellable<T>(_ work: @escaping (CancelFlag) throws -> T) async throws -> T {
        let flag = CancelFlag()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<T, Error>) in
                DispatchQueue.global(qos: .userInitiated).async {
                    do { continuation.resume(returning: try work(flag)) } catch { continuation.resume(throwing: error) }
                }
            }
        } onCancel: {
            flag.cancel()
        }
    }
}

/// Delivers progress to the caller's closure on the main queue, at most about 10 times a second.
final class ProgressReporter: @unchecked Sendable {
    private let callback: (Double) -> Void
    private let lock = NSLock()
    private var last = Date.distantPast
    private var lastValue = -1.0

    init(callback: @escaping (Double) -> Void) { self.callback = callback }

    func report(_ value: Double) {
        lock.lock()
        let now = Date()
        let due = now.timeIntervalSince(last) >= 0.1 && value != lastValue
        if due { last = now; lastValue = value }
        lock.unlock()
        if due { DispatchQueue.main.async { self.callback(value) } }
    }

    /// Must be called on the main queue: queued reports run before it.
    func finish() { callback(1) }
}

/// Lower-case path lookup over the files of a source, with the files below a folder found by binary search.
struct PathIndex {
    private let sorted: [(key: String, entry: SourceEntry)]

    init(_ files: [SourceEntry]) {
        sorted = files.map { (ModPath.key($0.path), $0) }.sorted { $0.key < $1.key }
    }

    func file(at path: String) -> SourceEntry? {
        let key = ModPath.key(path)
        let i = lowerBound(key)
        return i < sorted.count && sorted[i].key == key ? sorted[i].entry : nil
    }

    /// Every file below `folder` ("" = all), in path order.
    func files(under folder: String) -> [SourceEntry] {
        if folder.isEmpty { return sorted.map(\.entry) }
        let prefix = ModPath.key(folder) + "/"
        var i = lowerBound(prefix)
        var out: [SourceEntry] = []
        while i < sorted.count, sorted[i].key.hasPrefix(prefix) {
            out.append(sorted[i].entry)
            i += 1
        }
        return out
    }

    private func lowerBound(_ key: String) -> Int {
        var lo = 0, hi = sorted.count
        while lo < hi {
            let mid = (lo + hi) / 2
            if sorted[mid].key < key { lo = mid + 1 } else { hi = mid }
        }
        return lo
    }
}

/// The installer shown for an archive that holds several mod roots (`00 Main`, `01 Optional`): one group, one option per root.
enum SyntheticInstaller {
    static func make(name: String, options: [RootOption], assets: FomodAssets) -> FomodInstaller {
        let formatter = ByteCountFormatter()
        var plugins: [FomodPlugin] = []
        for (index, option) in options.enumerated() {
            let lower = option.displayName.lowercased()
            let mainish = ["main", "core", "base", "required", "default", "standard", "recommended"].contains { lower.contains($0) }
                || (index == 0 && (lower.first?.isNumber ?? false))
            let description = "\(option.fileCount) file\(option.fileCount == 1 ? "" : "s"), \(formatter.string(fromByteCount: option.sizeBytes)). "
                + "Folder: \(option.prefix.isEmpty ? "(top level)" : option.prefix)"
            let entry = FomodFileEntry(source: option.prefix, destination: option.destination, destinationIsDirectory: true,
                                       isFolder: true, alwaysInstall: false, installIfUsable: false, priority: index)
            plugins.append(FomodPlugin(name: option.displayName, description: description, imagePath: nil, flags: [], files: [entry],
                                       typeDescriptor: .fixed(mainish ? .recommended : .optional)))
        }
        let group = FomodGroup(name: "Options", type: .selectAtLeastOne, plugins: plugins)
        let step = FomodStep(name: "Choose what to install", visible: nil, groups: [group])
        return FomodInstaller(moduleName: name, info: FomodInfo(name: name), moduleImagePath: nil, steps: [step], requiredFiles: [],
                              conditionalInstalls: [], rootPrefix: "", isSynthetic: true, assets: assets)
    }
}
