// Device graphics profiles (GraphicsProfile.h, docs/ios-port/SETTINGS.md). The tables are the whole truth about what each device
// class runs: to add a device, add a profile (or point SelectProfileId at an existing one) and a row in SETTINGS.md.
#include "GraphicsProfile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace AppleShell::Graphics
{
namespace
{
// ---- tables ---------------------------------------------------------------------------------------------------------------------

// When an entry applies. The MetalFX session adds `r__render_scale` (the 3D scene renders at a fraction of the output size, the
// UI at native) and `r__upscaler`. Until the renderer has them the render size is the back buffer size (`vid_mode`), as on the
// first iPad preset; once it has them the back buffer is the display's native size and the scale does the saving.
enum class When : uint8_t
{
    Always,
    WithUpscaler,    // the engine has `r__render_scale`
    WithoutUpscaler, // it does not
};

struct Entry
{
    const char* key;
    const char* value; // "native" in vid_mode is the display's native size
    When when;
    Entry(const char* k, const char* v, When w = When::Always) : key(k), value(v), when(w) {}
};

struct ProfileDef
{
    const char* id;
    const char* title;
    const char* parent; // layers: the parent's entries come first, an entry with the same key replaces them
    bool measured;
    const char* notes;
    const char* command_line;
    std::vector<Entry> common;      // both modes
    std::vector<Entry> quality;     // Quality mode, after `common`
    std::vector<Entry> performance; // Performance mode, after `common`
    int fps_quality;                // default frame cap in each mode (0: the display's rate)
    int fps_performance;
};

// Settings the player may change on top of a profile. They are written once per UserDefaultsVersion (so a player's change is kept).
const Entry kUserDefaults[] = {
    {"r2_mblur_enabled", "off"}, // motion blur: off until the player turns it on
};
constexpr int kUserDefaultsVersion = 1;

// Commands every profile locks without giving them a value: the menu's old graphics options. They keep the engine's default (which
// is what the stock game starts with), a stale value in user.ltx is ignored, and nothing can change them while the locks are on.
const char* const kLockedWithoutValue[] = {
    "_preset", "vid_monitor", "rs_fullscreen", "rs_borderless",
    "r2_slight_fade", "r2_gloss_factor", "r2_gloss_min", "r2_sunshafts_value", "r2_sunshafts_min", "r2_sun_far", "r2_sun_near",
    "r2_sun_focus", "r2_sun_tsm", "r2_sun_static", "r2_tonemap", "r2_zfill", "r2_ls_bloom_fast", "r2_parallax_h", "r2_ssao_blur",
    "r2_ssao_opt_data", "r2_ssao_half_data", "r2_minmax_sm", "r3_minmax_sm", "r3_msaa_alphatest", "r3_msaa_opt", "r3_msaa_hybrid",
    "r3_use_dx10_1", "r3_dynamic_wet_surfaces_near", "r3_dynamic_wet_surfaces_far",
    "r__tf_mipbias", "r__optimize_static_geom", "r__optimize_dynamic_geom", "r__optimize_shadow_geom", "r__detail_height",
    "r__enable_grass_shadow", "r__actor_shadow", "r__geometry_lod_pow", "r__no_ram_textures", "r__ssa_glod_start",
    "r__ssa_glod_end", "r2_ssa_lod_a", "r2_ssa_lod_b", "r__wallmark_ttl", "g_wallmark_range_static", "g_wallmark_range_skeleton",
    "particle_update_mod", "step_particle_distance", "r2_dof_enable", "ai_use_torch_dynamic_lights", "r2_mt", "r2_wait_sleep",
    "r2_qsync", "rs_hom_depth_draw", "r2_shadow_cascede_zcul", "r2_shadow_cascede_old", "r4_wireframe", "rs_skeleton_update",
    "shader_param_1", "shader_param_2", "shader_param_3", "shader_param_4", "shader_param_5", "shader_param_6", "shader_param_7",
    "shader_param_8", "pfx_volumetric_mode",
};

// Prefixes of commands that belong to features this port cannot run: the Screen Space Shaders pack (its 34 shader files are not in
// the game data), HDR10 (not exercised), tessellation helpers. Everything under them keeps its default.
const char* const kLockedPrefixes[] = {"ssfx_", "r4_hdr10_"};

// Every Apple profile. Unsupported features are off here, so no profile can turn them on (SETTINGS.md, "Unsupported features").
const std::vector<Entry> kApple = {
    {"rs_v_sync", "on"},
    {"r__framelimit", "0"}, // the engine's own limiter busy-waits and is compiled out on iOS: the app paces frames (gfx_fps)
    {"r4_enable_tessellation", "off"}, // no hull/domain stages on Metal
    {"r3_volumetric_smoke", "off"},    // 3D fluid: needs geometry shaders and layered 3D targets
    {"r2_volumetric_lights", "off"},
    {"r4_hdr10_on", "0"},
    {"r__heatvision", "0"}, // its shader is not in the game data
    {"r2_smaa", "off"},
    {"r2_aa", "off"},
    {"r2_gi", "off"},
    {"r3_msaa", "st_opt_off"},
    {"r__supersample", "1"},
    {"r2_detail_bump", "on"},
    {"r2_soft_water", "on"},
    {"r2_soft_particles", "on"},
    {"r2_sun", "on"},
    {"r2_sun_details", "on"},
    {"r2_water_reflections", "on"},
    {"r__lens_flares", "on"},
    {"r2_sunshafts_mode", "screen_space"},
    {"r2_sunshafts_quality", "st_opt_low"},
};

const std::vector<Entry> kIpad = {
    {"rs_screenmode", "borderless"}, // the view covers the screen; the size comes from vid_mode
    {"vid_mode", "1366x1024", When::WithoutUpscaler},
    {"vid_mode", "native", When::WithUpscaler},
    {"snd_cache_size", "32"},
    {"r__tf_aniso", "4"},
};

const std::vector<Entry> kMac = {
    // The window and the display mode are the player's: a desktop OS has windows. Everything that costs GPU time is locked.
    {"r__tf_aniso", "8"},
};

// The measured classes. Numbers in SETTINGS.md ("Measurements").
const std::vector<ProfileDef> kProfiles = {
    {"apple", "Apple (shared)", nullptr, false, "Shared by every profile: unsupported features off.", "", kApple, {}, {}, 60, 60},

    // ---- iPad ----
    {"ipad", "iPad (shared)", "apple", false, "Shared by the iPad profiles.", "", kIpad, {}, {}, 60, 60},
    {"ipad-8gb", "iPad, 8 GB (iPad Air M3 measured)", "ipad", true,
        "Measured on the iPad Air 13-inch (M3, 8 GB, 60 Hz). Also the target of other 8 GB iPads with an M-series chip.", "-smap1536",
        {{"texture_lod", "1"}, {"r3_dynamic_wet_surfaces", "off"}, {"r2_ssao_mode", "default"}, {"r__geometry_lod", "1.0"}},
        // Quality: the render size of the first iPad preset (1366x1024 of a 2732x2048 panel)
        {{"r2_sun_quality", "st_opt_low"}, {"r2_ls_squality", "0.5"}, {"r2_ssao", "st_opt_low"}, {"r__detail_density", "0.4"},
         {"r__detail_radius", "35"}, {"rs_vis_distance", "0.8"}, {"r__render_scale", "0.5", When::WithUpscaler}, {"r__upscaler", "metalfx", When::WithUpscaler}},
        // Performance: a smaller render size and no AO, for a frame that never drops under 60
        {{"r2_sun_quality", "st_opt_low"}, {"r2_ls_squality", "0.5"}, {"r2_ssao", "st_opt_off"}, {"r__detail_density", "0.4"},
         {"r__detail_radius", "30"}, {"rs_vis_distance", "0.75"}, {"vid_mode", "1152x864", When::WithoutUpscaler},
         {"r__render_scale", "0.42", When::WithUpscaler}, {"r__upscaler", "metalfx", When::WithUpscaler}},
        60, 60},
    {"ipad-16gb", "iPad, 12 and 16 GB (extrapolated)", "ipad-8gb", false,
        "Not measured. Same GPU class as the 8 GB profile with the memory headroom spent on textures.", "-smap2048",
        {{"texture_lod", "0"}}, {}, {}, 60, 60},
    {"ipad-6gb", "iPad, 6 GB or less, or an A-series chip (extrapolated)", "ipad-8gb", false,
        "Not measured. One step down from the 8 GB profile in textures, render size and distances.", "-smap1024",
        {{"texture_lod", "2"}},
        {{"vid_mode", "1152x864", When::WithoutUpscaler}, {"r__render_scale", "0.42", When::WithUpscaler}, {"r__detail_radius", "25"},
         {"rs_vis_distance", "0.7"}},
        {{"vid_mode", "1024x768", When::WithoutUpscaler}, {"r__render_scale", "0.375", When::WithUpscaler}, {"r2_ssao", "st_opt_off"},
         {"r__detail_radius", "20"}, {"rs_vis_distance", "0.65"}},
        60, 60},

    // ---- Mac ----
    {"mac", "Mac (shared)", "apple", false, "Shared by the Mac profiles.", "", kMac, {}, {}, 60, 60},
    {"mac-pro", "Mac, M-series Pro class (M2 Pro 19-core GPU measured)", "mac", true,
        "Measured on a MacBook Pro 14-inch (M2 Pro, 19 GPU cores, 32 GB, 120 Hz).", "-smap2048",
        {{"texture_lod", "1"}, {"r2_ssao_mode", "default"}, {"r__geometry_lod", "1.0"}, {"r__detail_density", "0.5"}},
        {{"r2_sun_quality", "st_opt_medium"}, {"r2_ls_squality", "1.0"}, {"r3_dynamic_wet_surfaces", "on"}, {"r2_ssao", "st_opt_medium"},
         {"r__detail_radius", "100"}, {"rs_vis_distance", "1.0"}, {"r__render_scale", "0.7", When::WithUpscaler}, {"r__upscaler", "metalfx", When::WithUpscaler}},
        {{"r2_sun_quality", "st_opt_low"}, {"r2_ls_squality", "0.7"}, {"r3_dynamic_wet_surfaces", "off"}, {"r2_ssao", "st_opt_low"},
         {"r__detail_radius", "75"}, {"rs_vis_distance", "0.9"}, {"r__render_scale", "0.5", When::WithUpscaler}, {"r__upscaler", "metalfx", When::WithUpscaler}},
        60, 120},
    {"mac-base", "Mac, M-series base class (extrapolated)", "mac-pro", false,
        "Not measured. One tier below the measured Pro class (about half the GPU cores): its Quality is the Pro's Performance.", "-smap1536",
        {},
        {{"r2_sun_quality", "st_opt_low"}, {"r2_ls_squality", "0.7"}, {"r3_dynamic_wet_surfaces", "off"}, {"r2_ssao", "st_opt_low"},
         {"r__detail_radius", "75"}, {"rs_vis_distance", "0.9"}, {"r__render_scale", "0.6", When::WithUpscaler}},
        {{"r2_sun_quality", "st_opt_low"}, {"r2_ls_squality", "0.5"}, {"r3_dynamic_wet_surfaces", "off"}, {"r2_ssao", "st_opt_off"},
         {"r__detail_radius", "50"}, {"rs_vis_distance", "0.8"}, {"r__render_scale", "0.45", When::WithUpscaler}},
        60, 60},
    {"mac-base-8gb", "Mac, base class with 8 GB (extrapolated)", "mac-base", false,
        "Not measured. As the base class with one texture level less.", "-smap1536", {{"texture_lod", "2"}}, {}, {}, 60, 60},
    {"mac-max", "Mac, M-series Max class (extrapolated)", "mac-pro", false,
        "Not measured. One tier above the measured Pro class: its Performance is the Pro's Quality.", "-smap2048",
        {{"texture_lod", "0"}},
        {{"r2_sun_quality", "st_opt_high"}, {"r2_ssao", "st_opt_high"}, {"r__detail_density", "0.45"}, {"r__detail_radius", "125"},
         {"rs_vis_distance", "1.2"}, {"r__render_scale", "0.8", When::WithUpscaler}},
        {{"r__render_scale", "0.6", When::WithUpscaler}},
        60, 120},
    {"mac-ultra", "Mac, M-series Ultra class (extrapolated)", "mac-max", false,
        "Not measured. As the Max class; the extra GPU cores go to the render scale, not to settings the renderer cannot use.", "-smap2048",
        {}, {{"r__render_scale", "0.9", When::WithUpscaler}}, {{"r__render_scale", "0.7", When::WithUpscaler}}, 60, 120},
};

// ---- state ----------------------------------------------------------------------------------------------------------------------

struct State
{
    bool enabled = false;
    bool unlocked = false;
    Hardware hw;
    std::string id;
    Mode mode = Mode::Quality;
    int fps_choice = 0;
    std::set<std::string> locked_exact;
    std::set<std::string> with_value; // the part of locked_exact the profile gives a value
};

State& S()
{
    static State s;
    return s;
}

const ProfileDef* Find(const std::string& id)
{
    for (const ProfileDef& p : kProfiles)
        if (id == p.id)
            return &p;
    return nullptr;
}

std::string Lower(const char* s)
{
    std::string r(s ? s : "");
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return r;
}

// The chain of profiles from the root to `leaf`.
std::vector<const ProfileDef*> Chain(const ProfileDef* leaf)
{
    std::vector<const ProfileDef*> chain;
    for (const ProfileDef* p = leaf; p; p = p->parent ? Find(p->parent) : nullptr)
    {
        chain.push_back(p);
        if (chain.size() > 8)
            break;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

bool Applies(When when, bool has_upscaler)
{
    return when == When::Always || (when == When::WithUpscaler) == has_upscaler;
}

// Resolves a profile into key/value pairs. `keep_all`: documentation form (upscaler entries kept, tagged).
struct NoProbe : CommandProbe
{
    bool HasCommand(const char*) const override { return false; }
};

std::vector<Setting> Resolve(const ProfileDef* leaf, Mode mode, const CommandProbe* probe, const Hardware& hw, bool keep_all)
{
    const bool has_upscaler = probe && probe->HasCommand("r__render_scale");
    std::vector<Setting> out;
    std::map<std::string, size_t> where;
    auto put = [&](const Entry& e)
    {
        std::string key = e.key;
        std::string value = e.value;
        if (value == "native")
        {
            const int w = hw.native_width > 0 ? hw.native_width : 2732;
            const int h = hw.native_height > 0 ? hw.native_height : 2048;
            value = std::to_string(w) + "x" + std::to_string(h);
        }
        if (keep_all)
        {
            if (e.when == When::WithUpscaler)
                key += " [with MetalFX]";
            else if (e.when == When::WithoutUpscaler)
                key += " [before MetalFX]";
        }
        else if (!Applies(e.when, has_upscaler))
            return;
        auto it = where.find(key);
        if (it == where.end())
        {
            where[key] = out.size();
            out.push_back({key, value});
        }
        else
            out[it->second].value = value;
    };
    for (const ProfileDef* p : Chain(leaf))
    {
        for (const Entry& e : p->common)
            put(e);
        for (const Entry& e : mode == Mode::Quality ? p->quality : p->performance)
            put(e);
    }
    return out;
}

void BuildLockedSet(State& s)
{
    s.locked_exact.clear();
    s.with_value.clear();
    for (const char* c : kLockedWithoutValue)
        s.locked_exact.insert(c);
    if (const ProfileDef* leaf = Find(s.id))
    {
        Hardware hw = s.hw;
        for (Mode m : {Mode::Quality, Mode::Performance})
            for (const Setting& e : Resolve(leaf, m, nullptr, hw, true))
            {
                std::string key = e.key;
                const size_t bracket = key.find(" [");
                if (bracket != std::string::npos)
                    key.resize(bracket);
                s.locked_exact.insert(key);
                s.with_value.insert(key);
            }
    }
    // the Mac keeps the window and the display mode
    if (s.hw.family == "mac")
    {
        s.locked_exact.erase("vid_mode");
        s.locked_exact.erase("rs_screenmode");
    }
    // the player's own choices are never locked
    for (const Entry& e : kUserDefaults)
        s.locked_exact.erase(e.key);
    for (auto it = s.with_value.begin(); it != s.with_value.end();)
        it = s.locked_exact.count(*it) ? std::next(it) : s.with_value.erase(it);
}
} // namespace

const char* ToString(Mode mode)
{
    return mode == Mode::Quality ? "quality" : "performance";
}

bool ParseMode(const char* text, Mode& mode)
{
    const std::string t = Lower(text);
    if (t == "quality")
        mode = Mode::Quality;
    else if (t == "performance")
        mode = Mode::Performance;
    else
        return false;
    return true;
}

std::string SelectProfileId(const Hardware& hw)
{
    const double gib = double(hw.memory_bytes) / (1024.0 * 1024.0 * 1024.0);
    if (hw.family == "ipad" || hw.family == "iphone")
    {
        const bool a_series = hw.chip.rfind("Apple A", 0) == 0;
        // physicalMemory is a little under the nominal size: an 8 GB iPad Air reports 7.4 GiB, a 6 GB iPad 5.6
        if (gib >= 10.5)
            return "ipad-16gb";
        if (gib >= 7.0 && !a_series)
            return "ipad-8gb";
        return "ipad-6gb";
    }
    // A Mac: by effective GPU core count. An M1's cores are about 30% slower than an M2's.
    int cores = hw.gpu_cores;
    const std::string chip = Lower(hw.chip.c_str());
    if (cores <= 0)
        cores = chip.find("ultra") != std::string::npos ? 60 : chip.find("max") != std::string::npos ? 32 : chip.find("pro") != std::string::npos ? 16 : 8;
    const double effective = cores * (hw.chip_generation == 1 ? 0.7 : 1.0);
    // the tiers are conservative: the measured class is a 19-core M2 Pro, so an M1 Max (22 effective cores) stays in it
    if (effective > 40.5)
        return "mac-ultra";
    if (effective > 26.0)
        return "mac-max";
    if (effective > 10.5)
        return "mac-pro";
    return gib < 12.0 ? "mac-base-8gb" : "mac-base";
}

void Init(const Hardware& hw, const Options& options)
{
    State& s = S();
    s = State();
    s.hw = hw;
    if (Lower(options.profile.c_str()) == "none")
        return;
    std::string id = options.profile;
    if (id == "ipad")
        id = "ipad-8gb";
    else if (id == "mac" || id == "apple" || id.empty() || !Find(id))
        id = SelectProfileId(hw);
    s.id = id;
    s.enabled = true;
    s.unlocked = options.unlocked;
    // an iPad profile forced on a Mac is for testing its numbers: it keeps the iPad's rules, the display mode included
    if (id.rfind("ipad", 0) == 0 && hw.family == "mac")
        s.hw.family = "ipad";
    BuildLockedSet(s);
}

bool Enabled() { return S().enabled; }
bool LocksActive() { return S().enabled && !S().unlocked; }
const std::string& ProfileId() { return S().id; }

const std::string& ProfileTitle()
{
    static const std::string none = "none";
    const ProfileDef* p = Find(S().id);
    static std::string title;
    if (!p)
        return none;
    title = p->title;
    return title;
}

bool ProfileMeasured()
{
    const ProfileDef* p = Find(S().id);
    return p && p->measured;
}

const Hardware& GetHardware() { return S().hw; }

Mode CurrentMode() { return S().mode; }
void SetMode(Mode mode) { S().mode = mode; }

std::vector<int> FrameCaps()
{
    std::vector<int> caps = {30, 60};
    if (S().hw.max_refresh_hz >= 119.0f)
        caps.push_back(120);
    return caps;
}

int FrameCapChoice() { return S().fps_choice; }

bool SetFrameCap(int fps)
{
    if (fps < 0)
    {
        if (S().enabled && !S().unlocked)
            return false; // unlimited is for --unlocked-graphics and --graphics-profile none runs
        S().fps_choice = -1;
        return true;
    }
    if (fps != 0)
    {
        const std::vector<int> caps = FrameCaps();
        if (std::find(caps.begin(), caps.end(), fps) == caps.end())
            return false;
    }
    S().fps_choice = fps;
    return true;
}

int EffectiveFrameCap()
{
    State& s = S();
    if (!s.enabled || s.fps_choice < 0)
        return 0;
    if (s.fps_choice)
        return s.fps_choice;
    const ProfileDef* leaf = Find(s.id);
    int fps = leaf ? (s.mode == Mode::Quality ? leaf->fps_quality : leaf->fps_performance) : 60;
    // the default for the mode, limited to what the display runs
    const std::vector<int> caps = FrameCaps();
    while (fps > 0 && std::find(caps.begin(), caps.end(), fps) == caps.end())
        fps = fps > 60 ? 60 : fps > 30 ? 30 : 0;
    return fps;
}

std::vector<Setting> Settings(const CommandProbe& probe)
{
    State& s = S();
    const ProfileDef* leaf = Find(s.id);
    if (!leaf)
        return {};
    return Resolve(leaf, s.mode, &probe, s.hw, false);
}

std::vector<Setting> UserDefaults()
{
    std::vector<Setting> out;
    for (const Entry& e : kUserDefaults)
        out.push_back({e.key, e.value});
    return out;
}

int UserDefaultsVersion() { return kUserDefaultsVersion; }

const std::string& CommandLine()
{
    static std::string line;
    line.clear();
    if (const ProfileDef* leaf = Find(S().id))
        for (const ProfileDef* p : Chain(leaf))
            if (p->command_line && *p->command_line)
                line = p->command_line;
    return line;
}

bool IsLocked(const char* command)
{
    if (!LocksActive() || !command)
        return false;
    const std::string name = Lower(command);
    if (S().locked_exact.count(name))
        return true;
    for (const char* prefix : kLockedPrefixes)
        if (name.rfind(prefix, 0) == 0)
            return true;
    return false;
}

std::string ProfileValue(const char* command)
{
    if (!LocksActive() || !command)
        return {};
    const ProfileDef* leaf = Find(S().id);
    if (!leaf)
        return {};
    NoProbe probe;
    const std::string key = Lower(command);
    for (const Setting& e : Resolve(leaf, S().mode, &probe, S().hw, false))
        if (e.key == key)
            return e.value;
    return {};
}

bool MayTakeGameDefault(const char* command)
{
    if (!LocksActive() || !command)
        return false;
    const std::string name = Lower(command);
    return S().locked_exact.count(name) && !S().with_value.count(name);
}

bool HasProfileValue(const char* command)
{
    return LocksActive() && command && S().with_value.count(Lower(command));
}

std::vector<std::string> LockedCommands()
{
    return std::vector<std::string>(S().locked_exact.begin(), S().locked_exact.end());
}

std::vector<std::string> LockedPrefixes()
{
    return std::vector<std::string>(std::begin(kLockedPrefixes), std::end(kLockedPrefixes));
}

std::vector<ProfileInfo> AllProfiles()
{
    std::vector<ProfileInfo> out;
    for (const ProfileDef& p : kProfiles)
        out.push_back({p.id, p.title, p.parent ? p.parent : "", p.measured, p.notes});
    return out;
}

std::vector<Setting> ResolveForDocs(const std::string& profile_id, Mode mode)
{
    const ProfileDef* leaf = Find(profile_id);
    if (!leaf)
        return {};
    Hardware hw;
    hw.family = profile_id.rfind("ipad", 0) == 0 ? "ipad" : "mac";
    return Resolve(leaf, mode, nullptr, hw, true);
}

std::string Describe()
{
    const State& s = S();
    if (!s.enabled)
        return "graphics profile: none (PC behaviour)";
    char text[512];
    snprintf(text, sizeof(text), "graphics profile %s%s, %s mode, %d fps cap, locks %s (%s %s, %d GPU cores, %.1f GiB, %dx%d @ %.0f Hz)", s.id.c_str(),
        ProfileMeasured() ? "" : " (extrapolated)", ToString(s.mode), EffectiveFrameCap(), s.unlocked ? "off" : "on", s.hw.family.c_str(),
        s.hw.chip.empty() ? s.hw.machine.c_str() : s.hw.chip.c_str(), s.hw.gpu_cores, double(s.hw.memory_bytes) / (1024.0 * 1024.0 * 1024.0),
        s.hw.native_width, s.hw.native_height, double(s.hw.max_refresh_hz));
    return text;
}
} // namespace AppleShell::Graphics
