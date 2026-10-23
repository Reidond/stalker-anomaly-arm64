// The backdrop: the Zone, dark and out of focus. The game's own main-menu art (generated into the bundle at build time by
// tools/make_launcher_backdrop.sh, never committed), blurred, desaturated and darkened so that the glass and the type on it read; a rust
// glow from the top left and a faint radiation green at the bottom. Without the picture the same glows sit on a plain dark gradient.
import SwiftUI
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif

struct LauncherBackdrop: View {
    var path: String?

    var body: some View {
        ZStack {
            Color(red: 0.035, green: 0.037, blue: 0.034)
            if let picture = BackdropImage.load(path: path) {
                picture
                    .resizable()
                    .scaledToFill()
                    .saturation(0.35)
                    .blur(radius: 26, opaque: true)
                    .opacity(0.9)
                    .overlay(Color.black.opacity(0.38))
                    .clipped()
            }
            RadialGradient(colors: [Theme.rust.opacity(0.34), .clear], center: .topLeading, startRadius: 0, endRadius: 760)
            RadialGradient(colors: [Theme.radiation.opacity(0.07), .clear], center: .bottomTrailing, startRadius: 0, endRadius: 620)
            LinearGradient(colors: [.black.opacity(0.0), .black.opacity(0.55)], startPoint: .center, endPoint: .bottom)
        }
        .ignoresSafeArea()
        .accessibilityHidden(true)
    }
}

enum BackdropImage {
    nonisolated(unsafe) private static var cache: [String: Image] = [:]

    static func load(path: String?) -> Image? {
        let candidates = [path, Bundle.main.path(forResource: "launcher-backdrop", ofType: "jpg")].compactMap { $0 }
        for candidate in candidates {
            if let cached = cache[candidate] { return cached }
            #if canImport(UIKit)
            if let image = UIImage(contentsOfFile: candidate) {
                let result = Image(uiImage: image)
                cache[candidate] = result
                return result
            }
            #elseif canImport(AppKit)
            if let image = NSImage(contentsOfFile: candidate) {
                let result = Image(nsImage: image)
                cache[candidate] = result
                return result
            }
            #endif
        }
        return nil
    }
}
