// Item 5 (stretch): OpenAL Soft (the repo's pinned 1.25.2) through its SDL3 backend to Web Audio,
// with the program on a worker (PROXY_TO_PTHREAD): a looping sound circling the listener with HRTF
// on and an EFX EAX reverb on an auxiliary send. Reports what opened and whether the mixer runs.
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx-presets.h>
#include <AL/efx.h>

#include <SDL3/SDL.h>
#include <emscripten.h>
#include <emscripten/threading.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
ALCdevice* g_device = nullptr;
ALuint g_source = 0;
double g_start = 0, g_last_report = 0;
ALint g_last_offset = 0;
long long g_samples_played = 0;

void Report(const std::string& json)
{
    char* copy = strdup(json.c_str());
    MAIN_THREAD_ASYNC_EM_ASM({
        var t = UTF8ToString($0);
        _free($0);
        if (globalThis.audioReport) globalThis.audioReport(JSON.parse(t)); else console.log(t);
    }, copy);
}

std::string Quote(const char* s)
{
    std::string out = "\"";
    for (const char* p = s ? s : ""; *p; ++p)
        out += (*p == '"' || *p == '\\') ? std::string("\\") + *p : std::string(1, *p);
    return out + "\"";
}

void Tick()
{
    const double now = emscripten_get_now();
    const float a = float((now - g_start) / 1000.0 * 0.8);
    alSource3f(g_source, AL_POSITION, 2.0f * std::sin(a), 0.0f, -2.0f * std::cos(a));
    ALint offset = 0, state = 0;
    alGetSourcei(g_source, AL_SAMPLE_OFFSET, &offset);
    alGetSourcei(g_source, AL_SOURCE_STATE, &state);
    if (offset < g_last_offset)
        g_samples_played += 48000; // the 1 s buffer wrapped
    g_samples_played += offset - g_last_offset;
    g_last_offset = offset;
    if (now - g_last_report > 1000)
    {
        g_last_report = now;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "{\"type\":\"progress\",\"seconds\":%.2f,\"samples_played\":%lld,\"playing\":%s,\"al_error\":%d}",
                      (now - g_start) / 1000.0, g_samples_played, state == AL_PLAYING ? "true" : "false", alGetError());
        Report(buf);
    }
}

// A 1 s mono loop: decaying 220 Hz and 330 Hz plucks, so reverb and panning are audible.
std::vector<int16_t> MakeLoop()
{
    std::vector<int16_t> pcm(48000);
    for (size_t i = 0; i < pcm.size(); ++i)
    {
        const double t = double(i) / 48000.0;
        const double t2 = std::fmod(t, 0.5);
        const double f = t < 0.5 ? 220.0 : 330.0;
        pcm[i] = int16_t(12000.0 * std::exp(-t2 * 9.0) * std::sin(2.0 * M_PI * f * t2));
    }
    return pcm;
}
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void audio_command(int) {}

int main()
{
    const bool worker = !emscripten_is_main_browser_thread();
    const ALCint attrs[] = {ALC_HRTF_SOFT, ALC_TRUE, ALC_FREQUENCY, 48000, 0};
    g_device = alcOpenDevice(nullptr);
    if (!g_device)
    {
        Report("{\"type\":\"open\",\"ok\":false,\"error\":\"alcOpenDevice(NULL) failed\"}");
        return 0;
    }
    ALCcontext* ctx = alcCreateContext(g_device, attrs);
    alcMakeContextCurrent(ctx);

    ALCint hrtf = 0, freq = 0, sends = 0;
    alcGetIntegerv(g_device, ALC_HRTF_STATUS_SOFT, 1, &hrtf);
    alcGetIntegerv(g_device, ALC_FREQUENCY, 1, &freq);
    alcGetIntegerv(g_device, ALC_MAX_AUXILIARY_SENDS, 1, &sends);
    const char* hrtf_name = alcGetString(g_device, ALC_HRTF_SPECIFIER_SOFT);
    const bool efx = alcIsExtensionPresent(g_device, "ALC_EXT_EFX");

    const std::vector<int16_t> pcm = MakeLoop();
    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, AL_FORMAT_MONO16, pcm.data(), ALsizei(pcm.size() * sizeof(int16_t)), 48000);
    alGenSources(1, &g_source);
    alSourcei(g_source, AL_BUFFER, ALint(buffer));
    alSourcei(g_source, AL_LOOPING, AL_TRUE);

    ALuint effect = 0, slot = 0;
    ALenum efx_error = AL_NO_ERROR;
    if (efx)
    {
        const EFXEAXREVERBPROPERTIES r = EFX_REVERB_PRESET_HANGAR;
        alGenEffects(1, &effect);
        alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
        alEffectf(effect, AL_EAXREVERB_DENSITY, r.flDensity);
        alEffectf(effect, AL_EAXREVERB_DIFFUSION, r.flDiffusion);
        alEffectf(effect, AL_EAXREVERB_GAIN, r.flGain);
        alEffectf(effect, AL_EAXREVERB_GAINHF, r.flGainHF);
        alEffectf(effect, AL_EAXREVERB_DECAY_TIME, r.flDecayTime);
        alEffectf(effect, AL_EAXREVERB_REFLECTIONS_GAIN, r.flReflectionsGain);
        alEffectf(effect, AL_EAXREVERB_LATE_REVERB_GAIN, r.flLateReverbGain);
        alGenAuxiliaryEffectSlots(1, &slot);
        alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, ALint(effect));
        alSource3i(g_source, AL_AUXILIARY_SEND_FILTER, ALint(slot), 0, AL_FILTER_NULL);
        efx_error = alGetError();
    }
    alSourcePlay(g_source);

    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "{\"type\":\"open\",\"ok\":true,\"worker\":%s,\"device\":%s,\"hrtf_status\":%d,\"hrtf\":%s,\"frequency\":%d,"
                  "\"efx\":%s,\"aux_sends\":%d,\"reverb_error\":%d,\"sdl_audio_driver\":%s}",
                  worker ? "true" : "false", Quote(alcGetString(g_device, ALC_ALL_DEVICES_SPECIFIER)).c_str(), hrtf,
                  Quote(hrtf_name).c_str(), freq, efx ? "true" : "false", sends, int(efx_error),
                  Quote(SDL_GetCurrentAudioDriver()).c_str());
    Report(buf);
    g_start = emscripten_get_now();
    emscripten_set_main_loop(Tick, 0, false);
    return 0;
}
