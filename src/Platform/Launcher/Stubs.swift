// Temporary stand-in for ModDownloads (mod-downloads), written against the shared contract (docs/ios-port/MOD-MANAGER.md). It
// compiles only while the real module is not on the build's module path, and the file is deleted once ModDownloads is on main.
// (ModManagerCore is on main: its stand-in is gone.)
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
