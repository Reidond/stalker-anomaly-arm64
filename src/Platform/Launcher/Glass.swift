// Liquid Glass surfaces and what goes on them. `glassSurface` is the one place that decides how a surface is drawn:
//  - iPadOS / macOS 26 and later: `glassEffect` (regular glass, an amber or rust tint where it should speak), interactive for controls;
//  - earlier systems, or Reduce Transparency: a material (opaque dark for Reduce Transparency) with a hairline, so the shapes, the
//    spacing and the hit areas are the same.
// Glass groups that sit next to each other live in a `GlassGroup` (a `GlassEffectContainer`), so that they blend and morph.
import SwiftUI

enum GlassTint {
    case none, amber, rust, radiation

    var color: Color? {
        switch self {
        case .none: nil
        case .amber: Theme.amber
        case .rust: Theme.rust
        case .radiation: Theme.radiation
        }
    }
}

struct GlassSurface<S: InsettableShape>: ViewModifier {
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    @Environment(\.launcherSnapshot) private var snapshot
    let shape: S
    var tint: GlassTint = .none
    var interactive = false

    @available(iOS 26.0, macOS 26.0, *)
    private var glass: Glass {
        var result = Glass.regular
        if let color = tint.color { result = result.tint(color.opacity(0.32)) }
        if interactive { result = result.interactive() }
        return result
    }

    func body(content: Content) -> some View {
        if #available(iOS 26.0, macOS 26.0, *), !reduceTransparency, !snapshot {
            content.glassEffect(glass, in: shape)
        } else {
            content
                .background {
                    ZStack {
                        if reduceTransparency {
                            shape.fill(Color(white: 0.13))
                        } else {
                            shape.fill(.ultraThinMaterial)
                        }
                        if let color = tint.color { shape.fill(color.opacity(0.16)) }
                    }
                }
                .overlay(shape.strokeBorder(Color.white.opacity(0.14), lineWidth: 1))
                .shadow(color: .black.opacity(0.22), radius: 10, y: 4)
        }
    }
}

extension View {
    func glassSurface(radius: CGFloat = Theme.rowRadius, tint: GlassTint = .none, interactive: Bool = false) -> some View {
        modifier(GlassSurface(shape: RoundedRectangle(cornerRadius: radius, style: .continuous), tint: tint, interactive: interactive))
    }

    func glassCapsule(tint: GlassTint = .none, interactive: Bool = false) -> some View {
        modifier(GlassSurface(shape: Capsule(), tint: tint, interactive: interactive))
    }
}

/// Glass that is near other glass: one container, so that the shapes blend while they are close and morph when they change.
struct GlassGroup<Content: View>: View {
    var spacing: CGFloat = 12
    @ViewBuilder var content: () -> Content

    var body: some View {
        if #available(iOS 26.0, macOS 26.0, *) {
            GlassEffectContainer(spacing: spacing) { content() }
        } else {
            content()
        }
    }
}

/// Lets a glass shape morph into another one with the same id when the state changes (Play into "Starting").
struct GlassMorph: ViewModifier {
    let id: String
    let namespace: Namespace.ID

    func body(content: Content) -> some View {
        if #available(iOS 26.0, macOS 26.0, *) {
            content.glassEffectID(id, in: namespace)
        } else {
            content
        }
    }
}

extension View {
    func glassMorph(_ id: String, in namespace: Namespace.ID) -> some View { modifier(GlassMorph(id: id, namespace: namespace)) }
}

/// A row-sized button on glass: pressed it gives way a little (the system's interactive glass does that itself on iPadOS 26).
struct GlassRowButtonStyle: ButtonStyle {
    var radius: CGFloat = Theme.rowRadius
    var tint: GlassTint = .none
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .glassSurface(radius: radius, tint: tint, interactive: true)
            .scaleEffect(configuration.isPressed && !reduceMotion && !liquidGlassAvailable ? 0.97 : 1)
            .animation(LauncherMotion(reduced: reduceMotion).press, value: configuration.isPressed)
            .contentShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
    }
}

/// What a list row or a card sits on: the glass surface, and the ring and tint that show where the pad's focus is.
struct GlassRowBackground: View {
    var radius: CGFloat = Theme.rowRadius
    var focused = false
    var dimmed = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: radius, style: .continuous)
        Color.clear
            .glassSurface(radius: radius, tint: focused ? .amber : .none)
            .overlay {
                if focused {
                    shape.strokeBorder(Theme.amber, lineWidth: 3)
                }
            }
            .shadow(color: focused ? Theme.amber.opacity(0.45) : .clear, radius: 14)
            .opacity(dimmed ? 0.7 : 1)
            .animation(LauncherMotion(reduced: reduceMotion).focus, value: focused)
    }
}
