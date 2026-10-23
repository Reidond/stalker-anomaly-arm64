import ModDownloads
import SwiftUI

@main
struct DownloadsLabApp: App {
    var body: some Scene {
        WindowGroup { LabRoot() }
    }
}

enum LabPaths {
    static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }
    /// Stands in for `<mods root>/.downloads`.
    static var downloads: URL { documents.appendingPathComponent("ModDownloadsLab/.downloads") }
    static var report: URL { documents.appendingPathComponent("lab-report.txt") }
}

struct LabRoot: View {
    var body: some View {
        // the launcher shows the browser as a pushed screen of its NavigationStack, with the title set by the host: the same here
        NavigationStack {
            DownloadsView(downloadsDir: LabPaths.downloads,
                          onDownloaded: { url in SelfTest.note("onDownloaded: \(url.lastPathComponent)") },
                          onDownloadedFile: { file in
                              SelfTest.note("onDownloadedFile: \(file.fileName) \(file.size) bytes sha256=\(file.sha256) kind=\(file.kind) final=\(file.finalURL.host ?? "")")
                          })
                .navigationTitle("Browse")
        }
        .tint(DownloadsTheme.anomaly.accent)
        .task { await SelfTest.runIfRequested() }
    }
}
