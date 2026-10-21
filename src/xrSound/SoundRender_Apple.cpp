// Apple replacements for the Windows-only parts of xrSound: the WASAPI notification client (CNotificationClient) and
// the places where Windows picks up OpenAL Soft's settings and the audio endpoint. See docs/ios-port/SOUND.md.
#include "stdafx.h"
#pragma hdrstop

#include "NotificationClient.h"
#include "SoundRender_CoreA.h"
#include "AudioSession_Apple.h"
#include "SoundRender_Apple.h"

#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>

namespace xr_sound_apple
{
namespace
{
const ALCenum k_system_events[] = {ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT, ALC_EVENT_TYPE_DEVICE_ADDED_SOFT,
                                   ALC_EVENT_TYPE_DEVICE_REMOVED_SOFT};

std::atomic<bool> g_interrupted{false};      // the device was paused for an interruption
std::atomic<bool> g_session_pending{false};  // the session could not be (re)activated yet; retried when it can be

// OpenAL Soft's own device events (ALC_SOFT_system_events). They arrive on an OpenAL thread and only raise the same
// flags the WASAPI client raised; CSoundRender_Core::update() acts on them on the engine thread.
void ALC_APIENTRY on_alc_event(ALCenum event, ALCenum device_type, ALCdevice*, ALCsizei, const ALCchar*, void*) noexcept
{
	CSoundRender_CoreA* core = SoundRenderA;
	if (!core || device_type != ALC_PLAYBACK_DEVICE_SOFT)
		return;
	if (event == ALC_EVENT_TYPE_DEFAULT_DEVICE_CHANGED_SOFT)
		core->bPendingDefaultDeviceSwitch = TRUE;
	else
		core->bPendingDeviceListRefresh = TRUE;
}

void resume_after_interruption(CSoundRender_CoreA* core)
{
	if (!g_interrupted && !g_session_pending)
		return;
	if (!activate_session())
		return; // still in the background or in a call; the next InterruptionEnded / AppActive tries again
	g_session_pending = false;
	if (g_interrupted.exchange(false))
	{
		if (ALCdevice* device = core->al_device())
			alcDeviceResumeSOFT(device);
	}
	Msg("SOUND: audio session reactivated");
}

void on_session_event(SessionEvent event, void*)
{
	CSoundRender_CoreA* core = SoundRenderA;
	if (!core)
		return;
	switch (event)
	{
	case SessionEvent::InterruptionBegan:
		// the system stops the RemoteIO unit under OpenAL Soft; pause the device so it can be restarted cleanly
		g_interrupted = true;
		if (ALCdevice* device = core->al_device())
			alcDevicePauseSOFT(device);
		Msg("SOUND: audio session interrupted, device paused");
		break;
	case SessionEvent::InterruptionEnded:
	case SessionEvent::AppActive:
		resume_after_interruption(core);
		break;
	case SessionEvent::OutputChanged:
		core->bPendingDeviceListRefresh = TRUE;
		core->bPendingDefaultDeviceSwitch = TRUE;
		break;
	case SessionEvent::MediaServicesReset:
		// every audio object is invalid, whichever device was selected: reopen it (not only when it is the default)
		g_interrupted = false;
		if (!configure_session())
			g_session_pending = true;
		core->bPendingDeviceReopen = TRUE;
		break;
	}
}

// Anomaly keeps its OpenAL Soft settings (HRTF mode, headphone stereo mode, periods, source count) in bin\alsoft.ini
// and its HRTF data sets (default-44100.mhr, default-48000.mhr) in the game root. On Windows OpenAL Soft finds the file
// by itself and `hrtf-paths = .` is relative to the working directory, which is the game root. Elsewhere ALSOFT_CONF
// points OpenAL Soft at a copy written to the app data folder, with hrtf-paths made absolute.
void use_anomaly_alsoft_conf()
{
	if (getenv("ALSOFT_CONF"))
		return; // an explicit override wins
	string_path candidate[3];
	int n = 0;
	if (FS.path_exist("$fs_root$"))
		FS.update_path(candidate[n++], "$fs_root$", "bin\\alsoft.ini");
	xr_strconcat(candidate[n++], Core.ApplicationPath, "alsoft.ini");
	xr_strconcat(candidate[n++], Core.ApplicationPath, "bin\\alsoft.ini");
	for (int i = 0; i < n; ++i)
	{
		struct stat st;
		if (xr_stat(candidate[i], &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		std::string conf = xr_native_path(candidate[i]);
		if (FS.path_exist("$app_data_root$") && FS.path_exist("$fs_root$"))
		{
			string_path root_v, out_v;
			FS.update_path(root_v, "$fs_root$", "");
			FS.update_path(out_v, "$app_data_root$", "alsoft.ini");
			const std::string root = xr_native_path(root_v);
			const std::string out_path = xr_native_path(out_v);
			std::ifstream in(conf);
			std::ofstream out(out_path, std::ios::trunc);
			std::string line;
			while (out && std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.compare(0, 10, "hrtf-paths") == 0)
					line = "hrtf-paths = " + root;
				out << line << '\n';
			}
			if (out.good())
				conf = out_path;
		}
		setenv("ALSOFT_CONF", conf.c_str(), 0);
		Msg("SOUND: OpenAL Soft settings from %s", conf.c_str());
		return;
	}
	Msg("SOUND: no alsoft.ini found, OpenAL Soft defaults");
}
} // namespace

void prepare()
{
	// before the first alc* call, which is when OpenAL Soft reads its configuration
	use_anomaly_alsoft_conf();
	// A refusal (a call in progress, launched in the background) is retried on InterruptionEnded / AppActive.
	if (!configure_session())
		g_session_pending = true;
}
} // namespace xr_sound_apple

CNotificationClient::CNotificationClient() : m_bSystemEvents(false)
{
	if (alcIsExtensionPresent(nullptr, "ALC_SOFT_system_events"))
	{
		alcEventCallbackSOFT(xr_sound_apple::on_alc_event, nullptr);
		m_bSystemEvents = alcEventControlSOFT(ALCsizei(sizeof(xr_sound_apple::k_system_events) / sizeof(ALCenum)),
		                                      xr_sound_apple::k_system_events, ALC_TRUE) == ALC_TRUE;
	}
	Msg("SOUND: OpenAL system events %s", m_bSystemEvents ? "enabled" : "not available");
	xr_sound_apple::observe_session(xr_sound_apple::on_session_event, nullptr);
}

CNotificationClient::~CNotificationClient()
{
	xr_sound_apple::unobserve_session();
	if (m_bSystemEvents)
	{
		alcEventControlSOFT(ALCsizei(sizeof(xr_sound_apple::k_system_events) / sizeof(ALCenum)),
		                    xr_sound_apple::k_system_events, ALC_FALSE);
		alcEventCallbackSOFT(nullptr, nullptr);
	}
}
