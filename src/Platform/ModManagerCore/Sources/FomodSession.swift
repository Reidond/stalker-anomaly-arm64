import Combine
import Foundation

/// Position of a plugin inside the installer: indices into `steps`, `groups`, `plugins`.
public struct FomodPluginRef: Hashable, Sendable {
    public let step: Int
    public let group: Int
    public let plugin: Int
    var groupRef: FomodGroupRef { FomodGroupRef(step: step, group: group) }
}

public struct FomodGroupRef: Hashable, Sendable {
    public let step: Int
    public let group: Int
}

/// What a pad, a keyboard or the launcher's own focus layer can ask of the wizard.
public enum FomodCommand {
    case up, down, left, right
    /// Cross / Return / Space: pick the option under the cursor, or press the footer button under it.
    case confirm
    /// Circle / Escape: the previous step, or Cancel on the first one.
    case back
    /// R1 / Options: the primary button (Next, or Install on the last step), if it is enabled.
    case next
}

/// Where the wizard's cursor is: an option of the current step, or one of the footer buttons.
public enum FomodCursor: Hashable {
    case option(FomodPluginRef)
    /// Only when the page has a Cancel button (`hasCancelButton`).
    case cancel
    case back
    case primary
}

/// The state of one walk through an installer: which step is shown, what is selected, which steps the flags make visible.
/// The wizard drives it with `toggle`, `next` and `back`; the importer replays saved `FomodChoices` through it headless.
public final class FomodSession: ObservableObject {
    public let installer: FomodInstaller
    let environment: FomodEnvironment

    /// Index into `installer.steps` of the step on screen; nil when no step is visible (nothing to ask).
    @Published public private(set) var currentStep: Int?
    @Published public private(set) var selected: Set<FomodPluginRef> = []
    /// True while moving on, false after Back: the direction the page slides in from.
    @Published public private(set) var forward = true
    /// The highlighted element; moved by `send`, and set by a tap or a click.
    @Published public var cursor: FomodCursor?
    /// Whether the cursor is drawn: true after a command from a pad or the keyboard, false once a touch or a click is the
    /// last input (the same rule as the launcher's focus ring).
    @Published public var showsCursor = false
    /// True when the page has a Cancel button of its own (a host without one, such as a bare window, asks for it).
    public var hasCancelButton = false
    /// What Install and Cancel do; the wizard view sets them.
    public var onFinish: ((FomodChoices) -> Void)?
    public var onCancel: (() -> Void)?
    private var touched: Set<FomodGroupRef> = []
    private var history: [Int] = []
    private var saved: [String: [String]] = [:]

    public init(installer: FomodInstaller, environment: FomodEnvironment = FomodEnvironment(), choices: FomodChoices? = nil) {
        self.installer = installer
        self.environment = environment
        for s in choices?.selections ?? [] where saved[Self.savedKey(s.step, s.group)] == nil {
            saved[Self.savedKey(s.step, s.group)] = s.plugins
        }
        if let first = nextVisible(after: -1, flags: [:]) {
            currentStep = first
            enter(first)
        }
        cursor = firstCursor()
    }

    private static func savedKey(_ step: String, _ group: String) -> String { step + "\u{0}" + group }

    // MARK: Navigation

    public var stepModel: FomodStep? { currentStep.map { installer.steps[$0] } }
    /// 1-based position among the steps visited so far.
    public var stepNumber: Int { history.count + 1 }
    public var canGoBack: Bool { !history.isEmpty }
    public var isLastStep: Bool { currentStep == nil || nextStepIndex == nil }
    public var canGoNext: Bool { currentStep != nil && nextStepIndex != nil && currentStepIsValid }
    /// True when Install may be pressed: on the last step, and every group rule holds.
    public var canFinish: Bool { isLastStep && currentStepIsValid }

    private var nextStepIndex: Int? {
        guard let current = currentStep else { return nil }
        return nextVisible(after: current, flags: flags(through: current))
    }

    public func next() {
        guard canGoNext, let current = currentStep, let n = nextStepIndex else { return }
        advance(from: current, to: n)
    }

    /// Replay: move on whatever the validity.
    func advanceIgnoringRules() -> Bool {
        guard let current = currentStep, let n = nextStepIndex else { return false }
        advance(from: current, to: n)
        return true
    }

    private func advance(from current: Int, to n: Int) {
        history.append(current)
        forward = true
        currentStep = n
        enter(n)
        cursor = firstCursor()
    }

    public func back() {
        guard let previous = history.popLast() else { return }
        forward = false
        currentStep = previous
        cursor = firstCursor()
    }

    private func nextVisible(after index: Int, flags: [String: String]) -> Int? {
        var i = index + 1
        while i < installer.steps.count {
            if installer.steps[i].visible?.evaluate(flags: flags, environment: environment) ?? true { return i }
            i += 1
        }
        return nil
    }

    // MARK: Flags and types

    /// Flags set by the selected plugins of the steps already passed (the history), plus `through` itself when given.
    private func flags(through step: Int?) -> [String: String] {
        var out: [String: String] = [:]
        for s in history + (step.map { [$0] } ?? []) {
            for (g, group) in installer.steps[s].groups.enumerated() {
                for (p, plugin) in group.plugins.enumerated() where selected.contains(FomodPluginRef(step: s, group: g, plugin: p)) {
                    for flag in plugin.flags where !flag.name.isEmpty { out[flag.name.lowercased()] = flag.value }
                }
            }
        }
        return out
    }

    public func plugin(_ ref: FomodPluginRef) -> FomodPlugin { installer.steps[ref.step].groups[ref.group].plugins[ref.plugin] }

    /// The plugin's type now: a fixed one, or the first dependency pattern that holds on the flags of the earlier steps.
    public func effectiveType(_ ref: FomodPluginRef) -> FomodPluginType {
        switch plugin(ref).typeDescriptor {
        case .fixed(let type): return type
        case .dependent(let defaultType, let patterns):
            let known = flags(through: nil)
            for pattern in patterns where pattern.condition.evaluate(flags: known, environment: environment) { return pattern.type }
            return defaultType
        }
    }

    // MARK: Selection

    public func isSelected(_ ref: FomodPluginRef) -> Bool { selected.contains(ref) }

    /// Required plugins stay on, unusable ones stay off, and a SelectAll group has no choice.
    public func isLocked(_ ref: FomodPluginRef) -> Bool {
        let group = installer.steps[ref.step].groups[ref.group]
        if group.type == .selectAll { return true }
        switch effectiveType(ref) {
        case .required, .notUsable: return true
        default: break
        }
        // an installer that makes one option required in a pick-one group leaves nothing to pick
        if group.type.isSingleChoice {
            return group.plugins.indices.contains { effectiveType(FomodPluginRef(step: ref.step, group: ref.group, plugin: $0)) == .required }
        }
        return false
    }

    public func toggle(_ ref: FomodPluginRef) {
        guard currentStep == ref.step, !isLocked(ref) else { return }
        let group = installer.steps[ref.step].groups[ref.group]
        touched.insert(ref.groupRef)
        var next = selected
        let isOn = next.contains(ref)
        switch group.type {
        case .selectExactlyOne, .selectAtMostOne:
            for p in group.plugins.indices { next.remove(FomodPluginRef(step: ref.step, group: ref.group, plugin: p)) }
            if !isOn || group.type == .selectExactlyOne { next.insert(ref) }
        default:
            if isOn { next.remove(ref) } else { next.insert(ref) }
        }
        selected = next
    }

    /// Why the group cannot be left as it is, or nil.
    public func problem(in group: FomodGroupRef) -> String? {
        let model = installer.steps[group.step].groups[group.group]
        let on = model.plugins.indices.filter { selected.contains(FomodPluginRef(step: group.step, group: group.group, plugin: $0)) }.count
        switch model.type {
        case .selectExactlyOne: return on == 1 ? nil : "Choose one option."
        case .selectAtLeastOne: return on >= 1 ? nil : "Choose at least one option."
        case .selectAtMostOne: return on <= 1 ? nil : "Choose at most one option."
        case .selectAll, .selectAny: return nil
        }
    }

    public var currentStepIsValid: Bool {
        guard let step = currentStep else { return true }
        return installer.steps[step].groups.indices.allSatisfy { problem(in: FomodGroupRef(step: step, group: $0)) == nil }
    }

    /// Sets the starting selection of a step being entered: the saved choices when replaying, otherwise what the installer
    /// recommends. Groups the user has already changed keep their selection and only get the locks re-applied.
    private func enter(_ step: Int) {
        var next = selected
        for (g, group) in installer.steps[step].groups.enumerated() {
            let groupRef = FomodGroupRef(step: step, group: g)
            let refs = group.plugins.indices.map { FomodPluginRef(step: step, group: g, plugin: $0) }
            if touched.contains(groupRef) {
                for ref in refs {
                    switch effectiveType(ref) {
                    case .required: next.insert(ref)
                    case .notUsable: next.remove(ref)
                    default: break
                    }
                }
                continue
            }
            for ref in refs { next.remove(ref) }
            if let names = saved[Self.savedKey(installer.steps[step].name, group.name)] {
                for ref in refs where names.contains(plugin(ref).name) { next.insert(ref) }
                for ref in refs {
                    switch effectiveType(ref) {
                    case .required: next.insert(ref)
                    case .notUsable: next.remove(ref)
                    default: break
                    }
                }
                touched.insert(groupRef)
                continue
            }
            next.formUnion(defaults(for: group, refs: refs))
        }
        selected = next
    }

    private func defaults(for group: FomodGroup, refs: [FomodPluginRef]) -> Set<FomodPluginRef> {
        if group.type == .selectAll { return Set(refs) }
        let types = refs.map { effectiveType($0) }
        var out = Set<FomodPluginRef>()
        let required = refs.indices.filter { types[$0] == .required }
        let recommended = refs.indices.filter { types[$0] == .recommended }
        let usable = refs.indices.filter { types[$0] != .notUsable }
        switch group.type {
        case .selectExactlyOne:
            if let i = required.first ?? recommended.first ?? usable.first { out.insert(refs[i]) }
        case .selectAtMostOne:
            if let i = required.first ?? recommended.first { out.insert(refs[i]) }
        case .selectAtLeastOne:
            for i in required + recommended { out.insert(refs[i]) }
            if out.isEmpty, let i = usable.first { out.insert(refs[i]) }
        default:
            for i in required + recommended { out.insert(refs[i]) }
        }
        return out
    }

    // MARK: Cursor and commands

    /// The options of the current step in reading order.
    public var optionRefs: [FomodPluginRef] {
        guard let step = currentStep else { return [] }
        var out: [FomodPluginRef] = []
        for (g, group) in installer.steps[step].groups.enumerated() {
            for p in group.plugins.indices { out.append(FomodPluginRef(step: step, group: g, plugin: p)) }
        }
        return out
    }

    /// True when the primary button (Next, or Install on the last step) can be pressed.
    public var primaryEnabled: Bool { isLastStep ? canFinish : canGoNext }

    private func firstCursor() -> FomodCursor? { optionRefs.first.map { .option($0) } ?? .primary }

    /// Moves the cursor or acts on it. Anything the current page cannot do is ignored.
    public func send(_ command: FomodCommand) {
        showsCursor = true
        let options = optionRefs
        switch command {
        case .up:
            switch cursor {
            case .option(let ref)?:
                if let i = options.firstIndex(of: ref), i > 0 { cursor = .option(options[i - 1]) }
            case .cancel?, .back?, .primary?:
                if let last = options.last { cursor = .option(last) }
            case nil:
                cursor = firstCursor()
            }
        case .down:
            switch cursor {
            case .option(let ref)?:
                if let i = options.firstIndex(of: ref), i + 1 < options.count { cursor = .option(options[i + 1]) } else { cursor = .primary }
            case nil:
                cursor = firstCursor()
            default:
                break
            }
        case .left, .right:
            let row: [FomodCursor] = (hasCancelButton ? [.cancel] : []) + (canGoBack ? [.back] : []) + [.primary]
            guard let current = cursor, let i = row.firstIndex(of: current) else { return }
            let target = command == .left ? i - 1 : i + 1
            if row.indices.contains(target) { cursor = row[target] }
        case .confirm:
            switch cursor {
            case .option(let ref)?: toggle(ref)
            case .cancel?: onCancel?()
            case .back?: back()
            case .primary?: pressPrimary()
            case nil: cursor = firstCursor()
            }
        case .back:
            if canGoBack { back() } else { onCancel?() }
        case .next:
            pressPrimary()
        }
    }

    private func pressPrimary() {
        guard primaryEnabled else { return }
        if isLastStep { onFinish?(choices()) } else { next() }
    }

    // MARK: Results

    /// What was selected, for the steps passed and the one on screen.
    public func choices() -> FomodChoices {
        var out: [FomodChoices.Selection] = []
        for s in history + (currentStep.map { [$0] } ?? []) {
            let step = installer.steps[s]
            for (g, group) in step.groups.enumerated() {
                let names = group.plugins.indices
                    .filter { selected.contains(FomodPluginRef(step: s, group: g, plugin: $0)) }
                    .map { group.plugins[$0].name }
                out.append(FomodChoices.Selection(step: step.name, group: group.name, plugins: names))
            }
        }
        return FomodChoices(selections: out)
    }

    /// The files this walk installs, in install order (priority, then document order); later entries win a path.
    func resolvedFiles() -> [FomodFileEntry] {
        var all: [(entry: FomodFileEntry, order: Int)] = []
        var order = 0
        func add(_ entries: [FomodFileEntry]) {
            for entry in entries {
                all.append((entry, order))
                order += 1
            }
        }
        add(installer.requiredFiles)
        let visited = history + (currentStep.map { [$0] } ?? [])
        for s in visited {
            for (g, group) in installer.steps[s].groups.enumerated() {
                for (p, plugin) in group.plugins.enumerated() {
                    let ref = FomodPluginRef(step: s, group: g, plugin: p)
                    let on = selected.contains(ref)
                    let usable = effectiveType(ref) != .notUsable
                    add(plugin.files.filter { on || $0.alwaysInstall || ($0.installIfUsable && usable) })
                }
            }
        }
        let finalFlags = flags(through: currentStep)
        for conditional in installer.conditionalInstalls where conditional.condition.evaluate(flags: finalFlags, environment: environment) {
            add(conditional.files)
        }
        return all.sorted { $0.entry.priority != $1.entry.priority ? $0.entry.priority < $1.entry.priority : $0.order < $1.order }
            .map(\.entry)
    }
}

extension FomodInstaller {
    /// The files `choices` (or the defaults when nil) install. Choices that no longer fit the installer fall back to defaults.
    func resolvedFiles(choices: FomodChoices?, environment: FomodEnvironment) -> [FomodFileEntry] {
        let session = FomodSession(installer: self, environment: environment, choices: choices)
        while session.advanceIgnoringRules() {}
        return session.resolvedFiles()
    }
}
