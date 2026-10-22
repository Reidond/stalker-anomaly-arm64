// Checks the device graphics profile tables (GraphicsProfile.h) and prints them for the documentation.
#include <Platform/Apple/GraphicsProfile.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

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
    Check(G::FrameCaps() == std::vector<int>({30, 60, 120}), "120 Hz display: 30, 60, 120");
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

    options.unlocked = true;
    G::Init(Ipad(8, "Apple M3"), options);
    Check(G::SetFrameCap(-1) && G::EffectiveFrameCap() == 0, "unlimited allowed when unlocked");
}

void TestUpscalerEntries()
{
    G::Options options;
    G::Init(Ipad(8, "Apple M3"), options);
    const std::vector<G::Setting> without = G::Settings(NoCommands());
    const std::vector<G::Setting> with = G::Settings(WithUpscaler());
    Check(Value(without, "vid_mode") && !strcmp(Value(without, "vid_mode"), "1280x960"), "without MetalFX the render size is vid_mode (Quality 1280x960)");
    Check(!Value(without, "r__render_scale"), "without MetalFX no render scale is set");
    Check(Value(with, "r__render_scale") && !strcmp(Value(with, "r__render_scale"), "0.5"), "with MetalFX the iPad Quality scale is the floor, 0.5");
    Check(Value(with, "vid_mode") && !strcmp(Value(with, "vid_mode"), "2732x2048"), "with MetalFX the back buffer is the native size");
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
    TestUpscalerEntries();
    if (g_failures)
    {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("graphics_profile_test: ok\n");
    return 0;
}
