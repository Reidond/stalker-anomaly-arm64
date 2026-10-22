#pragma once

namespace anomaly_app
{
// Opts the process out of App Nap and automatic termination for its lifetime (Unattended.mm).
void BeginUnattendedActivity();
// macOS: NSApplicationActivationPolicyAccessory, so ordering a window front does not make the app active. SDL's own
// MAC_BACKGROUND_APP hint then leaves the policy alone. No-op elsewhere.
void SetBackgroundActivationPolicy();
} // namespace anomaly_app
