#include "GameInput/Config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace gameinput
{
std::vector<Tunable> Tunables(Config& c)
{
    return {
        {"move.deadzone", &c.move.deadzone, 0.0f, 0.5f, 0.01f},
        {"move.key_on", &c.move.key_on, 0.1f, 0.9f, 0.01f},
        {"move.key_off", &c.move.key_off, 0.05f, 0.8f, 0.01f},
        {"move.walk_threshold", &c.move.walk_threshold, 0.2f, 0.95f, 0.01f},
        {"move.walk_hysteresis", &c.move.walk_hysteresis, 0.0f, 0.2f, 0.01f},
        {"move.cardinal_snap_deg", &c.move.cardinal_snap_deg, 0.0f, 25.0f, 1.0f},
        {"move.walk_enabled", &c.move.walk_enabled, 0.0f, 1.0f, 1.0f},

        {"look.deadzone", &c.look.deadzone, 0.0f, 0.4f, 0.01f},
        {"look.exponent", &c.look.exponent, 1.0f, 3.0f, 0.1f},
        {"look.yaw_speed", &c.look.yaw_speed, 60.0f, 540.0f, 10.0f},
        {"look.pitch_speed", &c.look.pitch_speed, 40.0f, 400.0f, 10.0f},
        {"look.accel_gain", &c.look.accel_gain, 1.0f, 3.0f, 0.1f},
        {"look.accel_time", &c.look.accel_time, 0.1f, 1.5f, 0.05f},
        {"look.ads_scale", &c.look.ads_scale, 0.2f, 1.0f, 0.05f},
        {"look.invert_y", &c.look.invert_y, 0.0f, 1.0f, 1.0f},

        {"gyro.mode", &c.gyro.mode, 0.0f, 2.0f, 1.0f},
        {"gyro.sensitivity", &c.gyro.sensitivity, 0.2f, 4.0f, 0.1f},
        {"gyro.ads_scale", &c.gyro.ads_scale, 0.2f, 1.5f, 0.05f},
        {"gyro.noise_floor", &c.gyro.noise_floor, 0.0f, 0.05f, 0.002f},
        {"gyro.smoothing", &c.gyro.smoothing, 0.0f, 0.9f, 0.05f},
        {"gyro.player_space", &c.gyro.player_space, 0.0f, 1.0f, 1.0f},
        {"gyro.invert_x", &c.gyro.invert_x, 0.0f, 1.0f, 1.0f},
        {"gyro.invert_y", &c.gyro.invert_y, 0.0f, 1.0f, 1.0f},

        {"trigger.press", &c.trigger.press, 0.05f, 0.9f, 0.01f},
        {"trigger.release", &c.trigger.release, 0.02f, 0.85f, 0.01f},

        {"cursor.deadzone", &c.cursor.deadzone, 0.0f, 0.5f, 0.01f},
        {"cursor.speed", &c.cursor.speed, 200.0f, 1600.0f, 25.0f},
        {"cursor.exponent", &c.cursor.exponent, 1.0f, 3.0f, 0.1f},
        {"cursor.precision_scale", &c.cursor.precision_scale, 0.1f, 1.0f, 0.05f},
        {"cursor.dpad_step", &c.cursor.dpad_step, 16.0f, 256.0f, 8.0f},
        {"cursor.trackpad_gain", &c.cursor.trackpad_gain, 0.3f, 3.0f, 0.05f},
        {"cursor.trackpad_enabled", &c.cursor.trackpad_enabled, 0.0f, 1.0f, 1.0f},
        {"cursor.tap_to_click", &c.cursor.tap_to_click, 0.0f, 1.0f, 1.0f},
        {"cursor.scroll_rate", &c.cursor.scroll_rate, 1.0f, 30.0f, 0.5f},

        {"radial.hold_seconds", &c.radial.hold_seconds, 0.08f, 0.8f, 0.01f},
        {"radial.deadzone", &c.radial.deadzone, 0.2f, 0.8f, 0.01f},
        {"radial.sticky", &c.radial.sticky, 0.0f, 0.5f, 0.01f},

        {"feedback.rumble_scale", &c.feedback.rumble_scale, 0.0f, 1.0f, 0.05f},
        {"feedback.trigger_effects", &c.feedback.trigger_effects, 0.0f, 1.0f, 1.0f},
        {"feedback.lightbar", &c.feedback.lightbar, 0.0f, 1.0f, 1.0f},
        {"feedback.lightbar_brightness", &c.feedback.lightbar_brightness, 0.1f, 1.0f, 0.05f},
    };
}

std::string SerializeConfig(const Config& config)
{
    Config copy = config; // Tunables() wants a mutable Config; we only read
    std::string out;
    for (const Tunable& t : Tunables(copy))
    {
        char line[96];
        std::snprintf(line, sizeof(line), "%s = %g\n", t.key, double(*t.value));
        out += line;
    }
    return out;
}

int ParseConfig(Config& config, std::string_view text, std::vector<std::string>* errors)
{
    const std::vector<Tunable> tunables = Tunables(config);
    int applied = 0;
    int line_no = 0;
    while (!text.empty())
    {
        ++line_no;
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);

        const size_t comment = line.find_first_of("#;");
        if (comment != std::string_view::npos)
            line = line.substr(0, comment);
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            continue; // not a setting (profiles also hold bind_pad lines)

        auto trim = [](std::string_view s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.remove_suffix(1);
            return s;
        };
        const std::string key(trim(line.substr(0, eq)));
        const std::string value_text(trim(line.substr(eq + 1)));

        auto it = std::find_if(tunables.begin(), tunables.end(), [&](const Tunable& t) { return key == t.key; });
        if (it == tunables.end())
        {
            if (errors)
                errors->push_back("line " + std::to_string(line_no) + ": unknown setting '" + key + "'");
            continue;
        }
        char* end = nullptr;
        const float v = std::strtof(value_text.c_str(), &end);
        if (end == value_text.c_str() || *end != '\0')
        {
            if (errors)
                errors->push_back("line " + std::to_string(line_no) + ": bad number '" + value_text + "'");
            continue;
        }
        *it->value = std::min(std::max(v, it->min), it->max);
        ++applied;
    }
    return applied;
}
} // namespace gameinput
