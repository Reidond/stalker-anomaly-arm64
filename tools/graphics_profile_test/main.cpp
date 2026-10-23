// Checks the device graphics profile tables (GraphicsProfile.h) and prints them for the documentation.
#include <Platform/Apple/FramePacer.h>
#include <Platform/Apple/GraphicsProfile.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace G = AppleShell::Graphics;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        ++g_failures;
        printf("FAIL: %s\n", what);
    }
}

G::Hardware Ipad(double gib, const char* chip, float hz = 60.0f)
{
    G::Hardware hw;
    hw.family = "ipad";
    hw.machine = "iPad15,5";
    hw.chip = chip;
    hw.memory_bytes = uint64_t(gib * 1024.0 * 1024.0 * 1024.0);
    hw.native_width = 2732;
    hw.native_height = 2048;
    hw.max_refresh_hz = hz;
    return hw;
}

G::Hardware Mac(const char* chip, int generation, int cores, double gib, float hz = 60.0f)
{
    G::Hardware hw;
    hw.family = "mac";
    hw.machine = "Mac14,9";
    hw.chip = chip;
    hw.chip_generation = generation;
    hw.gpu_cores = cores;
    hw.memory_bytes = uint64_t(gib * 1024.0 * 1024.0 * 1024.0);
    hw.native_width = 3024;
    hw.native_height = 1964;
    hw.max_refresh_hz = hz;
    return hw;
}

struct NoCommands : G::CommandProbe
{
    bool HasCommand(const char*) const override { return false; }
};
struct WithUpscaler : G::CommandProbe
{
    bool HasCommand(const char* name) const override { return !strcmp(name, "r__render_scale") || !strcmp(name, "r__upscaler"); }
};

const char* Value(const std::vector<G::Setting>& settings, const char* key)
{
    for (const G::Setting& s : settings)
        if (s.key == key)
            return s.value.c_str();
    return nullptr;
}

void TestSelection()
{
    Check(G::SelectProfileId(Ipad(8, "Apple M3")) == "ipad-8gb", "iPad Air M3 8 GB -> ipad-8gb");
    Check(G::SelectProfileId(Ipad(7.44, "Apple M3")) == "ipad-8gb", "iPad Air M3 reports 7.44 GiB -> ipad-8gb");
    Check(G::SelectProfileId(Ipad(5.6, "Apple A16")) == "ipad-6gb", "6 GB iPad reports 5.6 GiB -> ipad-6gb");
    Check(G::SelectProfileId(Ipad(16, "Apple M4", 120.0f)) == "ipad-16gb", "iPad Pro 16 GB -> ipad-16gb");
    Check(G::SelectProfileId(Ipad(12, "Apple M4")) == "ipad-16gb", "iPad 12 GB -> ipad-16gb");
    Check(G::SelectProfileId(Ipad(6, "Apple A16")) == "ipad-6gb", "iPad A16 6 GB -> ipad-6gb");
    Check(G::SelectProfileId(Ipad(8, "Apple A17 Pro")) == "ipad-6gb", "iPad mini A17 Pro 8 GB -> ipad-6gb (A-series GPU)");
    Check(G::SelectProfileId(Mac("Apple M2 Pro", 2, 19, 32, 120.0f)) == "mac-pro", "M2 Pro 19 -> mac-pro");
    Check(G::SelectProfileId(Mac("Apple M2 Pro", 2, 16, 16)) == "mac-pro", "M2 Pro 16 -> mac-pro");
    Check(G::SelectProfileId(Mac("Apple M3", 3, 10, 16)) == "mac-base", "M3 10 -> mac-base");
    Check(G::SelectProfileId(Mac("Apple M1", 1, 8, 8)) == "mac-base-8gb", "M1 8 GB -> mac-base-8gb");
    Check(G::SelectProfileId(Mac("Apple M2 Max", 2, 38, 64)) == "mac-max", "M2 Max 38 -> mac-max");
    Check(G::SelectProfileId(Mac("Apple M1 Max", 1, 32, 32)) == "mac-pro", "M1 Max 32 (22 effective cores) stays in the measured class");
    Check(G::SelectProfileId(Mac("Apple M2 Ultra", 2, 76, 192)) == "mac-ultra", "M2 Ultra 76 -> mac-ultra");
    Check(G::SelectProfileId(Mac("Apple M4 Pro", 4, 0, 24)) == "mac-pro", "unknown core count falls back to the chip's name");
}

// Features the Metal renderer lacks must be off in every profile and both modes, and no profile may name a Screen Space Shaders variable.
void TestUnsupportedOff()
{
    const char* const off[][2] = {
        {"r4_enable_tessellation", "off"}, {"r3_volumetric_smoke", "off"}, {"r4_hdr10_on", "0"}, {"r__heatvision", "0"},
        {"r2_smaa", "off"}, {"r3_msaa", "st_opt_off"}};
    for (const G::ProfileInfo& info : G::AllProfiles())
    {
        for (G::Mode mode : {G::Mode::Quality, G::Mode::Performance})
        {
            const std::vector<G::Setting> settings = G::ResolveForDocs(info.id, mode);
            Check(!settings.empty(), (info.id + " resolves").c_str());
            std::set<std::string> seen;
            for (const G::Setting& s : settings)
            {
                Check(!s.value.empty(), (info.id + ": empty value for " + s.key).c_str());
                Check(s.key.rfind("ssfx_", 0) != 0, (info.id + ": names " + s.key).c_str());
                Check(seen.insert(s.key).second, (info.id + ": duplicate " + s.key).c_str());
            }
            for (const auto& o : off)
            {
                const char* v = Value(settings, o[0]);
                Check(v && !strcmp(v, o[1]), (info.id + ": " + o[0] + " must be " + o[1]).c_str());
            }
        }
    }
}

void TestLocks()
{
    G::Options options;
    G::Init(Ipad(8, "Apple M3"), options);
    Check(G::Enabled() && G::LocksActive(), "locks active on the iPad");
    for (const char* locked : {"r2_sun_quality", "R2_SUN_QUALITY", "texture_lod", "vid_mode", "rs_screenmode", "r3_msaa", "r4_enable_tessellation",
             "ssfx_ssr", "ssfx_anything_at_all", "r__render_scale", "r__upscaler", "r4_hdr10_exposure", "_preset", "r__framelimit", "r__detail_density"})
        Check(G::IsLocked(locked), (std::string(locked) + " is locked").c_str());
    for (const char* open : {"fov", "hud_fov", "r2_mblur_enabled", "gfx_mode", "gfx_fps", "hud_draw", "snd_volume_eff", "r__exposure", "vid_restart",
             "r__nightvision", "r2_dof", "r2_drops_control", "bind", "start", "cfg_save", "renderer"})
        Check(!G::IsLocked(open), (std::string(open) + " is not locked").c_str());

    G::Init(Mac("Apple M2 Pro", 2, 19, 32, 120.0f), options);
    Check(G::IsLocked("r2_sun_quality"), "Mac: sun quality locked");
    Check(!G::IsLocked("vid_mode") && !G::IsLocked("rs_screenmode"), "Mac: window and display mode stay the player's");

    options.unlocked = true;
    G::Init(Mac("Apple M2 Pro", 2, 19, 32), options);
    Check(G::Enabled() && !G::LocksActive() && !G::IsLocked("r2_sun_quality"), "--unlocked-graphics: profile on, locks off");

    options.unlocked = false;
    options.profile = "none";
    G::Init(Mac("Apple M2 Pro", 2, 19, 32), options);
    Check(!G::Enabled() && !G::LocksActive() && !G::IsLocked("r2_sun_quality"), "--graphics-profile none: nothing");
    Check(G::EffectiveFrameCap() == 0, "no profile: no frame cap");

    options.profile = "ipad";
    G::Init(Mac("Apple M2 Pro", 2, 19, 32), options);
    Check(G::ProfileId() == "ipad-8gb" && G::IsLocked("vid_mode"), "forcing the iPad profile on a Mac keeps the iPad's rules");
    options.profile = "no-such-profile";
    G::Init(Mac("Apple M2 Pro", 2, 19, 32), options);
    Check(G::ProfileId() == "mac-pro", "an unknown profile id falls back to the hardware's");
}

void TestModesAndCaps()
{
    G::Options options;
    G::Init(Mac("Apple M2 Pro", 2, 19, 32, 120.0f), options);
    Check(G::FrameCaps() == std::vector<int>({30, 40, 60, 120}), "120 Hz display: 30, 40, 60, 120");
    G::SetMode(G::Mode::Quality);
    Check(G::EffectiveFrameCap() == 60, "Quality defaults to 60");
    G::SetMode(G::Mode::Performance);
    Check(G::EffectiveFrameCap() == 120, "Performance defaults to 120 on a 120 Hz display");
    Check(G::SetFrameCap(30) && G::EffectiveFrameCap() == 30, "a chosen cap wins");
    Check(!G::SetFrameCap(144), "144 is not offered");
    Check(!G::SetFrameCap(-1), "unlimited is refused while locked");
    G::SetFrameCap(0);

    G::Init(Ipad(8, "Apple M3"), options);
    Check(G::FrameCaps() == std::vector<int>({30, 60}), "60 Hz display: 30, 60");
    G::SetMode(G::Mode::Performance);
    Check(G::EffectiveFrameCap() == 60, "Performance on 60 Hz stays 60");
    G::SetMode(G::Mode::Quality);
    Check(G::EffectiveFrameCap() == 30, "iPad Quality asks for 40, a 60 Hz panel shows 30 evenly (40 would alternate 16.7 and 33.3 ms)");
    Check(!G::SetFrameCap(40), "40 is not on offer on a 60 Hz panel");

    G::Init(Ipad(16, "Apple M4", 120.0f), options);
    Check(G::FrameCaps() == std::vector<int>({30, 40, 60, 120}), "120 Hz iPad: 30, 40, 60, 120");
    G::SetMode(G::Mode::Quality);
    Check(G::EffectiveFrameCap() == 40, "iPad Quality is 40 on a 120 Hz panel (every third refresh)");
    G::SetMode(G::Mode::Performance);
    Check(G::EffectiveFrameCap() == 60, "iPad Performance is a locked 60, also on a 120 Hz panel");
    G::SetMode(G::Mode::Quality);
    G::Init(Ipad(8, "Apple M3", 120.0f), options);
    Check(G::EffectiveFrameCap() == 40, "8 GB iPad Quality on a 120 Hz panel is 40");

    options.unlocked = true;
    G::Init(Ipad(8, "Apple M3"), options);
    Check(G::SetFrameCap(-1) && G::EffectiveFrameCap() == 0, "unlimited allowed when unlocked");
    Check(G::SetFrameCap(40) && G::EffectiveFrameCap() == 40, "unlocked: any rate, 40 on a 60 Hz panel (measured as the uneven case)");
    Check(!G::SetFrameCap(5) && !G::SetFrameCap(500), "unlocked: 10 to 240 only");
}

// Replays the pacer against a display link that ticks at every refresh of a `hz` panel, with a little jitter, over 20 s. A frame is shown
// at the tick that runs it. Returns the share of the frame intervals by length in refreshes, and the fps.
struct PacerRun
{
    std::map<int, double> share; // refreshes between two frames -> share of the frames
    double fps;
};

PacerRun ReplayPacer(double hz, int cap, double jitter_ms)
{
    AppleShell::FramePacer pacer;
    srand(1);
    std::vector<double> starts;
    const double period = 1000.0 / hz;
    for (int tick = 1; tick < int(20000.0 / period); ++tick)
    {
        const double jitter = jitter_ms * (rand() / double(RAND_MAX) * 2.0 - 1.0);
        const uint64_t now_ns = uint64_t((tick * period + jitter + 1000.0) * 1e6); // +1 s: the pacer reads 0 as "no schedule"
        const AppleShell::FramePacer::Step step = pacer.Begin(now_ns, 1000000000ull / cap, 5000000ull);
        if (step.run)
            starts.push_back(double(now_ns) * 1e-6);
    }
    PacerRun run;
    run.fps = double(starts.size()) / 20.0;
    for (size_t i = 1; i < starts.size(); ++i)
        run.share[int((starts[i] - starts[i - 1]) / period + 0.5)] += 1.0 / double(starts.size() - 1);
    return run;
}

void TestPacer()
{
    // 30 fps on 60 Hz: every second refresh; 60 on 60 and 120 on 120: every refresh; 60 on 120: every second; 40 on 120: every third
    for (const auto& c : {std::make_tuple(60.0, 30, 2), std::make_tuple(60.0, 60, 1), std::make_tuple(120.0, 60, 2),
             std::make_tuple(120.0, 40, 3), std::make_tuple(120.0, 30, 4), std::make_tuple(120.0, 120, 1)})
    {
        const PacerRun run = ReplayPacer(std::get<0>(c), std::get<1>(c), 0.3);
        const std::string name = std::to_string(int(std::get<0>(c))) + " Hz panel, cap " + std::to_string(std::get<1>(c));
        Check(run.share.count(std::get<2>(c)) && run.share.at(std::get<2>(c)) > 0.99, (name + ": every frame lasts the same whole number of refreshes").c_str());
        Check(run.fps > std::get<1>(c) - 0.5 && run.fps < std::get<1>(c) + 0.5, (name + ": the rate is the cap").c_str());
    }
    // 40 on 60 Hz: 1.5 refreshes, so frames alternate 17 and 33 ms (25 ms on average, none at 25)
    const PacerRun uneven = ReplayPacer(60.0, 40, 0.3);
    Check(uneven.share.count(1) && uneven.share.count(2) && uneven.share.at(1) > 0.45 && uneven.share.at(2) > 0.45 && uneven.share.size() == 2,
        "40 fps on a 60 Hz panel alternates 1 and 2 refreshes (17 and 33 ms), which is why it is not offered there");
    Check(uneven.fps > 39.5 && uneven.fps < 40.5, "40 fps on a 60 Hz panel still averages 40");
}

void TestUpscalerEntries()
{
    G::Options options;
    G::Init(Ipad(8, "Apple M3"), options);
    const std::vector<G::Setting> without = G::Settings(NoCommands());
    const std::vector<G::Setting> with = G::Settings(WithUpscaler());
    Check(Value(without, "vid_mode") && !strcmp(Value(without, "vid_mode"), "1152x864"), "without MetalFX the render size is vid_mode (Quality 1152x864)");
    Check(!Value(without, "r__render_scale"), "without MetalFX no render scale is set");
    Check(Value(with, "r__render_scale") && !strcmp(Value(with, "r__render_scale"), "0.6"), "with a render scale the iPad Quality scale is 0.6");
    Check(Value(with, "r__upscaler") && !strcmp(Value(with, "r__upscaler"), "bilinear") && Value(with, "r__upscale_sharpness"), "the iPad uses bilinear with sharpening");
    Check(Value(with, "vid_mode") && !strcmp(Value(with, "vid_mode"), "2732x2048"), "with a render scale the output is the panel's native size");
}

struct ClampProbe : G::CommandProbe
{
    float lo;
    explicit ClampProbe(float lo_) : lo(lo_) {}
    bool HasCommand(const char* name) const override { return !strcmp(name, "r__render_scale") || !strcmp(name, "r__upscaler") || !strcmp(name, "r__upscale_sharpness"); }
    bool Bounds(const char* name, float& l, float& h) const override { if (strcmp(name, "r__render_scale")) return false; l = lo; h = 1.0f; return true; }
};

void TestScaleClamp()
{
    G::Options options;
    G::Init(Ipad(8, "Apple M3"), options);
    const std::vector<G::Setting> floor05 = G::Settings(ClampProbe(0.5f));
    const std::vector<G::Setting> floor035 = G::Settings(ClampProbe(0.35f));
    Check(Value(floor05, "r__render_scale") && !strcmp(Value(floor05, "r__render_scale"), "0.6"), "Quality's 0.6 is inside the renderer's old range");
    Check(Value(floor035, "r__render_scale") && !strcmp(Value(floor035, "r__render_scale"), "0.6"), "with the minimum lowered the profile's own scale is used");
    G::SetMode(G::Mode::Performance);
    Check(Value(G::Settings(ClampProbe(0.5f)), "r__render_scale") && !strcmp(Value(G::Settings(ClampProbe(0.5f)), "r__render_scale"), "0.5"),
        "a render scale under the renderer's minimum is clamped to it (Performance 0.42 -> 0.5)");
    Check(Value(G::Settings(ClampProbe(0.35f)), "r__render_scale") && !strcmp(Value(G::Settings(ClampProbe(0.35f)), "r__render_scale"), "0.42"), "Performance scale 0.42");
    G::SetMode(G::Mode::Quality);
}

void Dump()
{
    for (const G::ProfileInfo& info : G::AllProfiles())
    {
        printf("\n### %s: %s%s\n\n%s\n\n", info.id.c_str(), info.title.c_str(), info.measured ? "" : " (not measured)", info.notes.c_str());
        const std::vector<G::Setting> q = G::ResolveForDocs(info.id, G::Mode::Quality);
        const std::vector<G::Setting> p = G::ResolveForDocs(info.id, G::Mode::Performance);
        printf("| Setting | Quality | Performance |\n|---|---|---|\n");
        for (const G::Setting& s : q)
        {
            const char* other = Value(p, s.key.c_str());
            printf("| `%s` | `%s` | `%s` |\n", s.key.c_str(), s.value.c_str(), other ? other : "");
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "--dump"))
    {
        Dump();
        return 0;
    }
    TestSelection();
    TestUnsupportedOff();
    TestLocks();
    TestModesAndCaps();
    TestPacer();
    TestUpscalerEntries();
    TestScaleClamp();
    if (g_failures)
    {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("graphics_profile_test: ok\n");
    return 0;
}
