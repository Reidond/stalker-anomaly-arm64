#include "GameInput/GameInput.h"

#include <algorithm>
#include <cmath>

namespace gameinput
{
namespace
{
constexpr KeyCode kPhysOf[size_t(PadButton::Count)] = {
    PK_Cross, PK_Circle, PK_Square, PK_Triangle, PK_L1, PK_R1, PK_L3, PK_R3, PK_DpadUp, PK_DpadDown, PK_DpadLeft,
    PK_DpadRight, PK_Create, PK_Options, PK_Ps, PK_Touchpad, PK_Mic, PK_PaddleLeft1, PK_PaddleLeft2, PK_PaddleRight1,
    PK_PaddleRight2,
};

MenuBinding Emulate(KeyCode key)
{
    MenuBinding b;
    b.kind = MenuBinding::Kind::Emulate;
    b.key = key;
    return b;
}

MenuBinding Simple(MenuBinding::Kind kind)
{
    MenuBinding b;
    b.kind = kind;
    return b;
}

MenuBinding UiCmd(UiCommand cmd)
{
    MenuBinding b;
    b.kind = MenuBinding::Kind::Ui;
    b.ui = cmd;
    return b;
}
} // namespace

// ---- MenuMap ----------------------------------------------------------------------------------

MenuMap MenuMap::Default()
{
    using K = MenuBinding::Kind;
    namespace ek = engine_key;
    MenuMap m;
    m.Set(PK_Cross, Emulate(ek::kMouse1));       // click / drag
    m.Set(PK_Square, Emulate(ek::kMouse2));      // context menu
    m.Set(PK_Circle, Emulate(ek::kEscape));      // back / close
    m.Set(PK_Triangle, Simple(K::DoubleClick));    // quick use / equip
    m.Set(PK_L1, Emulate(ek::kLControl));        // Ctrl+click: move item to the other list
    m.Set(PK_R1, Emulate(ek::kLShift));          // Shift+click
    m.Set(PK_L2, UiCmd(UiCommand::PagePrev));
    m.Set(PK_R2, UiCmd(UiCommand::PageNext));
    m.Set(PK_Touchpad, Emulate(ek::kMouse1));    // pressing the pad clicks
    m.Set(PK_L3, Simple(K::Internal));             // cursor precision
    m.Set(PK_R3, Simple(K::Internal));             // recentre cursor
    for (KeyCode k : {KeyCode(PK_DpadUp), KeyCode(PK_DpadDown), KeyCode(PK_DpadLeft), KeyCode(PK_DpadRight)})
        m.Set(k, Simple(K::Internal));             // cursor nudge / widget navigation
    for (KeyCode k : {KeyCode(PK_LStickUp), KeyCode(PK_LStickDown), KeyCode(PK_LStickLeft), KeyCode(PK_LStickRight),
             KeyCode(PK_RStickUp), KeyCode(PK_RStickDown), KeyCode(PK_RStickLeft), KeyCode(PK_RStickRight),
             KeyCode(PK_Walk), KeyCode(PK_TouchSwipeUp), KeyCode(PK_TouchSwipeDown), KeyCode(PK_TouchSwipeLeft),
             KeyCode(PK_TouchSwipeRight)})
        m.Set(k, Simple(K::Ignore));
    m.Set(PK_Ps, Simple(K::Ignore));               // pause would freeze the menu under the cursor
    // Options, Create and Mic stay "Pass": the engine's own bindings close or open dialogs.
    return m;
}

const MenuBinding& MenuMap::Lookup(KeyCode pad_key) const
{
    static const MenuBinding kPass;
    auto it = map_.find(pad_key);
    return it == map_.end() ? kPass : it->second;
}

// ---- GameInput --------------------------------------------------------------------------------

GameInput::GameInput() = default;

bool GameInput::ShouldEmit(KeyCode key) const
{
    if (!IsPadKey(key))
        return true;
    if (capture_)
        return true;
    if (has_custom_filter_)
        return key_filter_ ? key_filter_(key) : true;
    return bindings_.ActionForKey(key) != nullptr;
}

bool GameInput::IsKeyDown(KeyCode key) const
{
    auto it = emitted_.find(key);
    return it != emitted_.end() && it->second;
}

void GameInput::LookToCounts(Vec2 degrees, float counts_per_degree, int& dx, int& dy)
{
    dx = look_x_.Add(degrees.x * counts_per_degree);
    dy = look_y_.Add(degrees.y * counts_per_degree);
}

void GameInput::CursorToCounts(Vec2 ui_delta, int& dx, int& dy)
{
    dx = cur_x_.Add(ui_delta.x);
    dy = cur_y_.Add(ui_delta.y);
}

void GameInput::AddPulse(KeyCode key, int delay_frames)
{
    Pulse p;
    p.key = key;
    p.delay = delay_frames;
    pulses_.push_back(p);
}

void GameInput::ReleaseAll(std::vector<KeyEvent>& events)
{
    for (const auto& [key, down] : emitted_)
        if (down)
            events.push_back({key, false});
    emitted_.clear();
    pulses_.clear();
}

FrameOutput GameInput::Update(float dt, const PadSnapshot& pad)
{
    FrameOutput out;
    out.device_events = devices_.TakeEvents();
    dt = std::min(std::max(dt, 0.0001f), 0.1f);

    // ---- Disconnected: let go of everything -----------------------------------------------------
    if (!pad.connected)
    {
        if (was_connected_)
        {
            ReleaseAll(out.keys);
            look_.Reset();
            radial_.Cancel();
            feedback_.Reset();
            if (sink_)
                sink_->Silence();
            driver_.Invalidate();
            std::fill(std::begin(prev_phys_), std::end(prev_phys_), false);
            std::fill(std::begin(latched_), std::end(latched_), false);
        }
        was_connected_ = false;
        out.context = context_;
        return out;
    }
    if (!was_connected_)
    {
        driver_.Invalidate();
        // Whatever is held on reconnect must be released and pressed again.
        for (int i = 0; i < kPhysCount; ++i)
            latched_[i] = true;
    }
    was_connected_ = true;
    out.pad_active = true;

    // ---- Physical state -------------------------------------------------------------------------
    bool phys[kPhysCount] = {};
    auto P = [&](KeyCode k) -> bool& { return phys[k - kPadKeyBase]; };

    for (size_t b = 0; b < size_t(PadButton::Count); ++b)
        P(kPhysOf[b]) = pad.Down(PadButton(b));
    P(PK_L2) = l2_.Update(pad.l2, config_.trigger.press, config_.trigger.release);
    P(PK_R2) = r2_.Update(pad.r2, config_.trigger.press, config_.trigger.release);

    // ---- Context switch -------------------------------------------------------------------------
    if (wanted_context_ != context_ || wanted_capture_ != capture_)
    {
        ReleaseAll(out.keys);
        radial_.Cancel();
        look_.Reset();
        context_ = wanted_context_;
        capture_ = wanted_capture_;
        // Buttons still held from the old context must not act in the new one until released.
        for (int i = 0; i < kPhysCount; ++i)
            latched_[i] = phys[i];
    }
    out.context = context_;
    const bool menu = context_ == InputContext::Menu;
    feedback_.SetMenu(menu);

    bool raw_phys[kPhysCount];
    std::copy(std::begin(phys), std::end(phys), std::begin(raw_phys));
    for (int i = 0; i < kPhysCount; ++i)
    {
        if (latched_[i])
        {
            if (raw_phys[i])
                phys[i] = false;
            else
                latched_[i] = false;
        }
    }

    // Press edges of the physical keys (before virtual stick keys are added).
    bool edge[kPhysCount] = {};
    for (int i = 0; i < kPhysCount; ++i)
        edge[i] = phys[i] && !prev_phys_[i];
    auto Edge = [&](KeyCode k) { return edge[k - kPadKeyBase]; };

    aiming_ = aiming_override_ >= 0 ? aiming_override_ == 1 : (!menu && P(PK_L2));

    // ---- Gameplay: sticks, radial, look ---------------------------------------------------------
    bool radial_capturing = false;
    if (!menu)
    {
        // Radial menus first: they decide whether the right stick, D-pad and L1/R1 belong to them.
        std::vector<bool> wheel_down;
        for (const RadialWheel& w : radial_.Wheels())
            wheel_down.push_back(IsPadKey(w.open_key) && P(w.open_key));
        int page_step = 0;
        if (radial_.Capturing())
        {
            if (Edge(PK_DpadLeft))
                page_step = -1;
            else if (Edge(PK_DpadRight))
                page_step = +1;
        }
        out.radial_events = radial_.Update(dt, wheel_down, pad.right, page_step, config_.radial);
        for (const RadialEvent& e : out.radial_events)
        {
            if ((e.type == RadialEvent::Type::Tap || e.type == RadialEvent::Type::Confirmed) && e.key != 0)
                AddPulse(e.key);
        }
        radial_capturing = radial_.Capturing();
        out.radial = radial_.View();

        // Left stick -> movement keys and the walk modifier.
        const MoveConfig& mv = config_.move;
        const Vec2 lv = ApplyRadialDeadzone(pad.left, mv.deadzone, mv.outer);
        const float mag = lv.Length();
        const Vec2 ls = SnapToCardinal(lv, mv.cardinal_snap_deg);
        const bool up = dir_[0].Update(-ls.y, mv.key_on, mv.key_off);
        const bool down = dir_[1].Update(ls.y, mv.key_on, mv.key_off);
        const bool left = dir_[2].Update(-ls.x, mv.key_on, mv.key_off);
        const bool right = dir_[3].Update(ls.x, mv.key_on, mv.key_off);
        P(PK_LStickUp) = up;
        P(PK_LStickDown) = down;
        P(PK_LStickLeft) = left;
        P(PK_LStickRight) = right;
        const bool moving = up || down || left || right;
        // Walk needs a push that stays gentle for walk_delay: ramping the stick up from centre passes
        // through the gentle range for a few frames and must not blip into walk.
        if (moving && mv.walk_enabled > 0.5f)
        {
            const bool gentle = walk_active_ ? mag < mv.walk_threshold + mv.walk_hysteresis
                                             : mag < mv.walk_threshold - mv.walk_hysteresis;
            walk_timer_ = gentle ? walk_timer_ + dt : 0.0f;
            walk_active_ = gentle && (walk_active_ || walk_timer_ >= mv.walk_delay);
        }
        else
        {
            walk_timer_ = 0.0f;
            walk_active_ = false;
        }
        const bool walking = walk_active_;
        P(PK_Walk) = walking;
        out.move = {ls.x, -ls.y};
        out.move_magnitude = mag;
        out.walking = walking;

        // Right stick as digital keys (only useful when the user binds them).
        if (!radial_capturing)
        {
            const Vec2 rv = ApplyRadialDeadzone(pad.right, config_.look.deadzone, config_.look.outer);
            P(PK_RStickUp) = dir_[4].Update(-rv.y, mv.key_on, mv.key_off);
            P(PK_RStickDown) = dir_[5].Update(rv.y, mv.key_on, mv.key_off);
            P(PK_RStickLeft) = dir_[6].Update(-rv.x, mv.key_on, mv.key_off);
            P(PK_RStickRight) = dir_[7].Update(rv.x, mv.key_on, mv.key_off);
        }
        else
        {
            for (int i = 4; i < 8; ++i)
                dir_[i].Reset();
        }

        // Look: stick plus gyro.
        if (!radial_capturing)
            out.look_stick_deg = look_.Update(pad.right, dt, aiming_, config_.look);
        out.look_gyro_deg = gyro_.Update(pad, dt, aiming_, config_.gyro);
        out.look_deg = out.look_stick_deg + out.look_gyro_deg;

        // Touchpad swipes as one-shot keys.
        if (pad.has_touchpad)
        {
            const PadSnapshot::Finger& f = pad.finger[0];
            if (f.down && !swiping_)
            {
                swiping_ = true;
                swipe_start_ = f.pos;
                swipe_time_ = 0.0f;
            }
            else if (f.down)
            {
                swipe_time_ += dt;
            }
            if (f.down)
                swipe_last_ = f.pos;
            if (!f.down && swiping_)
            {
                swiping_ = false;
                const Vec2 d = swipe_last_ - swipe_start_;
                if (swipe_time_ < 0.5f && d.Length() > 0.30f)
                {
                    if (std::fabs(d.x) > std::fabs(d.y))
                        AddPulse(d.x > 0 ? PK_TouchSwipeRight : PK_TouchSwipeLeft);
                    else
                        AddPulse(d.y > 0 ? PK_TouchSwipeDown : PK_TouchSwipeUp);
                }
            }
        }
        else
        {
            swiping_ = false;
        }
    }
    else
    {
        look_.Reset();
        gyro_.Update(pad, dt, false, config_.gyro); // keep drift calibration running
        walk_active_ = false;
        walk_timer_ = 0.0f;
        if (!capture_)
            for (Hysteresis& h : dir_)
                h.Reset();
    }

    // ---- Menu key capture: stick directions become keys too ---------------------------------------
    if (menu && capture_)
    {
        const MoveConfig& mv = config_.move;
        const Vec2 lv = SnapToCardinal(ApplyRadialDeadzone(pad.left, mv.deadzone, mv.outer), mv.cardinal_snap_deg);
        const Vec2 rv = ApplyRadialDeadzone(pad.right, config_.look.deadzone, config_.look.outer);
        P(PK_LStickUp) = dir_[0].Update(-lv.y, mv.key_on, mv.key_off);
        P(PK_LStickDown) = dir_[1].Update(lv.y, mv.key_on, mv.key_off);
        P(PK_LStickLeft) = dir_[2].Update(-lv.x, mv.key_on, mv.key_off);
        P(PK_LStickRight) = dir_[3].Update(lv.x, mv.key_on, mv.key_off);
        P(PK_RStickUp) = dir_[4].Update(-rv.y, mv.key_on, mv.key_off);
        P(PK_RStickDown) = dir_[5].Update(rv.y, mv.key_on, mv.key_off);
        P(PK_RStickLeft) = dir_[6].Update(-rv.x, mv.key_on, mv.key_off);
        P(PK_RStickRight) = dir_[7].Update(rv.x, mv.key_on, mv.key_off);
    }

    // ---- Menu: cursor, wheel, UI commands -------------------------------------------------------
    if (menu)
    {
        out.cursor_active = true;
        if (Edge(PK_R3) && menu_map_.Lookup(PK_R3).kind == MenuBinding::Kind::Internal)
        {
            cursor_.Center(config_.cursor);
            out.cursor.jumped = true;
        }
        unsigned dpad = 0;
        if (P(PK_DpadUp))
            dpad |= 1u << unsigned(NavDir::Up);
        if (P(PK_DpadDown))
            dpad |= 1u << unsigned(NavDir::Down);
        if (P(PK_DpadLeft))
            dpad |= 1u << unsigned(NavDir::Left);
        if (P(PK_DpadRight))
            dpad |= 1u << unsigned(NavDir::Right);
        const bool recentred = out.cursor.jumped;
        out.cursor = cursor_.Update(dt, pad.left, P(PK_L3), dpad, pad.finger[0], pad.has_touchpad, config_.cursor, nav_);
        out.cursor.jumped = out.cursor.jumped || recentred;
        if (out.cursor.tapped)
            AddPulse(engine_key::kMouse1);
        out.wheel = scroller_.Update(dt, pad.right.y, config_.cursor);
    }

    // ---- Desired key state ----------------------------------------------------------------------
    std::map<KeyCode, bool> want;
    if (!menu)
    {
        for (int i = 0; i < kPhysCount; ++i)
        {
            const KeyCode k = kPadKeyBase + i;
            if (!phys[i])
                continue;
            // The wheel buttons only produce a tap when released early; the wheel owns them otherwise.
            bool is_wheel_key = false;
            for (const RadialWheel& w : radial_.Wheels())
                is_wheel_key = is_wheel_key || w.open_key == k;
            if (is_wheel_key)
                continue;
            if (radial_capturing && (k == PK_DpadUp || k == PK_DpadDown || k == PK_DpadLeft || k == PK_DpadRight))
                continue;
            want[k] = true;
        }
    }
    else
    {
        for (int i = 0; i < kPhysCount; ++i)
        {
            const KeyCode k = kPadKeyBase + i;
            if (capture_)
            {
                if (phys[i])
                    want[k] = true;
                continue;
            }
            const MenuBinding& b = menu_map_.Lookup(k);
            switch (b.kind)
            {
            case MenuBinding::Kind::Pass:
                if (phys[i])
                    want[k] = true;
                break;
            case MenuBinding::Kind::Emulate:
                if (phys[i])
                    want[b.key] = true;
                break;
            case MenuBinding::Kind::DoubleClick:
                if (edge[i])
                {
                    AddPulse(engine_key::kMouse1, 0);
                    AddPulse(engine_key::kMouse1, 2);
                }
                break;
            case MenuBinding::Kind::Ui:
                if (edge[i])
                    out.ui.push_back(b.ui);
                break;
            case MenuBinding::Kind::Internal:
            case MenuBinding::Kind::Ignore:
                break;
            }
        }
    }

    // Pulses: down for one frame, then up. A pulse on a key that is already down is absorbed.
    for (Pulse& p : pulses_)
    {
        if (p.delay > 0)
        {
            --p.delay;
            continue;
        }
        if (!p.down)
        {
            p.down = true;
            want[p.key] = true;
        }
        else
        {
            p.key = 0; // finished; the release happens because it is no longer wanted
        }
    }
    pulses_.erase(std::remove_if(pulses_.begin(), pulses_.end(), [](const Pulse& p) { return p.key == 0; }),
        pulses_.end());

    // ---- Diff against what the host has been told -----------------------------------------------
    std::vector<KeyEvent> releases, presses;
    for (auto& [key, was] : emitted_)
    {
        const auto it = want.find(key);
        const bool now = it != want.end() && it->second && ShouldEmit(key);
        if (was && !now)
            releases.push_back({key, false});
    }
    for (const auto& [key, wanted] : want)
    {
        if (!wanted || !ShouldEmit(key))
            continue;
        if (!IsKeyDown(key))
            presses.push_back({key, true});
    }
    for (const KeyEvent& e : releases)
        emitted_.erase(e.key);
    for (const KeyEvent& e : presses)
        emitted_[e.key] = true;
    out.keys.insert(out.keys.end(), releases.begin(), releases.end());
    out.keys.insert(out.keys.end(), presses.begin(), presses.end());

    for (const auto& [key, down] : emitted_)
    {
        if (!down)
            continue;
        out.held.push_back(key);
        if (const std::string* action = bindings_.ActionForKey(key))
            out.actions_down.push_back(*action);
    }
    for (const KeyEvent& e : presses)
        if (const std::string* action = bindings_.ActionForKey(e.key))
            out.actions_pressed.push_back(*action);

    std::copy(std::begin(phys), std::end(phys), std::begin(prev_phys_));

    // ---- Feedback -------------------------------------------------------------------------------
    feedback_.SetAiming(aiming_);
    feedback_.SetFiring(!menu && P(PK_R2));
    feedback_.Update(dt, config_.feedback);
    if (sink_)
        driver_.Apply(*sink_, feedback_.Current(), dt, config_.feedback);

    return out;
}
} // namespace gameinput
