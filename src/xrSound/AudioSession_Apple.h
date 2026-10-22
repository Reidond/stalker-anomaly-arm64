#pragma once
// Apple audio-session glue for xrSound (implemented in AudioSession_Apple.mm).
//
// Plain C++ on purpose: the Objective-C++ side includes only Apple headers, because Objective-C's BOOL and the
// Win32 shim's BOOL cannot share a translation unit.

namespace xr_sound_apple
{
enum class SessionEvent
{
	InterruptionBegan, // phone call, Siri, alarm, another app took the session: the audio unit is stopped
	InterruptionEnded, // the session may be reactivated
	AppActive,         // retry point if the session could not be reactivated in the background
	OutputChanged,     // an output device appeared or went away (headphones, USB/Bluetooth, AirPlay, ...)
	MediaServicesReset // mediaserverd restarted: every audio object is invalid
};

typedef void (*SessionEventFn)(SessionEvent event, void* user);

// iOS: AVAudioSession category (Ambient by default, ANOMALY_AUDIO_CATEGORY=ambient|solo|playback overrides it), then
// activates it. macOS has no audio session: does nothing and returns true.
bool configure_session();

// Reactivates the session after an interruption. Returns false while the system still refuses (app in background).
bool activate_session();

// iOS: the outputs of the current route, e.g. "Speaker" or "AirPods Max (BluetoothA2DPOutput)". "" elsewhere.
const char* output_route();

// Calls fn on the thread that posted the notification (main thread for all of the above).
void observe_session(SessionEventFn fn, void* user);
void unobserve_session();
} // namespace xr_sound_apple
