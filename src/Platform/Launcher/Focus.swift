// Pad and keyboard navigation on top of the stock SwiftUI controls. The model says which rows can be reached on the current screen
// (FocusItem, in reading order); D-pad up/down moves along them, Cross runs the row's action, Triangle its secondary one. The
// views only draw the focus (a ring on the row) and scroll to it; touch and the mouse go to the controls as usual.
import SwiftUI

enum Direction { case up, down, left, right }

/// What the player pressed, whatever the device. The pad, the keyboard and the touch UI all end up here.
enum PadButton: Int32 {
    case up = 0, down, left, right
    case confirm       // Cross, Return
    case back          // Circle, Escape
    case square        // reorder (hold)
    case triangle      // details
    case options       // Options / Start: Play
    case leftShoulder, rightShoulder
    case create        // Create / Share: unused
}

/// Which device was used last: the focus ring and the hint bar only show for the pad and the keyboard.
enum InputDevice { case pointer, pad, keyboard }

struct FocusItem {
    let id: String
    var enabled = true
    var isDefault = false
    var activate: () -> Void
    var secondary: (() -> Void)?
}

@MainActor
final class FocusStore: ObservableObject {
    @Published private(set) var focusedID: String?
    @Published var device: InputDevice = .pointer
    /// Buttons that are down right now (Square held = reorder mode).
    @Published private(set) var held: Set<PadButton> = []

    private(set) var items: [FocusItem] = []
    private var context = ""
    private var memory: [String: String] = [:]
    private var repeatTask: Task<Void, Never>?
    private var repeating: Direction?

    /// Asked first for every up/down press; true when it used the press (reordering).
    var directionInterceptor: ((Direction, _ reorder: Bool) -> Bool)?
    var backHandler: (() -> Void)?
    var optionsHandler: (() -> Void)?

    // MARK: items

    /// The reachable rows of the screen now showing. `context` names the screen: coming back to one, the focus returns to the row it
    /// had when it was left. `prefer` wins when it is among the items (a row that moved keeps the focus).
    func setItems(_ new: [FocusItem], context newContext: String, prefer: String? = nil) {
        if newContext != context {
            if let current = focusedID { memory[context] = current }
            context = newContext
            focusedID = nil
        }
        items = new
        if let prefer, new.contains(where: { $0.id == prefer }) {
            focusedID = prefer
        } else if let current = focusedID, new.contains(where: { $0.id == current }) {
            return
        } else if let remembered = memory[newContext], new.contains(where: { $0.id == remembered }) {
            focusedID = remembered
        } else {
            focusedID = (new.first { $0.isDefault && $0.enabled } ?? new.first { $0.enabled })?.id
        }
    }

    func focus(_ id: String) {
        if items.contains(where: { $0.id == id }) { focusedID = id }
    }

    func isFocused(_ id: String) -> Bool { device != .pointer && focusedID == id }

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
            if let item = items.first(where: { $0.id == focusedID }), item.enabled { item.activate() }
        case .triangle:
            items.first(where: { $0.id == focusedID })?.secondary?()
        case .back: backHandler?()
        case .options: optionsHandler?()
        default: break
        }
    }

    func release(_ button: PadButton) {
        held.remove(button)
    }

    func releaseAll() {
        held.removeAll()
        stopRepeat()
    }

    /// A direction with auto-repeat while it stays pressed: the first repeat after 0.35 s, then every 0.09 s.
    func direction(_ direction: Direction, pressed: Bool, from source: InputDevice = .pad) {
        if pressed {
            guard repeating != direction else { return }
            stopRepeat()
            device = source
            move(direction)
            repeating = direction
            repeatTask = Task { [weak self] in
                try? await Task.sleep(nanoseconds: 350_000_000)
                while !Task.isCancelled {
                    guard let self else { return }
                    self.move(direction)
                    try? await Task.sleep(nanoseconds: 90_000_000)
                }
            }
        } else if repeating == direction {
            stopRepeat()
        }
    }

    private func stopRepeat() {
        repeatTask?.cancel()
        repeatTask = nil
        repeating = nil
    }

    /// Up and down walk the rows in order; left and right have nothing to do on a list.
    func move(_ direction: Direction, reorder: Bool? = nil) {
        if directionInterceptor?(direction, reorder ?? held.contains(.square)) == true { return }
        guard direction == .up || direction == .down else { return }
        let reachable = items.filter(\.enabled)
        guard !reachable.isEmpty else { return }
        guard let current = reachable.firstIndex(where: { $0.id == focusedID }) else {
            focusedID = reachable.first?.id
            return
        }
        let next = direction == .down ? min(current + 1, reachable.count - 1) : max(current - 1, 0)
        focusedID = reachable[next].id
    }
}

// MARK: view side

/// The ring that shows where the pad's focus is: the accent color around the row, and a tint behind it in a list.
struct PadFocusModifier: ViewModifier {
    @EnvironmentObject private var focus: FocusStore
    let id: String

    func body(content: Content) -> some View {
        let focused = focus.isFocused(id)
        content
            .overlay {
                if focused {
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .strokeBorder(Color.accentColor, lineWidth: 3)
                        .padding(1)
                        .allowsHitTesting(false)
                }
            }
            .listRowBackground(focused ? Color.accentColor.opacity(0.16) : nil)
            .id(id)
    }
}

extension View {
    func padFocus(_ id: String) -> some View { modifier(PadFocusModifier(id: id)) }
}
