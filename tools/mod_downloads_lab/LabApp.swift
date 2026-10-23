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
    @State private var lastFile = "nothing downloaded yet"

    var body: some View {
        VStack(spacing: 0) {
            DownloadsView(downloadsDir: LabPaths.downloads,
                          onDownloaded: { url in SelfTest.note("onDownloaded: \(url.lastPathComponent)") },
                          onDownloadedFile: { file in
                              SelfTest.note("onDownloadedFile: \(file.fileName) \(file.size) bytes sha256=\(file.sha256) kind=\(file.kind) final=\(file.finalURL.host ?? "")")
                              lastFile = "\(file.fileName), \(file.size) bytes"
                          })
            Text("Lab: \(lastFile)")
                .font(.caption)
                .padding(4)
        }
        .task { await SelfTest.runIfRequested() }
    }
}
