// The import flow: inspect, then the FOMOD wizard if the mod has one, then install with progress, with errors and warnings
// shown in between. Everything that adds a mod (Files, the browser, a pasted link) comes through here.
import Foundation
import SwiftUI
#if canImport(ModManagerCore)
import ModManagerCore
#endif
#if canImport(ModDownloads)
import ModDownloads
#endif

@MainActor
final class ImportCoordinator: ObservableObject {
    enum Stage {
        case idle
        case working(title: String, detail: String, progress: Double?)
        case review(ImportPlan)                  // warnings the player has to accept
        case fomod(ImportPlan)
        case done(InstalledMod)
        case failed(title: String, message: String)
    }

    static let installingTitle = "Installing"

    @Published private(set) var stage: Stage = .idle
    var isActive: Bool {
        if case .idle = stage { return false }
        return true
    }

    private var library: ModLibrary?
    private var importer: ModImporter?
    private var task: Task<Void, Never>?
    private var scopedURL: URL?
    private var temporaryFile: URL?   // a download that is ours to delete afterwards
    private var downloadsDir = FileManager.default.temporaryDirectory

    func attach(library: ModLibrary, downloadsDir: URL? = nil) {
        self.library = library
        importer = ModImporter(library: library)
        if let downloadsDir { self.downloadsDir = downloadsDir }
    }

    func setDownloadsDir(_ url: URL) { downloadsDir = url }

    /// For the snapshot tool: shows a stage without doing the work.
    func preview(_ stage: Stage) { self.stage = stage }

    var isInstalling: Bool {
        if case .working(let title, _, _) = stage { return title == Self.installingTitle }
        return false
    }

    // MARK: entry points

    /// A file or folder the player picked (Files app, drag and drop) or that the browser downloaded.
    func begin(url: URL, scoped: Bool = false, temporary: Bool = false) {
        guard !isActive || isFinished else { return }
        startInspect(url: url, scoped: scoped, temporary: temporary)
    }

    private func startInspect(url: URL, scoped: Bool, temporary: Bool) {
        cleanup()
        if scoped, url.startAccessingSecurityScopedResource() { scopedURL = url }
        if temporary { temporaryFile = url }
        stage = .working(title: "Reading \(url.lastPathComponent)", detail: "Checking what is inside", progress: nil)
        run { [weak self] in
            guard let self, let importer = self.importer else { return }
            let plan = try await importer.inspect(url)
            try Task.checkCancellation()
            if plan.fomod != nil {
                self.stage = .fomod(plan)
            } else if !plan.warnings.isEmpty {
                self.stage = .review(plan)
            } else {
                try await self.perform(plan, choices: nil)
            }
        }
    }

    /// A pasted link: download it into the downloads folder, then import the file like any other.
    func download(link: URL) {
        guard !isActive || isFinished else { return }
        cleanup()
        stage = .working(title: "Downloading", detail: link.host ?? link.absoluteString, progress: nil)
        let directory = downloadsDir
        run { [weak self] in
            let file = try await LinkDownloader().download(link, to: directory) { fraction in
                Task { @MainActor [weak self] in
                    guard let self, case .working(let title, let detail, _) = self.stage else { return }
                    self.stage = .working(title: title, detail: detail, progress: fraction)
                }
            }
            try Task.checkCancellation()
            self?.startInspect(url: file, scoped: false, temporary: true)
        }
    }

    /// The review screen's Install button, or the wizard's Finish.
    func install(_ plan: ImportPlan, choices: FomodChoices?) {
        run { [weak self] in try await self?.perform(plan, choices: choices) }
    }

    func cancel() {
        task?.cancel()
        task = nil
        cleanup()
        stage = .idle
    }

    /// Circle: leave where it is safe to leave. Not while files are being written.
    func back() {
        switch stage {
        case .working(let title, _, _) where title == Self.installingTitle: break
        default: cancel()
        }
    }

    func dismiss() {
        task = nil
        cleanup()
        stage = .idle
    }

    private var isFinished: Bool {
        switch stage {
        case .done, .failed, .idle: return true
        default: return false
        }
    }

    // MARK: work

    private func perform(_ plan: ImportPlan, choices: FomodChoices?) async throws {
        guard let importer else { return }
        stage = .working(title: Self.installingTitle, detail: plan.displayName, progress: 0)
        let installed = try await importer.install(plan, choices: choices) { [weak self] fraction in
            Task { @MainActor [weak self] in
                guard let self, case .working(let title, let detail, _) = self.stage else { return }
                self.stage = .working(title: title, detail: detail, progress: fraction)
            }
        }
        cleanup()
        stage = .done(installed)
    }

    private func run(_ work: @escaping @MainActor () async throws -> Void) {
        task?.cancel()
        task = Task { @MainActor [weak self] in
            do {
                try await work()
            } catch is CancellationError {
                // the player left
            } catch {
                guard let self, !Task.isCancelled else { return }
                self.cleanup()
                self.stage = .failed(title: "Could not add the mod", message: Self.describe(error))
            }
        }
    }

    /// Lets go of the picked file and deletes a download of ours (the mod is stored; the archive is not needed any more).
    private func cleanup() {
        if let scopedURL {
            scopedURL.stopAccessingSecurityScopedResource()
            self.scopedURL = nil
        }
        if let temporaryFile {
            try? FileManager.default.removeItem(at: temporaryFile)
            self.temporaryFile = nil
        }
    }

    static func describe(_ error: Error) -> String {
        let text = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
        return text.isEmpty ? String(describing: error) : text
    }
}
