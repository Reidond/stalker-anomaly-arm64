// The C entry points the app shell calls (AnomalyLauncher.h), and the hosting of the SwiftUI launcher inside the SDL window the
// shell created: a child view controller on iPadOS, a subview of the window's content view on macOS. The shell owns the window and
// the event loop; the launcher draws itself, is fed the pad (and, on iPadOS, the keyboard) through the functions below, and is
// released before the engine creates its own window.
import Foundation
import SwiftUI
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif

typealias DataStatusCallback = @convention(c) (UnsafeMutableRawPointer?) -> UnsafeMutablePointer<CChar>?

@MainActor
final class LauncherHost {
    let model: LauncherModel
    private var stickState: [Int32: Direction] = [:]
    private var autoPlay: Task<Void, Never>?
    #if canImport(UIKit)
    private var controller: UIHostingController<LauncherRootView>?
    #elseif canImport(AppKit)
    private var hostingView: NSHostingView<LauncherRootView>?
    private var keyMonitor: Any?
    private weak var window: NSWindow?
    #endif

    init?(nativeWindow: UnsafeMutableRawPointer?, config: LauncherConfig, dataStatus: DataStatusCallback?, user: UnsafeMutableRawPointer?) {
        model = LauncherModel(config: config)
        if let dataStatus {
            // the shell's validation, as JSON; the callback's string is malloc'd for us to free
            model.dataProvider = {
                guard let raw = dataStatus(user) else { return DataStatus() }
                defer { free(raw) }
                let decoder = JSONDecoder()
                decoder.keyDecodingStrategy = .convertFromSnakeCase
                return (try? decoder.decode(DataStatus.self, from: Data(bytes: raw, count: strlen(raw)))) ?? DataStatus()
            }
        }
        model.start()
        guard let nativeWindow, attach(to: nativeWindow) else {
            model.stop()
            return nil
        }
        if let delay = config.autoPlayMs {
            autoPlay = Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(max(delay, 0)) * 1_000_000)
                self?.model.play()
            }
        }
    }

    // MARK: hosting

    private func attach(to pointer: UnsafeMutableRawPointer) -> Bool {
        let root = LauncherRootView(model: model)
        #if canImport(UIKit)
        let window = Unmanaged<UIWindow>.fromOpaque(pointer).takeUnretainedValue()
        guard let parent = window.rootViewController else { return false }
        let host = UIHostingController(rootView: root)
        host.view.backgroundColor = .systemBackground
        host.view.frame = parent.view.bounds
        host.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        parent.addChild(host)
        parent.view.addSubview(host.view)
        host.didMove(toParent: parent)
        controller = host
        return true
        #elseif canImport(AppKit)
        let window = Unmanaged<NSWindow>.fromOpaque(pointer).takeUnretainedValue()
        guard let content = window.contentView else { return false }
        window.backgroundColor = .windowBackgroundColor
        let view = NSHostingView(rootView: root)
        view.frame = content.bounds
        view.autoresizingMask = [.width, .height]
        content.addSubview(view)
        hostingView = view
        self.window = window
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp]) { [weak self] event in
            nonisolated(unsafe) let event = event
            guard let self else { return event }
            return MainActor.assumeIsolated { self.handle(key: event) ? nil : event }
        }
        return true
        #endif
    }

    func close() {
        autoPlay?.cancel()
        model.stop()
        #if canImport(UIKit)
        if let controller {
            controller.willMove(toParent: nil)
            controller.view.removeFromSuperview()
            controller.removeFromParent()
        }
        controller = nil
        #elseif canImport(AppKit)
        if let keyMonitor { NSEvent.removeMonitor(keyMonitor) }
        keyMonitor = nil
        hostingView?.removeFromSuperview()
        hostingView = nil
        #endif
    }

    // MARK: input

    func button(_ code: Int32, down: Bool, keyboard: Bool) {
        guard let button = PadButton(rawValue: code) else { return }
        let source: InputDevice = keyboard ? .keyboard : .pad
        switch button {
        case .up, .down, .left, .right:
            let direction: Direction = button == .up ? .up : button == .down ? .down : button == .left ? .left : .right
            model.focus.direction(direction, pressed: down, from: source)
        default:
            if down { model.focus.press(button, from: source) } else { model.focus.release(button) }
        }
    }

    /// Axis 0/1 = left stick X/Y (-1...1, down is positive): a direction once it is pushed past 0.55, released below 0.35.
    func axis(_ axis: Int32, value: Float) {
        let positive: Direction = axis == 0 ? .right : .down
        let negative: Direction = axis == 0 ? .left : .up
        let current = stickState[axis]
        var next: Direction?
        if value > (current == positive ? 0.35 : 0.55) { next = positive }
        else if value < (current == negative ? -0.35 : -0.55) { next = negative }
        guard next != current else { return }
        if let current { model.focus.direction(current, pressed: false) }
        if let next { model.focus.direction(next, pressed: true) }
        stickState[axis] = next
    }

    #if canImport(AppKit)
    /// The Mac keyboard: arrows move, Return / Space select, Esc goes back, I shows the details, P plays, Option+arrows reorder,
    /// Delete removes. Keys pass through to a text field while one is being edited.
    private func handle(key event: NSEvent) -> Bool {
        guard event.window === window, window?.isKeyWindow == true else { return false }
        if window?.firstResponder is NSText {
            if event.type == .keyDown && event.keyCode == 53 { window?.makeFirstResponder(nil); return true }
            return false
        }
        let down = event.type == .keyDown
        let reorder = event.modifierFlags.contains(.option)
        let focus = model.focus
        switch event.keyCode {
        case 126, 125, 123, 124:
            let direction: Direction = event.keyCode == 126 ? .up : event.keyCode == 125 ? .down : event.keyCode == 123 ? .left : .right
            if down {
                focus.device = .keyboard
                focus.move(direction, reorder: reorder)
            }
        case 36, 76, 49:
            if down && !event.isARepeat { focus.press(.confirm, from: .keyboard); focus.release(.confirm) }
        case 53:
            if down && !event.isARepeat { focus.press(.back, from: .keyboard); focus.release(.back) }
        case 34:
            if down && !event.isARepeat { focus.press(.triangle, from: .keyboard); focus.release(.triangle) }
        case 35:
            if down && !event.isARepeat { focus.press(.options, from: .keyboard); focus.release(.options) }
        case 51, 117:
            if down, !model.modalActive, model.screen == .mods, let id = focus.focusedID, id.hasPrefix("mod:") {
                model.removalCandidate = String(id.dropFirst(4))
            }
        default:
            return false
        }
        return true
    }
    #endif

    // MARK: snapshot

    func snapshot(to path: String) -> Bool {
        #if canImport(UIKit)
        guard let view = controller?.view else { return false }
        let renderer = UIGraphicsImageRenderer(bounds: view.bounds)
        let image = renderer.image { _ in view.drawHierarchy(in: view.bounds, afterScreenUpdates: true) }
        guard let data = image.pngData() else { return false }
        return (try? data.write(to: URL(fileURLWithPath: path))) != nil
        #elseif canImport(AppKit)
        guard let view = hostingView, let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return false }
        view.cacheDisplay(in: view.bounds, to: rep)
        guard let data = rep.representation(using: .png, properties: [:]) else { return false }
        return (try? data.write(to: URL(fileURLWithPath: path))) != nil
        #endif
    }
}

// MARK: C entry points

private func host(_ pointer: UnsafeMutableRawPointer?) -> LauncherHost? {
    pointer.map { Unmanaged<LauncherHost>.fromOpaque($0).takeUnretainedValue() }
}

/// Opens the launcher inside `nativeWindow` (the UIWindow / NSWindow of an SDL window). `configJSON` is a LauncherConfig;
/// `dataStatus` is called (on the main thread) whenever the launcher wants the game-data check again and returns a malloc'd JSON
/// string of a DataStatus. Returns the launcher, or NULL when it could not be hosted.
@MainActor @_cdecl("anomaly_launcher_open")
public func anomaly_launcher_open(_ nativeWindow: UnsafeMutableRawPointer?, _ configJSON: UnsafePointer<CChar>?,
                                  _ dataStatus: (@convention(c) (UnsafeMutableRawPointer?) -> UnsafeMutablePointer<CChar>?)?,
                                  _ user: UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer? {
    guard let configJSON else { return nil }
    let decoder = JSONDecoder()
    decoder.keyDecodingStrategy = .convertFromSnakeCase
    guard let config = try? decoder.decode(LauncherConfig.self, from: Data(bytes: configJSON, count: strlen(configJSON))) else { return nil }
    guard let launcher = LauncherHost(nativeWindow: nativeWindow, config: config, dataStatus: dataStatus, user: user) else { return nil }
    return Unmanaged.passRetained(launcher).toOpaque()
}

/// 0 while the launcher is up, 1 once the player pressed Play, 2 for Quit.
@MainActor @_cdecl("anomaly_launcher_result")
public func anomaly_launcher_result(_ launcher: UnsafeMutableRawPointer?) -> Int32 {
    host(launcher)?.model.result.rawValue ?? 0
}

/// A pad button (or key, with `keyboard` set) in the launcher's own numbering: 0 up, 1 down, 2 left, 3 right, 4 confirm (Cross),
/// 5 back (Circle), 6 square, 7 triangle, 8 options.
@MainActor @_cdecl("anomaly_launcher_button")
public func anomaly_launcher_button(_ launcher: UnsafeMutableRawPointer?, _ button: Int32, _ down: Int32, _ keyboard: Int32) {
    host(launcher)?.button(button, down: down != 0, keyboard: keyboard != 0)
}

/// The left stick: axis 0 is X, 1 is Y (down is positive), value -1...1.
@MainActor @_cdecl("anomaly_launcher_axis")
public func anomaly_launcher_axis(_ launcher: UnsafeMutableRawPointer?, _ axis: Int32, _ value: Float) {
    host(launcher)?.axis(axis, value: value)
}

/// Writes a PNG of the launcher's view. Returns 1 on success.
@MainActor @_cdecl("anomaly_launcher_snapshot")
public func anomaly_launcher_snapshot(_ launcher: UnsafeMutableRawPointer?, _ path: UnsafePointer<CChar>?) -> Int32 {
    guard let path else { return 0 }
    return host(launcher)?.snapshot(to: String(cString: path)) == true ? 1 : 0
}

/// Removes the launcher from its window and releases it. The window is the shell's to destroy afterwards.
@MainActor @_cdecl("anomaly_launcher_close")
public func anomaly_launcher_close(_ launcher: UnsafeMutableRawPointer?) {
    guard let launcher else { return }
    let object = Unmanaged<LauncherHost>.fromOpaque(launcher)
    object.takeUnretainedValue().close()
    object.release()
}
