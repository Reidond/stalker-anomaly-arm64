// Graphics preset for the iPad (docs/ios-port/IPAD.md). The engine's settings live in appdata/user.ltx, one console command per
// line, executed before the renderer starts. On iOS the shell merges the preset's lines into that file the first time (and again
// whenever the preset's version is raised), so the player's other settings, key bindings and later in-game changes to settings
// the preset does not name are kept. Engine command line flags the console cannot set (shadow map size) go through
// PresetCommandLine().
#include "ShellInternal.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace AppleShell::Internal
{
namespace
{
struct Entry
{
    const char* key;
    const char* value;
};

// Raise this when the table below changes: the next launch merges it into the player's user.ltx again.
constexpr int kIpadPresetVersion = 4;

// iPad Air 13" (M3, 8 GB): 2732x2048 panel, 7.95 GB under jetsam with increased-memory-limit. The footprint on desktop settings
// was 4.6 GB in the Cordon, 2.5 GB of it Metal (3024x1964, texture LOD 1).
const Entry kIpadEntries[] = {
    // resolution: 4:3 like the panel; 1366x1024 is the panel at 1x points, the Metal layer scales it to 2732x2048
    {"rs_screenmode", "borderless"}, // borderless takes the size from vid_mode; windowed uses the 2732x2048 view
    {"vid_mode", "1366x1024"},
    {"r__supersample", "1"},
    {"rs_v_sync", "on"},
    {"texture_lod", "1"},
    {"r__tf_aniso", "4"},
    // shadows and lighting
    {"r2_sun_quality", "st_opt_low"},
    {"r2_ls_squality", "0.5"},
    {"r3_dynamic_wet_surfaces_sm_res", "1024"},
    {"r3_dynamic_wet_surfaces", "off"}, // 3.5 ms of GPU at 1920x1440 in the rain
    {"r2_volumetric_lights", "off"},
    {"r2_sunshafts_mode", "screen_space"},
    {"r2_ssao", "st_opt_low"},
    // vegetation and view distance
    {"r__detail_density", "0.4"},
    {"r__detail_radius", "35"},
    {"rs_vis_distance", "0.8"},
    // features the Metal renderer does not have or cannot afford
    {"r4_enable_tessellation", "off"},
    {"r3_volumetric_smoke", "off"},
    {"r2_mblur_enabled", "off"},
    // sound cache (MB of decoded sound kept in memory)
    {"snd_cache_size", "32"},
};

// Flags the engine reads from its command line and the console has no command for.
const char* const kIpadCommandLine = "-smap1536";

std::string ReadAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Replaces the lines of `path` whose command is a preset key, appends the keys it does not have yet.
bool MergeIntoUserLtx(const std::string& path)
{
    const std::string text = ReadAll(path);
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
    }
    std::vector<bool> done(sizeof(kIpadEntries) / sizeof(kIpadEntries[0]), false);
    for (std::string& line : lines)
    {
        const size_t space = line.find(' ');
        const std::string key = line.substr(0, space);
        for (size_t i = 0; i < done.size(); ++i)
        {
            if (!done[i] && key == kIpadEntries[i].key)
            {
                line = std::string(kIpadEntries[i].key) + " " + kIpadEntries[i].value;
                done[i] = true;
            }
        }
    }
    for (size_t i = 0; i < done.size(); ++i)
        if (!done[i])
            lines.push_back(std::string(kIpadEntries[i].key) + " " + kIpadEntries[i].value);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (const std::string& line : lines)
        out << line << eol;
    return bool(out);
}

std::string g_command_line;
} // namespace

// preset: "ipad" applies the iPad table, "" does nothing. reset: apply even when this version was applied before.
void ApplyDevicePreset(const std::string& preset, bool reset, const std::string& appdata, const std::string& app_support)
{
    g_command_line.clear();
    if (preset != "ipad")
        return;
    g_command_line = kIpadCommandLine;

    const std::string stamp_path = app_support + "/device-preset.version";
    char want[64];
    snprintf(want, sizeof(want), "ipad %d", kIpadPresetVersion);
    std::string have = ReadAll(stamp_path);
    while (!have.empty() && (have.back() == '\n' || have.back() == '\r'))
        have.pop_back();
    if (!reset && have == want)
    {
        Log("device preset: %s already applied", want);
        return;
    }
    const std::string user_ltx = appdata + "/user.ltx";
    if (!MergeIntoUserLtx(user_ltx))
    {
        Log("device preset: cannot write %s", user_ltx.c_str());
        return;
    }
    std::ofstream(stamp_path, std::ios::binary | std::ios::trunc) << want << "\n";
    Log("device preset: applied %s to %s (%zu settings, command line '%s')", want, user_ltx.c_str(),
        sizeof(kIpadEntries) / sizeof(kIpadEntries[0]), kIpadCommandLine);
}

const std::string& DevicePresetCommandLine()
{
    return g_command_line;
}
} // namespace AppleShell::Internal
