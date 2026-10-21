// Backend-neutral input types: the raw controller sample the layer consumes each frame.
//
// Nothing in the core library depends on SDL or the engine; the SDL backend (SdlPadSource) fills a
// PadSnapshot from SDL_Gamepad, and tests build snapshots by hand.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace gameinput
{
struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    float Length() const { return std::sqrt(x * x + y * y); }
};

// Physical buttons, in a fixed order that does not depend on SDL's enum values.
enum class PadButton : uint8_t
{
    South, // Cross / A
    East,  // Circle / B
    West,  // Square / X
    North, // Triangle / Y
    LeftShoulder,
    RightShoulder,
    LeftStick,
    RightStick,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Back,  // Create / View
    Start, // Options / Menu
    Guide, // PS / Xbox
    Touchpad,
    Misc1, // Mic mute / Share
    PaddleLeft1,
    PaddleLeft2,
    PaddleRight1,
    PaddleRight2,
    Count
};

struct PadSnapshot
{
    bool connected = false;
    uint32_t buttons = 0; // bit n = PadButton(n) is down

    // Sticks are -1..1 with +x right and +y DOWN (screen convention, same as SDL and mouse deltas).
    Vec2 left;
    Vec2 right;
    // Analog triggers, 0..1.
    float l2 = 0.0f;
    float r2 = 0.0f;

    struct Finger
    {
        bool down = false;
        Vec2 pos;            // 0..1 across the pad, y down
        float pressure = 0.0f;
    };
    bool has_touchpad = false;
    Finger finger[2];

    // Gyro in rad/s and accelerometer in m/s^2, SDL sensor axes: +x right, +y up, +z toward the
    // player when the pad lies flat. The accelerometer reads +g on the axis pointing up.
    bool has_gyro = false;
    float gyro[3] = {};
    bool has_accel = false;
    float accel[3] = {};

    float battery = -1.0f; // 0..1, negative if unknown
    bool charging = false;

    bool Down(PadButton b) const { return (buttons >> uint32_t(b)) & 1u; }
    void Set(PadButton b, bool down)
    {
        if (down)
            buttons |= 1u << uint32_t(b);
        else
            buttons &= ~(1u << uint32_t(b));
    }
};
} // namespace gameinput
