#pragma once
// Apple-only entry point of xrSound (SoundRender_Apple.cpp).
namespace xr_sound_apple
{
// Call before the sound manager is created: points OpenAL Soft at Anomaly's alsoft.ini and sets up the iOS audio
// session. Both have to happen before the first alc* call.
void prepare();
} // namespace xr_sound_apple
