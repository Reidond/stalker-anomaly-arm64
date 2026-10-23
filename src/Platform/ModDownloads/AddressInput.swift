import Foundation

/// Turns what the user typed or pasted into the address field into something the browser can load.
enum AddressInput {
    enum Result: Equatable {
        case empty
        case url(URL)
        /// Text that is not a link: a web search for it.
        case search(URL)
        /// A link with a scheme other than http(s) ("javascript:", "file:", "itms-apps:"): refused.
        case unsupported(scheme: String)
    }

    static func resolve(_ text: String) -> Result {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return .empty }
        if trimmed.contains(where: { $0.isWhitespace }) { return search(trimmed) }

        if let scheme = explicitScheme(trimmed) {
            guard scheme == "http" || scheme == "https" else { return .unsupported(scheme: scheme) }
            if let url = URL(string: trimmed), url.host != nil, !(url.host ?? "").isEmpty { return .url(url) }
            return search(trimmed)
        }

        // no scheme: "github.com/foo", "localhost:8000/a.zip", "192.168.1.5/x"
        let host = trimmed.split(separator: "/", maxSplits: 1, omittingEmptySubsequences: false)[0]
            .split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false)[0]
            .split(separator: "?", maxSplits: 1, omittingEmptySubsequences: false)[0]
        let isLocal = host == "localhost" || host.hasSuffix(".local") || isIPv4(String(host))
        guard host.contains(".") || isLocal else { return search(trimmed) }
        if let url = URL(string: (isLocal ? "http://" : "https://") + trimmed), url.host != nil { return .url(url) }
        return search(trimmed)
    }

    /// Only absolute http(s) links, for the "download this link" field. Nil for anything else.
    static func httpLink(_ text: String) -> URL? {
        if case .url(let url) = resolve(text), let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https" {
            return url
        }
        return nil
    }

    private static func search(_ text: String) -> Result {
        var components = URLComponents(string: "https://duckduckgo.com/")!
        components.queryItems = [URLQueryItem(name: "q", value: text)]
        return .search(components.url!)
    }

    /// "https" for "https://x", "javascript" for "javascript:alert(1)", nil for "localhost:8000" and "github.com/x".
    private static func explicitScheme(_ text: String) -> String? {
        guard let colon = text.firstIndex(of: ":") else { return nil }
        let scheme = text[..<colon]
        guard let first = scheme.first, first.isLetter,
              scheme.allSatisfy({ $0.isLetter || $0.isNumber || $0 == "+" || $0 == "-" || $0 == "." }) else { return nil }
        let rest = text[text.index(after: colon)...]
        if rest.hasPrefix("//") { return scheme.lowercased() }
        // "host:8000" is a host and a port, "mailto:x" or "javascript:x" is a scheme
        if let firstChar = rest.first, firstChar.isNumber, rest.prefix(while: { $0.isNumber }).count >= 1,
           rest.drop(while: { $0.isNumber }).first.map({ $0 == "/" || $0 == "?" || $0 == "#" }) ?? true {
            return nil
        }
        return scheme.lowercased()
    }

    private static func isIPv4(_ s: String) -> Bool {
        let parts = s.split(separator: ".", omittingEmptySubsequences: false)
        return parts.count == 4 && parts.allSatisfy { Int($0).map { (0...255).contains($0) } ?? false }
    }
}
