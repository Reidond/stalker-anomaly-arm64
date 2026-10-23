import ImageIO
import SwiftUI

/// The FOMOD installer as a wizard: one page per step, with the groups of options as grouped sections, and Back / Next / Install
/// below.
///
/// Look: the system's. An inset-grouped list on iPadOS and a grouped form on the Mac, switches for "choose any", checkmark rows
/// for "choose one", Dynamic Type text styles and semantic colours, and the Liquid Glass button styles on iOS 26 / macOS 26 and
/// later (bordered before). The accent comes from `FomodTheme` (the launcher's amber); the host may add a backdrop. The page is
/// meant to sit in a host that has a title and a Cancel button, such as the launcher's import sheet; a bare window asks for its own
/// with `showsCancel`.
///
/// Input, all at once:
/// - Touch and mouse: tap a row or a button.
/// - Keyboard: the arrow keys move a highlight over the options and then the footer buttons, Return or Space presses it,
///   Escape goes back (cancels on the first step).
/// - Controller: the view has no controller code of its own. The code that reads the pad creates the `FomodSession` itself,
///   hands it to `init(session:onFinish:onCancel:)` and calls `session.send(.up / .down / .left / .right / .confirm / .back /
///   .next)`: the same highlight, the same actions. The highlight is drawn like the launcher's: an accent ring around the row
///   and a tint behind it, only while a pad or the keyboard was the last input.
///
/// Motion is a short spring without bounce (a fade with Reduce Motion), none for moving the cursor. Every row has an
/// accessibility label, value and traits.
public struct FomodWizardView: View {
    private enum Source {
        case installer(FomodInstaller, FomodEnvironment)
        case session(FomodSession)
    }

    private let source: Source
    private let showsCancel: Bool
    private let onFinish: (FomodChoices) -> Void
    private let onCancel: () -> Void

    public init(installer: FomodInstaller, showsCancel: Bool = false, onFinish: @escaping (FomodChoices) -> Void,
                onCancel: @escaping () -> Void) {
        source = .installer(installer, FomodEnvironment())
        self.showsCancel = showsCancel
        self.onFinish = onFinish
        self.onCancel = onCancel
    }

    /// `environment` answers `fileDependency` questions (is that file in the game folder or an enabled mod?).
    public init(installer: FomodInstaller, environment: FomodEnvironment, showsCancel: Bool = false,
                onFinish: @escaping (FomodChoices) -> Void, onCancel: @escaping () -> Void) {
        source = .installer(installer, environment)
        self.showsCancel = showsCancel
        self.onFinish = onFinish
        self.onCancel = onCancel
    }

    /// For a caller that drives the wizard itself (a pad): the caller owns `session` and keeps it for as long as the wizard shows.
    public init(session: FomodSession, showsCancel: Bool = false, onFinish: @escaping (FomodChoices) -> Void,
                onCancel: @escaping () -> Void) {
        source = .session(session)
        self.showsCancel = showsCancel
        self.onFinish = onFinish
        self.onCancel = onCancel
        session.onFinish = onFinish   // a pad may press Install before the first redraw
        session.onCancel = onCancel
        session.hasCancelButton = showsCancel
    }

    public var body: some View {
        switch source {
        case .installer(let installer, let environment):
            OwnedWizard(installer: installer, environment: environment, showsCancel: showsCancel, onFinish: onFinish,
                        onCancel: onCancel)
        case .session(let session):
            WizardPages(session: session, showsCancel: showsCancel, onFinish: onFinish, onCancel: onCancel)
        }
    }
}

/// Creates the session once and keeps it across updates of the parent.
private struct OwnedWizard: View {
    @StateObject private var session: FomodSession
    let showsCancel: Bool
    let onFinish: (FomodChoices) -> Void
    let onCancel: () -> Void

    init(installer: FomodInstaller, environment: FomodEnvironment, showsCancel: Bool, onFinish: @escaping (FomodChoices) -> Void,
         onCancel: @escaping () -> Void) {
        _session = StateObject(wrappedValue: FomodSession(installer: installer, environment: environment))
        self.showsCancel = showsCancel
        self.onFinish = onFinish
        self.onCancel = onCancel
    }

    var body: some View { WizardPages(session: session, showsCancel: showsCancel, onFinish: onFinish, onCancel: onCancel) }
}

// MARK: - Pages

private struct WizardPages: View {
    @ObservedObject var session: FomodSession
    let showsCancel: Bool
    let onFinish: (FomodChoices) -> Void
    let onCancel: () -> Void

    @Environment(\.fomodTheme) private var theme
    @Environment(\.fomodReduceMotion) private var reduceMotion
    @FocusState private var keyboardFocus: Bool

    private var motion: FomodMotion { FomodMotion(reduce: reduceMotion) }

    var body: some View {
        ScrollViewReader { proxy in
            ZStack {
                form
                    .id(session.currentStep ?? -1)
                    .transition(.asymmetric(insertion: enterTransition, removal: .opacity))
            }
            .animation(motion.spring, value: session.currentStep)
            .onChange(of: session.cursor) { _, cursor in
                if case .option(let ref)? = cursor { proxy.scrollTo(ref, anchor: .center) }
            }
        }
        .background { if let backdrop = theme.backdrop { backdrop.ignoresSafeArea() } }
        .safeAreaInset(edge: .bottom, spacing: 0) { footer }
        .tint(theme.accent)
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
            session.hasCancelButton = showsCancel
            keyboardFocus = true
        }
    }

    /// The new page comes in from the side the player is heading to; an exit is only a fade, softer than the entrance.
    private var enterTransition: AnyTransition {
        reduceMotion ? .opacity : .opacity.combined(with: .offset(x: session.forward ? 28 : -28))
    }

    // MARK: Form

    private var form: some View {
        FomodForm(hidesBackground: theme.backdrop != nil) {
            // a section with no rows draws only its header: the page's title, without a card around it
            Section {} header: { stepHeader }
            if let step = session.currentStep, let model = session.stepModel {
                ForEach(Array(model.groups.enumerated()), id: \.offset) { g, group in
                    groupSection(step: step, index: g, group: group)
                }
            }
        }
    }

    private var stepHeader: some View {
        VStack(alignment: .leading, spacing: 10) {
            if session.stepNumber == 1,
               let image = FomodImageCache.image(at: session.installer.imageURL(for: session.installer.moduleImagePath)) {
                FomodPicture(image: image, radius: 14, maxHeight: 150)
            }
            if let step = session.stepModel {
                VStack(alignment: .leading, spacing: 2) {
                    Text(session.installer.moduleName.isEmpty ? "Install options" : session.installer.moduleName)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    Text("Step \(session.stepNumber): \(step.name)")
                        .font(.title2.weight(.bold))
                        .foregroundStyle(.primary)
                        .contentTransition(reduceMotion ? .opacity : .interpolate)
                        .animation(motion.spring, value: session.stepNumber)
                }
            } else {
                Text("This installer has nothing to choose.").font(.title3.weight(.semibold)).foregroundStyle(.primary)
            }
        }
        .textCase(nil)
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.top, 8)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isHeader)
    }

    private func groupSection(step: Int, index: Int, group: FomodGroup) -> some View {
        let ref = FomodGroupRef(step: step, group: index)
        let problem = session.problem(in: ref)
        return Section {
            ForEach(Array(group.plugins.enumerated()), id: \.offset) { p, _ in
                optionRow(FomodPluginRef(step: step, group: index, plugin: p), single: group.type.isSingleChoice)
            }
        } header: {
            Text(group.name)
        } footer: {
            if let problem {
                Label(problem, systemImage: "exclamationmark.circle.fill").foregroundStyle(.red)
            } else {
                Text(Self.rule(for: group.type))
            }
        }
    }

    // MARK: Rows

    @ViewBuilder
    private func optionRow(_ ref: FomodPluginRef, single: Bool) -> some View {
        let plugin = session.plugin(ref)
        let on = session.isSelected(ref)
        let locked = session.isLocked(ref)
        let type = session.effectiveType(ref)
        Group {
            if single {
                Button {
                    session.showsCursor = false
                    session.cursor = .option(ref)
                    session.toggle(ref)
                } label: {
                    HStack(spacing: 12) {
                        OptionLabel(plugin: plugin, type: type, locked: locked, installer: session.installer)
                        ZStack {
                            if on {
                                Image(systemName: "checkmark")
                                    .fontWeight(.semibold)
                                    .foregroundStyle(.tint)
                                    .transition(.fomodIcon(reduce: reduceMotion))
                            }
                        }
                        .frame(width: 22)
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .opacity(locked && !on ? 0.55 : 1)
            } else {
                Toggle(isOn: Binding(get: { on }, set: { _ in
                    session.showsCursor = false
                    session.cursor = .option(ref)
                    session.toggle(ref)
                })) {
                    OptionLabel(plugin: plugin, type: type, locked: locked, installer: session.installer)
                }
                .toggleStyle(.switch)
                .disabled(locked)
            }
        }
        .animation(motion.spring, value: on)
        .cursorRow(session: session, ref: .option(ref))
        .id(ref)
        .accessibilityLabel(plugin.name)
        .accessibilityValue(on ? "selected" : "not selected")
        .accessibilityHint(locked ? (Self.badge(for: type, locked: true)?.text ?? "") : "")
        .accessibilityAddTraits(on ? .isSelected : [])
    }

    // MARK: Footer

    private var footer: some View {
        FomodGlassGroup(spacing: 12) {
            HStack(spacing: 4) {
                if showsCancel {
                    Button { session.showsCursor = false; onCancel() } label: { Text("Cancel").frame(maxWidth: .infinity) }
                        .modifier(FomodButtonStyle(prominent: false))
                        .cursorButton(session: session, cursor: .cancel)
                }
                Button { session.showsCursor = false; session.back() } label: { Text("Back").frame(maxWidth: .infinity) }
                    .modifier(FomodButtonStyle(prominent: false))
                    .disabled(!session.canGoBack)
                    .cursorButton(session: session, cursor: .back)
                Button {
                    session.showsCursor = false
                    if session.isLastStep { onFinish(session.choices()) } else { session.next() }
                } label: {
                    Text(session.isLastStep ? "Install" : "Next")
                        .font(.body.weight(.semibold))
                        .frame(maxWidth: .infinity)
                        .contentTransition(reduceMotion ? .opacity : .interpolate)
                }
                .modifier(FomodButtonStyle(prominent: true))
                .disabled(!session.primaryEnabled)
                .cursorButton(session: session, cursor: .primary)
                .animation(motion.spring, value: session.isLastStep)
            }
            .controlSize(.large)
            .frame(maxWidth: 560)
            .padding(.horizontal, 20)
            .padding(.vertical, 12)
            .frame(maxWidth: .infinity)
        }
        .background(.bar)
    }

    // MARK: Helpers

    static func rule(for type: FomodGroupType) -> String {
        switch type {
        case .selectExactlyOne: return "Choose one."
        case .selectAtMostOne: return "Choose one or none."
        case .selectAtLeastOne: return "Choose at least one."
        case .selectAll: return "All of these are included."
        case .selectAny: return "Choose any."
        }
    }

    static func badge(for type: FomodPluginType, locked: Bool) -> (text: String, symbol: String)? {
        switch type {
        case .required: return ("Required", "lock.fill")
        case .recommended: return ("Recommended", "star.fill")
        case .notUsable: return ("Not usable with your choices", "exclamationmark.triangle.fill")
        case .couldBeUsable: return ("May not work with your choices", "exclamationmark.triangle.fill")
        case .optional: return locked ? ("Included", "lock.fill") : nil
        }
    }
}

// MARK: - Pieces

/// The grouped list of Settings on iPadOS; the grouped form on the Mac.
private struct FomodForm<Content: View>: View {
    var hidesBackground: Bool
    @ViewBuilder var content: () -> Content

    var body: some View {
        #if os(macOS)
        Form { content() }
            .formStyle(.grouped)
            .scrollContentBackground(hidesBackground ? .hidden : .automatic)
        #else
        List { content() }
            .listStyle(.insetGrouped)
            .scrollContentBackground(hidesBackground ? .hidden : .automatic)
        #endif
    }
}

/// A row's text: the name, the description beneath it, and a one-line mark (Recommended, Required, Not usable).
private struct OptionLabel: View {
    let plugin: FomodPlugin
    let type: FomodPluginType
    let locked: Bool
    let installer: FomodInstaller

    var body: some View {
        HStack(spacing: 12) {
            if let image = FomodImageCache.image(at: installer.imageURL(for: plugin.imagePath)) {
                FomodPicture(image: image, radius: 8, thumbnail: CGSize(width: 62, height: 44))
                    .accessibilityHidden(true)
            }
            VStack(alignment: .leading, spacing: 3) {
                Text(plugin.name).font(.headline).foregroundStyle(type == .notUsable ? Color.secondary : Color.primary)
                if !plugin.description.isEmpty {
                    Text(plugin.description).font(.subheadline).foregroundStyle(.secondary).lineLimit(3)
                }
                if let badge = WizardPages.badge(for: type, locked: locked) {
                    Label(badge.text, systemImage: badge.symbol)
                        .font(.caption)
                        .foregroundStyle(type == .recommended ? AnyShapeStyle(.tint) : AnyShapeStyle(.secondary))
                        .symbolRenderingMode(type == .notUsable || type == .couldBeUsable ? .multicolor : .monochrome)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

/// The cursor as the launcher draws its pad focus: an accent ring around the row and a tint behind it.
private struct CursorRowModifier: ViewModifier {
    @ObservedObject var session: FomodSession
    let ref: FomodCursor
    @Environment(\.fomodTheme) private var theme

    func body(content: Content) -> some View {
        let shown = session.showsCursor && session.cursor == ref
        content
            .overlay {
                if shown {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .strokeBorder(theme.accent, lineWidth: 3)
                        .padding(-4)
                        .allowsHitTesting(false)
                        .transaction { $0.animation = nil }   // the cursor is where the finger is, not on its way
                }
            }
            .listRowBackground(shown ? theme.accent.opacity(0.16) : nil)
    }
}

/// The same ring on a footer button. It is drawn outside the button, in a 4 pt margin of its own: an AppKit button clips what
/// is drawn over it.
private struct CursorButtonModifier: ViewModifier {
    @ObservedObject var session: FomodSession
    let cursor: FomodCursor
    @Environment(\.fomodTheme) private var theme

    func body(content: Content) -> some View {
        content
            .padding(4)
            .overlay {
                if session.showsCursor && session.cursor == cursor {
                    Capsule().strokeBorder(theme.accent, lineWidth: 3).allowsHitTesting(false)
                        .transaction { $0.animation = nil }
                }
            }
    }
}

private extension View {
    func cursorRow(session: FomodSession, ref: FomodCursor) -> some View { modifier(CursorRowModifier(session: session, ref: ref)) }
    func cursorButton(session: FomodSession, cursor: FomodCursor) -> some View { modifier(CursorButtonModifier(session: session, cursor: cursor)) }
}

/// An option's picture: rounded, with the hairline outline images get (white at 10% on dark, black at 10% on light). A banner
/// keeps its own shape; a thumbnail fills a fixed frame.
private struct FomodPicture: View {
    let image: CGImage
    let radius: CGFloat
    var maxHeight: CGFloat = 300
    /// A fixed frame for a thumbnail, which the picture fills.
    var thumbnail: CGSize?
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: radius, style: .continuous)
        let outline = (scheme == .dark ? Color.white : Color.black).opacity(0.10)
        if let thumbnail {
            Image(decorative: image, scale: 1)
                .resizable()
                .aspectRatio(contentMode: .fill)
                .frame(width: thumbnail.width, height: thumbnail.height)
                .clipShape(shape)
                .overlay(shape.strokeBorder(outline, lineWidth: 1))
        } else {
            Image(decorative: image, scale: 1)
                .resizable()
                .scaledToFit()
                .clipShape(shape)
                .overlay(shape.strokeBorder(outline, lineWidth: 1))
                .frame(maxWidth: .infinity, maxHeight: maxHeight)
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
