// Shared-memory layout between the page's WebHID DualSense module (page/dualsense.js) and the
// engine thread. The page writes PadShared after every input report; the engine reads it into a
// gameinput::PadSnapshot. The engine writes PadFeedbackShared (what IFeedbackSink receives); the
// page turns it into output reports. Offsets are fixed: dualsense.js PAD_LAYOUT mirrors them.
//
// Both blocks use a sequence lock: the writer makes seq odd, writes, then makes it even again; a
// reader copies the block and retries if seq was odd or changed meanwhile.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "GameInput/Feedback.h"
#include "GameInput/Types.h"

namespace lab::pad
{
enum PadFlags : uint32_t
{
    kConnected = 1u << 0,
    kHasTouchpad = 1u << 1,
    kHasGyro = 1u << 2,
    kHasAccel = 1u << 3,
    kCharging = 1u << 4,
    kBluetooth = 1u << 5,
    kCalibrated = 1u << 6, // IMU calibration from feature report 0x05 applied
};

struct PadShared
{
    uint32_t seq;          // 0
    uint32_t flags;        // 4  PadFlags
    uint32_t buttons;      // 8  bit n = gameinput::PadButton(n)
    float left[2];         // 12 -1..1, +y down
    float right[2];        // 20
    float l2, r2;          // 28, 32  0..1
    struct Finger
    {
        uint32_t down;     // 0/1
        float x, y;        // 0..1, y down
        float pressure;    // 0..1 (DualSense reports touch only: 1 while down)
    } finger[2];           // 36, 52
    float gyro[3];         // 68 rad/s, SDL axes
    float accel[3];        // 80 m/s^2, SDL axes
    float battery;         // 92 0..1, negative if unknown
    uint32_t report_count; // 96
    uint32_t pad0;         // 100
    double epoch_ms;       // 104 performance.timeOrigin + event.timeStamp of the last report
};
static_assert(sizeof(PadShared) == 112);
static_assert(offsetof(PadShared, finger) == 36 && offsetof(PadShared, gyro) == 68 && offsetof(PadShared, epoch_ms) == 104);

struct TriggerShared
{
    uint32_t type;      // gameinput::TriggerEffect::Type
    float start, end, strength, end_strength, amplitude, frequency;
};
static_assert(sizeof(TriggerShared) == 28);

enum FeedbackFlags : uint32_t
{
    kRumble = 1u << 0,
    kLed = 1u << 1,
    kTriggers = 1u << 2,
    kPlayerLeds = 1u << 3,
};

struct PadFeedbackShared
{
    uint32_t seq;             // 0
    uint32_t flags;           // 4  FeedbackFlags: which parts are valid
    float motor_low;          // 8  heavy motor (MotorState::low)
    float motor_high;         // 12 light motor (MotorState::high)
    uint8_t led[3];           // 16 r, g, b
    uint8_t player_leds;      // 19 bits 0..4
    TriggerShared left;       // 20 L2
    TriggerShared right;      // 48 R2
};
static_assert(sizeof(PadFeedbackShared) == 76 && offsetof(PadFeedbackShared, right) == 48);

// Copies the latest pad state; false if the writer was mid-update twice in a row.
inline bool ReadPad(const PadShared& src, gameinput::PadSnapshot& out, uint32_t* report_count = nullptr)
{
    PadShared copy;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const uint32_t s0 = __atomic_load_n(&src.seq, __ATOMIC_ACQUIRE);
        std::memcpy(&copy, &src, sizeof(copy));
        const uint32_t s1 = __atomic_load_n(&src.seq, __ATOMIC_ACQUIRE);
        if (s0 == s1 && (s0 & 1u) == 0)
        {
            out.connected = copy.flags & kConnected;
            out.buttons = copy.buttons;
            out.left = {copy.left[0], copy.left[1]};
            out.right = {copy.right[0], copy.right[1]};
            out.l2 = copy.l2;
            out.r2 = copy.r2;
            out.has_touchpad = copy.flags & kHasTouchpad;
            for (int i = 0; i < 2; ++i)
            {
                out.finger[i].down = copy.finger[i].down != 0;
                out.finger[i].pos = {copy.finger[i].x, copy.finger[i].y};
                out.finger[i].pressure = copy.finger[i].pressure;
            }
            out.has_gyro = copy.flags & kHasGyro;
            out.has_accel = copy.flags & kHasAccel;
            for (int i = 0; i < 3; ++i)
            {
                out.gyro[i] = copy.gyro[i];
                out.accel[i] = copy.accel[i];
            }
            out.battery = copy.battery;
            out.charging = copy.flags & kCharging;
            if (report_count)
                *report_count = copy.report_count;
            return true;
        }
    }
    return false;
}

inline TriggerShared ToShared(const gameinput::TriggerEffect& e)
{
    return {uint32_t(e.type), e.start, e.end, e.strength, e.end_strength, e.amplitude, e.frequency};
}

// What an IFeedbackSink built on this layout does: publish, then the page sends output reports.
inline void WriteFeedback(PadFeedbackShared& dst, uint32_t flags, const gameinput::MotorState& motors, gameinput::Rgb led,
                          const gameinput::TriggerPair& triggers, uint8_t player_leds)
{
    const uint32_t s = __atomic_load_n(&dst.seq, __ATOMIC_RELAXED);
    __atomic_store_n(&dst.seq, s + 1, __ATOMIC_RELEASE);
    dst.flags = flags;
    dst.motor_low = motors.low;
    dst.motor_high = motors.high;
    dst.led[0] = led.r;
    dst.led[1] = led.g;
    dst.led[2] = led.b;
    dst.player_leds = player_leds;
    dst.left = ToShared(triggers.left);
    dst.right = ToShared(triggers.right);
    __atomic_store_n(&dst.seq, s + 2, __ATOMIC_RELEASE);
}
} // namespace lab::pad
