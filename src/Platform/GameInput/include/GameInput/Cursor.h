// Virtual mouse cursor for menus (inventory, PDA, trade, dialogs, main menu).
//
// Position is kept in the engine's UI space (1024x768 by default): that is the space CUICursor
// works in, and its UpdateCursorPosition(dx, dy) adds deltas straight to it. Three ways to move:
// the left stick, the DualSense touchpad used as a trackpad, and D-pad nudges / widget jumps.
#pragma once

#include "GameInput/Config.h"
#include "GameInput/Filters.h"
#include "GameInput/Types.h"

namespace gameinput
{
enum class NavDir : uint8_t { Up, Down, Left, Right };

// Optional hook so D-pad navigation can jump between real widgets. The adapter implements it by
// walking the visible CUIWindow tree; without one the D-pad just nudges the cursor.
class INavigationProvider
{
public:
    virtual ~INavigationProvider() = default;
    // Find the focusable widget nearest to `from` in direction `dir`; return false if none.
    virtual bool FindNeighbor(Vec2 from, NavDir dir, Vec2& out_center) = 0;
};

struct CursorFrame
{
    Vec2 pos;          // UI units
    Vec2 delta;        // movement this frame, UI units (what to feed IR_OnMouseMove)
    bool jumped = false; // pos changed discontinuously (D-pad widget jump, recentre): use SetUICursorPosition
    bool tapped = false; // trackpad tap-to-click fired this frame
};

class VirtualCursor
{
public:
    VirtualCursor();

    // stick: left stick, +y down. precision: L3 held. dpad: bitmask of 1<<NavDir currently held.
    // finger: first touchpad finger (valid when pad.has_touchpad).
    CursorFrame Update(float dt, Vec2 stick, bool precision, unsigned dpad_mask,
        const PadSnapshot::Finger& finger, bool touchpad_available, const CursorConfig& cfg,
        INavigationProvider* nav);

    Vec2 Position() const { return pos_; }
    void SetPosition(Vec2 pos, const CursorConfig& cfg);
    void Center(const CursorConfig& cfg);

private:
    void Clamp(const CursorConfig& cfg);

    Vec2 pos_ = {512.0f, 384.0f};
    bool finger_was_down_ = false;
    Vec2 finger_last_;
    float finger_time_ = 0.0f;
    float finger_travel_ = 0.0f;
    unsigned dpad_prev_ = 0;
    float dpad_timer_[4] = {};
    bool dpad_repeating_[4] = {};
};

// Right-stick scrolling for lists: converts deflection into mouse-wheel notches with a carried
// remainder. Positive = scroll up (engine IR_OnMouseWheel direction convention).
class WheelScroller
{
public:
    int Update(float dt, float stick_y, const CursorConfig& cfg);

private:
    float carry_ = 0.0f;
};
} // namespace gameinput
