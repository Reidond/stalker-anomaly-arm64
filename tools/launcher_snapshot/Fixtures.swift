// Made-up mods for the snapshots: a throwaway mods root in the temporary folder, laid out as the contract says (<id>/mod.json and
// <id>/content/...). Names, authors and files are invented; no mod content is involved.
import Foundation

struct FixtureMod {
    var id: String
    var name: String
    var version: String
    var author: String
    var description: String
    var source: [String: String]
    var fileCount: Int
    var sizeBytes: Int64
    var enabled = true
    var files: [String] = []
}

enum Fixtures {
    static let mods: [FixtureMod] = [
        FixtureMod(id: "sample-hud-tweaks", name: "Sample HUD Tweaks", version: "1.4", author: "North Camp",
                   description: "Smaller icons, a quieter minimap and larger text for the PDA. A made-up mod for the screenshots.",
                   source: ["kind": "file", "file_name": "sample_hud_tweaks_1.4.zip"], fileCount: 38, sizeBytes: 4_900_000,
                   files: ["gamedata/configs/ui/ui_hud.xml", "gamedata/configs/ui/ui_pda.xml", "gamedata/textures/ui/ui_icons.dds"]),
        FixtureMod(id: "sample-weather-pack", name: "Sample Weather Pack", version: "2.0.1", author: "Cordon Studio",
                   description: "Longer storms, thicker fog in the marshes and a darker night. A made-up mod for the screenshots.",
                   source: ["kind": "url", "url": "https://example.org/mods/sample-weather-pack"], fileCount: 214, sizeBytes: 182_400_000,
                   files: ["gamedata/configs/weathers/default.ltx", "gamedata/configs/ui/ui_hud.xml", "gamedata/textures/sky/sky_clear.dds"]),
        FixtureMod(id: "sample-reload-sounds", name: "Sample Reload Sounds", version: "0.9", author: "Bar Rostok",
                   description: "Replaces the reload sounds of the rifles.", source: ["kind": "browser", "url": "https://example.org/files/reload"],
                   fileCount: 57, sizeBytes: 61_300_000, enabled: false, files: ["gamedata/sounds/weapons/reload_ak.ogg"]),
        FixtureMod(id: "sample-trader-prices", name: "Sample Trader Prices", version: "1.1", author: "Yantar Lab",
                   description: "Rebalances what the traders pay.", source: ["kind": "file", "file_name": "prices.7z"], fileCount: 6,
                   sizeBytes: 220_000, files: ["gamedata/configs/trade/trade_generic.ltx"]),
    ]

    @discardableResult
    static func build(into root: URL, mods: [FixtureMod] = Fixtures.mods) throws -> URL {
        let manager = FileManager.default
        try? manager.removeItem(at: root)
        try manager.createDirectory(at: root, withIntermediateDirectories: true)
        for mod in mods {
            let folder = root.appendingPathComponent(mod.id)
            for file in mod.files {
                let url = folder.appendingPathComponent("content").appendingPathComponent(file)
                try manager.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                try Data("fixture".utf8).write(to: url)
            }
            let json: [String: Any] = [
                "schema": 1, "id": mod.id, "name": mod.name, "version": mod.version, "author": mod.author, "description": mod.description,
                "source": mod.source, "installed_at": "2026-10-23T18:30:00Z", "fomod_choices": NSNull(),
                "file_count": mod.fileCount, "size_bytes": mod.sizeBytes,
            ]
            try JSONSerialization.data(withJSONObject: json, options: [.prettyPrinted, .sortedKeys]).write(to: folder.appendingPathComponent("mod.json"))
        }
        let list: [String: Any] = ["schema": 1, "mods": mods.map { ["id": $0.id, "enabled": $0.enabled] as [String: Any] }]
        try JSONSerialization.data(withJSONObject: list, options: [.prettyPrinted, .sortedKeys]).write(to: root.appendingPathComponent("modlist.json"))
        return root
    }
}
