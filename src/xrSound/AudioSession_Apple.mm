// AVAudioSession setup and notifications for xrSound on iOS/iPadOS. OpenAL Soft's CoreAudio backend opens a RemoteIO
// unit but never touches the audio session, so the category, activation and interruption handling live here.
// On macOS there is no audio session and everything is a no-op.
#include "AudioSession_Apple.h"

#include <TargetConditionals.h>
#import <Foundation/Foundation.h>
#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace xr_sound_apple
{
#if TARGET_OS_IPHONE
namespace
{
std::atomic<SessionEventFn> g_fn{nullptr};
std::atomic<void*> g_user{nullptr};
NSMutableArray* g_tokens = nil;

void emit(SessionEvent e)
{
	if (SessionEventFn fn = g_fn.load())
		fn(e, g_user.load());
}

NSString* category_from_env()
{
	const char* v = getenv("ANOMALY_AUDIO_CATEGORY");
	if (v && !strcmp(v, "playback")) return AVAudioSessionCategoryPlayback; // keeps playing with the screen locked
	if (v && !strcmp(v, "solo")) return AVAudioSessionCategorySoloAmbient;  // silences other apps' audio
	// Ambient mixes with other apps' audio (music, podcasts) and yields to calls and Siri: the right default for a game
	// that is not the user's primary audio source.
	return AVAudioSessionCategoryAmbient;
}
} // namespace

bool configure_session()
{
	AVAudioSession* session = [AVAudioSession sharedInstance];
	NSError* error = nil;
	if (![session setCategory:category_from_env() mode:AVAudioSessionModeDefault options:0 error:&error])
	{
		NSLog(@"xrSound: AVAudioSession setCategory failed: %@", error);
		return false;
	}
	return activate_session();
}

bool activate_session()
{
	NSError* error = nil;
	if (![[AVAudioSession sharedInstance] setActive:YES error:&error])
	{
		NSLog(@"xrSound: AVAudioSession setActive failed: %@", error);
		return false;
	}
	return true;
}

void observe_session(SessionEventFn fn, void* user)
{
	unobserve_session();
	g_user.store(user);
	g_fn.store(fn);
	NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
	g_tokens = [NSMutableArray array];
	auto add = ^(NSString* name, void (^block)(NSNotification*)) {
		[g_tokens addObject:[center addObserverForName:name object:nil queue:nil usingBlock:block]];
	};
	add(AVAudioSessionInterruptionNotification, ^(NSNotification* n) {
		NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
		emit(type == AVAudioSessionInterruptionTypeBegan ? SessionEvent::InterruptionBegan
		                                                 : SessionEvent::InterruptionEnded);
	});
	add(AVAudioSessionRouteChangeNotification, ^(NSNotification* n) {
		NSUInteger reason = [n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
		if (reason == AVAudioSessionRouteChangeReasonNewDeviceAvailable ||
		    reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable)
			emit(SessionEvent::OutputChanged);
	});
	add(AVAudioSessionMediaServicesWereResetNotification, ^(NSNotification*) { emit(SessionEvent::MediaServicesReset); });
	// by name: UIKit is not needed for this one notification
	add(@"UIApplicationDidBecomeActiveNotification", ^(NSNotification*) { emit(SessionEvent::AppActive); });
}

void unobserve_session()
{
	g_fn.store(nullptr);
	NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
	for (id token in g_tokens)
		[center removeObserver:token];
	g_tokens = nil;
}
#else
bool configure_session() { return true; }
bool activate_session() { return true; }
void observe_session(SessionEventFn, void*) {}
void unobserve_session() {}
#endif
} // namespace xr_sound_apple
