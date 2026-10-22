// App Nap and automatic termination would slow an unattended run's frame pacing and timers down while the app is in the
// background; an NSProcessInfo activity keeps the process running at full speed.
#import <Foundation/Foundation.h>

#include "Unattended.h"

namespace anomaly_app
{
void BeginUnattendedActivity()
{
    static id activity = nil;
    if (activity)
        return;
    NSActivityOptions options = NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical;
    activity = [[NSProcessInfo processInfo] beginActivityWithOptions:options reason:@"Anomaly unattended run"];
    [[NSProcessInfo processInfo] disableAutomaticTermination:@"Anomaly unattended run"];
}
} // namespace anomaly_app
