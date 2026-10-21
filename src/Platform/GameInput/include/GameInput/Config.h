// All tunables for the controller layer, with defaults chosen for a DualSense on STALKER Anomaly.
//
// Plain data on purpose: the engine's options screen, the input_lab sliders and the per-device
// profile files all read and write the same struct through Tunables().
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gameinput
{
enum class GyroMode : int
{
    Off = 0,
    AdsOnly = 1, // gyro adds to stick aim only while aiming down sights
    Always = 2,
};

struct MoveConfig
{
    float deadzone = 0.18f;       // radial inner deadzone
    float outer = 0.97f;          // magnitude treated as full deflection
    float key_on = 0.42f;         // a direction key engages above this (after deadzone rescale)
    float key_off = 0.32f;        // ...and releases below this
    float walk_threshold = 0.62f; // moving below this magnitude holds gp_walk (slow walk)
    float walk_hysteresis = 0.05f;
    float cardinal_snap_deg = 9.0f; // snap near-axis pushes to the axis so strafing is not diagonal
    float walk_enabled = 1.0f;      // 0 = never produce gp_walk (always run)
};

struct LookConfig
{
    float deadzone = 0.10f;
    float outer = 0.98f;
    float exponent = 1.7f;     // response curve: 1 = linear, higher = finer control near centre
    float yaw_speed = 220.0f;  // degrees per second at full deflection
    float pitch_speed = 150.0f;
    float accel_gain = 1.5f;      // extra speed multiplier reached after accel_time of full push (1 = off)
    float accel_time = 0.45f;
    float accel_threshold = 0.85f; // deflection that counts as "pushing to the edge"
    float ads_scale = 0.55f;       // look speed multiplier while aiming down sights
    float invert_y = 0.0f;
};

struct GyroConfig
{
    float mode = float(GyroMode::Off); // a GyroMode value; stored as float so it fits Tunables()
    float sensitivity = 1.0f;  // 1 = one degree of view per degree of controller rotation
    float ads_scale = 0.6f;    // extra multiplier while aiming down sights
    float noise_floor = 0.010f; // rad/s below which motion is treated as sensor noise
    float smoothing = 0.45f;    // 0 = raw, towards 1 = heavier smoothing
    float player_space = 1.0f;  // 1 = yaw measured around world-up using gravity; 0 = fixed controller axes
    float invert_x = 0.0f;
    float invert_y = 0.0f;

    GyroMode Mode() const { return GyroMode(int(mode + 0.5f)); }
};

struct TriggerConfig
{
    float press = 0.35f; // digital threshold for gp_l2 / gp_r2
    float release = 0.25f;
};

struct CursorConfig
{
    float ui_width = 1024.0f; // the engine's UI base resolution (UI_BASE_WIDTH x UI_BASE_HEIGHT)
    float ui_height = 768.0f;
    float deadzone = 0.14f;
    float speed = 700.0f;     // UI units per second at full deflection
    float exponent = 1.9f;
    float precision_scale = 0.30f; // speed multiplier while L3 is held
    float dpad_step = 96.0f;       // UI units per D-pad nudge when no widget provider is attached
    float repeat_delay = 0.35f;
    float repeat_rate = 0.09f;
    float trackpad_gain = 1.35f;   // cursor travel per full pad width, in multiples of ui_width
    float trackpad_enabled = 1.0f;
    float tap_to_click = 0.0f;
    float scroll_rate = 9.0f;      // wheel notches per second at full right-stick deflection
    float scroll_deadzone = 0.30f;
};

struct RadialConfig
{
    float hold_seconds = 0.22f; // holding the wheel button this long opens it; shorter is a tap
    float deadzone = 0.45f;     // stick magnitude needed to pick a slot
    float sticky = 0.18f;       // fraction of a sector the stick must pass before the pick changes
};

struct FeedbackConfig
{
    float rumble_scale = 1.0f;       // 0 turns rumble off
    float trigger_effects = 1.0f;    // 0 turns adaptive triggers off
    float lightbar = 1.0f;           // 0 leaves the light bar alone
    float lightbar_brightness = 0.8f;
};

struct Config
{
    MoveConfig move;
    LookConfig look;
    GyroConfig gyro;
    TriggerConfig trigger;
    CursorConfig cursor;
    RadialConfig radial;
    FeedbackConfig feedback;
};

// Reflection over Config for sliders and persistence. Pointers stay valid as long as the Config does.
struct Tunable
{
    const char* key; // "look.exponent"
    float* value;
    float min;
    float max;
    float step;
};
std::vector<Tunable> Tunables(Config& config);

// "key = value" lines; unknown keys are reported, bad numbers are ignored, values are clamped.
std::string SerializeConfig(const Config& config);
int ParseConfig(Config& config, std::string_view text, std::vector<std::string>* errors = nullptr);
} // namespace gameinput
