#include "GameInput/Gyro.h"

#include <algorithm>
#include <cmath>

namespace gameinput
{
namespace
{
constexpr float kRadToDeg = 57.29577951f;
constexpr float kStillRate = 0.04f;      // rad/s around the current bias that counts as "not moving"
constexpr float kCalibrateAfter = 1.0f;  // seconds of stillness before the bias is trusted
constexpr float kBiasTimeConstant = 0.6f; // seconds

float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
} // namespace

void GyroAim::Reset()
{
    *this = GyroAim();
}

Vec2 GyroAim::Update(const PadSnapshot& pad, float dt, bool aiming, const GyroConfig& cfg)
{
    if (!pad.has_gyro || dt <= 0.0f)
    {
        rate_ = {};
        return {};
    }

    // Zero-rate offset: while the pad barely moves, slowly pull the bias towards the raw reading.
    // Done even when the mode is Off so switching it on mid-game starts calibrated.
    const float dev[3] = {pad.gyro[0] - bias_[0], pad.gyro[1] - bias_[1], pad.gyro[2] - bias_[2]};
    if (std::sqrt(Dot(dev, dev)) < kStillRate)
    {
        still_time_ += dt;
        const float a = 1.0f - std::exp(-dt / kBiasTimeConstant);
        for (int i = 0; i < 3; ++i)
            bias_[i] += (pad.gyro[i] - bias_[i]) * a;
        if (still_time_ >= kCalibrateAfter)
            calibrated_ = true;
    }
    else
    {
        still_time_ = 0.0f;
    }

    // Gravity direction in controller space, low-passed so hand shake does not leak into yaw.
    if (pad.has_accel)
    {
        const float len = std::sqrt(Dot(pad.accel, pad.accel));
        if (len > 1.0f)
        {
            const float n[3] = {pad.accel[0] / len, pad.accel[1] / len, pad.accel[2] / len};
            const float a = have_gravity_ ? 1.0f - std::exp(-dt / 0.2f) : 1.0f;
            for (int i = 0; i < 3; ++i)
                gravity_[i] += (n[i] - gravity_[i]) * a;
            const float gl = std::sqrt(Dot(gravity_, gravity_));
            if (gl > 1e-4f)
                for (float& g : gravity_)
                    g /= gl;
            have_gravity_ = true;
        }
    }

    const GyroMode mode = cfg.Mode();
    if (mode == GyroMode::Off || (mode == GyroMode::AdsOnly && !aiming))
    {
        rate_ = {};
        return {};
    }

    float g[3] = {pad.gyro[0] - bias_[0], pad.gyro[1] - bias_[1], pad.gyro[2] - bias_[2]};

    // Rotation rate about world-up (counter-clockwise from above is positive). With the pad held
    // upright, up in controller space is -z; lying flat it is +y. Gravity tells us which.
    float up[3] = {0.0f, 0.0f, -1.0f};
    if (cfg.player_space > 0.5f && have_gravity_)
        std::copy(gravity_, gravity_ + 3, up);
    const float yaw_ccw = Dot(g, up);

    // Positive rotation about the pad's x axis tips its top edge towards the player = look down.
    Vec2 target = {-yaw_ccw * kRadToDeg, g[0] * kRadToDeg}; // deg/s: x right+, y down+

    // Soft noise floor: shrink the vector instead of gating it so slow deliberate motion still counts.
    const float floor_deg = cfg.noise_floor * kRadToDeg;
    for (float* v : {&target.x, &target.y})
    {
        const float m = std::fabs(*v);
        *v = (m <= floor_deg) ? 0.0f : std::copysign(m - floor_deg, *v);
    }

    const float alpha = 1.0f - std::pow(std::min(std::max(cfg.smoothing, 0.0f), 0.98f), dt * 60.0f);
    rate_.x += (target.x - rate_.x) * alpha;
    rate_.y += (target.y - rate_.y) * alpha;

    float scale = cfg.sensitivity * (aiming ? cfg.ads_scale : 1.0f);
    Vec2 out = {rate_.x * scale * dt, rate_.y * scale * dt};
    if (cfg.invert_x > 0.5f)
        out.x = -out.x;
    if (cfg.invert_y > 0.5f)
        out.y = -out.y;
    return out;
}
} // namespace gameinput
