// Radial menus: hold a shoulder button, flick the right stick towards a slot, release to fire it.
//
// This is the model only (what the wheels contain and how a stick picks a slot); drawing belongs to
// the host. A slot does not name an engine action directly: it names a virtual pad key
// (gp_radial_N), and the binding map says which action that key triggers. So the stock Controls
// menu can retarget any slot, and the wheel labels follow the bindings.
#pragma once

#include <string>
#include <vector>

#include "GameInput/Config.h"
#include "GameInput/PadKeys.h"
#include "GameInput/Types.h"

namespace gameinput
{
struct RadialSlot
{
    KeyCode key = 0;     // gp_radial_N fired when this slot is confirmed
    std::string label;   // fallback label if the key is not bound to an action
};

struct RadialPage
{
    std::string title;
    std::vector<RadialSlot> slots; // clockwise from the top; 2..12 entries
};

struct RadialWheel
{
    std::string name;
    KeyCode open_key = 0; // hold to open; a short tap fires open_key itself
    std::vector<RadialPage> pages;
};

struct RadialEvent
{
    enum class Type { Tap, Opened, Confirmed, Cancelled, PageChanged };
    Type type = Type::Tap;
    int wheel = -1;
    int page = 0;
    KeyCode key = 0; // Tap: the wheel's open key. Confirmed: the slot's key.
};

struct RadialView
{
    bool pending = false; // button held but not yet long enough to open
    bool open = false;
    int wheel = -1;
    int page = 0;
    int page_count = 0;
    int selected = -1;    // slot index or -1
    int slot_count = 0;
    Vec2 stick;           // right stick as used for selection
};

// Index of the slot a stick direction points at, for `count` slots laid out clockwise from the top.
// -1 if the stick is inside the deadzone. `current` enables stickiness at sector borders.
int RadialPickSlot(Vec2 stick, int count, int current, const RadialConfig& cfg);

class RadialMenu
{
public:
    RadialMenu();

    static std::vector<RadialWheel> DefaultWheels();
    void SetWheels(std::vector<RadialWheel> wheels);
    const std::vector<RadialWheel>& Wheels() const { return wheels_; }

    // wheel_down[i]: wheel i's open button is held. stick: right stick (+y down).
    // page_step: -1/+1 on the frame a page-flip was requested, else 0.
    std::vector<RadialEvent> Update(float dt, const std::vector<bool>& wheel_down, Vec2 stick, int page_step,
        const RadialConfig& cfg);

    // True while a wheel is pending or open: the host should hold back the open key itself.
    bool Active() const { return state_ != State::Idle; }
    // True only while a wheel is on screen: stick and D-pad belong to it.
    bool Capturing() const { return state_ == State::Open; }
    RadialView View() const;
    void Cancel();

private:
    enum class State { Idle, Pending, Open };
    State state_ = State::Idle;
    int wheel_ = -1;
    int page_ = 0;
    int selected_ = -1;
    float held_ = 0.0f;
    Vec2 stick_;
    std::vector<RadialWheel> wheels_;
};
} // namespace gameinput
