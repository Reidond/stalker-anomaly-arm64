#pragma once
// Counters of what the sound renderer hands to OpenAL, for the systems check (docs/ios-port/SYSTEMS-CHECK.md, tools/sweep/check.sh).
// Printed to the engine log every 10 s by CSoundRender_Core::update as
//   snd: emitters started N, sources played N (rewound N, restarted after an underrun N), stopped N, rendering N of N, AL errors N
// An "AL error" is whatever alGetError reports right after a play or queue call (release builds do not check AL calls otherwise).
#include <atomic>

struct SoundCounters
{
	std::atomic<u32> emitters_started{0}; // CSoundRender_Emitter::start: a sound event reached the renderer
	std::atomic<u32> sources_played{0};   // alSourcePlay after a target took an emitter (CSoundRender_TargetA::render)
	std::atomic<u32> sources_rewound{0};  // looped emitters started again
	std::atomic<u32> underruns{0};        // a source found stopped with its queue unprocessed and played again
	std::atomic<u32> targets_stopped{0};
	std::atomic<u32> al_errors{0};
};

inline SoundCounters g_sound_counters;
