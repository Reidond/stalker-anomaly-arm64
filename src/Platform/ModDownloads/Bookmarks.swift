import Foundation

public struct Bookmark: Codable, Identifiable, Hashable, Sendable {
    public var id: UUID
    public var title: String
    public var url: URL

    public init(id: UUID = UUID(), title: String, url: URL) {
        self.id = id
        self.title = title
        self.url = url
    }
}

/// The browser's bookmarks: two built-in ones (ModDB's Anomaly addons, a GitHub search) that cannot be removed,
/// and the user's own list, kept in UserDefaults.
@MainActor
final class BookmarkStore: ObservableObject {
    static let builtIn: [Bookmark] = [
        Bookmark(id: UUID(uuidString: "6A1F5F0E-4B1B-4B9A-9E52-0D6F7C0A0001")!, title: "ModDB: Anomaly addons",
                 url: URL(string: "https://www.moddb.com/mods/stalker-anomaly/addons")!),
        Bookmark(id: UUID(uuidString: "6A1F5F0E-4B1B-4B9A-9E52-0D6F7C0A0002")!, title: "GitHub: Anomaly addons",
                 url: URL(string: "https://github.com/search?q=stalker+anomaly+addon&type=repositories")!),
    ]

    @Published private(set) var user: [Bookmark] = []

    private let defaults: UserDefaults
    private let key: String

    init(defaults: UserDefaults = .standard, key: String = "ModDownloads.bookmarks.v1") {
        self.defaults = defaults
        self.key = key
        if let data = defaults.data(forKey: key), let saved = try? JSONDecoder().decode([Bookmark].self, from: data) {
            user = saved.filter { Self.isWebURL($0.url) }
        }
    }

    var all: [Bookmark] { Self.builtIn + user }

    func isBookmarked(_ url: URL?) -> Bool {
        guard let url else { return false }
        return all.contains { $0.url == url }
    }

    /// Adds a bookmark; false when the URL is not http(s) or is already in the list.
    @discardableResult
    func add(title: String, url: URL) -> Bool {
        guard Self.isWebURL(url), !isBookmarked(url) else { return false }
        let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
        user.append(Bookmark(title: trimmed.isEmpty ? (url.host ?? url.absoluteString) : trimmed, url: url))
        save()
        return true
    }

    func remove(id: UUID) {
        user.removeAll { $0.id == id }
        save()
    }

    func move(from source: IndexSet, to destination: Int) {
        user.move(fromOffsets: source, toOffset: destination)
        save()
    }

    func rename(id: UUID, to title: String) {
        let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, let index = user.firstIndex(where: { $0.id == id }) else { return }
        user[index].title = trimmed
        save()
    }

    static func isWebURL(_ url: URL) -> Bool {
        let scheme = url.scheme?.lowercased()
        return (scheme == "http" || scheme == "https") && url.host != nil
    }

    private func save() {
        if let data = try? JSONEncoder().encode(user) {
            defaults.set(data, forKey: key)
        }
    }
}
