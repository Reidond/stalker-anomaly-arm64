// The launcher's state: which screen is up, the mod library, the game-data check and the way out (Play or Quit).
import Foundation
import SwiftUI
import Combine
#if canImport(ModManagerCore)
import ModManagerCore
#endif

/// What the app passes in when it opens the launcher (JSON, see LauncherEntry.swift).
struct LauncherConfig: Decodable {
    var modsRoot: String
    var downloadsDir: String?
    var version = ""
    var platform = "macos"
    var allowQuit = true
    var logoPath: String?
    var deviceLine: String?
    var notices: [String] = []
    var autoPlayMs: Int?

    var isTouchDevice: Bool { platform == "ios" }
}

/// The result of the shell's game-data validation.
struct DataStatus: Decodable, Equatable {
    var state = "ok"           // ok | incomplete | notfound
    var summary = ""
    var help = ""
    var root = ""
    var filesOk = 0
    var filesExpected = 0

    var isOK: Bool { state == "ok" }
}

enum LauncherResult: Int32 {
    case running = 0, play = 1, quit = 2
}

enum Screen: Hashable {
    case home
    case mods
    case details(String)
    case add
    case browse
}

/// Per-mod view of `ModLibrary.conflicts()`: for each mod, the files it wins (and over whom) and the files it loses.
struct ConflictSummary {
    struct Pair: Identifiable {
        let other: String
        var paths: [String]
        var id: String { other }
    }
    struct Entry {
        var wins: [Pair] = []     // `other` is overridden by this mod
        var loses: [Pair] = []    // `other` overrides this mod
        var total: Int { wins.reduce(0) { $0 + $1.paths.count } + loses.reduce(0) { $0 + $1.paths.count } }
    }

    private(set) var byMod: [String: Entry] = [:]

    init() {}

    init(_ list: [ModConflict]) {
        var wins: [String: [String: [String]]] = [:]
        var loses: [String: [String: [String]]] = [:]
        for conflict in list {
            for other in conflict.overridden {
                wins[conflict.winner, default: [:]][other, default: []].append(conflict.path)
                loses[other, default: [:]][conflict.winner, default: []].append(conflict.path)
            }
        }
        for id in Set(wins.keys).union(loses.keys) {
            var entry = Entry()
            entry.wins = (wins[id] ?? [:]).map { Pair(other: $0.key, paths: $0.value.sorted()) }.sorted { $0.other < $1.other }
            entry.loses = (loses[id] ?? [:]).map { Pair(other: $0.key, paths: $0.value.sorted()) }.sorted { $0.other < $1.other }
            byMod[id] = entry
        }
    }

    subscript(id: String) -> Entry { byMod[id] ?? Entry() }
}

/// What the details screen reads from a mod's own mod.json (the contract's fields; any may be missing).
struct ModMeta: Decodable {
    struct Source: Decodable {
        var kind: String?
        var url: String?
        var fileName: String?
        var sha256: String?
    }
    var name: String?
    var version: String?
    var author: String?
    var description: String?
    var source: Source?
    var installedAt: String?

    static func load(from folder: URL) -> ModMeta? {
        guard let data = try? Data(contentsOf: folder.appendingPathComponent("mod.json")) else { return nil }
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        return try? decoder.decode(ModMeta.self, from: data)
    }
}

struct Toast: Identifiable, Equatable {
    let id = UUID()
    var text: String
    var isError: Bool
}

@MainActor
final class LauncherModel: ObservableObject {
    let config: LauncherConfig
    let focus = FocusStore()

    @Published private(set) var stack: [Screen] = [.home]
    @Published private(set) var library: ModLibrary?
    @Published private(set) var libraryError: String?
    @Published private(set) var data = DataStatus()
    @Published private(set) var conflicts = ConflictSummary()
    @Published private(set) var result: LauncherResult = .running
    @Published var toast: Toast?
    @Published var showFilePicker = false
    @Published var removalCandidate: String? {
        didSet { syncFocusScope() }
    }
    @Published private(set) var launching = false
    let imports: ImportCoordinator

    /// Re-reads the game data (the shell's validation). nil in the snapshot tool.
    var dataProvider: (() -> DataStatus)?

    private var libraryObserver: AnyCancellable?
    private var importObserver: AnyCancellable?
    private var focusObserver: AnyCancellable?
    private var dataTimer: Task<Void, Never>?
    private var toastTask: Task<Void, Never>?
    private var focusMemory: [Screen: String] = [:]
    private var conflictTask: Task<Void, Never>?

    var screen: Screen { stack.last ?? .home }
    var downloadsDir: URL {
        URL(fileURLWithPath: config.downloadsDir ?? (config.modsRoot as NSString).appendingPathComponent(".downloads"))
    }
    var mods: [InstalledMod] { library?.mods ?? [] }
    var enabledCount: Int { mods.filter(\.enabled).count }
    var modalActive: Bool { imports.isActive || removalCandidate != nil }
    var canPlay: Bool { data.isOK && !launching }

    init(config: LauncherConfig, data: DataStatus = DataStatus()) {
        self.config = config
        self.data = data
        self.imports = ImportCoordinator()
        do {
            let library = try ModLibrary(root: URL(fileURLWithPath: config.modsRoot))
            self.library = library
            imports.attach(library: library)
            libraryObserver = library.objectWillChange.sink { [weak self] _ in
                // the change lands after this notification
                Task { @MainActor [weak self] in
                    self?.objectWillChange.send()
                    self?.refreshConflicts()
                }
            }
        } catch {
            libraryError = error.localizedDescription
        }
        importObserver = imports.objectWillChange.sink { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.objectWillChange.send()
                self.syncFocusScope()
            }
        }
        focusObserver = focus.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
        focus.backHandler = { [weak self] in self?.back() }
        focus.directionInterceptor = { [weak self] direction, reorder in
            // Square held + up/down on a mod: move it in the load order instead of the focus
            guard let self, reorder, direction == .up || direction == .down, !self.modalActive, self.screen == .mods,
                  let id = self.focus.focusedID, id.hasPrefix("mod:") else { return false }
            self.move(String(id.dropFirst(4)), by: direction == .up ? -1 : 1)
            return true
        }
        focus.optionsHandler = { [weak self] in self?.optionsPressed() }
        refreshConflicts()
    }

    /// The cards that sit on top (import flow, remove confirmation) take the focus; underneath, nothing can be reached.
    private func syncFocusScope() {
        focus.activeScope = modalActive ? "modal" : "main"
    }

    // MARK: lifecycle

    func start() {
        rescan()
        dataTimer = Task { [weak self] in
            // while the data is not usable a copy may be under way: look again every 2 s
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                guard let self else { return }
                if !self.data.isOK { self.rescan() }
            }
        }
    }

    func stop() {
        dataTimer?.cancel()
        dataTimer = nil
        toastTask?.cancel()
        conflictTask?.cancel()
        focus.releaseAll()
        imports.cancel()
    }

    func rescan() {
        if let fresh = dataProvider?(), fresh != data { data = fresh }
    }

    /// For the snapshot tool: a game-data state without a shell behind it.
    func setData(_ status: DataStatus) { data = status }

    // MARK: navigation

    func push(_ next: Screen) {
        guard next != screen else { return }
        if let id = focus.focusedID { focusMemory[screen] = id }
        withAnimation(.easeOut(duration: 0.18)) { stack.append(next) }
    }

    func pop() {
        guard stack.count > 1 else { return }
        focus.pendingFocus = focusMemory[stack[stack.count - 2]]
        withAnimation(.easeOut(duration: 0.18)) { _ = stack.removeLast() }
    }

    func back() {
        if removalCandidate != nil {
            removalCandidate = nil
        } else if imports.isActive {
            imports.back()
        } else {
            pop()
        }
    }

    private func optionsPressed() {
        if !modalActive, screen == .home || screen == .mods { play() }
    }

    // MARK: play / quit

    func play() {
        guard canPlay else { return }
        launching = true
        persistModList()
        result = .play
    }

    func quit() {
        result = .quit
    }

    /// modlist.json is what the engine reads at start. Every library change writes it already; writing it once more here makes
    /// sure it exists and lists every installed mod, also on a first run with nothing installed.
    private func persistModList() {
        let root = URL(fileURLWithPath: config.modsRoot)
        let entries = mods.map { ["id": $0.id, "enabled": $0.enabled] as [String: Any] }
        let json: [String: Any] = ["schema": 1, "mods": entries]
        do {
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            let data = try JSONSerialization.data(withJSONObject: json, options: [.prettyPrinted, .sortedKeys])
            try data.write(to: root.appendingPathComponent("modlist.json"), options: .atomic)
        } catch {
            show("Could not write the mod list: \(error.localizedDescription)", error: true)
        }
    }

    // MARK: mods

    func toggle(_ id: String) {
        guard let library, let mod = mods.first(where: { $0.id == id }) else { return }
        do { try library.setEnabled(id, !mod.enabled) } catch { show("Could not change \(mod.name): \(error.localizedDescription)", error: true) }
    }

    /// One step up or down the load order. `delta` -1 is lower priority (earlier), +1 higher (later: wins).
    func move(_ id: String, by delta: Int) {
        guard let from = mods.firstIndex(where: { $0.id == id }) else { return }
        let target = from + delta
        guard mods.indices.contains(target) else { return }
        move(from: from, to: target)
    }

    func move(from: Int, to target: Int) {
        guard let library, from != target, mods.indices.contains(from), mods.indices.contains(target) else { return }
        do {
            try library.move(fromOffsets: IndexSet(integer: from), toOffset: target > from ? target + 1 : target)
        } catch {
            show("Could not reorder: \(error.localizedDescription)", error: true)
        }
    }

    func remove(_ id: String) {
        guard let library else { return }
        let name = mods.first { $0.id == id }?.name ?? id
        do {
            try library.remove(id)
            show("Removed \(name)", error: false)
            if case .details(let open) = screen, open == id { pop() }
        } catch {
            show("Could not remove \(name): \(error.localizedDescription)", error: true)
        }
        removalCandidate = nil
    }

    func refreshConflicts() {
        guard let library else { return }
        conflictTask?.cancel()
        conflictTask = Task { @MainActor [weak self] in
            // after the change that triggered this has been applied, and once for a burst of toggles
            try? await Task.sleep(nanoseconds: 60_000_000)
            guard !Task.isCancelled, let self else { return }
            self.conflicts = ConflictSummary(library.conflicts())
        }
    }

    func conflictCount(of id: String) -> Int { conflicts[id].total }

    func name(of id: String) -> String { mods.first { $0.id == id }?.name ?? id }

    // MARK: toast

    func show(_ text: String, error: Bool) {
        toast = Toast(text: text, isError: error)
        toastTask?.cancel()
        toastTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: error ? 6_000_000_000 : 3_500_000_000)
            if !Task.isCancelled { self?.toast = nil }
        }
    }
}
