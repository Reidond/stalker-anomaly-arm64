// Device graphics profiles: the console-like settings model of the Apple builds (docs/ios-port/SETTINGS.md).
//
// At start-up the shell detects what it runs on (Hardware), picks one profile for it, and the engine applies that profile's settings
// after user.ltx has been read, on every start. While the profile is active (Enabled and not unlocked) the engine refuses console
// commands that change a locked setting, wherever they come from (options menu, console, scripts, addons, user.ltx). The player's
// own choices are a short list: the mode (Quality or Performance), the frame rate cap, and the settings the profile leaves
// unlocked (field of view, brightness, motion blur, HUD).
//
// Pure C++ with no engine or platform dependency, so tools/graphics_profile_test checks the tables on any host. The detection that
// fills Hardware is in DeviceInfo.mm.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AppleShell::Graphics
{
enum class Mode
{
    Quality,     // the best image the device sustains at the mode's rate (40 on the iPad where the display shows it evenly, else 30 or 60)
    Performance, // a lower render scale at a locked 60 fps (120 on a Mac display that has it)
};
const char* ToString(Mode mode);
bool ParseMode(const char* text, Mode& mode);

struct Hardware
{
    std::string family;       // "ipad", "iphone", "mac"
    std::string machine;      // hw.machine ("iPad15,5") or hw.model ("Mac14,9")
    std::string chip;         // "Apple M3", "Apple M2 Pro"; empty when unknown
    int chip_generation = 0;  // 1 for M1 / A14 ..., 0 when unknown
    int gpu_cores = 0;        // 0 when the OS does not tell (iPadOS)
    uint64_t memory_bytes = 0;
    int native_width = 0;     // the display's physical pixels, landscape
    int native_height = 0;
    float max_refresh_hz = 60.0f;
    bool builtin_display = true; // false: an external display on a Mac
};

struct Setting
{
    std::string key;
    std::string value;
};

// Command line: `--graphics-profile <id|none>` and `--unlocked-graphics` (Shell.mm reads them, with commandline.txt).
struct Options
{
    std::string profile; // a profile id forces it; "none" turns the whole mechanism off (PC behaviour); "" picks by hardware
    bool unlocked = false;
};

// Detects the class of `hw` and selects the profile. Idempotent for the same inputs. Call once, early (Shell::Init).
void Init(const Hardware& hw, const Options& options);

// ---- state of the active profile ----------------------------------------------------------------------------------------------

bool Enabled();       // a profile is selected
bool LocksActive();   // Enabled and not unlocked: locked settings are refused, the menu is the reduced one
const std::string& ProfileId();
const std::string& ProfileTitle();
bool ProfileMeasured(); // tuned from measurements on this class (the rest is extrapolated and says so in SETTINGS.md)
const Hardware& GetHardware();
std::string Describe(); // one line for the log: profile, class, mode, frame cap

Mode CurrentMode();
void SetMode(Mode mode);

// The frame rate caps the player may choose from on this display, the ones it shows evenly (a frame lasts a whole number of refreshes):
// 30 and 60, and 40 and 120 where the display runs 120 Hz.
std::vector<int> FrameCaps();
// The player's choice; 0 means "the profile's default for the mode", -1 "unlimited" (only with --unlocked-graphics or no profile).
// SetFrameCap refuses a value that is not in FrameCaps() (with --unlocked-graphics any rate from 10 to 240).
// The mode's default is the largest rate in FrameCaps() that is not above the profile's: Quality's 40 is 30 on a 60 Hz display.
int FrameCapChoice();
bool SetFrameCap(int fps);
// What applies now: the choice, or the mode's default. 0: no cap, the app runs at the display's rate.
int EffectiveFrameCap();

// Settings to run through the console, in order: the profile (parents first, a later entry replaces an earlier one with the same
// key) for the current mode, then the frame cap. `has_command` reports whether the engine has the command (the MetalFX
// settings only exist once the renderer provides them); entries for a missing command are dropped.
struct CommandProbe
{
    virtual ~CommandProbe() = default;
    virtual bool HasCommand(const char* name) const = 0;
    // The range of a numeric command; false when it has none. A value outside it is a syntax error in the console, not a clamp, so
    // the profile clamps `r__render_scale` to what this build accepts (its range widens when the renderer's minimum is lowered).
    virtual bool Bounds(const char*, float&, float&) const { return false; }
};
std::vector<Setting> Settings(const CommandProbe& probe);

// Settings applied once per `UserDefaultsVersion()`: starting values of settings the profile leaves unlocked. They are not locked.
std::vector<Setting> UserDefaults();
int UserDefaultsVersion();

// Engine command line flags the profile needs (the shadow map size has no console command), e.g. "-smap1536".
const std::string& CommandLine();

// True when `command` (a console command name, any case) is locked by the active profile: every setting the profile lists, the
// settings the menu used to offer, and the prefixes that hold only unsupported features. Always false when !LocksActive().
bool IsLocked(const char* command);
// True when the active profile gives `command` a value (the rest of the locked settings keep the game's own defaults).
bool HasProfileValue(const char* command);
// True for the locked settings the profile gives no value: they keep the game's own starting values (default_controls.ltx may set them).
// A prefix-locked setting (ssfx_*) and a setting with a profile value are not among them.
bool MayTakeGameDefault(const char* command);
// The value the profile gives `command` in the current mode, "" when it gives none (the upscaler entries are not considered).
std::string ProfileValue(const char* command, const CommandProbe& probe);
// Every command name IsLocked() knows by name, sorted (the tests and the documentation list them).
std::vector<std::string> LockedCommands();
std::vector<std::string> LockedPrefixes();

// ---- the table, for tools and the documentation ---------------------------------------------------------------------------------

struct ProfileInfo
{
    std::string id;
    std::string title;
    std::string parent;
    bool measured = false;
    std::string notes;
};
std::vector<ProfileInfo> AllProfiles();
// The resolved settings of `profile_id` for `mode`, without a probe (every entry kept, upscaler entries marked by a "?" prefix on
// the key). Empty for an unknown id.
std::vector<Setting> ResolveForDocs(const std::string& profile_id, Mode mode);
// The profile `hw` would get by default.
std::string SelectProfileId(const Hardware& hw);
} // namespace AppleShell::Graphics
