import ImageIO
import SwiftUI

/// The FOMOD installer as a wizard: one page per step, groups of options with images and descriptions, Back / Next / Install.
///
/// Look: Liquid Glass over a dark Anomaly backdrop (iOS 26 / macOS 26 and later; an ultra-thin material before that; solid
/// panels with Reduce Transparency). Colours and the backdrop come from `FomodTheme` in the environment: `.fomodTheme(...)`.
/// Motion is short and springy, a fade with Reduce Motion, and none at all for moving the cursor.
///
/// Input, all at once:
/// - Touch and mouse: tap an option or a footer button.
/// - Keyboard: the arrow keys move a highlight over the options and then the footer buttons, Return or Space presses it,
///   Escape goes back (Cancel on the first step).
/// - Controller: the view has no controller code of its own. The code that reads the pad creates the `FomodSession` itself,
///   hands it to `init(session:onFinish:onCancel:)` and calls `session.send(.up / .down / .left / .right / .confirm / .back /
///   .next)`: the same highlight, the same actions. (In the launcher: D-pad, Cross, Circle and R1.) The highlight is a
///   double ring (accent outside, white inside) with a glow, readable on glass whatever is behind it.
///
/// Every element has an accessibility label, value and traits, so VoiceOver and Switch Control work through the same actions.
public struct FomodWizardView: View {
    private enum Source {
        case installer(FomodInstaller, FomodEnvironment)
        case session(FomodSession)
    }

    private let source: Source
    private let onFinish: (FomodChoices) -> Void
    private let onCancel: () -> Void

    public init(installer: FomodInstaller, onFinish: @escaping (FomodChoices) -> Void, onCancel: @escaping () -> Void) {
        source = .installer(installer, FomodEnvironment())
        self.onFinish = onFinish
        self.onCancel = onCancel
    }

    /// `environment` answers `fileDependency` questions (is that file in the game folder or an enabled mod?).
    public init(installer: FomodInstaller, environment: FomodEnvironment, onFinish: @escaping (FomodChoices) -> Void,
                onCancel: @escaping () -> Void) {
        source = .installer(installer, environment)
        self.onFinish = onFinish
        self.onCancel = onCancel
    }

    /// For a caller that drives the wizard itself (a pad): the caller owns `session` and keeps it for as long as the wizard shows.
    public init(session: FomodSession, onFinish: @escaping (FomodChoices) -> Void, onCancel: @escaping () -> Void) {
        source = .session(session)
        self.onFinish = onFinish
        self.onCancel = onCancel
        session.onFinish = onFinish   // a pad may press Install before the first redraw
        session.onCancel = onCancel
    }

    public var body: some View {
        switch source {
        case .installer(let installer, let environment):
            OwnedWizard(installer: installer, environment: environment, onFinish: onFinish, onCancel: onCancel)
        case .session(let session):
            WizardPages(session: session, onFinish: onFinish, onCancel: onCancel)
        }
    }
}

/// Creates the session once and keeps it across updates of the parent.
private struct OwnedWizard: View {
    @StateObject private var session: FomodSession
    let onFinish: (FomodChoices) -> Void
    let onCancel: () -> Void

    init(installer: FomodInstaller, environment: FomodEnvironment, onFinish: @escaping (FomodChoices) -> Void,
         onCancel: @escaping () -> Void) {
        _session = StateObject(wrappedValue: FomodSession(installer: installer, environment: environment))
        self.onFinish = onFinish
        self.onCancel = onCancel
    }

    var body: some View { WizardPages(session: session, onFinish: onFinish, onCancel: onCancel) }
}

// MARK: - Pages

private struct WizardPages: View {
    @ObservedObject var session: FomodSession
    let onFinish: (FomodChoices) -> Void
    let onCancel: () -> Void

    @Environment(\.fomodTheme) private var theme
    @Environment(\.fomodReduceMotion) private var reduceMotion
    @Environment(\.fomodReduceTransparency) private var reduceTransparency
    @Environment(\.fomodFlatLayout) private var flat
    @Environment(\.fomodGlassDisabled) private var glassDisabled
    @FocusState private var keyboardFocus: Bool
    @State private var hovered: FomodPluginRef?
    @State private var settled = false
    @Namespace private var glassSpace

    private var motion: FomodMotion { FomodMotion(reduce: reduceMotion) }

    private var glassActive: Bool {
        if reduceTransparency || glassDisabled { return false }
        if #available(iOS 26, macOS 26, *) { return true }
        return false
    }

    var body: some View {
        ZStack {
            if let backdrop = theme.backdrop { backdrop }
            VStack(spacing: 12) {
                header
                GeometryReader { geometry in
                    Group {
                        if geometry.size.width >= 640 {
                            HStack(alignment: .top, spacing: 12) {
                                optionList.frame(width: max(geometry.size.width * 0.55, 320))
                                detailPanel.frame(maxWidth: .infinity, maxHeight: .infinity)
                            }
                        } else {
                            VStack(spacing: 12) {
                                optionList.frame(maxHeight: .infinity)
                                detailPanel.frame(height: min(geometry.size.height * 0.38, 220))
                            }
                        }
                    }
                    .frame(width: geometry.size.width, height: geometry.size.height, alignment: .top)
                }
                .frame(maxHeight: .infinity)
                footer
            }
            .padding(12)
        }
        .foregroundStyle(theme.text)
        .environment(\.colorScheme, .dark)
        .environment(\.fomodGlassActive, glassActive)
        .environment(\.fomodAnimateEntrance, settled)
        .focusable()
        .focused($keyboardFocus)
        .focusEffectDisabled()
        .onKeyPress(keys: [.upArrow, .downArrow, .leftArrow, .rightArrow, .return, .space, .escape]) { press in
            switch press.key {
            case .upArrow: session.send(.up)
            case .downArrow: session.send(.down)
            case .leftArrow: session.send(.left)
            case .rightArrow: session.send(.right)
            case .escape: session.send(.back)
            default: session.send(.confirm)
            }
            return .handled
        }
        .onAppear {
            session.onFinish = onFinish
            session.onCancel = onCancel
            keyboardFocus = true
            DispatchQueue.main.async { settled = true }
        }
    }

    // MARK: Header and footer

    private var header: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 3) {
                Text(session.installer.moduleName.isEmpty ? "Install options" : session.installer.moduleName)
                    .font(.system(size: 24, weight: .heavy))
                    .lineLimit(2)
                Group {
                    if let step = session.stepModel {
                        Text("Step \(session.stepNumber)  ·  \(step.name)")
                    } else {
                        Text("This installer has nothing to choose.")
                    }
                }
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(theme.accent)
                .contentTransition(reduceMotion ? .opacity : .interpolate)
                .animation(motion.spring, value: session.stepNumber)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 22)
        .padding(.vertical, 14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .fomodSurface(RoundedRectangle(cornerRadius: theme.cornerRadius - 4, style: .continuous))
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isHeader)
    }

    private var footer: some View {
        FomodGlassGroup(spacing: 16) {
            HStack(spacing: 12) {
                FooterButton(title: "Cancel", cursor: .cancel, enabled: true, prominent: false, session: session,
                             glassID: "cancel", namespace: glassSpace) { onCancel() }
                Spacer(minLength: 12)
                FooterButton(title: "Back", cursor: .back, enabled: session.canGoBack, prominent: false, session: session,
                             glassID: "back", namespace: glassSpace) { session.back() }
                // Next and Install are one glass shape that changes its label, so it morphs instead of swapping
                if session.isLastStep {
                    FooterButton(title: "Install", cursor: .primary, enabled: session.canFinish, prominent: true, session: session,
                                 glassID: "primary", namespace: glassSpace) { onFinish(session.choices()) }
                } else {
                    FooterButton(title: "Next", cursor: .primary, enabled: session.canGoNext, prominent: true, session: session,
                                 glassID: "primary", namespace: glassSpace) { session.next() }
                }
            }
        }
        .animation(motion.spring, value: session.isLastStep)
        .padding(.horizontal, 4)
    }

    // MARK: Options

    private var optionList: some View {
        ScrollViewReader { proxy in
            ZStack(alignment: .top) {
                ScrollOrFlat {
                    FomodGlassGroup(spacing: 4) {
                        VStack(alignment: .leading, spacing: 22) {
                            if let step = session.currentStep, let model = session.stepModel {
                                ForEach(Array(model.groups.enumerated()), id: \.offset) { g, group in
                                    groupView(step: step, index: g, group: group)
                                        .modifier(FomodStaggerIn(index: g))
                                }
                            }
                        }
                        .padding(.horizontal, 4)
                        .padding(.vertical, 6)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .id(session.currentStep ?? -1)
                .transition(.asymmetric(insertion: enterTransition, removal: .opacity))
            }
            .animation(motion.spring, value: session.currentStep)
            .onChange(of: session.cursor) { _, cursor in
                if case .option(let ref)? = cursor { proxy.scrollTo(ref, anchor: .center) }
            }
        }
    }

    /// The new page comes in from the side the player is heading to; an exit is only a fade, softer than the entrance.
    private var enterTransition: AnyTransition {
        reduceMotion ? .opacity : .opacity.combined(with: .offset(x: session.forward ? 28 : -28))
    }

    private func groupView(step: Int, index: Int, group: FomodGroup) -> some View {
        let ref = FomodGroupRef(step: step, group: index)
        return VStack(alignment: .leading, spacing: 10) {
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Text(group.name).font(.system(size: 20, weight: .bold))
                    Text(Self.rule(for: group.type)).font(.system(size: 14, weight: .medium)).foregroundStyle(theme.dimText)
                }
                .accessibilityElement(children: .combine)
                .accessibilityAddTraits(.isHeader)
                LinearGradient(colors: [theme.rust, theme.rust.opacity(0)], startPoint: .leading, endPoint: .trailing)
                    .frame(height: 1)
            }
            if let problem = session.problem(in: ref) {
                Label(problem, systemImage: "exclamationmark.circle.fill")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(theme.danger)
                    .transition(.opacity)
            }
            ForEach(Array(group.plugins.enumerated()), id: \.offset) { p, _ in
                optionRow(FomodPluginRef(step: step, group: index, plugin: p), single: group.type.isSingleChoice)
            }
        }
        .animation(motion.quick, value: session.problem(in: ref))
    }

    private func optionRow(_ ref: FomodPluginRef, single: Bool) -> some View {
        let plugin = session.plugin(ref)
        let on = session.isSelected(ref)
        let locked = session.isLocked(ref)
        let type = session.effectiveType(ref)
        let highlighted = session.cursor == .option(ref)
        let shape = RoundedRectangle(cornerRadius: 20, style: .continuous)
        let position = session.optionRefs.firstIndex(of: ref) ?? 0
        return Button {
            session.cursor = .option(ref)
            session.toggle(ref)
        } label: {
            HStack(spacing: 14) {
                SelectionMark(on: on, single: single)
                VStack(alignment: .leading, spacing: 3) {
                    Text(plugin.name)
                        .font(.system(size: 18, weight: .semibold))
                        .foregroundStyle(type == .notUsable ? theme.dimText : theme.text)
                        .multilineTextAlignment(.leading)
                    if let badge = Self.badge(for: type, locked: locked) {
                        HStack(spacing: 6) {
                            if type == .recommended {
                                Circle().fill(theme.signal).frame(width: 7, height: 7)   // the one place radiation green is used
                            } else if type == .couldBeUsable || type == .notUsable {
                                Image(systemName: "exclamationmark.triangle.fill").font(.system(size: 11)).foregroundStyle(theme.rust)
                            }
                            Text(badge).font(.system(size: 13, weight: .medium)).foregroundStyle(theme.dimText)
                        }
                    }
                }
                Spacer(minLength: 0)
                if locked {
                    Image(systemName: "lock.fill").font(.system(size: 13, weight: .semibold)).foregroundStyle(theme.dimText)
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)
            .frame(maxWidth: .infinity, minHeight: 60, alignment: .leading)
            .contentShape(shape)
            .fomodSurface(shape, tint: on ? theme.accent : nil, strength: 0.30, interactive: !locked,
                          id: "row-\(position)", in: glassSpace)
            .overlay(CursorRing(shape: shape, visible: highlighted))
            .opacity(locked && !on ? 0.62 : 1)
        }
        .buttonStyle(FomodPressStyle())
        .focusable(false)
        .id(ref)
        .onHover { inside in
            if inside { hovered = ref } else if hovered == ref { hovered = nil }
        }
        .accessibilityLabel(plugin.name)
        .accessibilityValue(on ? "selected" : "not selected")
        .accessibilityHint(locked ? (Self.badge(for: type, locked: true) ?? "") : "")
        .accessibilityAddTraits(on ? .isSelected : [])
    }

    // MARK: Detail

    /// What the side panel is about: the hovered option, the highlighted one, or the first selected one.
    private var shownPlugin: FomodPluginRef? {
        if let hovered, session.currentStep == hovered.step { return hovered }
        if case .option(let ref)? = session.cursor, session.currentStep == ref.step { return ref }
        guard let step = session.currentStep, let model = session.stepModel else { return nil }
        for (g, group) in model.groups.enumerated() {
            for p in group.plugins.indices where session.isSelected(FomodPluginRef(step: step, group: g, plugin: p)) {
                return FomodPluginRef(step: step, group: g, plugin: p)
            }
        }
        return nil
    }

    private var detailPanel: some View {
        let outer = theme.cornerRadius
        let padding: CGFloat = 18
        return ScrollOrFlat {
            ZStack(alignment: .topLeading) {
                VStack(alignment: .leading, spacing: 14) {
                    if let ref = shownPlugin {
                        detail(of: ref, imageRadius: outer - padding)
                    } else if let image = FomodImageCache.image(at: session.installer.imageURL(for: session.installer.moduleImagePath)) {
                        FomodPicture(image: image, radius: outer - padding)
                    } else {
                        Text("Select an option to read about it.").foregroundStyle(theme.dimText)
                    }
                }
                .id(shownPlugin)
                .transition(.opacity)
            }
            .animation(motion.quick, value: shownPlugin)
            .padding(padding)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        .fomodSurface(RoundedRectangle(cornerRadius: outer, style: .continuous))
        .clipShape(RoundedRectangle(cornerRadius: outer, style: .continuous))
    }

    @ViewBuilder
    private func detail(of ref: FomodPluginRef, imageRadius: CGFloat) -> some View {
        let plugin = session.plugin(ref)
        if let image = FomodImageCache.image(at: session.installer.imageURL(for: plugin.imagePath)) {
            FomodPicture(image: image, radius: imageRadius)
        }
        Text(plugin.name).font(.system(size: 22, weight: .bold))
        if plugin.description.isEmpty {
            Text("No description.").foregroundStyle(theme.dimText)
        } else {
            Text(plugin.description)
                .font(.system(size: 16))
                .foregroundStyle(theme.text.opacity(0.88))
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    // MARK: Helpers

    static func rule(for type: FomodGroupType) -> String {
        switch type {
        case .selectExactlyOne: return "Choose one"
        case .selectAtMostOne: return "Choose one or none"
        case .selectAtLeastOne: return "Choose at least one"
        case .selectAll: return "All included"
        case .selectAny: return "Choose any"
        }
    }

    static func badge(for type: FomodPluginType, locked: Bool) -> String? {
        switch type {
        case .required: return "Required"
        case .recommended: return "Recommended"
        case .notUsable: return "Not usable with your choices"
        case .couldBeUsable: return "May not work with your choices"
        case .optional: return locked ? "Included" : nil
        }
    }
}

// MARK: - Pieces

/// The radio dot or the checkbox of an option: the frame stays, the mark pops in and out (scale 0.25 to 1, opacity, blur).
private struct SelectionMark: View {
    let on: Bool
    let single: Bool
    @Environment(\.fomodTheme) private var theme
    @Environment(\.fomodReduceMotion) private var reduceMotion

    var body: some View {
        let size: CGFloat = 26
        ZStack {
            if single {
                Circle().strokeBorder(on ? theme.accent : theme.dimText, lineWidth: 2)
            } else {
                RoundedRectangle(cornerRadius: 8, style: .continuous).strokeBorder(on ? theme.accent : theme.dimText, lineWidth: 2)
            }
            if on {
                Group {
                    if single {
                        Circle().fill(theme.accent).frame(width: size - 12, height: size - 12)
                    } else {
                        Image(systemName: "checkmark").font(.system(size: 14, weight: .bold)).foregroundStyle(theme.accent)
                    }
                }
                .transition(.fomodIcon(reduce: reduceMotion))
            }
        }
        .frame(width: size, height: size)
        .animation(reduceMotion ? .easeOut(duration: 0.15) : .spring(duration: 0.3, bounce: 0), value: on)
        .accessibilityHidden(true)
    }
}

/// The highlight of the cursor: an accent ring with a white line inside and a glow. It moves without animation: a D-pad or the
/// arrow keys are used constantly, and the ring must be where the finger is.
private struct CursorRing<S: InsettableShape>: View {
    let shape: S
    let visible: Bool
    @Environment(\.fomodTheme) private var theme

    var body: some View {
        shape.strokeBorder(theme.accent, lineWidth: 3)
            .overlay(shape.inset(by: 3).strokeBorder(Color.white.opacity(0.85), lineWidth: 1))
            .shadow(color: theme.accent.opacity(0.55), radius: 10)
            .opacity(visible ? 1 : 0)
            .allowsHitTesting(false)
            .transaction { $0.animation = nil }
    }
}

/// Cancel, Back, Next / Install. The cursor ring shows on them like on the options.
private struct FooterButton: View {
    let title: String
    let cursor: FomodCursor
    let enabled: Bool
    let prominent: Bool
    @ObservedObject var session: FomodSession
    let glassID: String
    let namespace: Namespace.ID
    let action: () -> Void

    @Environment(\.fomodTheme) private var theme

    var body: some View {
        let shape = Capsule()
        Button {
            session.cursor = cursor
            if enabled { action() }
        } label: {
            Text(title)
                .font(.system(size: 18, weight: prominent ? .bold : .semibold))
                .foregroundStyle(prominent && enabled ? Color(red: 0.10, green: 0.07, blue: 0.02) : theme.text)
                .padding(.horizontal, 26)
                .frame(minWidth: 104, minHeight: 52)
                .contentShape(shape)
                .fomodSurface(shape, tint: prominent && enabled ? theme.accent : nil, strength: prominent ? 0.9 : 0,
                              interactive: enabled, id: glassID, in: namespace)
                .overlay(CursorRing(shape: shape, visible: session.cursor == cursor).padding(-3))
                .opacity(enabled ? 1 : 0.45)
        }
        .buttonStyle(FomodPressStyle())
        .focusable(false)
        .accessibilityLabel(title)
        .accessibilityAddTraits(enabled ? [] : .isStaticText)
    }
}

/// An option's picture: concentric with its card, with the hairline outline images get (white at 10% on dark).
private struct FomodPicture: View {
    let image: CGImage
    let radius: CGFloat

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: max(radius, 6), style: .continuous)
        Image(decorative: image, scale: 1)
            .resizable()
            .scaledToFit()
            .clipShape(shape)
            .overlay(shape.strokeBorder(Color.white.opacity(0.10), lineWidth: 1))
            .frame(maxWidth: .infinity, maxHeight: 300)
    }
}

/// After a step change a page's groups come in one after another, 60 ms apart, rising 10 pt while they fade in. Reduce Motion:
/// a plain fade. The first page is not animated.
struct FomodStaggerIn: ViewModifier {
    var index: Int
    @Environment(\.fomodReduceMotion) private var reduceMotion
    @Environment(\.fomodFlatLayout) private var flat
    @Environment(\.fomodAnimateEntrance) private var animate
    @State private var shown = false

    func body(content: Content) -> some View {
        // the first page is simply there; only the pages after a step change come in
        content
            .opacity(shown || flat || !animate ? 1 : 0)
            .offset(y: shown || flat || !animate || reduceMotion ? 0 : 10)
            .onAppear {
                guard !flat, animate, !shown else { return }
                withAnimation(FomodMotion(reduce: reduceMotion).stagger(index)) { shown = true }
            }
    }
}

/// Decodes option images once, at screen size: installer pictures are often much larger than the panel.
enum FomodImageCache {
    private static let cache = NSCache<NSURL, CGImageBox>()

    private final class CGImageBox {
        let image: CGImage
        init(_ image: CGImage) { self.image = image }
    }

    static func image(at url: URL?) -> CGImage? {
        guard let url else { return nil }
        if let hit = cache.object(forKey: url as NSURL) { return hit.image }
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 1024,
        ]
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else { return nil }
        cache.setObject(CGImageBox(image), forKey: url as NSURL)
        return image
    }
}

/// `ImageRenderer` draws nothing inside a ScrollView and never runs `onAppear`, so the offscreen tests flatten the pages with
/// this switch.
struct FomodFlatLayoutKey: EnvironmentKey { static let defaultValue = false }

extension EnvironmentValues {
    var fomodFlatLayout: Bool {
        get { self[FomodFlatLayoutKey.self] }
        set { self[FomodFlatLayoutKey.self] = newValue }
    }
}

private struct ScrollOrFlat<Content: View>: View {
    @Environment(\.fomodFlatLayout) private var flat
    @ViewBuilder let content: () -> Content

    var body: some View {
        if flat {
            content().frame(minHeight: 0, maxHeight: .infinity, alignment: .top).clipped()
        } else {
            ScrollView { content() }.scrollIndicators(.hidden)
        }
    }
}
