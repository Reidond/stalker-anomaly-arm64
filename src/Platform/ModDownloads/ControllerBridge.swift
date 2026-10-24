import GameController
import SwiftUI

/// How a gamepad drives the page. The native controls (toolbar, bookmarks, list) are plain focusable buttons and follow the
/// system focus; a web page has no focus ring, so the pad scrolls it and points at links with a cursor.
///
///   left stick / D-pad   scroll the page
///   right stick          move the cursor (shown while it is in use)
///   A                    tap what is under the cursor (a click; counts as a user tap)
///   L1 / R1, B           back, forward, back
///   R3                   put the cursor back in the middle
enum ControllerMapping {
    static let deadzone: Float = 0.18
    /// Points per second at full deflection.
    static let scrollSpeed: Double = 1100
    /// Fractions of the page per second at full deflection.
    static let cursorSpeed: Double = 0.75

    /// Radial deadzone, rescaled so the first usable deflection starts at 0, then a squared response for fine control.
    static func shape(x: Float, y: Float) -> (x: Double, y: Double) {
        let magnitude = (x * x + y * y).squareRoot()
        guard magnitude > deadzone else { return (0, 0) }
        let scaled = min(1, (magnitude - deadzone) / (1 - deadzone))
        let curved = scaled * scaled
        return (Double(x / magnitude * curved), Double(y / magnitude * curved))
    }

    /// Stick (y up is positive) to a scroll delta in points; pushing up scrolls up (negative y).
    static func scrollDelta(x: Float, y: Float, dt: Double) -> (dx: Double, dy: Double) {
        let s = shape(x: x, y: y)
        return (s.x * scrollSpeed * dt, -s.y * scrollSpeed * dt)
    }

    /// New cursor position (fractions of the page, kept inside it).
    static func movedCursor(_ cursor: (x: Double, y: Double), stickX: Float, stickY: Float, dt: Double) -> (x: Double, y: Double) {
        let s = shape(x: stickX, y: stickY)
        return (min(1, max(0, cursor.x + s.x * cursorSpeed * dt)), min(1, max(0, cursor.y - s.y * cursorSpeed * dt)))
    }
}

/// Polls the connected gamepad (it sets no handlers, so it cannot take input away from anything else) while the browser is on screen.
@MainActor
final class ControllerBridge: ObservableObject {
    @Published private(set) var cursor = CGPoint(x: 0.5, y: 0.5)
    @Published private(set) var cursorVisible = false

    private weak var browser: BrowserModel?
    private var timer: Timer?
    private var observers: [NSObjectProtocol] = []
    private var lastTick = ProcessInfo.processInfo.systemUptime
    private var lastCursorUse: TimeInterval = 0
    private var previous = Buttons()

    private struct Buttons {
        var a = false, b = false, l1 = false, r1 = false, r3 = false
    }

    func start(browser: BrowserModel) {
        self.browser = browser
        let center = NotificationCenter.default
        observers = [
            center.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.updateTimer() }
            },
            center.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.updateTimer() }
            },
        ]
        updateTimer()
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        observers.forEach(NotificationCenter.default.removeObserver)
        observers = []
        cursorVisible = false
    }

    private func updateTimer() {
        let has = GCController.controllers().contains { $0.extendedGamepad != nil }
        if has, timer == nil {
            lastTick = ProcessInfo.processInfo.systemUptime
            let t = Timer(timeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated { self?.tick() }
            }
            RunLoop.main.add(t, forMode: .common)
            timer = t
        } else if !has {
            timer?.invalidate()
            timer = nil
            cursorVisible = false
        }
    }

    private func tick() {
        guard let browser, let pad = GCController.controllers().compactMap(\.extendedGamepad).first else { return }
        let now = ProcessInfo.processInfo.systemUptime
        let dt = min(0.1, now - lastTick)
        lastTick = now

        var sx = pad.leftThumbstick.xAxis.value
        var sy = pad.leftThumbstick.yAxis.value
        if sx == 0 && sy == 0 {
            sx = pad.dpad.xAxis.value
            sy = pad.dpad.yAxis.value
        }
        let scroll = ControllerMapping.scrollDelta(x: sx, y: sy, dt: dt)
        if scroll.dx != 0 || scroll.dy != 0 { browser.scroll(byX: scroll.dx, y: scroll.dy) }

        let rx = pad.rightThumbstick.xAxis.value, ry = pad.rightThumbstick.yAxis.value
        if ControllerMapping.shape(x: rx, y: ry) != (0, 0) {
            let moved = ControllerMapping.movedCursor((Double(cursor.x), Double(cursor.y)), stickX: rx, stickY: ry, dt: dt)
            cursor = CGPoint(x: moved.x, y: moved.y)
            lastCursorUse = now
            cursorVisible = true
        } else if cursorVisible, now - lastCursorUse > 4 {
            cursorVisible = false
        }

        let buttons = Buttons(a: pad.buttonA.isPressed, b: pad.buttonB.isPressed, l1: pad.leftShoulder.isPressed,
                              r1: pad.rightShoulder.isPressed, r3: pad.rightThumbstickButton?.isPressed ?? false)
        if buttons.a && !previous.a {
            cursorVisible = true
            lastCursorUse = now
            browser.tap(atFractionX: Double(cursor.x), y: Double(cursor.y))
        }
        if (buttons.b && !previous.b) || (buttons.l1 && !previous.l1), browser.canGoBack { browser.goBack() }
        if buttons.r1 && !previous.r1, browser.canGoForward { browser.goForward() }
        if buttons.r3 && !previous.r3 { cursor = CGPoint(x: 0.5, y: 0.5) }
        previous = buttons
    }
}

/// Sits over the web view: owns the bridge and draws the cursor.
struct ControllerLayer: View {
    let browser: BrowserModel
    @StateObject private var bridge = ControllerBridge()
    @Environment(\.downloadsTheme) private var theme

    var body: some View {
        GeometryReader { geo in
            if bridge.cursorVisible {
                // accent ring with a dark core: visible on a white page and on a black one
                Circle()
                    .strokeBorder(theme.accent, lineWidth: 3)
                    .background(Circle().fill(Color.black.opacity(0.45)))
                    .frame(width: 28, height: 28)
                    .shadow(color: .black.opacity(0.5), radius: 3)
                    .position(x: bridge.cursor.x * geo.size.width, y: bridge.cursor.y * geo.size.height)
                    .transition(.opacity)
            }
        }
        .allowsHitTesting(false)
        .animation(.easeOut(duration: 0.15), value: bridge.cursorVisible)
        .onAppear { bridge.start(browser: browser) }
        .onDisappear { bridge.stop() }
    }
}
