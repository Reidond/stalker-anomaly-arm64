// Stand-in for xrSound while that module is not enabled (-DANOMALY_BUILD_XRSOUND=ON swaps in the real one): the globals
// the console commands register and the manager entry points. There is no sound; `Sound` stays null.
#include "../../../src/xrEngine/stdafx.h"

XRSOUND_API u32 psSoundModel = 0;
XRSOUND_API float psSoundVEffects = 1.f;
XRSOUND_API float psSoundVFactor = 1.f;
XRSOUND_API float psSoundVMusic = 1.f;
XRSOUND_API float psSoundVMusicFactor = 1.f;
XRSOUND_API float psSoundRolloff = 1.f;
XRSOUND_API float psSoundMaxDistanceMultiplier = 1.f;
XRSOUND_API float psSoundMaxAIDistanceMultiplier = 1.f;
XRSOUND_API float psSoundOcclusionScale = 0.5f;
XRSOUND_API Flags32 psSoundFlags = {0};
XRSOUND_API int psSoundTargets = 32;
XRSOUND_API float snd_efx_environment_change_time = 1.f;
XRSOUND_API float psSpeedOfSound = 343.f;
XRSOUND_API int psSoundCacheSizeMB = 32;
XRSOUND_API xr_token* snd_devices_token = nullptr;
XRSOUND_API xr_string snd_device_name = "";

BOOL reverb_overwrite = FALSE;
float psReverbDensity = 1.f, psReverbDiffusion = 1.f, psReverbGain = 0.32f, psReverbGainHF = 0.89f, psReverbGainLF = 1.f;
float psReverbDecayTime = 1.49f, psReverbDecayHFRatio = 0.83f, psReverbDecayLFRatio = 1.f;
float psReverbReflectionsGain = 0.05f, psReverbReflectionsDelay = 0.007f, psReverbReflectionsPan = 0.f;
float psReverbLateReverbGain = 1.26f, psReverbLateReverbDelay = 0.011f, psReverbLateReflectionsPan = 0.f;
float psReverbEchoTime = 0.25f, psReverbEchoDepth = 0.f, psReverbModulationTime = 0.25f, psReverbModulationDepth = 0.f;
float psReverbAirAbsorptionGainHF = 0.994f, psReverbHFReference = 5000.f, psReverbLFReference = 250.f;
float psReverbRoomRolloffFactor = 0.f;
BOOL psReverbDecayHFLimit = TRUE;

namespace soundSmoothingParams
{
float pitchVariationPower = 1.f;
float distanceBasedDelayPower = 1.f;
float distanceBasedDelayMinDistance = 1.f;
float power = 1.f;
int steps = 1;
float alpha = 1.f;
float getAlpha() { return alpha; }
float getTimeDeltaSmoothing() { return 1.f; }
float getSmoothedValue(float, float value, float) { return value; }
} // namespace soundSmoothingParams

CSound_manager_interface* Sound = nullptr;

void CSound_manager_interface::_create(int stage)
{
	Msg("! xrengine_smoke stub: xrSound is not linked (stage %d), there is no sound", stage);
}

void CSound_manager_interface::_destroy() {}
