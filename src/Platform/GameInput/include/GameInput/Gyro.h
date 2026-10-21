// Gyro aim: turns controller rotation into view rotation, with drift calibration.
#pragma once

#include "GameInput/Config.h"
#include "GameInput/Types.h"

namespace gameinput
{
class GyroAim
{
public:
    // Returns degrees for this frame in the same convention as LookFilter: x = yaw (+right),
    // y = pitch (+down). Zero when the mode is Off, or AdsOnly and not aiming.
    Vec2 Update(const PadSnapshot& pad, float dt, bool aiming, const GyroConfig& cfg);
    void Reset();

    // Diagnostics for the lab.
    Vec2 LastRate() const { return rate_; }        // smoothed view-space rate in deg/s
    const float* Bias() const { return bias_; }    // estimated gyro zero offset, rad/s
    bool Calibrated() const { return calibrated_; }
    bool Stationary() const { return still_time_ > 0.0f; }

private:
    float bias_[3] = {};
    float gravity_[3] = {0.0f, 1.0f, 0.0f};
    bool have_gravity_ = false;
    float still_time_ = 0.0f;
    bool calibrated_ = false;
    Vec2 rate_;
};
} // namespace gameinput
