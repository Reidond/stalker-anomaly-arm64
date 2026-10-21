#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_CoreA.h"
#ifdef XR_PLATFORM_APPLE
#include "SoundRender_Apple.h"
#endif

XRSOUND_API xr_token* snd_devices_token = NULL;
XRSOUND_API xr_string snd_device_name;

void CSound_manager_interface::_create(int stage)
{
	if (stage == 0)
	{
#ifdef XR_PLATFORM_APPLE
		xr_sound_apple::prepare();
#endif
		SoundRenderA = xr_new<CSoundRender_CoreA>();
		SoundRender = SoundRenderA;
		Sound = SoundRender;

		if (strstr(Core.Params, "-nosound"))
		{
			SoundRender->bPresent = FALSE;
			return;
		}
		else
			SoundRender->bPresent = TRUE;
	}

	if (!SoundRender->bPresent) return;
	Sound->_initialize(stage);
}

void CSound_manager_interface::_destroy()
{
	Sound->_clear();
	xr_delete(SoundRender);
	Sound = 0;
}
