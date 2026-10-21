#include "GameInput/DualSenseTriggers.h"

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <cstdio>

namespace gameinput
{
namespace
{
float Clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }

GCDualSenseGamepad* FindDualSense(int player_index)
{
    GCDualSenseGamepad* first = nil;
    for (GCController* controller in [GCController controllers])
    {
        GCExtendedGamepad* gamepad = controller.extendedGamepad;
        if (![gamepad isKindOfClass:[GCDualSenseGamepad class]])
            continue;
        if (player_index >= 0 && int(controller.playerIndex) == player_index)
            return (GCDualSenseGamepad*)gamepad;
        if (!first)
            first = (GCDualSenseGamepad*)gamepad;
    }
    return first;
}

void ApplyOne(GCDualSenseAdaptiveTrigger* trigger, const TriggerEffect& e)
{
    switch (e.type)
    {
    case TriggerEffect::Type::Off:
        [trigger setModeOff];
        break;
    case TriggerEffect::Type::Feedback:
        [trigger setModeFeedbackWithStartPosition:Clamp01(e.start) resistiveStrength:Clamp01(e.strength)];
        break;
    case TriggerEffect::Type::Weapon:
    {
        // The framework wants start < end within 0..1.
        const float start = std::min(Clamp01(e.start), 0.94f);
        const float end = std::min(1.0f, std::max(Clamp01(e.end), start + 0.05f));
        [trigger setModeWeaponWithStartPosition:start endPosition:end resistiveStrength:Clamp01(e.strength)];
        break;
    }
    case TriggerEffect::Type::Vibration:
        [trigger setModeVibrationWithStartPosition:Clamp01(e.start)
                                         amplitude:Clamp01(e.amplitude)
                                         frequency:Clamp01(e.frequency)];
        break;
    case TriggerEffect::Type::Slope:
    {
        const float start = std::min(Clamp01(e.start), 0.94f);
        const float end = std::min(1.0f, std::max(Clamp01(e.end), start + 0.05f));
        [trigger setModeSlopeFeedbackWithStartPosition:start
                                           endPosition:end
                                         startStrength:Clamp01(e.strength)
                                           endStrength:Clamp01(e.end_strength)];
        break;
    }
    }
}
} // namespace

int DualSenseTriggers::ConnectedCount()
{
    int count = 0;
    @autoreleasepool
    {
        for (GCController* controller in [GCController controllers])
            if ([controller.extendedGamepad isKindOfClass:[GCDualSenseGamepad class]])
                ++count;
    }
    return count;
}

bool DualSenseTriggers::Apply(int player_index, const TriggerPair& pair)
{
    @autoreleasepool
    {
        GCDualSenseGamepad* pad = FindDualSense(player_index);
        if (!pad)
            return false;
        @try
        {
            ApplyOne(pad.leftTrigger, pair.left);
            ApplyOne(pad.rightTrigger, pair.right);
        }
        @catch (NSException* exception)
        {
            NSLog(@"DualSense trigger effect rejected: %@", exception.reason);
            return false;
        }
        return true;
    }
}

bool DualSenseTriggers::Off(int player_index)
{
    return Apply(player_index, TriggerPair{});
}

TriggerReadback DualSenseTriggers::Read(int player_index)
{
    TriggerReadback r;
    @autoreleasepool
    {
        GCDualSenseGamepad* pad = FindDualSense(player_index);
        if (!pad)
            return r;
        r.valid = true;
        r.left_mode = int(pad.leftTrigger.mode);
        r.right_mode = int(pad.rightTrigger.mode);
        r.left_status = int(pad.leftTrigger.status);
        r.right_status = int(pad.rightTrigger.status);
        r.left_arm = pad.leftTrigger.armPosition;
        r.right_arm = pad.rightTrigger.armPosition;
    }
    return r;
}

std::vector<std::string> DualSenseTriggers::Describe()
{
    std::vector<std::string> lines;
    @autoreleasepool
    {
        NSArray<GCController*>* controllers = [GCController controllers];
        if (controllers.count == 0)
            lines.push_back("GameController: no controllers");
        for (GCController* controller in controllers)
        {
            char buffer[256];
            const bool ds = [controller.extendedGamepad isKindOfClass:[GCDualSenseGamepad class]];
            std::snprintf(buffer, sizeof(buffer), "GameController: '%s' category='%s' dualsense=%s battery=%.0f%% player=%d",
                controller.vendorName ? controller.vendorName.UTF8String : "?",
                controller.productCategory ? controller.productCategory.UTF8String : "?", ds ? "yes" : "no",
                controller.battery ? double(controller.battery.batteryLevel) * 100.0 : -1.0, int(controller.playerIndex));
            lines.emplace_back(buffer);
        }
    }
    return lines;
}
} // namespace gameinput
