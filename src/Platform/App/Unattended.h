#pragma once

namespace anomaly_app
{
// Opts the process out of App Nap and automatic termination for its lifetime (Unattended.mm).
void BeginUnattendedActivity();
// macOS: NSApplicationActivationPolicyAccessory, so ordering a window front does not make the app active. SDL's own
// MAC_BACKGROUND_APP hint then leaves the policy alone. No-op elsewhere.
void SetBackgroundActivationPolicy();
// Script `audiointerrupt began|ended` (iOS; a no-op on macOS): posts the AVAudioSession interruption notification a phone call, Siri
// or an alarm would, so the sound code's pause and resume path runs without one. The system does not really take the session away.
void PostAudioInterruption(bool began);
} // namespace anomaly_app
