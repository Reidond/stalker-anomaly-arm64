import Foundation

/// Where a mod came from (`source` in mod.json).
public struct ModOrigin: Codable, Hashable {
    public enum Kind: String, Codable { case file, url, browser }

    public var kind: Kind
    public var url: String?
    public var fileName: String?
    public var sha256: String?

    public init(kind: Kind = .file, url: String? = nil, fileName: String? = nil, sha256: String? = nil) {
        self.kind = kind
        self.url = url
        self.fileName = fileName
        self.sha256 = sha256
    }

    enum CodingKeys: String, CodingKey { case kind, url, fileName = "file_name", sha256 }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        kind = (try? c.decode(Kind.self, forKey: .kind)) ?? .file
        url = try c.decodeIfPresent(String.self, forKey: .url)
        fileName = try c.decodeIfPresent(String.self, forKey: .fileName)
        sha256 = try c.decodeIfPresent(String.self, forKey: .sha256)
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(kind, forKey: .kind)
        try Self.encodeOrNull(&c, url, .url)
        try Self.encodeOrNull(&c, fileName, .fileName)
        try Self.encodeOrNull(&c, sha256, .sha256)
    }

    static func encodeOrNull<K: CodingKey>(_ c: inout KeyedEncodingContainer<K>, _ value: String?, _ key: K) throws {
        if let value { try c.encode(value, forKey: key) } else { try c.encodeNil(forKey: key) }
    }
}

/// `<root>/<id>/mod.json` (contract point 2).
struct ModManifest: Codable, Equatable {
    var schema: Int = 1
    var id: String
    var name: String
    var version: String?
    var author: String?
    var description: String?
    var source: ModOrigin = ModOrigin()
    var installedAt: String
    var fomodChoices: [FomodChoices.Selection]?
    var fileCount: Int
    var sizeBytes: Int64

    enum CodingKeys: String, CodingKey {
        case schema, id, name, version, author, description, source
        case installedAt = "installed_at"
        case fomodChoices = "fomod_choices"
        case fileCount = "file_count"
        case sizeBytes = "size_bytes"
    }

    init(id: String, name: String, version: String?, author: String?, description: String?, source: ModOrigin,
         installedAt: Date, fomodChoices: [FomodChoices.Selection]?, fileCount: Int, sizeBytes: Int64) {
        self.id = id
        self.name = name
        self.version = version
        self.author = author
        self.description = description
        self.source = source
        self.installedAt = Self.format(installedAt)
        self.fomodChoices = fomodChoices
        self.fileCount = fileCount
        self.sizeBytes = sizeBytes
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        schema = (try? c.decode(Int.self, forKey: .schema)) ?? 1
        id = try c.decode(String.self, forKey: .id)
        name = (try? c.decode(String.self, forKey: .name)) ?? id
        version = try? c.decodeIfPresent(String.self, forKey: .version)
        author = try? c.decodeIfPresent(String.self, forKey: .author)
        description = try? c.decodeIfPresent(String.self, forKey: .description)
        source = (try? c.decode(ModOrigin.self, forKey: .source)) ?? ModOrigin()
        installedAt = (try? c.decode(String.self, forKey: .installedAt)) ?? ""
        fomodChoices = try? c.decodeIfPresent([FomodChoices.Selection].self, forKey: .fomodChoices)
        fileCount = (try? c.decode(Int.self, forKey: .fileCount)) ?? 0
        sizeBytes = (try? c.decode(Int64.self, forKey: .sizeBytes)) ?? 0
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(schema, forKey: .schema)
        try c.encode(id, forKey: .id)
        try c.encode(name, forKey: .name)
        try ModOrigin.encodeOrNull(&c, version, .version)
        try ModOrigin.encodeOrNull(&c, author, .author)
        try ModOrigin.encodeOrNull(&c, description, .description)
        try c.encode(source, forKey: .source)
        try c.encode(installedAt, forKey: .installedAt)
        if let fomodChoices { try c.encode(fomodChoices, forKey: .fomodChoices) } else { try c.encodeNil(forKey: .fomodChoices) }
        try c.encode(fileCount, forKey: .fileCount)
        try c.encode(sizeBytes, forKey: .sizeBytes)
    }

    var installedDate: Date? { Self.parse(installedAt) }

    // ISO8601DateFormatter is documented thread-safe.
    private static let formatter: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        return f
    }()
    static func format(_ date: Date) -> String { formatter.string(from: date) }
    static func parse(_ text: String) -> Date? { formatter.date(from: text) }

    static func encoder() -> JSONEncoder {
        let e = JSONEncoder()
        e.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        return e
    }
}

/// `<root>/modlist.json` (contract point 3).
struct ModListFile: Codable {
    struct Entry: Codable, Equatable {
        var id: String
        var enabled: Bool
    }

    var schema: Int = 1
    var mods: [Entry] = []
}
