import ImageIO
import SwiftUI

/// The FOMOD installer as a wizard: one page per step, groups of options with images and descriptions, Back / Next / Install.
///
/// Input, all at once:
/// - Touch and mouse: tap an option or a footer button.
/// - Keyboard: the arrow keys move a highlight over the options and then the footer buttons, Return or Space presses it,
///   Escape goes back (Cancel on the first step).
/// - Controller: the view has no controller code of its own. The code that reads the pad creates the `FomodSession` itself,
///   hands it to `init(session:onFinish:onCancel:)` and calls `session.send(.up / .down / .left / .right / .confirm / .back /
///   .next)`: the same highlight, the same actions. (In the launcher: D-pad, Cross, Circle and R1.)
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

private struct WizardPages: View {
    @ObservedObject var session: FomodSession
    let onFinish: (FomodChoices) -> Void
    let onCancel: () -> Void

    @FocusState private var keyboardFocus: Bool
    @State private var hovered: FomodPluginRef?

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            GeometryReader { geometry in
                if geometry.size.width >= 640 {
                    HStack(alignment: .top, spacing: 0) {
                        optionList.frame(width: max(geometry.size.width * 0.55, 320))
                        Divider()
                        detailPanel.frame(maxWidth: .infinity)
                    }
                } else {
                    VStack(spacing: 0) {
                        optionList
                        Divider()
                        detailPanel.frame(height: min(geometry.size.height * 0.4, 220))
                    }
                }
            }
            Divider()
            footer
        }
        .background(.background)
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
        }
    }

    // MARK: Header and footer

    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(session.installer.moduleName.isEmpty ? "Install options" : session.installer.moduleName)
                .font(.title2.weight(.semibold))
                .lineLimit(2)
            if let step = session.stepModel {
                Text("Step \(session.stepNumber): \(step.name)").font(.headline).foregroundStyle(.secondary)
            } else {
                Text("This installer has nothing to choose.").font(.headline).foregroundStyle(.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isHeader)
    }

    private var footer: some View {
        HStack(spacing: 12) {
            FooterButton(title: "Cancel", cursor: .cancel, enabled: true, prominent: false, session: session) { onCancel() }
            Spacer()
            FooterButton(title: "Back", cursor: .back, enabled: session.canGoBack, prominent: false, session: session) { session.back() }
            if session.isLastStep {
                FooterButton(title: "Install", cursor: .primary, enabled: session.canFinish, prominent: true, session: session) {
                    onFinish(session.choices())
                }
            } else {
                FooterButton(title: "Next", cursor: .primary, enabled: session.canGoNext, prominent: true, session: session) { session.next() }
            }
        }
        .padding(16)
    }

    // MARK: Options

    private var optionList: some View {
        ScrollViewReader { proxy in
            ScrollOrFlat {
                VStack(alignment: .leading, spacing: 20) {
                    if let step = session.currentStep, let model = session.stepModel {
                        ForEach(Array(model.groups.enumerated()), id: \.offset) { g, group in
                            groupView(step: step, index: g, group: group)
                        }
                    }
                }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .onChange(of: session.cursor) { _, cursor in
                if case .option(let ref)? = cursor { withAnimation(.easeOut(duration: 0.15)) { proxy.scrollTo(ref, anchor: .center) } }
            }
        }
    }

    private func groupView(step: Int, index: Int, group: FomodGroup) -> some View {
        let ref = FomodGroupRef(step: step, group: index)
        return VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                Text(group.name).font(.headline)
                Text(Self.rule(for: group.type)).font(.subheadline).foregroundStyle(.secondary)
            }
            .accessibilityElement(children: .combine)
            .accessibilityAddTraits(.isHeader)
            if let problem = session.problem(in: ref) {
                Label(problem, systemImage: "exclamationmark.circle").font(.subheadline).foregroundStyle(.red)
            }
            ForEach(Array(group.plugins.enumerated()), id: \.offset) { p, _ in
                optionRow(FomodPluginRef(step: step, group: index, plugin: p), single: group.type.isSingleChoice)
            }
        }
    }

    private func optionRow(_ ref: FomodPluginRef, single: Bool) -> some View {
        let plugin = session.plugin(ref)
        let on = session.isSelected(ref)
        let locked = session.isLocked(ref)
        let type = session.effectiveType(ref)
        let highlighted = session.cursor == .option(ref)
        return Button {
            session.cursor = .option(ref)
            session.toggle(ref)
        } label: {
            HStack(spacing: 12) {
                Image(systemName: Self.symbol(on: on, single: single))
                    .font(.title3)
                    .foregroundStyle(on ? Color.accentColor : Color.secondary)
                    .frame(width: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text(plugin.name).font(.body).foregroundStyle(type == .notUsable ? Color.secondary : Color.primary)
                    if let badge = Self.badge(for: type, locked: locked) {
                        Text(badge).font(.caption).foregroundStyle(.secondary)
                    }
                }
                Spacer(minLength: 0)
            }
            .padding(.vertical, 10)
            .padding(.horizontal, 12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .contentShape(Rectangle())
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(on ? Color.accentColor.opacity(0.14) : Color.secondary.opacity(0.08))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .strokeBorder(highlighted ? Color.accentColor : Color.clear, lineWidth: 3)
            )
            .opacity(locked && !on ? 0.6 : 1)
        }
        .buttonStyle(.plain)
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
        ScrollOrFlat {
            VStack(alignment: .leading, spacing: 12) {
                if let ref = shownPlugin {
                    detail(of: ref)
                } else if let image = FomodImageCache.image(at: session.installer.imageURL(for: session.installer.moduleImagePath)) {
                    Image(decorative: image, scale: 1).resizable().scaledToFit().frame(maxWidth: .infinity)
                } else {
                    Text("Select an option to read about it.").foregroundStyle(.secondary)
                }
            }
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    @ViewBuilder
    private func detail(of ref: FomodPluginRef) -> some View {
        let plugin = session.plugin(ref)
        if let image = FomodImageCache.image(at: session.installer.imageURL(for: plugin.imagePath)) {
            Image(decorative: image, scale: 1)
                .resizable()
                .scaledToFit()
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .frame(maxWidth: .infinity, maxHeight: 300)
        }
        Text(plugin.name).font(.title3.weight(.semibold))
        if plugin.description.isEmpty {
            Text("No description.").foregroundStyle(.secondary)
        } else {
            Text(plugin.description).fixedSize(horizontal: false, vertical: true)
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

    static func symbol(on: Bool, single: Bool) -> String {
        single ? (on ? "largecircle.fill.circle" : "circle") : (on ? "checkmark.square.fill" : "square")
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

/// Cancel, Back, Next / Install: drawn by hand so the cursor ring shows on them like on the options.
private struct FooterButton: View {
    let title: String
    let cursor: FomodCursor
    let enabled: Bool
    let prominent: Bool
    @ObservedObject var session: FomodSession
    let action: () -> Void

    var body: some View {
        Button {
            session.cursor = cursor
            if enabled { action() }
        } label: {
            Text(title)
                .font(.body.weight(prominent ? .semibold : .regular))
                .foregroundStyle(prominent && enabled ? Color.white : Color.primary)
                .padding(.vertical, 10)
                .padding(.horizontal, 20)
                .background(
                    Capsule().fill(prominent && enabled ? Color.accentColor : Color.secondary.opacity(0.18))
                )
                .overlay(
                    Capsule().strokeBorder(session.cursor == cursor ? Color.accentColor : Color.clear, lineWidth: 3).padding(-3)
                )
                .opacity(enabled ? 1 : 0.45)
        }
        .buttonStyle(.plain)
        .focusable(false)
        .accessibilityLabel(title)
        .accessibilityAddTraits(enabled ? [] : .isStaticText)
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

/// `ImageRenderer` draws nothing inside a ScrollView, so the offscreen tests flatten the pages with this switch.
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
            content().frame(maxHeight: .infinity, alignment: .top)
        } else {
            ScrollView { content() }
        }
    }
}
