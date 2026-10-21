// Stick and trigger conditioning: deadzones, response curves, hysteresis, look acceleration.
#pragma once

#include "GameInput/Config.h"
#include "GameInput/Types.h"

namespace gameinput
{
// Scaled radial deadzone: magnitudes below `inner` map to zero, `outer` and above map to 1, and
// everything between is rescaled linearly, so there is no jump when the stick leaves the deadzone.
Vec2 ApplyRadialDeadzone(Vec2 v, float inner, float outer);

// Response curve on a 0..1 magnitude: pow(m, exponent). Direction is preserved by the caller.
float ResponseCurve(float magnitude, float exponent);

// If `v` points within `snap_deg` of a cardinal axis, drops the minor component, so pushing "up"
// with a slightly crooked thumb does not strafe.
Vec2 SnapToCardinal(Vec2 v, float snap_deg);

// On/off with separate thresholds so a value hovering at the edge does not chatter.
class Hysteresis
{
public:
    // Returns the new state.
    bool Update(float value, float on, float off)
    {
        if (state_)
            state_ = value > off;
        else
            state_ = value >= on;
        return state_;
    }
    bool State() const { return state_; }
    void Reset() { state_ = false; }

private:
    bool state_ = false;
};

// Right-stick look: deadzone -> curve -> degrees per second -> optional ramp-up while the stick is
// held at the edge -> ADS slowdown. Output is degrees for this frame: x = yaw (+right), y = pitch
// (+down, like a mouse dy).
class LookFilter
{
public:
    Vec2 Update(Vec2 stick, float dt, bool aiming, const LookConfig& cfg);
    void Reset() { ramp_ = 0.0f; }
    float Ramp() const { return ramp_; } // 0..1, for the lab display

private:
    float ramp_ = 0.0f;
};

// Splits a fractional per-frame value into whole counts and carries the remainder, so slow stick
// motion still reaches the engine's integer mouse deltas instead of being rounded away.
class CountAccumulator
{
public:
    int Add(float value)
    {
        carry_ += value;
        const int whole = int(carry_ < 0.0f ? std::ceil(carry_) : std::floor(carry_));
        carry_ -= float(whole);
        return whole;
    }
    void Reset() { carry_ = 0.0f; }

private:
    float carry_ = 0.0f;
};
} // namespace gameinput
