// DualSense adaptive triggers through Apple's GameController framework.
//
// SDL3 reaches Apple controllers through the same GCController objects but exposes no adaptive
// trigger API for them, so this talks to GCDualSenseGamepad directly (Objective-C++ in
// DualSenseTriggers.mm). It only sets trigger modes; button, stick and sensor input stay with SDL.
#pragma once

#include <string>
#include <vector>

#include "GameInput/Feedback.h"

namespace gameinput
{
// Raw state read back from GCDualSenseAdaptiveTrigger, for verifying effects on the device. The
// framework updates `status` asynchronously, a few frames after a mode is set.
struct TriggerReadback
{
    bool valid = false;
    int left_mode = -1;
    int right_mode = -1;
    int left_status = -1; // GCDualSenseAdaptiveTriggerStatus, -1 = unknown
    int right_status = -1;
    float left_arm = 0.0f; // 0..1
    float right_arm = 0.0f;
};

class DualSenseTriggers
{
public:
    // Number of GameController-visible DualSense pads connected right now.
    static int ConnectedCount();
    static bool Available() { return ConnectedCount() > 0; }

    // Applies both trigger effects. `player_index` picks among several DualSense pads (-1 or no
    // match = the first). Returns false if there is no DualSense to talk to.
    static bool Apply(int player_index, const TriggerPair& pair);
    static bool Off(int player_index);

    static TriggerReadback Read(int player_index);
    // One line per GameController device: name, product category, DualSense or not, battery.
    static std::vector<std::string> Describe();
};
} // namespace gameinput
