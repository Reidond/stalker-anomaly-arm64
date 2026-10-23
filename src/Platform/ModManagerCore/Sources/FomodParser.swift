import Foundation

/// A small DOM over `XMLParser`. Element and attribute names are lower-cased and stripped of prefixes: real-world FOMOD files
/// differ in case (`moduleName`, `ModuleName`) and some carry namespace prefixes.
final class XMLElement2 {
    let name: String
    var attributes: [String: String] = [:]
    var children: [XMLElement2] = []
    var text = ""

    init(name: String) { self.name = name }

    func child(_ name: String) -> XMLElement2? { children.first { $0.name == name } }
    func all(_ name: String) -> [XMLElement2] { children.filter { $0.name == name } }
    func attr(_ name: String) -> String? { attributes[name] }
    var trimmedText: String { text.trimmingCharacters(in: .whitespacesAndNewlines) }
}

enum FomodXML {
    /// Decodes the bytes of a FOMOD XML file (UTF-8 with or without BOM, UTF-16 LE/BE with or without BOM, or a legacy
    /// 8-bit file), drops the XML declaration whose encoding may be wrong, and escapes the bare `&` authors leave in text.
    static func normalizedText(_ data: Data) -> String? {
        var bytes = [UInt8](data)
        var encoding: String.Encoding = .utf8
        if bytes.starts(with: [0xEF, 0xBB, 0xBF]) {
            bytes.removeFirst(3)
        } else if bytes.starts(with: [0xFF, 0xFE]) {
            bytes.removeFirst(2)
            encoding = .utf16LittleEndian
        } else if bytes.starts(with: [0xFE, 0xFF]) {
            bytes.removeFirst(2)
            encoding = .utf16BigEndian
        } else if bytes.count >= 4, bytes[0] == 0x3C, bytes[1] == 0x00 {
            encoding = .utf16LittleEndian
        } else if bytes.count >= 4, bytes[0] == 0x00, bytes[1] == 0x3C {
            encoding = .utf16BigEndian
        }
        var text = String(bytes: bytes, encoding: encoding)
        if text == nil { text = String(bytes: bytes, encoding: .windowsCP1252) ?? String(bytes: bytes, encoding: .isoLatin1) }
        guard var result = text else { return nil }
        result = result.replacingOccurrences(of: #"^\s*<\?xml[^>]*\?>"#, with: "", options: .regularExpression)
        result = result.replacingOccurrences(of: #"&(?!(?:amp|lt|gt|quot|apos|#[0-9]+|#x[0-9a-fA-F]+);)"#, with: "&amp;",
                                             options: .regularExpression)
        // anything before the root element (a stray character, a second BOM)
        if let lt = result.firstIndex(of: "<") { result = String(result[lt...]) }
        return result
    }

    static func parse(_ data: Data) throws -> XMLElement2 {
        guard let text = normalizedText(data), let utf8 = text.data(using: .utf8) else {
            throw ModImportError.invalidFomod("the file is not text")
        }
        let builder = Builder()
        let parser = XMLParser(data: utf8)
        parser.delegate = builder
        parser.shouldProcessNamespaces = false
        parser.shouldResolveExternalEntities = false
        guard parser.parse(), let root = builder.root else {
            let why = parser.parserError?.localizedDescription ?? "unreadable XML"
            throw ModImportError.invalidFomod("\(why) (line \(parser.lineNumber))")
        }
        return root
    }

    private final class Builder: NSObject, XMLParserDelegate {
        var root: XMLElement2?
        var stack: [XMLElement2] = []

        func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?, qualifiedName qName: String?,
                    attributes attributeDict: [String: String] = [:]) {
            let element = XMLElement2(name: local(elementName))
            for (key, value) in attributeDict { element.attributes[local(key)] = value }
            if let parent = stack.last { parent.children.append(element) } else if root == nil { root = element }
            stack.append(element)
        }

        func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
            _ = stack.popLast()
        }

        func parser(_ parser: XMLParser, foundCharacters string: String) { stack.last?.text += string }

        func parser(_ parser: XMLParser, foundCDATA CDATABlock: Data) {
            stack.last?.text += String(decoding: CDATABlock, as: UTF8.self)
        }

        private func local(_ name: String) -> String {
            (name.split(separator: ":").last.map(String.init) ?? name).lowercased()
        }
    }
}

enum FomodParser {
    /// Parses `ModuleConfig.xml` (+ optional `info.xml`). `rootPrefix` is where the installer's relative paths start.
    static func parse(config: Data, info: Data?, rootPrefix: String, assets: FomodAssets) throws -> FomodInstaller {
        let root = try FomodXML.parse(config)
        guard root.name == "config" else { throw ModImportError.invalidFomod("the root element is <\(root.name)>, not <config>") }

        let fomodInfo = (info.flatMap { try? parseInfo(FomodXML.parse($0)) }) ?? FomodInfo()
        let moduleName = root.child("modulename")?.trimmedText ?? ""
        let moduleImage = root.child("moduleimage")?.attr("path").flatMap { $0.isEmpty ? nil : $0 }

        var steps: [FomodStep] = []
        if let installSteps = root.child("installsteps") {
            for step in ordered(installSteps.all("installstep"), by: installSteps.attr("order"), name: { $0.attr("name") ?? "" }) {
                var groups: [FomodGroup] = []
                if let groupsNode = step.child("optionalfilegroups") {
                    for group in ordered(groupsNode.all("group"), by: groupsNode.attr("order"), name: { $0.attr("name") ?? "" }) {
                        groups.append(try parseGroup(group))
                    }
                }
                steps.append(FomodStep(name: step.attr("name") ?? "", visible: step.child("visible").map(parseCondition),
                                       groups: groups))
            }
        }

        var required: [FomodFileEntry] = []
        if let node = root.child("requiredinstallfiles") { required = try parseFiles(node) }

        var conditional: [FomodConditionalInstall] = []
        if let patterns = root.child("conditionalfileinstalls")?.child("patterns") {
            for pattern in patterns.all("pattern") {
                let condition = pattern.child("dependencies").map(parseCondition) ?? parseLooseCondition(pattern)
                var files: [FomodFileEntry] = []
                if let node = pattern.child("files") { files = try parseFiles(node) }
                conditional.append(FomodConditionalInstall(condition: condition, files: files))
            }
        }

        let name = !moduleName.isEmpty ? moduleName : (fomodInfo.name ?? "")
        return FomodInstaller(moduleName: name, info: fomodInfo, moduleImagePath: moduleImage, steps: steps,
                              requiredFiles: required, conditionalInstalls: conditional, rootPrefix: rootPrefix,
                              isSynthetic: false, assets: assets)
    }

    static func parseInfo(_ root: XMLElement2) -> FomodInfo {
        func text(_ name: String) -> String? {
            let value = root.child(name)?.trimmedText
            return value?.isEmpty == false ? value : nil
        }
        return FomodInfo(name: text("name"), author: text("author"), version: text("version"),
                         description: text("description"), website: text("website"))
    }

    private static func ordered(_ items: [XMLElement2], by order: String?, name: (XMLElement2) -> String) -> [XMLElement2] {
        switch (order ?? "Ascending").trimmingCharacters(in: .whitespaces).lowercased() {
        case "explicit": return items
        case "descending": return items.sorted { name($0).compare(name($1), options: .caseInsensitive) == .orderedDescending }
        default: return items.sorted { name($0).compare(name($1), options: .caseInsensitive) == .orderedAscending }
        }
    }

    private static func parseGroup(_ node: XMLElement2) throws -> FomodGroup {
        var plugins: [FomodPlugin] = []
        if let pluginsNode = node.child("plugins") {
            for plugin in ordered(pluginsNode.all("plugin"), by: pluginsNode.attr("order"), name: { $0.attr("name") ?? "" }) {
                plugins.append(try parsePlugin(plugin))
            }
        }
        return FomodGroup(name: node.attr("name") ?? "", type: FomodGroupType(attribute: node.attr("type")), plugins: plugins)
    }

    private static func parsePlugin(_ node: XMLElement2) throws -> FomodPlugin {
        var flags: [(name: String, value: String)] = []
        for flag in node.child("conditionflags")?.all("flag") ?? [] {
            flags.append((flag.attr("name") ?? "", flag.trimmedText))
        }
        var files: [FomodFileEntry] = []
        if let filesNode = node.child("files") { files = try parseFiles(filesNode) }
        let image = node.child("image")?.attr("path").flatMap { $0.isEmpty ? nil : $0 }
        return FomodPlugin(name: node.attr("name") ?? "", description: node.child("description")?.trimmedText ?? "",
                           imagePath: image, flags: flags, files: files,
                           typeDescriptor: parseTypeDescriptor(node.child("typedescriptor")))
    }

    private static func parseTypeDescriptor(_ node: XMLElement2?) -> FomodTypeDescriptor {
        guard let node else { return .fixed(.optional) }
        if let type = node.child("type") { return .fixed(FomodPluginType(attribute: type.attr("name"))) }
        guard let dependent = node.child("dependencytype") else { return .fixed(.optional) }
        let defaultType = FomodPluginType(attribute: dependent.child("defaulttype")?.attr("name"))
        var patterns: [(condition: FomodCondition, type: FomodPluginType)] = []
        for pattern in dependent.child("patterns")?.all("pattern") ?? [] {
            let condition = pattern.child("dependencies").map(parseCondition) ?? parseLooseCondition(pattern)
            patterns.append((condition, FomodPluginType(attribute: pattern.child("type")?.attr("name"))))
        }
        return .dependent(defaultType: defaultType, patterns: patterns)
    }

    private static func parseFiles(_ node: XMLElement2) throws -> [FomodFileEntry] {
        var out: [FomodFileEntry] = []
        for child in node.children where child.name == "file" || child.name == "folder" {
            guard let source = child.attr("source") else { continue }
            let rawDestination = child.attr("destination")
            let destination: String? = try rawDestination.map { try ModPath.sanitizeLoose($0) }
            let isDirectory = rawDestination.map { $0.hasSuffix("/") || $0.hasSuffix("\\") } ?? false
            out.append(FomodFileEntry(source: try ModPath.sanitizeLoose(source), destination: destination,
                                      destinationIsDirectory: isDirectory, isFolder: child.name == "folder",
                                      alwaysInstall: bool(child.attr("alwaysinstall")), installIfUsable: bool(child.attr("installifusable")),
                                      priority: Int(child.attr("priority")?.trimmingCharacters(in: .whitespaces) ?? "") ?? 0))
        }
        return out
    }

    private static func bool(_ text: String?) -> Bool {
        ["true", "1", "yes"].contains((text ?? "").trimmingCharacters(in: .whitespaces).lowercased())
    }

    // MARK: Conditions

    /// A `<dependencies operator="And|Or">` element (also `<visible>` and `<moduleDependencies>`).
    static func parseCondition(_ node: XMLElement2) -> FomodCondition {
        var items: [FomodCondition] = []
        for child in node.children {
            switch child.name {
            case "flagdependency": items.append(.flag(name: child.attr("flag") ?? "", value: child.attr("value") ?? ""))
            case "filedependency":
                let state: FomodFileState
                switch (child.attr("state") ?? "").lowercased() {
                case "active": state = .active
                case "inactive": state = .inactive
                default: state = .missing
                }
                items.append(.file(path: child.attr("file") ?? "", state: state))
            case "dependencies": items.append(parseCondition(child))
            case "gamedependency", "fosedependency", "versiondependency": items.append(.satisfied)
            default: break
            }
        }
        return (node.attr("operator") ?? "And").lowercased() == "or" ? .any(items) : .all(items)
    }

    /// Older files put the dependency elements straight into `<pattern>`.
    private static func parseLooseCondition(_ node: XMLElement2) -> FomodCondition { parseCondition(node) }
}
