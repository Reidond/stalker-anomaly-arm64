// Frame pacing for the frame rate cap (gfx_fps; docs/ios-port/SETTINGS.md, "Frame rate cap"). Pure, so tools/graphics_profile_test can
// replay it against a simulated display.
//
// The cap is a time between frames. A frame is shown at a display refresh, so what the player sees is even only when that time is a whole
// number of refreshes: 30 fps on a 60 Hz panel (every second refresh), 40 fps on a 120 Hz one (every third). 40 fps on 60 Hz is 1.5
// refreshes: frames alternate 16.7 and 33.3 ms. FrameCaps() in GraphicsProfile offers only the even rates.
#pragma once

#include <cstdint>

namespace AppleShell
{
struct FramePacer
{
    struct Step
    {
        bool run;          // false: skip this tick (the display link calls again at the next refresh)
        uint64_t sleep_ns; // when running: sleep this long first (0: now)
    };

    uint64_t next_ns = 0; // when the next frame may start; 0: no schedule yet

    void Reset() { next_ns = 0; }

    // Called at the top of each loop iteration at `now_ns` with a cap of `interval_ns` between frames. `skip_over_ns` > 0 is for a loop
    // that is called at every display refresh (the iPad's display link): a tick that is more than that early is skipped instead of
    // slept through, because the next refresh is closer to the target than this one. 0: always sleep (the Mac's loop runs free).
    Step Begin(uint64_t now_ns, uint64_t interval_ns, uint64_t skip_over_ns)
    {
        if (next_ns == 0 || now_ns > next_ns + interval_ns)
            next_ns = now_ns; // no schedule, or behind by more than a frame: do not catch up
        uint64_t sleep_ns = 0;
        if (now_ns + 1000000ull < next_ns)
        {
            const uint64_t wait_ns = next_ns - now_ns;
            if (skip_over_ns && wait_ns > skip_over_ns)
                return {false, 0};
            sleep_ns = wait_ns;
        }
        next_ns += interval_ns;
        return {true, sleep_ns};
    }
};
} // namespace AppleShell
