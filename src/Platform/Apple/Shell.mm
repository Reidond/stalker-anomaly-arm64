// Shell core: paths, directory setup, generated fsgame.ltx, session bookkeeping, init/shutdown.

#include "ShellInternal.h"

#include <SDL3/SDL.h>

#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace AppleShell
{
namespace
{
struct ShellState
{
    bool initialized = false;
    Paths paths;
    std::chrono::steady_clock::time_point start;
    bool previous_session_unexpected = false;
    bool previous_crash_log = false;
    std::string session_file;
};

ShellState& State()
{
    static ShellState state;
    return state;
}

std::string ToStd(NSString* string)
{
    return string ? std::string(string.fileSystemRepresentation) : std::string();
}

std::string SearchPath(NSSearchPathDirectory directory)
{
    NSArray<NSString*>* found = NSSearchPathForDirectoriesInDomains(directory, NSUserDomainMask, YES);
    return found.count ? ToStd(found[0]) : std::string();
}

std::string Normalize(const std::string& path)
{
    NSString* s = [NSString stringWithUTF8String:path.c_str()];
    return ToStd(s.stringByExpandingTildeInPath.stringByStandardizingPath);
}

bool MakeDirs(const std::string& path)
{
    NSError* error = nil;
    NSString* s = [NSString stringWithUTF8String:path.c_str()];
    if (![NSFileManager.defaultManager createDirectoryAtPath:s withIntermediateDirectories:YES attributes:nil error:&error])
    {
        Log("failed to create %s: %s", path.c_str(), error.localizedDescription.UTF8String);
        return false;
    }
    return true;
}

#if TARGET_OS_IOS
void ExcludeFromBackup(const std::string& path)
{
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    NSError* error = nil;
    if (![url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&error])
        Log("could not exclude %s from backup: %s", path.c_str(), error.localizedDescription.UTF8String);
}
#endif

bool FileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool WriteFileIfChanged(const std::string& path, const std::string& content)
{
    if (ReadFile(path) == content)
        return true;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return bool(out);
}

// The engine's log of the run before (and its crash report, if it left one) is copied next to the shell log, which is where the Files app
// shows it: Library/ (where the engine writes appdata/logs) is not visible there, and a script error of an addon is only in the engine log.
// Done before this run's engine starts: the engine moves its previous log aside when it opens the new one.
void KeepPreviousEngineLogs(const std::string& logs_dir, const std::string& shell_dir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path newest_log, newest_crash;
    fs::file_time_type log_time{}, crash_time{};
    for (const auto& entry : fs::directory_iterator(logs_dir, ec))
    {
        const std::string name = entry.path().filename().string();
        if (name.rfind("xray_", 0) != 0 || !entry.is_regular_file(ec))
            continue;
        const auto time = entry.last_write_time(ec);
        const bool is_log = name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0;
        const bool is_crash = name.size() > 10 && name.compare(name.size() - 10, 10, ".crash.txt") == 0;
        if (is_log && (newest_log.empty() || time > log_time))
            newest_log = entry.path(), log_time = time;
        else if (is_crash && (newest_crash.empty() || time > crash_time))
            newest_crash = entry.path(), crash_time = time;
    }
    if (!newest_log.empty())
        fs::copy_file(newest_log, fs::path(shell_dir) / "anomaly-engine.prev.log", fs::copy_options::overwrite_existing, ec);
    if (!newest_crash.empty())
        fs::copy_file(newest_crash, fs::path(shell_dir) / "anomaly-engine-crash.prev.txt", fs::copy_options::overwrite_existing, ec);
}

// Where the game data lives. Documents/Anomaly on iPad; configurable on the Mac.
std::string ResolveDataRoot(const InitParams& params, const std::string& documents, DataRootSource& source)
{
    source = DataRootSource::PlatformDefault;
    std::string root = documents + "/Anomaly";

    if (NSString* saved = [NSUserDefaults.standardUserDefaults stringForKey:@"AnomalyDataRoot"]; saved.length)
    {
        root = Normalize(ToStd(saved));
        source = DataRootSource::Preference;
    }
    if (const char* env = getenv("ANOMALY_DATA_ROOT"); env && *env)
    {
        root = Normalize(env);
        source = DataRootSource::Environment;
    }
    for (int i = 1; params.argv && i + 1 < params.argc; ++i)
    {
        if (strcmp(params.argv[i], "--data-root") == 0)
        {
            root = Normalize(params.argv[i + 1]);
            source = DataRootSource::CommandLine;
        }
    }
    if (params.data_root_override && *params.data_root_override)
    {
        root = Normalize(params.data_root_override);
        source = DataRootSource::InitParams;
    }
    return root;
}

// Where the Monolith gamedata overlay comes from: --overlay <dir>, ANOMALY_OVERLAY, or the bundle's Resources/overlay.
std::string ResolveOverlaySource(const InitParams& params)
{
    std::string dir;
    if (NSString* resources = NSBundle.mainBundle.resourcePath; resources.length)
    {
        const std::string bundled = ToStd(resources) + "/overlay";
        if (FileExists(bundled))
            dir = bundled;
    }
    if (const char* env = getenv("ANOMALY_OVERLAY"); env && *env)
        dir = Normalize(env);
    for (int i = 1; params.argv && i + 1 < params.argc; ++i)
        if (strcmp(params.argv[i], "--overlay") == 0)
            dir = Normalize(params.argv[i + 1]);
    return dir;
}

// Whether the volume of 'dir' tells "Scripts" from "scripts". The engine's own lookups ignore case either way, but an addon unzipped in
// the Files app lands in one merged folder on a case-insensitive volume and in two folders on a case-sensitive one, so the log says.
bool VolumeIsCaseSensitive(const std::string& dir)
{
    const std::string a = dir + "/.case_probe_Aa";
    const std::string b = dir + "/.case_probe_aA";
    unlink(a.c_str());
    unlink(b.c_str());
    FILE* f = fopen(a.c_str(), "wb");
    if (!f)
        return false;
    fclose(f);
    struct stat st;
    const bool sensitive = stat(b.c_str(), &st) != 0;
    unlink(a.c_str());
    return sensitive;
}

std::string BuildFsGameLtx(const Paths& p)
{
    std::ostringstream out;
    out << "; Generated by the Apple app shell on every launch. Edits are overwritten.\n";
    out << "; The app container path changes when the app is reinstalled, so the absolute roots below are rewritten each run.\n";
    out << ";   game data (read-only): " << p.data_root << "\n";
    out << ";   appdata (writable)   : " << p.appdata << "\n";
    out << ";   caches (disposable)  : " << p.caches << "\n";
    out << ";\n";
    out << ";abbreviation           = recurs| notif\t| root\t\t\t | add\t\t\t\t| ext\t\t\t| description\n";
    // Absolute roots first: the engine treats a root that is not a known alias as a literal directory.
    out << "$fs_root$               = false | false\t| " << p.data_root << "\n";
    out << "$app_data_root$         = true \t| false\t| " << p.appdata << "\n";
    out << "$app_cache_root$        = false | false\t| " << p.caches << "\n";
    out << "$shader_cache$          = false | false\t| " << p.shader_cache << "\n";
    out << "$lua_cache$             = false | false\t| " << p.lua_cache << "\n";
    out << R"LTX($arch_dir$              = false	| false	| $fs_root$		 | db\
$arch_dir_configs$		= false	| false	| $arch_dir$	 | configs\
$arch_dir_maps$         = false	| false	| $arch_dir$	 | levels\
$arch_dir_meshes$       = false	| false	| $arch_dir$	 | meshes\
$arch_dir_sounds$		= false	| false	| $arch_dir$	 | sounds\
$arch_dir_textures$		= false	| false	| $arch_dir$	 | textures\
$arch_dir_resource$     = false	| false	| $arch_dir$	 | mods\
)LTX";
    // The Monolith overlay sits after the game's own archives (it overrides them, as the loose files do on Windows) and
    // before db\patches, db\addons and the loose gamedata folder, which override it.
    if (!p.overlay_archive_dir.empty() && FileExists(p.overlay_archive_dir + "/monolith_overlay.db0"))
        out << "$monolith_overlay$      = false | false\t| " << p.overlay_archive_dir << "\n";
    out << R"LTX($arch_dir_patches$		= false	| true 	| $arch_dir$	 | patches\
$arch_dir_addons$		= false	| true 	| $arch_dir$	 | addons\
$game_data$             = true 	| true 	| $fs_root$		 | gamedata\
$game_ai$               = true 	| false	| $game_data$	 | ai\
$game_spawn$            = true 	| false	| $game_data$	 | spawns\
$game_levels$           = true 	| false	| $game_data$	 | levels\
$game_meshes$           = true 	| true 	| $game_data$	 | meshes\			| *.ogf;*.omf	| Game Object files
$game_anims$            = true 	| true 	| $game_data$	 | anims\			| *.anm;*.anms	| Animation files
$game_dm$               = true 	| true 	| $game_data$	 | meshes\			| *.dm			| Detail Model files
$game_shaders$          = true 	| true 	| $game_data$	 | shaders\
$game_particles$        = true 	| true 	| $game_data$	 | particles\		| *.pe;*.pg
$game_sounds$           = true 	| true 	| $game_data$	 | sounds\
$game_textures$         = true 	| true 	| $game_data$	 | textures\
$game_config$           = true 	| false	| $game_data$	 | configs\
$game_addons$           = true 	| false	| $game_data$	 | addons\
$game_weathers$         = true 	| false	| $game_config$	 | environment\weathers
$game_weather_effects$  = true 	| false	| $game_config$	 | environment\weather_effects
$textures$              = true 	| true 	| $game_data$	 | textures\
$level$                 = false	| false	| $game_levels$
$game_scripts$          = true 	| false	| $game_data$	 | scripts\			| *.script		| Game script files
$logs$                  = true 	| false	| $app_data_root$| logs\
$screenshots$           = true 	| false	| $app_data_root$| screenshots\
$game_saves$            = true 	| false	| $app_data_root$| savedgames\		| *.scop;*.scoc
$downloads$             = false	| false	| $app_data_root$
$game_text$				= true 	| false	| $game_config$	 | text\
$textures_ui$ 			= true 	| true 	| $textures$	 | ui\
$warfare_presets$		= true 	| false	| $game_config$	 | warfare_presets\	| *.ltx
)LTX";
    return out.str();
}

void UncaughtObjCException(NSException* exception)
{
    std::string text = std::string("Uncaught Objective-C exception: ") + exception.name.UTF8String + "\nreason: " +
        (exception.reason ? exception.reason.UTF8String : "(none)") + "\n";
    for (NSString* frame in exception.callStackSymbols)
        text += std::string(frame.UTF8String) + "\n";
    Internal::WriteCrashText(text.c_str());
    Log("%s", text.c_str());
}
} // namespace

namespace Internal
{
double SecondsSinceInit()
{
    auto& s = State();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
}

void WriteSessionState(const char* state)
{
    auto& s = State();
    if (s.session_file.empty())
        return;
    std::ofstream out(s.session_file, std::ios::trunc);
    out << state << "\n";
}
} // namespace Internal

bool Init(const InitParams& params)
{
    auto& s = State();
    if (s.initialized)
        return true;
    @autoreleasepool
    {
        s.start = std::chrono::steady_clock::now();
        Paths& p = s.paths;

        const std::string documents = SearchPath(NSDocumentDirectory);
        std::string app_support = SearchPath(NSApplicationSupportDirectory);
        std::string caches = SearchPath(NSCachesDirectory);
        std::string logs_root = SearchPath(NSLibraryDirectory) + "/Logs";
#if !TARGET_OS_IOS
        // ANOMALY_STATE_ROOT=<dir> keeps everything the app writes (appdata with saves, user.ltx, screenshots and the
        // engine log; caches; shell log) under <dir>/{Application Support,Caches,Logs}, so scripted runs do not share
        // user.ltx, saves and logs with other instances.
        if (const char* root = getenv("ANOMALY_STATE_ROOT"); root && *root)
        {
            const std::string r = Normalize(root);
            app_support = r + "/Application Support";
            caches = r + "/Caches";
            logs_root = r + "/Logs";
        }
#endif
        p.data_root = ResolveDataRoot(params, documents, p.data_root_source);

        p.app_support = app_support + "/Anomaly";
        p.appdata = p.app_support + "/appdata";
        p.logs = p.appdata + "/logs";
        p.saves = p.appdata + "/savedgames";
        p.caches = caches + "/Anomaly";
        p.shader_cache = p.caches + "/shaders";
        p.lua_cache = p.caches + "/lua";
#if TARGET_OS_IOS
        p.shell_dir = documents;
#else
        p.shell_dir = logs_root + "/Anomaly";
#endif
        p.shell_log = p.shell_dir + "/anomaly-shell.log";
        p.crash_log = p.shell_dir + "/anomaly-crash.log";
        p.crash_log_previous = p.shell_dir + "/anomaly-crash.prev.log";
        p.fsgame_ltx = p.app_support + "/fsgame.ltx";
        p.overlay_source = ResolveOverlaySource(params);
        p.overlay_archive_dir = p.caches + "/overlay";

        bool ok = true;
        for (const std::string& dir : {p.app_support, p.appdata, p.logs, p.saves, p.caches, p.shader_cache, p.lua_cache, p.shell_dir})
            ok = MakeDirs(dir) && ok;
        // ANOMALY_CLEAR_SHADER_CACHE=1 starts with an empty shader cache (every shader is translated again): for cold-start tests,
        // on the iPad through `devicectl ... --environment-variables`, where there is no state root to point somewhere else.
        const char* clear_cache = getenv("ANOMALY_CLEAR_SHADER_CACHE");
        const bool cleared_shader_cache = clear_cache && *clear_cache && *clear_cache != '0';
        if (cleared_shader_cache)
        {
            [NSFileManager.defaultManager removeItemAtPath:[NSString stringWithUTF8String:p.shader_cache.c_str()] error:nil];
            ok = MakeDirs(p.shader_cache) && ok;
        }
#if TARGET_OS_IOS
        // Make Documents/Anomaly exist so it shows up in Files and Finder, and keep 17 GB out of backups.
        if (p.data_root_source == DataRootSource::PlatformDefault && MakeDirs(p.data_root))
            ExcludeFromBackup(p.data_root);
#endif

        // Previous-session bookkeeping happens before this run overwrites anything.
        // A crash file from the last run moves aside, so "the previous run crashed" is true for one launch only.
        s.previous_crash_log = FileExists(p.crash_log);
        if (s.previous_crash_log)
            rename(p.crash_log.c_str(), p.crash_log_previous.c_str());
        s.session_file = p.app_support + "/session.state";
        const std::string last_state = ReadFile(s.session_file);
        s.previous_session_unexpected = last_state.rfind("running", 0) == 0;

        KeepPreviousEngineLogs(p.logs, p.shell_dir);
        Internal::OpenLog(p.shell_log, p.shell_dir + "/anomaly-shell.prev.log");
        Internal::RedirectSDLLog();
        Internal::SetCrashContext(std::string("app: ") + params.app_name + "  pid: " + std::to_string(getpid()));
        if (params.install_crash_handler)
        {
            Internal::InstallCrashHandler(p.crash_log);
            NSSetUncaughtExceptionHandler(&UncaughtObjCException);
        }
        Internal::WriteSessionState("running");

        Log("Anomaly shell starting: %s", params.app_name);
        Log("data root (%d): %s", int(p.data_root_source), p.data_root.c_str());
        Log("appdata: %s", p.appdata.c_str());
        Log("caches: %s", p.caches.c_str());
        if (cleared_shader_cache)
            Log("ANOMALY_CLEAR_SHADER_CACHE: shader cache emptied (%s)", p.shader_cache.c_str());
        Log("shell dir: %s", p.shell_dir.c_str());
        Log("HOME: %s", getenv("HOME") ? getenv("HOME") : "(unset)");
        Log("volume: file names are case-%s", VolumeIsCaseSensitive(p.caches) ? "sensitive" : "insensitive");
        if (s.previous_session_unexpected)
            Log("previous session ended unexpectedly (crash, jetsam or force quit)");
        if (s.previous_crash_log)
            Log("the previous run crashed, report kept in %s", p.crash_log_previous.c_str());

        DeviceInfo device = QueryDeviceInfo();
        Log("device %s, %s, %u cores, %s RAM, memory budget %s, storage free %s of %s", device.model.c_str(), device.os.c_str(),
            device.cpu_cores, FormatBytes(device.physical_memory).c_str(),
            device.has_memory_budget ? FormatBytes(device.available_memory).c_str() : "n/a",
            FormatBytes(device.storage_free_raw).c_str(), FormatBytes(device.storage_total).c_str());

        {
            // The device's graphics profile (GraphicsProfile.h). `--graphics-profile <id|none>` and `--unlocked-graphics` come from
            // commandline.txt (the engine reads the same file; iPad: Documents/, Mac: the app support folder), then from the command
            // line, which wins. `--device-preset none|ipad` is the old spelling.
            Graphics::Options options;
            std::vector<std::string> words;
            {
                std::istringstream file(ReadFile(
#if TARGET_OS_IOS
                    p.shell_dir
#else
                    p.app_support
#endif
                    + "/commandline.txt"));
                std::string line;
                while (std::getline(file, line))
                {
                    const size_t first = line.find_first_not_of(" \t\r");
                    if (first == std::string::npos || line[first] == '#')
                        continue;
                    std::istringstream tokens(line);
                    for (std::string word; tokens >> word;)
                        words.push_back(word);
                }
            }
            for (int i = 1; params.argv && i < params.argc; ++i)
                words.push_back(params.argv[i]);
            for (size_t i = 0; i < words.size(); ++i)
            {
                if (words[i] == "--unlocked-graphics")
                    options.unlocked = true;
                else if (i + 1 < words.size() && words[i] == "--graphics-profile")
                    options.profile = words[i + 1];
                else if (i + 1 < words.size() && words[i] == "--device-preset")
                    options.profile = words[i + 1] == "none" ? "none" : words[i + 1] == "ipad" ? "ipad" : options.profile;
            }
            Graphics::Init(Internal::DetectHardware(), options);
            Log("%s", Graphics::Describe().c_str());
            if (Graphics::Enabled() && !Graphics::CommandLine().empty())
                Log("graphics profile: engine command line '%s'", Graphics::CommandLine().c_str());
        }

        // The generated fsgame.ltx mounts the archive only when this run packed (or found current) the one for the overlay it
        // uses: an archive left in the cache by another build or a failed run must not be mounted.
        if (!p.overlay_source.empty())
        {
            if (!Internal::BuildOverlayArchive(p.overlay_source, p.overlay_archive_dir))
            {
                Log("overlay: nothing packed from %s, not mounted", p.overlay_source.c_str());
                p.overlay_archive_dir.clear();
            }
        }
        else
        {
            Log("overlay: none (no Resources/overlay in the bundle, no --overlay): the Monolith scripts and shaders are missing");
            p.overlay_archive_dir.clear();
        }
        WriteFsGameLtx();
        Internal::StartObservers();
        Internal::RecordEvent("shell", std::string("started, data ") + (FileExists(p.data_root + "/db") ? "folder present" : "folder missing"));
        Internal::RecordEvent("thermal", std::string("initial state ") + ToString(device.thermal) + (device.low_power_mode ? ", Low Power Mode on" : ""));
        SetKeepScreenAwake(params.keep_screen_awake);
        s.initialized = true;
        return ok;
    }
}

void Shutdown()
{
    auto& s = State();
    if (!s.initialized)
        return;
    Log("Anomaly shell shutting down");
    Internal::StopObservers();
    Internal::WriteSessionState("clean");
    Internal::RestoreSDLLog();
    Internal::CloseLog();
    s.initialized = false;
}

bool IsInitialized()
{
    return State().initialized;
}

const std::string& DevicePresetCommandLine()
{
    return Graphics::CommandLine();
}

const Paths& GetPaths()
{
    return State().paths;
}

bool WriteFsGameLtx()
{
    const Paths& p = State().paths;
    if (p.fsgame_ltx.empty())
        return false;
    const bool ok = WriteFileIfChanged(p.fsgame_ltx, BuildFsGameLtx(p));
    Log("fsgame.ltx %s: %s", ok ? "ready" : "WRITE FAILED", p.fsgame_ltx.c_str());
    return ok;
}

bool PreviousSessionEndedUnexpectedly()
{
    return State().previous_session_unexpected;
}

bool PreviousCrashLogExists()
{
    return State().previous_crash_log;
}

std::string DataCopyInstructions(const Paths& paths)
{
    return ValidateGameData(paths.data_root).help;
}

} // namespace AppleShell
