// GameInput: the controller layer's entry point.
//
// Once per frame the host passes in a PadSnapshot and gets back a FrameOutput: key edges in the
// engine's key space (pad keys such as gp_cross, plus emulated mouse/keyboard keys in menus),
// movement, look and cursor deltas, radial-menu state, and device events. Feedback (rumble, light
// bar, adaptive triggers) flows the other way through feedback() and an IFeedbackSink.
//
// Contexts:
//   Gameplay - sticks move and look; face buttons produce pad keys the engine binds to actions.
//   Menu     - a UI dialog with a cursor is on top: the left stick, the touchpad and the D-pad move a
//              virtual cursor, and buttons turn into mouse clicks / modifier keys (see MenuMap).
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "GameInput/Bindings.h"
#include "GameInput/Config.h"
#include "GameInput/Cursor.h"
#include "GameInput/Device.h"
#include "GameInput/Feedback.h"
#include "GameInput/Filters.h"
#include "GameInput/Gyro.h"
#include "GameInput/PadKeys.h"
#include "GameInput/Radial.h"
#include "GameInput/Types.h"

namespace gameinput
{
enum class InputContext : uint8_t { Gameplay, Menu };

enum class UiCommand : uint8_t { PagePrev, PageNext };

struct KeyEvent
{
    KeyCode key = 0;
    bool down = false;
};

// What a physical button does while a menu is open.
struct MenuBinding
{
    enum class Kind
    {
        Pass,        // emit the pad key itself, so engine bindings (e.g. inventory toggle) still work
        Emulate,     // emit `key` (a mouse button / DIK code) for as long as the button is held
        DoubleClick, // emit a left-button double click
        Ui,          // fire `ui` once on press
        Internal,    // consumed by the layer (cursor precision, recentre, D-pad navigation)
        Ignore,
    };
    Kind kind = Kind::Pass;
    KeyCode key = 0;
    UiCommand ui = UiCommand::PagePrev;
};

class MenuMap
{
public:
    static MenuMap Default();
    const MenuBinding& Lookup(KeyCode pad_key) const;
    void Set(KeyCode pad_key, MenuBinding binding) { map_[pad_key] = binding; }

private:
    std::map<KeyCode, MenuBinding> map_;
};

struct FrameOutput
{
    bool pad_active = false; // a pad is connected and drove this frame
    InputContext context = InputContext::Gameplay;

    // Key edges since the last frame, releases first. Pad keys, radial keys and emulated engine keys.
    std::vector<KeyEvent> keys;
    // Every key down this frame (including ones pressed now), for IR_OnKeyboardHold.
    std::vector<KeyCode> held;
    // Names of the actions that went down / are down, via the layer's BindingMap (diagnostics and
    // consumers that do not use engine bindings).
    std::vector<std::string> actions_pressed;
    std::vector<std::string> actions_down;

    // Gameplay: analog movement. x: strafe, right positive. y: forward positive. The same push
    // also drives the gp_lstick_* / gp_walk keys, which is what the engine actually moves with.
    Vec2 move;
    float move_magnitude = 0.0f;
    bool walking = false;

    // Gameplay: view rotation this frame in degrees (x: right positive, y: down positive, like a
    // mouse delta).
    Vec2 look_stick_deg;
    Vec2 look_gyro_deg;
    Vec2 look_deg;

    // Menu: virtual cursor.
    bool cursor_active = false;
    CursorFrame cursor;
    int wheel = 0; // mouse wheel notches this frame, positive = up
    std::vector<UiCommand> ui;

    RadialView radial;
    std::vector<RadialEvent> radial_events;

    std::vector<DeviceEvent> device_events;
};

class GameInput
{
public:
    GameInput();

    Config& config() { return config_; }
    const Config& config() const { return config_; }
    BindingMap& bindings() { return bindings_; }
    const BindingMap& bindings() const { return bindings_; }
    MenuMap& menu_map() { return menu_map_; }
    RadialMenu& radial() { return radial_; }
    const RadialMenu& radial() const { return radial_; }
    Feedback& feedback() { return feedback_; }
    const Feedback& feedback() const { return feedback_; }
    DeviceRegistry& devices() { return devices_; }
    const DeviceRegistry& devices() const { return devices_; }

    void SetContext(InputContext context) { wanted_context_ = context; }
    InputContext context() const { return context_; }
    // While a Controls-menu field is waiting for a key, the layer reports every pad button (and stick
    // direction) as its own pad key instead of turning it into a mouse click, so it can be bound.
    // Buttons already held when capture starts are ignored until released.
    void SetKeyCapture(bool capture) { wanted_capture_ = capture; }
    bool key_capture() const { return capture_; }
    // Engine-reported aiming state (zoomed in). Without it the layer assumes L2 held = aiming.
    void SetAiming(bool aiming) { aiming_override_ = aiming ? 1 : 0; }
    void ClearAimingOverride() { aiming_override_ = -1; }
    bool aiming() const { return aiming_; }

    void SetFeedbackSink(IFeedbackSink* sink) { sink_ = sink; }
    void SetNavigationProvider(INavigationProvider* nav) { nav_ = nav; }
    // Which pad keys to emit. Default: only keys that are bound, in the menu map, or radial keys, so
    // the engine's script key callbacks are not spammed by unbound inputs. Pass nullptr to emit all.
    void SetKeyFilter(std::function<bool(KeyCode)> filter) { key_filter_ = std::move(filter); has_custom_filter_ = true; }
    void ClearKeyFilter() { key_filter_ = nullptr; has_custom_filter_ = true; }

    // Place the virtual cursor, e.g. when a dialog opens with the engine's cursor already somewhere.
    void SetCursorPosition(Vec2 ui_pos) { cursor_.SetPosition(ui_pos, config_.cursor); }
    Vec2 cursor_position() const { return cursor_.Position(); }

    // Device lifecycle, called from the host's event loop (SdlPadSource does this).
    DeviceIdentity OnDeviceConnected(DeviceIdentity id) { return devices_.Connect(std::move(id)); }
    void OnDeviceDisconnected(uint32_t instance_id) { devices_.Disconnect(instance_id); }

    // One frame. `pad.connected == false` releases everything and silences feedback.
    FrameOutput Update(float dt, const PadSnapshot& pad);

    // Whether a key (pad or emulated) is down right now: the pad-side answer for iGetAsyncKeyState.
    bool IsKeyDown(KeyCode key) const;

    // Whole-number mouse counts from fractional degrees / UI units, carrying the remainder across
    // frames. counts_per_degree comes from the engine's current sensitivity and FOV.
    void LookToCounts(Vec2 degrees, float counts_per_degree, int& dx, int& dy);
    void CursorToCounts(Vec2 ui_delta, int& dx, int& dy);

    // State shown in the lab.
    const GyroAim& gyro() const { return gyro_; }
    const LookFilter& look_filter() const { return look_; }

private:
    bool ShouldEmit(KeyCode key) const;
    void ReleaseAll(std::vector<KeyEvent>& events);

    struct Pulse
    {
        KeyCode key = 0;
        int delay = 0;
        bool down = false;
    };
    void AddPulse(KeyCode key, int delay_frames = 0);

    Config config_;
    BindingMap bindings_ = BindingMap::Default();
    MenuMap menu_map_ = MenuMap::Default();
    RadialMenu radial_;
    Feedback feedback_;
    FeedbackDriver driver_;
    DeviceRegistry devices_;
    IFeedbackSink* sink_ = nullptr;
    INavigationProvider* nav_ = nullptr;
    std::function<bool(KeyCode)> key_filter_;
    bool has_custom_filter_ = false;

    InputContext wanted_context_ = InputContext::Gameplay;
    InputContext context_ = InputContext::Gameplay;
    bool wanted_capture_ = false;
    bool capture_ = false;
    int aiming_override_ = -1;
    bool aiming_ = false;
    bool was_connected_ = false;

    // Filters and stateful helpers.
    LookFilter look_;
    GyroAim gyro_;
    VirtualCursor cursor_;
    WheelScroller scroller_;
    CountAccumulator look_x_, look_y_, cur_x_, cur_y_;
    Hysteresis l2_, r2_;
    Hysteresis dir_[8]; // 4 left-stick + 4 right-stick direction keys
    Hysteresis walk_;
    Hysteresis touch_dir_[4];

    // Key bookkeeping: what has been told to the host.
    static constexpr int kPhysCount = kPadButtonKeyEnd - kPadKeyBase;
    std::map<KeyCode, bool> emitted_;
    std::vector<Pulse> pulses_;
    bool prev_phys_[kPhysCount] = {}; // physical state last frame, for press edges
    bool latched_[kPhysCount] = {};   // held across a context switch: ignored until released
    Vec2 swipe_start_;
    Vec2 swipe_last_;
    bool swiping_ = false;
    float swipe_time_ = 0.0f;
};
} // namespace gameinput
