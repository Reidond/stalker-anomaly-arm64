#include "GameInput/Device.h"

#include <algorithm>

namespace gameinput
{
const char* PadModelName(PadModel model)
{
    switch (model)
    {
    case PadModel::DualSense: return "dualsense";
    case PadModel::DualSenseEdge: return "dualsense-edge";
    case PadModel::DualShock4: return "dualshock4";
    case PadModel::XboxOne: return "xbox-one";
    case PadModel::XboxSeries: return "xbox-series";
    case PadModel::SwitchPro: return "switch-pro";
    case PadModel::Other: return "other";
    case PadModel::Unknown: break;
    }
    return "unknown";
}

ControllerFamily FamilyOf(PadModel model)
{
    switch (model)
    {
    case PadModel::DualSense:
    case PadModel::DualSenseEdge:
    case PadModel::DualShock4: return ControllerFamily::PlayStation;
    case PadModel::XboxOne:
    case PadModel::XboxSeries: return ControllerFamily::Xbox;
    case PadModel::SwitchPro: return ControllerFamily::Nintendo;
    default: return ControllerFamily::Generic;
    }
}

std::string DeviceRegistry::MakeStableKey(const DeviceIdentity& id) const
{
    std::string base = PadModelName(id.model);
    base += ':';
    base += id.guid.empty() ? id.name : id.guid;
    if (!id.serial.empty())
        return base + ':' + id.serial; // a serial is already unique per pad

    // Identical pads without serials: lowest free ordinal among those currently connected.
    for (int ordinal = 1;; ++ordinal)
    {
        const std::string candidate = ordinal == 1 ? base : base + '#' + std::to_string(ordinal);
        const bool taken = std::any_of(devices_.begin(), devices_.end(), [&](const DeviceIdentity& d) {
            return d.stable_key == candidate && d.instance_id != id.instance_id;
        });
        if (!taken)
            return candidate;
    }
}

DeviceIdentity DeviceRegistry::Connect(DeviceIdentity id)
{
    id.family = FamilyOf(id.model);
    id.stable_key = MakeStableKey(id);

    auto it = std::find_if(devices_.begin(), devices_.end(),
        [&](const DeviceIdentity& d) { return d.instance_id == id.instance_id; });
    if (it == devices_.end())
    {
        devices_.push_back(std::move(id));
        it = devices_.end() - 1;
    }
    else
    {
        *it = std::move(id);
    }

    events_.push_back({DeviceEvent::Type::Connected, *it});
    if (active_ == 0)
    {
        active_ = it->instance_id;
        events_.push_back({DeviceEvent::Type::ActiveChanged, *it});
    }
    return *it;
}

bool DeviceRegistry::Disconnect(uint32_t instance_id)
{
    auto it = std::find_if(devices_.begin(), devices_.end(),
        [&](const DeviceIdentity& d) { return d.instance_id == instance_id; });
    if (it == devices_.end())
        return false;

    events_.push_back({DeviceEvent::Type::Disconnected, *it});
    devices_.erase(it);

    if (active_ == instance_id)
    {
        active_ = 0;
        if (!devices_.empty())
        {
            active_ = devices_.front().instance_id;
            events_.push_back({DeviceEvent::Type::ActiveChanged, devices_.front()});
        }
    }
    return true;
}

void DeviceRegistry::NoteActivity(uint32_t instance_id)
{
    if (instance_id == active_)
        return;
    auto it = std::find_if(devices_.begin(), devices_.end(),
        [&](const DeviceIdentity& d) { return d.instance_id == instance_id; });
    if (it == devices_.end())
        return;
    active_ = instance_id;
    events_.push_back({DeviceEvent::Type::ActiveChanged, *it});
}

const DeviceIdentity* DeviceRegistry::Active() const
{
    for (const DeviceIdentity& d : devices_)
        if (d.instance_id == active_)
            return &d;
    return nullptr;
}

std::vector<DeviceEvent> DeviceRegistry::TakeEvents()
{
    std::vector<DeviceEvent> out;
    out.swap(events_);
    return out;
}
} // namespace gameinput
