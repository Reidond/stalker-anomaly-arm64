#include "GameInput/Cursor.h"

#include <algorithm>
#include <cmath>

namespace gameinput
{
VirtualCursor::VirtualCursor() = default;

void VirtualCursor::Clamp(const CursorConfig& cfg)
{
    pos_.x = std::min(std::max(pos_.x, 0.0f), cfg.ui_width);
    pos_.y = std::min(std::max(pos_.y, 0.0f), cfg.ui_height);
}

void VirtualCursor::SetPosition(Vec2 pos, const CursorConfig& cfg)
{
    pos_ = pos;
    Clamp(cfg);
}

void VirtualCursor::Center(const CursorConfig& cfg)
{
    pos_ = {cfg.ui_width * 0.5f, cfg.ui_height * 0.5f};
}

CursorFrame VirtualCursor::Update(float dt, Vec2 stick, bool precision, unsigned dpad_mask,
    const PadSnapshot::Finger& finger, bool touchpad_available, const CursorConfig& cfg,
    INavigationProvider* nav)
{
    CursorFrame out;
    const Vec2 start = pos_;

    // Left stick: velocity control with a response curve, slowed while precision is held.
    const Vec2 dz = ApplyRadialDeadzone(stick, cfg.deadzone, 1.0f);
    const float mag = dz.Length();
    if (mag > 0.0f)
    {
        const float speed = cfg.speed * ResponseCurve(mag, cfg.exponent) * (precision ? cfg.precision_scale : 1.0f);
        pos_.x += dz.x / mag * speed * dt;
        pos_.y += dz.y / mag * speed * dt;
    }

    // Touchpad as a trackpad: relative motion, with mild speed-dependent gain so small flicks stay
    // precise and long swipes cross the screen.
    if (touchpad_available && cfg.trackpad_enabled > 0.5f)
    {
        if (finger.down)
        {
            if (finger_was_down_)
            {
                const Vec2 d = finger.pos - finger_last_;
                const float travel = d.Length();
                const float boost = 1.0f + std::min(travel * 40.0f, 1.5f);
                pos_.x += d.x * cfg.trackpad_gain * cfg.ui_width * boost * (precision ? cfg.precision_scale : 1.0f);
                pos_.y += d.y * cfg.trackpad_gain * cfg.ui_width * boost * (precision ? cfg.precision_scale : 1.0f);
                finger_travel_ += travel;
            }
            else
            {
                finger_time_ = 0.0f;
                finger_travel_ = 0.0f;
            }
            finger_time_ += dt;
            finger_last_ = finger.pos;
        }
        else if (finger_was_down_)
        {
            if (cfg.tap_to_click > 0.5f && finger_time_ < 0.22f && finger_travel_ < 0.02f)
                out.tapped = true;
        }
        finger_was_down_ = finger.down;
    }
    else
    {
        finger_was_down_ = false;
    }

    // D-pad: first press moves once (widget jump or fixed nudge), then repeats while held.
    static const NavDir kDirs[4] = {NavDir::Up, NavDir::Down, NavDir::Left, NavDir::Right};
    for (int i = 0; i < 4; ++i)
    {
        const unsigned bit = 1u << unsigned(kDirs[i]);
        const bool down = (dpad_mask & bit) != 0;
        const bool was = (dpad_prev_ & bit) != 0;
        bool fire = false;
        if (down && !was)
        {
            fire = true;
            dpad_timer_[i] = cfg.repeat_delay;
            dpad_repeating_[i] = false;
        }
        else if (down)
        {
            dpad_timer_[i] -= dt;
            if (dpad_timer_[i] <= 0.0f)
            {
                fire = true;
                dpad_timer_[i] += cfg.repeat_rate;
                dpad_repeating_[i] = true;
            }
        }
        if (!fire)
            continue;

        Vec2 target;
        if (nav && nav->FindNeighbor(pos_, kDirs[i], target))
        {
            pos_ = target;
            out.jumped = true;
        }
        else
        {
            const float step = cfg.dpad_step;
            switch (kDirs[i])
            {
            case NavDir::Up: pos_.y -= step; break;
            case NavDir::Down: pos_.y += step; break;
            case NavDir::Left: pos_.x -= step; break;
            case NavDir::Right: pos_.x += step; break;
            }
        }
    }
    dpad_prev_ = dpad_mask;

    Clamp(cfg);
    out.pos = pos_;
    out.delta = pos_ - start;
    return out;
}

int WheelScroller::Update(float dt, float stick_y, const CursorConfig& cfg)
{
    const float a = std::fabs(stick_y);
    if (a <= cfg.scroll_deadzone)
    {
        carry_ = 0.0f;
        return 0;
    }
    const float t = (a - cfg.scroll_deadzone) / (1.0f - cfg.scroll_deadzone);
    // Stick up (negative y) scrolls up.
    carry_ += (stick_y < 0.0f ? 1.0f : -1.0f) * t * cfg.scroll_rate * dt;
    const int whole = int(carry_ < 0.0f ? std::ceil(carry_) : std::floor(carry_));
    carry_ -= float(whole);
    return whole;
}
} // namespace gameinput
