#include "GameInput/SdlPadSource.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gameinput
{
namespace
{
struct ButtonMap
{
    SDL_GamepadButton sdl;
    PadButton pad;
};

const ButtonMap kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, PadButton::South},
    {SDL_GAMEPAD_BUTTON_EAST, PadButton::East},
    {SDL_GAMEPAD_BUTTON_WEST, PadButton::West},
    {SDL_GAMEPAD_BUTTON_NORTH, PadButton::North},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PadButton::LeftShoulder},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PadButton::RightShoulder},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, PadButton::LeftStick},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PadButton::RightStick},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, PadButton::DpadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PadButton::DpadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PadButton::DpadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PadButton::DpadRight},
    {SDL_GAMEPAD_BUTTON_BACK, PadButton::Back},
    {SDL_GAMEPAD_BUTTON_START, PadButton::Start},
    {SDL_GAMEPAD_BUTTON_GUIDE, PadButton::Guide},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD, PadButton::Touchpad},
    {SDL_GAMEPAD_BUTTON_MISC1, PadButton::Misc1},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, PadButton::PaddleLeft1},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, PadButton::PaddleLeft2},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, PadButton::PaddleRight1},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, PadButton::PaddleRight2},
};

float Axis(SDL_Gamepad* pad, SDL_GamepadAxis axis)
{
    return std::max(-1.0f, std::min(1.0f, float(SDL_GetGamepadAxis(pad, axis)) / 32767.0f));
}

Uint16 Motor(float v)
{
    return Uint16(std::max(0.0f, std::min(1.0f, v)) * 65535.0f);
}

DeviceIdentity DescribeDevice(SDL_Gamepad* pad, SDL_JoystickID id)
{
    DeviceIdentity d;
    d.instance_id = id;
    if (const char* name = SDL_GetGamepadName(pad))
        d.name = name;
    char guid[33] = {};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), guid, sizeof(guid));
    d.guid = guid;
    if (const char* serial = SDL_GetGamepadSerial(pad))
        d.serial = serial;
    d.vendor = SDL_GetGamepadVendor(pad);
    d.product = SDL_GetGamepadProduct(pad);

    const bool edge = d.name.find("Edge") != std::string::npos;
    switch (SDL_GetGamepadType(pad))
    {
    case SDL_GAMEPAD_TYPE_PS5: d.model = edge ? PadModel::DualSenseEdge : PadModel::DualSense; break;
    case SDL_GAMEPAD_TYPE_PS4: d.model = PadModel::DualShock4; break;
    case SDL_GAMEPAD_TYPE_XBOXONE:
        d.model = d.name.find("Series") != std::string::npos ? PadModel::XboxSeries : PadModel::XboxOne;
        break;
    case SDL_GAMEPAD_TYPE_XBOX360: d.model = PadModel::XboxOne; break;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: d.model = PadModel::SwitchPro; break;
    default: d.model = PadModel::Other; break;
    }

    d.caps.gyro = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO);
    d.caps.accel = SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    d.caps.touchpad = SDL_GetNumGamepadTouchpads(pad) > 0;
    const SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
    d.caps.rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    d.caps.trigger_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
    d.caps.rgb_led = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
    d.caps.adaptive_triggers = d.IsDualSense() && DualSenseTriggers::Available();
    return d;
}
} // namespace

SdlPadSource::SdlPadSource(GameInput& input) : input_(input) {}

SdlPadSource::~SdlPadSource()
{
    input_.SetFeedbackSink(nullptr);
    for (Pad& p : pads_)
        if (p.handle)
            SDL_CloseGamepad(p.handle);
}

void SdlPadSource::Init()
{
    input_.SetFeedbackSink(this);
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count))
    {
        for (int i = 0; i < count; ++i)
            Open(ids[i]);
        SDL_free(ids);
    }
}

SdlPadSource::Pad* SdlPadSource::Find(SDL_JoystickID id)
{
    for (Pad& p : pads_)
        if (p.id == id)
            return &p;
    return nullptr;
}

SdlPadSource::Pad* SdlPadSource::ActivePad()
{
    return Find(input_.devices().ActiveInstance());
}

void SdlPadSource::Open(SDL_JoystickID id)
{
    if (Find(id))
        return;
    SDL_Gamepad* handle = SDL_OpenGamepad(id);
    if (!handle)
    {
        SDL_Log("GameInput: SDL_OpenGamepad(%u) failed: %s", unsigned(id), SDL_GetError());
        return;
    }
    // Gyro and accelerometer stay on from the start so drift calibration is ready when gyro aim is
    // switched on.
    if (SDL_GamepadHasSensor(handle, SDL_SENSOR_GYRO))
        SDL_SetGamepadSensorEnabled(handle, SDL_SENSOR_GYRO, true);
    if (SDL_GamepadHasSensor(handle, SDL_SENSOR_ACCEL))
        SDL_SetGamepadSensorEnabled(handle, SDL_SENSOR_ACCEL, true);

    Pad pad;
    pad.handle = handle;
    pad.id = id;
    pad.has_touchpad = SDL_GetNumGamepadTouchpads(handle) > 0;
    pads_.push_back(pad);

    const DeviceIdentity dev = input_.OnDeviceConnected(DescribeDevice(handle, id));
    SDL_Log("GameInput: connected '%s' model=%s guid=%s key=%s gyro=%d accel=%d touchpad=%d rumble=%d trigger_rumble=%d "
            "led=%d adaptive_triggers=%d",
        dev.name.c_str(), PadModelName(dev.model), dev.guid.c_str(), dev.stable_key.c_str(), dev.caps.gyro,
        dev.caps.accel, dev.caps.touchpad, dev.caps.rumble, dev.caps.trigger_rumble, dev.caps.rgb_led,
        dev.caps.adaptive_triggers);
}

void SdlPadSource::Close(SDL_JoystickID id)
{
    auto it = std::find_if(pads_.begin(), pads_.end(), [&](const Pad& p) { return p.id == id; });
    if (it == pads_.end())
        return;
    SDL_Log("GameInput: disconnected instance %u", unsigned(id));
    input_.OnDeviceDisconnected(id);
    SDL_CloseGamepad(it->handle);
    pads_.erase(it);
}

bool SdlPadSource::HandleEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_EVENT_GAMEPAD_ADDED:
        Open(event.gdevice.which);
        return true;
    case SDL_EVENT_GAMEPAD_REMOVED:
        Close(event.gdevice.which);
        return true;
    default:
        return false;
    }
}

bool SdlPadSource::HasInput(const Pad& pad) const
{
    for (const ButtonMap& b : kButtons)
        if (SDL_GetGamepadButton(pad.handle, b.sdl))
            return true;
    return std::fabs(Axis(pad.handle, SDL_GAMEPAD_AXIS_LEFTX)) > 0.5f || std::fabs(Axis(pad.handle, SDL_GAMEPAD_AXIS_LEFTY)) > 0.5f ||
        std::fabs(Axis(pad.handle, SDL_GAMEPAD_AXIS_RIGHTX)) > 0.5f || std::fabs(Axis(pad.handle, SDL_GAMEPAD_AXIS_RIGHTY)) > 0.5f ||
        Axis(pad.handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.3f || Axis(pad.handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.3f;
}

void SdlPadSource::Sample(const Pad& pad, PadSnapshot& s) const
{
    SDL_Gamepad* g = pad.handle;
    s.connected = true;
    for (const ButtonMap& b : kButtons)
        s.Set(b.pad, SDL_GetGamepadButton(g, b.sdl));

    s.left = {Axis(g, SDL_GAMEPAD_AXIS_LEFTX), Axis(g, SDL_GAMEPAD_AXIS_LEFTY)};
    s.right = {Axis(g, SDL_GAMEPAD_AXIS_RIGHTX), Axis(g, SDL_GAMEPAD_AXIS_RIGHTY)};
    s.l2 = std::max(0.0f, Axis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    s.r2 = std::max(0.0f, Axis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));

    if (pad.has_touchpad)
    {
        s.has_touchpad = true;
        for (int i = 0; i < 2; ++i)
        {
            bool down = false;
            float x = 0, y = 0, pressure = 0;
            if (SDL_GetGamepadTouchpadFinger(g, 0, i, &down, &x, &y, &pressure))
            {
                s.finger[i].down = down;
                s.finger[i].pos = {x, y};
                s.finger[i].pressure = pressure;
            }
        }
    }

    float data[3];
    if (SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) && SDL_GetGamepadSensorData(g, SDL_SENSOR_GYRO, data, 3))
    {
        s.has_gyro = true;
        std::memcpy(s.gyro, data, sizeof(data));
    }
    if (SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL) && SDL_GetGamepadSensorData(g, SDL_SENSOR_ACCEL, data, 3))
    {
        s.has_accel = true;
        std::memcpy(s.accel, data, sizeof(data));
    }

    int percent = -1;
    const SDL_PowerState state = SDL_GetGamepadPowerInfo(g, &percent);
    s.battery = percent >= 0 ? float(percent) / 100.0f : -1.0f;
    s.charging = state == SDL_POWERSTATE_CHARGING;
}

PadSnapshot SdlPadSource::Poll()
{
    PadSnapshot snapshot;

    // Whichever pad was touched last drives the game.
    for (const Pad& p : pads_)
    {
        if (p.id != input_.devices().ActiveInstance() && HasInput(p))
        {
            input_.devices().NoteActivity(p.id);
            break;
        }
    }

    if (Pad* pad = ActivePad())
    {
        Sample(*pad, snapshot);
        if (HasInput(*pad))
            last_input_ns_ = SDL_GetTicksNS();
    }
    return snapshot;
}

FrameOutput SdlPadSource::Update(float dt)
{
    return input_.Update(dt, Poll());
}

float SdlPadSource::SecondsSinceInput() const
{
    return last_input_ns_ == 0 ? 1e9f : float(double(SDL_GetTicksNS() - last_input_ns_) / 1e9);
}

std::vector<std::string> SdlPadSource::DescribeCapabilities() const
{
    std::vector<std::string> lines;
    for (const DeviceIdentity& d : input_.devices().Devices())
    {
        char buffer[320];
        std::snprintf(buffer, sizeof(buffer),
            "%s '%s' %s gyro=%d accel=%d touchpad=%d rumble=%d trig_rumble=%d led=%d adaptive=%d",
            d.instance_id == input_.devices().ActiveInstance() ? "*" : " ", d.name.c_str(), PadModelName(d.model),
            d.caps.gyro, d.caps.accel, d.caps.touchpad, d.caps.rumble, d.caps.trigger_rumble, d.caps.rgb_led,
            d.caps.adaptive_triggers);
        lines.emplace_back(buffer);
    }
    for (std::string& line : DualSenseTriggers::Describe())
        lines.push_back(std::move(line));
    return lines;
}

// ---- IFeedbackSink ------------------------------------------------------------------------------

void SdlPadSource::SetRumble(const MotorState& m)
{
    Pad* pad = ActivePad();
    if (!pad)
        return;
    const DeviceIdentity* dev = input_.devices().Active();
    // Duration is a safety net: the driver refreshes it while an effect plays.
    constexpr Uint32 kDurationMs = 250;
    if (!SDL_RumbleGamepad(pad->handle, Motor(m.low), Motor(m.high), kDurationMs))
        return;
    if (dev && dev->caps.trigger_rumble)
        SDL_RumbleGamepadTriggers(pad->handle, Motor(m.left_trigger), Motor(m.right_trigger), kDurationMs);
}

void SdlPadSource::SetLed(Rgb color)
{
    Pad* pad = ActivePad();
    const DeviceIdentity* dev = input_.devices().Active();
    if (pad && dev && dev->caps.rgb_led)
        SDL_SetGamepadLED(pad->handle, color.r, color.g, color.b);
}

void SdlPadSource::SetTriggers(const TriggerPair& triggers)
{
    const DeviceIdentity* dev = input_.devices().Active();
    if (!dev || !dev->IsDualSense())
        return;
    DualSenseTriggers::Apply(active_player_index_, triggers);
}

void SdlPadSource::Silence()
{
    if (Pad* pad = ActivePad())
    {
        SDL_RumbleGamepad(pad->handle, 0, 0, 0);
        SDL_RumbleGamepadTriggers(pad->handle, 0, 0, 0);
    }
    DualSenseTriggers::Off(active_player_index_);
}
} // namespace gameinput
