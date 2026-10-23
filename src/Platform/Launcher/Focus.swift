// Console-style focus: one focused item at a time, moved by the D-pad, the stick or the arrow keys, activated with Cross / Return.
// Every focusable view reports its frame; the store picks the nearest item in the direction pressed. Touch and mouse go
// straight to the views and only set the focus on the way.
import SwiftUI

enum Direction { case up, down, left, right }

/// What the player pressed, whatever the device. The pad, the keyboard and the touch UI all end up here.
enum PadButton: Int32 {
    case up = 0, down, left, right
    case confirm       // Cross, Return
    case back          // Circle, Escape
    case square        // reorder (hold) / secondary
    case triangle      // details
    case options       // Options / Start: Play
    case leftShoulder, rightShoulder
    case create        // Create / Share: unused
}

enum InputDevice { case touch, pad, keyboard }

/// What a focusable view does when it is activated (Cross / Return) or its secondary action is asked for (Triangle). A new box
/// is made every time the view's body runs, so the store never calls a closure that captured old state.
final class FocusActions: Equatable {
    let enabled: Bool
    let activate: () -> Void
    let secondary: (() -> Void)?

    init(enabled: Bool, activate: @escaping () -> Void, secondary: (() -> Void)?) {
        self.enabled = enabled
        self.activate = activate
        self.secondary = secondary
    }

    static func == (lhs: FocusActions, rhs: FocusActions) -> Bool { lhs === rhs }
}

/// What a focusable view reports up the tree: its frame in the launcher's coordinate space, the scope it belongs to and its actions.
struct FocusFrame: Equatable {
    var rect: CGRect
    var scope: String
    var isDefault: Bool
    var actions: FocusActions
}

struct FocusFramesKey: PreferenceKey {
    static var defaultValue: [String: FocusFrame] = [:]
    static func reduce(value: inout [String: FocusFrame], nextValue: () -> [String: FocusFrame]) {
        value.merge(nextValue()) { $1 }
    }
}

@MainActor
final class FocusStore: ObservableObject {
    @Published private(set) var focusedID: String?
    @Published var device: InputDevice = .touch
    /// Buttons that are down right now (Square held = reorder mode).
    @Published private(set) var held: Set<PadButton> = []

    var activeScope = "main" {
        didSet {
            guard activeScope != oldValue else { return }
            if let current = focusedID { scopeMemory[oldValue] = current }
            if let remembered = scopeMemory[activeScope], frames[remembered]?.scope == activeScope {
                focusedID = remembered
            } else {
                refocusIfNeeded(force: true)
            }
        }
    }
    /// The snapshot tool renders once, without a run loop to wait on: it applies frame updates at once.
    var synchronousUpdates = false
    /// Asked first for every directional press; true when it used the press (reordering).
    var directionInterceptor: ((Direction, _ reorder: Bool) -> Bool)?
    var backHandler: (() -> Void)?
    var optionsHandler: (() -> Void)?
    /// A focus wish that waits for the item to exist (coming back from a sub-screen).
    var pendingFocus: String?

    private(set) var frames: [String: FocusFrame] = [:]
    private var scopeMemory: [String: String] = [:]
    private var repeatTask: Task<Void, Never>?
    private var repeating: (Direction, Int)?

    // MARK: frames

    /// Called from a preference change, i.e. while views are being updated: the focus (published state) changes right after.
    func update(frames new: [String: FocusFrame]) {
        frames = new
        if synchronousUpdates {
            refocusIfNeeded(force: false)
        } else {
            Task { @MainActor [weak self] in self?.refocusIfNeeded(force: false) }
        }
    }

    private func refocusIfNeeded(force: Bool) {
        let candidates = frames.filter { $0.value.scope == activeScope }
        if let pending = pendingFocus, candidates[pending] != nil {
            focusedID = pending
            pendingFocus = nil
            return
        }
        if !force, let current = focusedID, candidates[current] != nil { return }
        if candidates.isEmpty {
            if force { focusedID = nil }
            return
        }
        let preferred = candidates.filter { $0.value.isDefault }
        let pool = preferred.isEmpty ? candidates : preferred
        focusedID = pool.min { a, b in
            let (ra, rb) = (a.value.rect, b.value.rect)
            return abs(ra.minY - rb.minY) > 8 ? ra.minY < rb.minY : ra.minX < rb.minX
        }?.key
    }

    /// For the snapshot tool: the focus a pad would have after the screen came up.
    func focusDefault(scope: String) {
        activeScope = scope
        refocusIfNeeded(force: true)
    }

    func focus(_ id: String) {
        guard focusedID != id else { return }
        focusedID = id
    }

    // MARK: input

    func press(_ button: PadButton, from source: InputDevice = .pad) {
        device = source
        held.insert(button)
        switch button {
        case .up: move(.up)
        case .down: move(.down)
        case .left: move(.left)
        case .right: move(.right)
        case .confirm:
            if let actions = focusedID.flatMap({ frames[$0]?.actions }), actions.enabled { actions.activate() }
        case .triangle:
            if let secondary = focusedID.flatMap({ frames[$0]?.actions.secondary }) { secondary() }
        case .back: backHandler?()
        case .options: optionsHandler?()
        default: break
        }
    }

    func release(_ button: PadButton) {
        held.remove(button)
        if let (direction, _) = repeating, button == Self.button(for: direction) { stopRepeat() }
    }

    func releaseAll() {
        held.removeAll()
        stopRepeat()
    }

    /// Direction with auto-repeat while it stays pressed: the first repeat after 0.35 s, then every 0.09 s.
    func direction(_ direction: Direction, pressed: Bool, from source: InputDevice = .pad) {
        if pressed {
            guard repeating?.0 != direction else { return }
            stopRepeat()
            device = source
            move(direction)
            repeating = (direction, 0)
            repeatTask = Task { [weak self] in
                try? await Task.sleep(nanoseconds: 350_000_000)
                while !Task.isCancelled {
                    guard let self else { return }
                    self.move(direction)
                    try? await Task.sleep(nanoseconds: 90_000_000)
                }
            }
        } else if repeating?.0 == direction {
            stopRepeat()
        }
    }

    private func stopRepeat() {
        repeatTask?.cancel()
        repeatTask = nil
        repeating = nil
    }

    private static func button(for direction: Direction) -> PadButton {
        switch direction {
        case .up: .up
        case .down: .down
        case .left: .left
        case .right: .right
        }
    }

    func move(_ direction: Direction, reorder: Bool? = nil) {
        if directionInterceptor?(direction, reorder ?? held.contains(.square)) == true { return }
        guard let current = focusedID, let from = frames[current]?.rect else {
            refocusIfNeeded(force: true)
            return
        }
        let origin = CGPoint(x: from.midX, y: from.midY)
        var best: (id: String, score: CGFloat)?
        for (id, frame) in frames where id != current && frame.scope == activeScope {
            let center = CGPoint(x: frame.rect.midX, y: frame.rect.midY)
            let dx = center.x - origin.x, dy = center.y - origin.y
            let along: CGFloat, across: CGFloat
            switch direction {
            case .up: (along, across) = (-dy, abs(dx))
            case .down: (along, across) = (dy, abs(dx))
            case .left: (along, across) = (-dx, abs(dy))
            case .right: (along, across) = (dx, abs(dy))
            }
            // in the direction, and not behind the edge of the current item
            guard along > 4 else { continue }
            let overlap: Bool
            switch direction {
            case .up, .down: overlap = frame.rect.maxX > from.minX && frame.rect.minX < from.maxX
            case .left, .right: overlap = frame.rect.maxY > from.minY && frame.rect.minY < from.maxY
            }
            let score = along + across * (overlap ? 1.0 : 3.0) + (overlap ? 0 : 400)
            if best == nil || score < best!.score { best = (id, score) }
        }
        if let best { focusedID = best.id }
    }

    func isFocused(_ id: String) -> Bool { focusedID == id }
}

// MARK: view side

private struct FocusScopeKey: EnvironmentKey { static let defaultValue = "main" }

extension EnvironmentValues {
    var focusScope: String {
        get { self[FocusScopeKey.self] }
        set { self[FocusScopeKey.self] = newValue }
    }
}

/// Makes a view a stop of the focus: it reports its frame, draws nothing itself (the content reads `isFocused` from the
/// closure) and runs `onActivate` for Cross / Return, and `onSecondary` for Triangle.
struct LauncherFocusable<Content: View>: View {
    let id: String
    var isDefault = false
    var enabled = true
    var onActivate: () -> Void
    var onSecondary: (() -> Void)?
    @ViewBuilder var content: (_ focused: Bool) -> Content

    @EnvironmentObject private var store: FocusStore
    @Environment(\.focusScope) private var scope

    var body: some View {
        let focused = store.isFocused(id)
        content(focused)
            .background(
                GeometryReader { proxy in
                    Color.clear.preference(
                        key: FocusFramesKey.self,
                        value: [id: FocusFrame(rect: proxy.frame(in: .named("launcher")), scope: scope, isDefault: isDefault,
                                               actions: FocusActions(enabled: enabled, activate: onActivate, secondary: onSecondary))])
                }
            )
            .contentShape(Rectangle())
            #if os(macOS)
            .onHover { inside in if inside && enabled { store.focus(id) } }
            #endif
            .onTapGesture {
                #if os(iOS)
                store.device = .touch
                #endif
                store.focus(id)
                if enabled { onActivate() }
            }
            .id(id)
    }
}
