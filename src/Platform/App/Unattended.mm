// App Nap and automatic termination would slow an unattended run's frame pacing and timers down while the app is in the
// background; an NSProcessInfo activity keeps the process running at full speed.
#import <Foundation/Foundation.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <AVFoundation/AVFoundation.h>
#endif

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
#if TARGET_OS_OSX
    [[NSProcessInfo processInfo] disableAutomaticTermination:@"Anomaly unattended run"]; // macOS only; unattended mode is not used on iOS
#endif
}

void SetBackgroundActivationPolicy()
{
#if TARGET_OS_OSX
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
#endif
}

void PostAudioInterruption(bool began)
{
#if !TARGET_OS_OSX
    NSDictionary* info = @{
        AVAudioSessionInterruptionTypeKey : @(began ? AVAudioSessionInterruptionTypeBegan : AVAudioSessionInterruptionTypeEnded),
        AVAudioSessionInterruptionOptionKey : @(began ? 0 : AVAudioSessionInterruptionOptionShouldResume),
    };
    [[NSNotificationCenter defaultCenter] postNotificationName:AVAudioSessionInterruptionNotification
                                                        object:[AVAudioSession sharedInstance]
                                                      userInfo:info];
#else
    (void)began;
#endif
}
} // namespace anomaly_app
