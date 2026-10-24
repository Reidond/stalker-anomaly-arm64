// The launcher's state: which screen is up, the mod library, the game-data check and the way out (Play or Quit).
import Foundation
import SwiftUI
import Combine
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif
import ModManagerCore

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
    let imports: ImportCoordinator

    /// The screens pushed on top of home (bound to the NavigationStack: its back button and swipe change this too).
    @Published var path: [Screen] = [] {
        didSet { scheduleFocusRefresh() }
    }
    /// The in-app browser on the iPad: a full-screen cover with its own NavigationStack (pushed onto the launcher's stack it makes
    /// SwiftUI drop the stack's pushed screens). On the Mac it is a pushed screen (`path`).
    @Published var showBrowser = false {
        didSet { scheduleFocusRefresh() }
    }
    @Published private(set) var library: ModLibrary?
    @Published private(set) var libraryError: String?
    @Published private(set) var data = DataStatus() {
        didSet { scheduleFocusRefresh() }
    }
    @Published private(set) var conflicts = ConflictSummary()
    @Published private(set) var result: LauncherResult = .running
    @Published private(set) var launching = false {
        didSet { scheduleFocusRefresh() }
    }
    @Published var toast: Toast?
    @Published var showFilePicker = false
    @Published var removalCandidate: String? {
        didSet { scheduleFocusRefresh() }
    }
    /// The link field of Add mod (here, not in the view, so that the pad's Paste and Download rows reach it).
    @Published var linkText = "" {
        didSet { scheduleFocusRefresh() }
    }
    /// Bumped when Cross is pressed on the link row: the view puts the keyboard in the field.
    @Published private(set) var linkFocusRequest = 0

    /// Re-reads the game data (the shell's validation). nil in the snapshot tool.
    var dataProvider: (() -> DataStatus)?

    private var libraryObserver: AnyCancellable?
    private var importObserver: AnyCancellable?
    private var focusObserver: AnyCancellable?
    private var dataTimer: Task<Void, Never>?
    private var toastTask: Task<Void, Never>?
    private var conflictTask: Task<Void, Never>?
    private var focusTask: Task<Void, Never>?

    var screen: Screen { showBrowser ? .browse : (path.last ?? .home) }
    var downloadsDir: URL {
        URL(fileURLWithPath: config.downloadsDir ?? (config.modsRoot as NSString).appendingPathComponent(".downloads"))
    }
    var mods: [InstalledMod] { library?.mods ?? [] }
    var enabledCount: Int { mods.filter(\.enabled).count }
    var modalActive: Bool { imports.isActive || removalCandidate != nil }
    var canPlay: Bool { data.isOK && !launching }
    var linkURL: URL? {
        let trimmed = linkText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: trimmed), let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https", url.host != nil else { return nil }
        return url
    }

    init(config: LauncherConfig, data: DataStatus = DataStatus()) {
        self.config = config
        self.data = data
        self.imports = ImportCoordinator()
        do {
            let library = try ModLibrary(root: URL(fileURLWithPath: config.modsRoot))
            self.library = library
            imports.attach(library: library, downloadsDir: downloadsDir)
            libraryObserver = library.objectWillChange.sink { [weak self] _ in
                // the change lands after this notification
                Task { @MainActor [weak self] in
                    self?.objectWillChange.send()
                    self?.refreshConflicts()
                    self?.scheduleFocusRefresh()
                }
            }
        } catch {
            libraryError = error.localizedDescription
        }
        importObserver = imports.objectWillChange.sink { [weak self] _ in
            Task { @MainActor [weak self] in
                self?.objectWillChange.send()
                self?.scheduleFocusRefresh()
            }
        }
        focusObserver = focus.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
        focus.backHandler = { [weak self] in self?.back() }
        focus.pressInterceptor = { [weak self] button in
            // while a FOMOD is being answered the wizard has the pad: Cross picks, Circle goes back, R1 / Options go on
            guard let self, let session = self.imports.fomodSession else { return false }
            switch button {
            case .confirm: session.send(.confirm)
            case .back: session.send(.back)
            case .options, .rightShoulder: session.send(.next)
            default: return false
            }
            return true
        }
        focus.directionInterceptor = { [weak self] direction, reorder in
            if let session = self?.imports.fomodSession {
                switch direction {
                case .up: session.send(.up)
                case .down: session.send(.down)
                case .left: session.send(.left)
                case .right: session.send(.right)
                }
                return true
            }
            // Square held + up/down on a mod: move it in the load order instead of the focus
            guard let self, reorder, direction == .up || direction == .down, !self.modalActive, self.screen == .mods,
                  let id = self.focus.focusedID, id.hasPrefix("mod:") else { return false }
            self.move(String(id.dropFirst(4)), by: direction == .up ? -1 : 1)
            return true
        }
        focus.optionsHandler = { [weak self] in self?.optionsPressed() }
        refreshConflicts()
        refreshFocus()
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
        focusTask?.cancel()
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
        guard next != screen, next != .home else { return }
        #if os(iOS)
        if next == .browse { showBrowser = true; return }
        #endif
        path.append(next)
    }

    func pop() {
        if showBrowser { showBrowser = false; return }
        if !path.isEmpty { path.removeLast() }
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

    func binding(for mod: InstalledMod) -> Binding<Bool> {
        Binding(get: { [weak self] in self?.mods.first { $0.id == mod.id }?.enabled ?? mod.enabled },
                set: { [weak self] value in self?.setEnabled(mod.id, value) })
    }

    func setEnabled(_ id: String, _ enabled: Bool) {
        guard let library, let mod = mods.first(where: { $0.id == id }), mod.enabled != enabled else { return }
        do { try library.setEnabled(id, enabled) } catch { show("Could not change \(mod.name): \(error.localizedDescription)", error: true) }
    }

    func toggle(_ id: String) {
        guard let mod = mods.first(where: { $0.id == id }) else { return }
        setEnabled(id, !mod.enabled)
    }

    /// One step up or down the load order. `delta` -1 is lower priority (earlier), +1 higher (later: wins).
    func move(_ id: String, by delta: Int) {
        guard let from = mods.firstIndex(where: { $0.id == id }) else { return }
        let target = from + delta
        guard mods.indices.contains(target) else { return }
        move(from: from, to: target)
    }

    func move(from: Int, to target: Int) {
        guard from != target, mods.indices.contains(from), mods.indices.contains(target) else { return }
        moveOffsets(IndexSet(integer: from), toOffset: target > from ? target + 1 : target)
    }

    /// For the list's own drag and edit mode.
    func moveOffsets(_ source: IndexSet, toOffset destination: Int) {
        guard let library else { return }
        do { try library.move(fromOffsets: source, toOffset: destination) } catch { show("Could not reorder: \(error.localizedDescription)", error: true) }
    }

    func remove(_ id: String) {
        guard let library else { return }
        let name = mods.first { $0.id == id }?.name ?? id
        removalCandidate = nil
        do {
            try library.remove(id)
            show("Removed \(name)", error: false)
            if case .details(let open) = screen, open == id { pop() }
        } catch {
            show("Could not remove \(name): \(error.localizedDescription)", error: true)
        }
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

    // MARK: adding

    func pasteLink() {
        #if canImport(UIKit)
        if let text = UIPasteboard.general.string { linkText = text }
        #elseif canImport(AppKit)
        if let text = NSPasteboard.general.string(forType: .string) { linkText = text }
        #endif
    }

    func startLinkDownload() {
        guard let url = linkURL else { return }
        imports.download(link: url)
        linkText = ""
    }

    func requestLinkFocus() { linkFocusRequest += 1 }

    /// After an import: switch the new mod on and look at the list.
    func enableAfterImport(_ mod: InstalledMod) {
        setEnabled(mod.id, true)
        imports.dismiss()
        if screen != .mods { push(.mods) }
    }

    func openModsAfterImport() {
        imports.dismiss()
        if screen != .mods { push(.mods) }
    }

    // MARK: toast

    func show(_ text: String, error: Bool) {
        toast = Toast(text: text, isError: error)
        toastTask?.cancel()
        toastTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: error ? 6_000_000_000 : 3_500_000_000)
            if !Task.isCancelled { self?.toast = nil }
        }
    }

    // MARK: what the pad can reach

    /// Coalesces the state changes of one tick into one refresh (the published values change after their willChange notification).
    func scheduleFocusRefresh() {
        focusTask?.cancel()
        focusTask = Task { @MainActor [weak self] in
            await Task.yield()
            guard !Task.isCancelled else { return }
            self?.refreshFocus()
        }
    }

    func refreshFocus() {
        let (context, items) = focusItems()
        focus.setItems(items, context: context)
    }

    private func focusItems() -> (String, [FocusItem]) {
        if let id = removalCandidate {
            return ("remove", [
                FocusItem(id: "remove.confirm", activate: { [weak self] in self?.remove(id) }),
                FocusItem(id: "remove.cancel", isDefault: true, activate: { [weak self] in self?.removalCandidate = nil }),
            ])
        }
        if imports.isActive { return ("import", importItems()) }
        switch screen {
        case .home:
            var items: [FocusItem] = []
            if !data.isOK { items.append(FocusItem(id: "home.rescan", isDefault: true, activate: { [weak self] in self?.rescan() })) }
            items.append(FocusItem(id: "home.play", enabled: canPlay, isDefault: data.isOK, activate: { [weak self] in self?.play() }))
            items.append(FocusItem(id: "home.mods", enabled: library != nil, activate: { [weak self] in self?.push(.mods) }))
            items.append(FocusItem(id: "home.add", enabled: library != nil, activate: { [weak self] in self?.push(.add) }))
            if config.allowQuit { items.append(FocusItem(id: "home.quit", activate: { [weak self] in self?.quit() })) }
            return ("home", items)
        case .mods:
            var items = mods.enumerated().map { index, mod in
                FocusItem(id: "mod:\(mod.id)", isDefault: index == 0,
                          activate: { [weak self] in self?.toggle(mod.id) },
                          secondary: { [weak self] in self?.push(.details(mod.id)) })
            }
            items.append(FocusItem(id: "mods.add", isDefault: mods.isEmpty, activate: { [weak self] in self?.push(.add) }))
            return ("mods", items)
        case .details(let id):
            guard let mod = mods.first(where: { $0.id == id }) else { return ("details", []) }
            return ("details:\(id)", [
                FocusItem(id: "details.toggle", isDefault: true, activate: { [weak self] in self?.toggle(id) }),
                FocusItem(id: "details.later", enabled: mods.last?.id != mod.id, activate: { [weak self] in self?.move(id, by: 1) }),
                FocusItem(id: "details.earlier", enabled: mods.first?.id != mod.id, activate: { [weak self] in self?.move(id, by: -1) }),
                FocusItem(id: "details.remove", activate: { [weak self] in self?.removalCandidate = id }),
            ])
        case .add:
            return ("add", [
                FocusItem(id: "add.files", isDefault: true, activate: { [weak self] in self?.showFilePicker = true }),
                FocusItem(id: "add.browse", activate: { [weak self] in self?.push(.browse) }),
                FocusItem(id: "add.link", activate: { [weak self] in self?.requestLinkFocus() }),
                FocusItem(id: "add.paste", activate: { [weak self] in self?.pasteLink() }),
                FocusItem(id: "add.download", enabled: linkURL != nil, activate: { [weak self] in self?.startLinkDownload() }),
            ])
        case .browse:
            return ("browse", [])
        }
    }

    private func importItems() -> [FocusItem] {
        switch imports.stage {
        case .idle, .fomod:
            return []
        case .working(let title, _, _):
            return title == ImportCoordinator.installingTitle ? [] : [FocusItem(id: "import.cancel", isDefault: true, activate: { [weak self] in self?.imports.cancel() })]
        case .review(let plan):
            return [
                FocusItem(id: "import.install", activate: { [weak self] in self?.imports.continueImport(plan) }),
                FocusItem(id: "import.cancel", isDefault: true, activate: { [weak self] in self?.imports.cancel() }),
            ]
        case .done(let mod):
            var items: [FocusItem] = []
            if !mod.enabled { items.append(FocusItem(id: "import.enable", isDefault: true, activate: { [weak self] in self?.enableAfterImport(mod) })) }
            items.append(FocusItem(id: "import.mods", isDefault: mod.enabled, activate: { [weak self] in self?.openModsAfterImport() }))
            items.append(FocusItem(id: "import.done", activate: { [weak self] in self?.imports.dismiss() }))
            return items
        case .failed:
            return [FocusItem(id: "import.close", isDefault: true, activate: { [weak self] in self?.imports.dismiss() })]
        }
    }
}
