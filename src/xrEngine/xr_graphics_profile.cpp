// The engine side of the device graphics profile: see xr_graphics_profile.h.
#include "stdafx.h"

#ifdef XR_PLATFORM_APPLE

#include "xr_graphics_profile.h"
#include "xr_ioconsole.h"
#include "xr_ioc_cmd.h"

#include <Platform/Apple/GraphicsProfile.h>
#include <Platform/Apple/Shell.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <string>

namespace Gfx = AppleShell::Graphics;

namespace
{
int g_bypass = 0;              // > 0 while the profile itself runs commands: the lock lets them through
int g_defaults = 0;            // > 0 while default_controls runs (DefaultsScope)
bool g_user_config = false;    // user.ltx is being read
bool g_applied = false;        // the profile has been applied once (mode changes re-apply from then on)
std::set<std::string> g_refused;       // commands already reported
std::set<std::string> g_stale_in_user; // locked commands user.ltx set (all of them: cfg_save writes every setting)
std::vector<std::pair<std::string, std::string>> g_user_lines; // name and value of each of them the profile has a value for (compared at the end:
                                                               // the mode that decides the value is a line of the same file)

struct Bypass
{
    Bypass() { ++g_bypass; }
    ~Bypass() { --g_bypass; }
};

struct EngineProbe : Gfx::CommandProbe
{
    bool HasCommand(const char* name) const override { return Console && Console->GetCommand(name) != nullptr; }
};

std::string LowerName(const char* s)
{
    std::string r(s ? s : "");
    for (char& c : r)
        c = char(tolower((unsigned char)c));
    return r;
}

void Report(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Report(const char* format, ...)
{
    char text[640];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Msg("%s", text);
    AppleShell::Log("%s", text);
}

// "0.5" and "0.5000", "on" and "ON": the same setting
bool SameValue(const std::string& a, const std::string& b)
{
    char *end_a = nullptr, *end_b = nullptr;
    const double x = strtod(a.c_str(), &end_a), y = strtod(b.c_str(), &end_b);
    if (!a.empty() && !b.empty() && *end_a == 0 && *end_b == 0)
        return fabs(x - y) < 1e-6;
    return LowerName(a.c_str()) == LowerName(b.c_str());
}

void RunSetting(const std::string& key, const std::string& value)
{
    Bypass bypass;
    const std::string line = key + " " + value;
    Console->Execute(line.c_str());
}

std::string StampPath()
{
    return AppleShell::GetPaths().app_support + "/graphics-defaults.version";
}

// ---- console commands ----------------------------------------------------------------------------------------------------------

class CCC_GfxMode : public IConsole_Command
{
public:
    CCC_GfxMode(LPCSTR N) : IConsole_Command(N) {}
    void Execute(LPCSTR args) override
    {
        Gfx::Mode mode;
        if (!Gfx::ParseMode(args, mode))
        {
            InvalidSyntax();
            return;
        }
        if (mode == Gfx::CurrentMode())
            return;
        Gfx::SetMode(mode);
        if (g_applied)
            xr_gfx::ApplyProfile("mode change");
    }
    void Status(TStatus& S) override { xr_strcpy(S, Gfx::ToString(Gfx::CurrentMode())); }
    void Info(TInfo& I) override { xr_strcpy(I, "quality | performance: the graphics mode of the device profile"); }
    void fill_tips(vecTips& tips, u32 mode) override
    {
        tips.push_back("quality");
        tips.push_back("performance");
    }
};

class CCC_GfxFps : public IConsole_Command
{
public:
    CCC_GfxFps(LPCSTR N) : IConsole_Command(N) {}
    void Execute(LPCSTR args) override
    {
        const std::string value = LowerName(args);
        if (value == "auto")
        {
            Gfx::SetFrameCap(0);
            return;
        }
        if (value == "unlimited")
        {
            if (!Gfx::SetFrameCap(-1))
                Report("! graphics: gfx_fps unlimited is for --unlocked-graphics runs");
            return;
        }
        const int fps = atoi(value.c_str());
        if (fps <= 0 || !Gfx::SetFrameCap(fps))
        {
            InvalidSyntax();
            return;
        }
    }
    void Status(TStatus& S) override
    {
        const int choice = Gfx::FrameCapChoice();
        if (choice == 0)
            xr_strcpy(S, "auto");
        else if (choice < 0)
            xr_strcpy(S, "unlimited");
        else
            xr_sprintf(S, sizeof(S), "%d", choice);
    }
    void Info(TInfo& I) override { xr_strcpy(I, "auto | 30 | 60 | 120 (where the display runs 120 Hz): the frame rate cap"); }
    void fill_tips(vecTips& tips, u32 mode) override
    {
        tips.push_back("auto");
        for (int fps : Gfx::FrameCaps())
            tips.push_back(std::to_string(fps).c_str());
    }
};

class CCC_GfxProfile : public IConsole_Command
{
public:
    CCC_GfxProfile(LPCSTR N) : IConsole_Command(N) { bEmptyArgsHandled = TRUE; }
    void Execute(LPCSTR args) override
    {
        Msg("%s", Gfx::Describe().c_str());
        if (!Gfx::Enabled())
            return;
        EngineProbe probe;
        for (const Gfx::Setting& s : Gfx::Settings(probe))
            Msg("  %s %s", s.key.c_str(), s.value.c_str());
        Msg("  frame cap %d (choice %d)", Gfx::EffectiveFrameCap(), Gfx::FrameCapChoice());
    }
    void Info(TInfo& I) override { xr_strcpy(I, "prints the device graphics profile and the settings it applies"); }
};
} // namespace

namespace xr_gfx
{
void RegisterCommands()
{
    CMD1(CCC_GfxMode, "gfx_mode");
    CMD1(CCC_GfxFps, "gfx_fps");
    CMD1(CCC_GfxProfile, "gfx_profile");
}

bool LocksActive() { return Gfx::LocksActive(); }
bool IsLocked(const char* command) { return Gfx::IsLocked(command); }
const char* ProfileId() { return Gfx::ProfileId().c_str(); }
bool IsMac() { return Gfx::GetHardware().family == "mac"; }
void FrameCaps(xr_vector<int>& out)
{
	for (int fps : Gfx::FrameCaps())
		out.push_back(fps);
}

DefaultsScope::DefaultsScope(const char* command) : active(command && LowerName(command) == "default_controls")
{
	if (active)
		++g_defaults;
}

DefaultsScope::~DefaultsScope()
{
	if (active)
		--g_defaults;
}

bool Refuse(const char* command, const char* args)
{
    if (g_bypass || !Gfx::LocksActive() || !Gfx::IsLocked(command))
        return false;
    if (g_defaults)
        return !Gfx::MayTakeGameDefault(command); // the game's starting values reach what the profile leaves alone, silently
    const std::string name = LowerName(command);
    if (g_user_config)
    {
        g_stale_in_user.insert(name);
        if (Gfx::HasProfileValue(command))
        {
            std::string value = args ? args : "";
            while (!value.empty() && (value.back() == ' ' || value.back() == '\r'))
                value.pop_back();
            g_user_lines.emplace_back(name, value);
        }
        return true;
    }
    if (g_refused.insert(name).second)
        Report("! graphics: '%s %s' refused: locked by the %s profile. Further attempts on %s are not logged.", command, args ? args : "",
            Gfx::ProfileId().c_str(), command);
    return true;
}

void ApplyProfile(const char* why)
{
    if (!Gfx::Enabled())
        return;
    EngineProbe probe;
    const std::vector<Gfx::Setting> settings = Gfx::Settings(probe);
    size_t unknown = 0;
    std::string unknown_list;
    for (const Gfx::Setting& s : settings)
    {
        if (!Console->GetCommand(s.key.c_str()))
        {
            ++unknown;
            unknown_list += (unknown_list.empty() ? "" : ", ") + s.key;
            continue;
        }
        // the renderer is chosen before user.ltx is read; setting it again at a mode change would only print a restart notice
        if (s.key == "renderer" && g_applied)
            continue;
        RunSetting(s.key, s.value);
    }
    Report("* graphics profile %s applied (%s): %zu settings, mode %s, frame cap %d%s%s", Gfx::ProfileId().c_str(), why, settings.size() - unknown,
        Gfx::ToString(Gfx::CurrentMode()), Gfx::EffectiveFrameCap(), unknown ? ", not in this build: " : "", unknown_list.c_str());
    g_applied = true;
}

void BeginUserConfig()
{
    g_user_config = Gfx::LocksActive();
    g_stale_in_user.clear();
    g_user_lines.clear();
}

void EndUserConfig()
{
    g_user_config = false;
    if (!Gfx::Enabled())
        return;
    // every line counts, not only the first: a later line of the same setting wins in an unlocked engine
    std::vector<std::string> differing;
    for (const auto& line : g_user_lines)
    {
        const std::string setting = line.first + " " + line.second;
        if (!SameValue(line.second, Gfx::ProfileValue(line.first.c_str())) && std::find(differing.begin(), differing.end(), setting) == differing.end())
            differing.push_back(setting);
    }
    if (!g_stale_in_user.empty())
    {
        std::string list;
        size_t shown = 0;
        for (const std::string& setting : differing)
        {
            if (++shown > 12)
            {
                list += ", ...";
                break;
            }
            list += (list.empty() ? "" : ", ") + setting;
        }
        Report("* graphics: user.ltx has %zu locked settings, ignored (the %s profile applies); %zu of them differ from the profile%s%s",
            g_stale_in_user.size(), Gfx::ProfileId().c_str(), differing.size(), list.empty() ? "" : ": ", list.c_str());
    }
    g_stale_in_user.clear();
    g_user_lines.clear();

    // the player's own starting values, once per version
    const std::string want = "defaults " + std::to_string(Gfx::UserDefaultsVersion());
    std::string have;
    {
        std::ifstream in(StampPath());
        std::getline(in, have);
        while (!have.empty() && (have.back() == '\r' || have.back() == ' '))
            have.pop_back();
    }
    if (have != want)
    {
        for (const Gfx::Setting& s : Gfx::UserDefaults())
            RunSetting(s.key, s.value);
        std::ofstream(StampPath(), std::ios::trunc) << want << "\n";
        Report("* graphics: starting values for the player's settings applied (%s)", want.c_str());
    }
    ApplyProfile("start");
}
} // namespace xr_gfx

#endif // XR_PLATFORM_APPLE
