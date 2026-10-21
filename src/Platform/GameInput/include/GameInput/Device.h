// Controller identity and connect/disconnect bookkeeping.
//
// SDL hands out a new instance ID every time a pad connects, and iOS reports no serial number for
// GameController devices, so neither can key a saved profile. DeviceRegistry derives a stable key
// from what is stable (model, GUID, serial when there is one) and disambiguates two identical pads
// by connection order.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "GameInput/PadKeys.h"

namespace gameinput
{
enum class PadModel : uint8_t
{
    Unknown,
    DualSense,
    DualSenseEdge,
    DualShock4,
    XboxOne,
    XboxSeries,
    SwitchPro,
    Other,
};

struct DeviceCaps
{
    bool gyro = false;
    bool accel = false;
    bool touchpad = false;
    bool rumble = false;
    bool trigger_rumble = false;
    bool rgb_led = false;
    bool adaptive_triggers = false; // filled in by the native GameController probe
};

struct DeviceIdentity
{
    uint32_t instance_id = 0; // SDL joystick instance ID; changes on every reconnect
    std::string name;
    std::string guid; // 32 hex characters
    std::string serial;
    uint16_t vendor = 0;
    uint16_t product = 0;
    PadModel model = PadModel::Unknown;
    ControllerFamily family = ControllerFamily::Generic;
    DeviceCaps caps;
    std::string stable_key; // e.g. "dualsense:0300...:serial" or "...#2" for a second identical pad

    bool IsDualSense() const { return model == PadModel::DualSense || model == PadModel::DualSenseEdge; }
};

const char* PadModelName(PadModel model);
ControllerFamily FamilyOf(PadModel model);

struct DeviceEvent
{
    enum class Type { Connected, Disconnected, ActiveChanged };
    Type type = Type::Connected;
    DeviceIdentity device;
};

// Tracks every connected pad and which one currently drives the game: the first to connect, then
// whichever last produced input. Disconnecting the active pad promotes another connected one.
class DeviceRegistry
{
public:
    // Registers a pad (or refreshes it if the instance is already known) and fills in its stable key.
    DeviceIdentity Connect(DeviceIdentity id);
    bool Disconnect(uint32_t instance_id);
    void NoteActivity(uint32_t instance_id);

    const DeviceIdentity* Active() const;
    uint32_t ActiveInstance() const { return active_; }
    const std::vector<DeviceIdentity>& Devices() const { return devices_; }
    bool AnyConnected() const { return !devices_.empty(); }

    std::vector<DeviceEvent> TakeEvents();

private:
    std::string MakeStableKey(const DeviceIdentity& id) const;

    std::vector<DeviceIdentity> devices_;
    uint32_t active_ = 0;
    std::vector<DeviceEvent> events_;
};
} // namespace gameinput
