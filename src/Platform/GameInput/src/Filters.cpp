#include "GameInput/Filters.h"

#include <algorithm>
#include <cmath>

namespace gameinput
{
namespace
{
constexpr float kPi = 3.14159265358979f;
float Clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
} // namespace

Vec2 ApplyRadialDeadzone(Vec2 v, float inner, float outer)
{
    const float len = v.Length();
    if (len <= inner || len <= 1e-6f)
        return {};
    const float span = std::max(outer - inner, 1e-3f);
    const float scaled = Clamp01((len - inner) / span);
    const float k = scaled / len;
    return {v.x * k, v.y * k};
}

float ResponseCurve(float magnitude, float exponent)
{
    return std::pow(Clamp01(magnitude), std::max(exponent, 0.01f));
}

Vec2 SnapToCardinal(Vec2 v, float snap_deg)
{
    const float len = v.Length();
    if (len < 1e-4f || snap_deg <= 0.0f)
        return v;
    const float snap = std::sin(snap_deg * kPi / 180.0f);
    if (std::fabs(v.x) / len < snap)
        v.x = 0.0f;
    else if (std::fabs(v.y) / len < snap)
        v.y = 0.0f;
    return v;
}

Vec2 LookFilter::Update(Vec2 stick, float dt, bool aiming, const LookConfig& cfg)
{
    const Vec2 dz = ApplyRadialDeadzone(stick, cfg.deadzone, cfg.outer);
    const float mag = dz.Length();
    if (mag <= 0.0f)
    {
        // Let the ramp decay fast so a quick flick back to centre does not leave a boosted next push.
        ramp_ = std::max(0.0f, ramp_ - dt * 4.0f);
        return {};
    }

    if (cfg.accel_gain > 1.0f && mag >= cfg.accel_threshold)
        ramp_ = std::min(1.0f, ramp_ + dt / std::max(cfg.accel_time, 0.01f));
    else
        ramp_ = std::max(0.0f, ramp_ - dt * 3.0f);

    const float curved = ResponseCurve(mag, cfg.exponent);
    const float gain = 1.0f + (cfg.accel_gain - 1.0f) * ramp_;
    const float ads = aiming ? cfg.ads_scale : 1.0f;
    const Vec2 dir = {dz.x / mag, dz.y / mag};

    Vec2 out;
    out.x = dir.x * curved * cfg.yaw_speed * gain * ads * dt;
    out.y = dir.y * curved * cfg.pitch_speed * gain * ads * dt;
    if (cfg.invert_y > 0.5f)
        out.y = -out.y;
    return out;
}
} // namespace gameinput
