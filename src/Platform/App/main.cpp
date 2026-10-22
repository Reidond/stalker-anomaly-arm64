// The Anomaly app: SDL3 callbacks around xr_engine::Init / Frame / Shutdown (src/xrEngine/xr_engine_app.h).
//
//   anomaly [--data-root <path>] [--exit-after <seconds>] [--script <file>] [--show-launcher]
//           [--unattended [--size <w>x<h>] [--show-window]] [--foreground] [engine flags...]
//
// Everything that is not one of the app's own flags goes to the engine as its command line (the same flags the Windows
// build takes, e.g. -nointro, -ltx, -start, -nosound), after the contents of commandline.txt (xrEngine, XRENGINE.md).
// The launcher (game-data status and how to copy the data) is shown when the data is missing or incomplete, or with
// --show-launcher.
//
// --unattended (implied by --script and --exit-after, unless --foreground is given) is for runs nobody watches, on a Mac
// somebody is working on: the app stays in the background (SDL's MAC_BACKGROUND_APP and ACTIVATE_WHEN_* hints, set before
// SDL_Init: no dock icon, no focus, no new Space), the engine renders offscreen at --size (default 1280x720 pixels; no window
// is ever shown, --show-window gives it a windowed one in the background), does not pause when unfocused, sound goes to OpenAL Soft's null device, and App Nap is off so frame pacing stays real. The engine
// reads its settings from appdata/unattended.ltx (a copy of user.ltx plus those overrides, written at start) and saves them
// there, so the player's user.ltx is left alone.
//
// --script <file> replays a timeline against the running engine, for runs without a person at the keyboard (one step per
// line, `#` starts a comment, the time is seconds since the engine came up):
//
//   <t> console <command...>        Console->Execute: `start server(all/single/alife/new) client(localhost)`, `quit`, ...
//   <t> key <name> [ms]             SDL key (SDL_GetScancodeFromName: W, Space, Escape, F1, Left Shift ...) pressed for ms (80)
//   <t> keydown <name> | keyup <name>
//   <t> move <x> <y>                mouse pointer, window points
//   <t> relmove <dx> <dy>           relative mouse motion
//   <t> click <x> <y> [button]      move, press and release (1 left, 2 middle, 3 right)
//   <t> waitlevel [seconds [level]] hold the timeline until a level (that one, if named) is loaded and playing (default
//                                   limit 240 s); the steps after it run with t counted from that moment, so a scene can
//                                   follow a load of any length
//   <t> padon                       attach a virtual DualSense (to drive the controller layer without a person holding one)
//   <t> pad <button> [ms]           press a button for ms (80): cross circle square triangle l1 r1 l3 r3 create options touchpad ps
//                                   up down left right
//   <t> paddown <button> | padup <button>
//   <t> padaxis <axis> <value>      lx ly rx ry l2 r2, -1..1 (triggers 0..1); stays until changed
//   <t> ballast <MB>                allocate and touch that much memory (to provoke the memory warnings), in 64 MB chunks
//   <t> unballast                   free it again
//   <t> log <text...>               a line in the shell log
//   <t> quit                        leave the app

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <Platform/Apple/Launcher.h>
#include <Platform/Apple/Shell.h>

#include <mtl_d3d11/mtl_stats.h>

#include "Unattended.h"
#include "xrEngine/xr_engine_app.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Process-wide GPU time and frame counts of the Metal D3D11 shim (src/Platform/MetalD3D11/include/mtl_d3d11/mtl_d3d11.h)
namespace mtl_d3d11
{
uint64_t GpuTimeNsTotal();
uint64_t PresentCountTotal();
} // namespace mtl_d3d11

namespace
{
struct ScriptStep
{
    double at = 0; // seconds since the engine came up
    std::string command;
    std::string args;
    double release_at = -1; // key presses: when the matching key-up is due
    bool pad = false;       // the release is for a pad button
};

struct App
{
    // the launcher's own window, while it is up
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<AppleShell::Launcher> launcher;

    bool engine_up = false;
    int argc = 0;
    char** argv = nullptr;
    double exit_after_s = 0;
    Uint64 engine_start_ns = 0;
    int exit_code = 0;
    Uint64 fps_window_ns = 0; // start of the current frame-rate window
    unsigned fps_frames = 0;
    Uint64 frame_cpu_ns = 0;     // time spent inside xr_engine::Frame() in the current window
    Uint64 frame_cpu_max_ns = 0;
    Uint64 footprint_peak = 0;   // largest process footprint sampled in the current window
    Uint64 gpu_ns_mark = 0;      // mtl_d3d11::GpuTimeNsTotal() / PresentCountTotal() at the start of the window
    Uint64 present_mark = 0;
    Uint64 footprint_sample_ns = 0;
    Uint64 quit_requested_ns = 0; // set when the app asked the engine to quit; it disconnects and unloads the level first

    // where the scripted pointer is, window points: motion events carry the position and the delta
    float pointer_x = 0, pointer_y = 0;
    std::vector<ScriptStep> script;
    size_t script_next = 0;
    std::vector<ScriptStep> pending_releases;
    mtl_d3d11::RenderStats last_stats; // at the end of the previous 10 s window
    bool waiting_level = false; // a `waitlevel` step is holding the timeline
    double wait_started_s = 0, wait_limit_s = 0, wait_step_at = 0;
    std::string wait_level_name;
    std::vector<void*> ballast; // script `ballast`: memory held on purpose
    SDL_Joystick* virtual_pad = nullptr; // script `padon`

    AppleShell::ListenerId lifecycle_listener = 0;
    bool saving = false;            // a background save is under way (its frames must not start another)
    Uint64 last_background_save_ns = 0;

    bool unattended = false;
    bool show_window = false; // --show-window: unattended, but with a (background) window instead of offscreen rendering
    int unattended_width = 1280, unattended_height = 720; // pixels
    std::vector<std::string> engine_args; // argv plus what unattended mode adds
    std::vector<char*> engine_argv;
};

App* g_app = nullptr; // the lifecycle listener has no user pointer
std::string ReadFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Unattended mode's engine settings: user.ltx with windowed, size and keep-running-unfocused on top (later lines win),
// handed to the engine as `-ltx unattended.ltx`, which it loads and saves instead of user.ltx.
void PrepareUnattendedConfig(App& app, const AppleShell::Paths& paths)
{
    for (int i = 1; i < app.argc; ++i)
        if (!strcmp(app.argv[i], "-ltx"))
            return; // the caller chose its own
    std::string ltx = ReadFile(paths.appdata + "/user.ltx");
    if (!ltx.empty() && ltx.back() != '\n')
        ltx += '\n';
#if !defined(SDL_PLATFORM_IOS)
    ltx += "rs_screenmode windowed\n";
    ltx += "vid_mode " + std::to_string(app.unattended_width) + "x" + std::to_string(app.unattended_height) + "\n";
#endif
    ltx += "g_always_active on\n";
    const std::string path = paths.appdata + "/unattended.ltx";
    std::ofstream(path, std::ios::binary | std::ios::trunc) << ltx;
    app.engine_args.assign(app.argv, app.argv + app.argc);
    app.engine_args.push_back("-ltx");
    app.engine_args.push_back("unattended.ltx");
    for (std::string& arg : app.engine_args)
        app.engine_argv.push_back(arg.data());
    app.engine_argv.push_back(nullptr);
    app.argc = int(app.engine_args.size());
    app.argv = app.engine_argv.data();
    AppleShell::Log("anomaly: unattended: %dx%d windowed, settings in %s", app.unattended_width, app.unattended_height,
        path.c_str());
}

bool LoadScript(App& app, const char* path)
{
    std::ifstream in(path);
    if (!in)
    {
        AppleShell::Log("anomaly: cannot read script %s", path);
        return false;
    }
    std::string line;
    while (std::getline(in, line))
    {
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#')
            continue;
        std::istringstream words(line.substr(first));
        ScriptStep step;
        if (!(words >> step.at >> step.command))
            continue;
        std::getline(words, step.args);
        const size_t arg_start = step.args.find_first_not_of(" \t");
        step.args = arg_start == std::string::npos ? "" : step.args.substr(arg_start);
        // a trailing ` # comment` (not on console/log lines, whose text may hold a '#')
        if (step.command != "console" && step.command != "log")
        {
            const size_t hash = step.args.find('#');
            if (hash != std::string::npos && (hash == 0 || step.args[hash - 1] == ' ' || step.args[hash - 1] == '\t'))
                step.args.resize(hash);
        }
        while (!step.args.empty() && (step.args.back() == '\r' || step.args.back() == ' '))
            step.args.pop_back();
        app.script.push_back(step);
    }
    std::stable_sort(app.script.begin(), app.script.end(), [](const ScriptStep& a, const ScriptStep& b) { return a.at < b.at; });
    AppleShell::Log("anomaly: script %s, %zu steps", path, app.script.size());
    return true;
}

SDL_WindowID EngineWindowID()
{
    return SDL_GetWindowID(static_cast<SDL_Window*>(xr_engine::WindowHandle()));
}

void PushKey(const std::string& name, bool down)
{
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    if (code == SDL_SCANCODE_UNKNOWN)
    {
        AppleShell::Log("anomaly: script: unknown key '%s'", name.c_str());
        return;
    }
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.windowID = EngineWindowID();
    e.key.scancode = code;
    e.key.key = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
    e.key.down = down;
    SDL_PushEvent(&e);
}

void PushMotion(App& app, float x, float y)
{
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.timestamp = SDL_GetTicksNS();
    e.motion.windowID = EngineWindowID();
    e.motion.x = x;
    e.motion.y = y;
    e.motion.xrel = x - app.pointer_x;
    e.motion.yrel = y - app.pointer_y;
    app.pointer_x = x;
    app.pointer_y = y;
    SDL_PushEvent(&e);
}

void PushButton(int button, bool down, float x, float y)
{
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.timestamp = SDL_GetTicksNS();
    e.button.windowID = EngineWindowID();
    e.button.button = Uint8(button);
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
}

// Script pad: a virtual gamepad that SDL reports like a DualSense (the way tools/xrinput_smoke drives the controller layer)
bool AttachVirtualPad(App& app)
{
    if (app.virtual_pad)
        return true;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = 0xFFFFFFFFu;
    desc.axis_mask = 0xFFFFFFFFu;
    desc.name = "anomaly script pad";
    desc.vendor_id = 0x054C;
    desc.product_id = 0x0CE6; // DualSense
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    app.virtual_pad = id ? SDL_OpenJoystick(id) : nullptr;
    AppleShell::Log("anomaly: script: virtual pad %s", app.virtual_pad ? "attached" : SDL_GetError());
    return app.virtual_pad != nullptr;
}

int PadButtonFromName(const std::string& name)
{
    static const struct { const char* name; SDL_GamepadButton button; } table[] = {
        {"cross", SDL_GAMEPAD_BUTTON_SOUTH}, {"circle", SDL_GAMEPAD_BUTTON_EAST}, {"square", SDL_GAMEPAD_BUTTON_WEST},
        {"triangle", SDL_GAMEPAD_BUTTON_NORTH}, {"l1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {"r1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"l3", SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {"r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {"create", SDL_GAMEPAD_BUTTON_BACK}, {"options", SDL_GAMEPAD_BUTTON_START},
        {"touchpad", SDL_GAMEPAD_BUTTON_TOUCHPAD}, {"ps", SDL_GAMEPAD_BUTTON_GUIDE}, {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},
        {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    for (const auto& entry : table)
        if (name == entry.name)
            return int(entry.button);
    return -1;
}

void SetPadButton(App& app, const std::string& name, bool down)
{
    const int button = PadButtonFromName(name);
    if (!app.virtual_pad || button < 0)
    {
        AppleShell::Log("anomaly: script: no pad or unknown button '%s'", name.c_str());
        return;
    }
    SDL_SetJoystickVirtualButton(app.virtual_pad, button, down);
}

void SetPadAxis(App& app, const std::string& args)
{
    char name[16] = {};
    float value = 0;
    if (!app.virtual_pad || sscanf(args.c_str(), "%15s %f", name, &value) != 2)
    {
        AppleShell::Log("anomaly: script: padaxis needs a pad and '<axis> <value>'");
        return;
    }
    static const struct { const char* name; SDL_GamepadAxis axis; bool trigger; } table[] = {
        {"lx", SDL_GAMEPAD_AXIS_LEFTX, false}, {"ly", SDL_GAMEPAD_AXIS_LEFTY, false}, {"rx", SDL_GAMEPAD_AXIS_RIGHTX, false},
        {"ry", SDL_GAMEPAD_AXIS_RIGHTY, false}, {"l2", SDL_GAMEPAD_AXIS_LEFT_TRIGGER, true},
        {"r2", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, true},
    };
    for (const auto& entry : table)
        if (!strcmp(name, entry.name))
        {
            const float clamped = std::max(entry.trigger ? 0.f : -1.f, std::min(1.f, value));
            SDL_SetJoystickVirtualAxis(app.virtual_pad, entry.axis, Sint16(clamped * 32767.f));
            return;
        }
    AppleShell::Log("anomaly: script: unknown pad axis '%s'", name);
}

// Runs the steps that are due. Returns false once the script asked to quit.
bool RunScript(App& app)
{
    double now = double(SDL_GetTicksNS() - app.engine_start_ns) * 1e-9;
    if (app.waiting_level)
    {
        const bool timed_out = now - app.wait_started_s > app.wait_limit_s;
        if (!xr_engine::IsLevelReady(app.wait_level_name.c_str()) && !timed_out)
            return true;
        AppleShell::Log("anomaly: script: %s after %.1f s", timed_out ? "waitlevel timed out" : "level ready",
            now - app.wait_started_s);
        // later steps count from here: t of the waitlevel step is "now"
        app.engine_start_ns = SDL_GetTicksNS() - Uint64(app.wait_step_at * 1e9);
        app.waiting_level = false;
        now = app.wait_step_at;
    }
    for (size_t i = 0; i < app.pending_releases.size();)
    {
        if (now >= app.pending_releases[i].release_at)
        {
            if (app.pending_releases[i].pad)
                SetPadButton(app, app.pending_releases[i].args, false);
            else
                PushKey(app.pending_releases[i].args, false);
            app.pending_releases.erase(app.pending_releases.begin() + long(i));
        }
        else
            ++i;
    }
    while (app.script_next < app.script.size() && app.script[app.script_next].at <= now)
    {
        const ScriptStep& step = app.script[app.script_next++];
        AppleShell::Log("anomaly: script t=%.1f %s %s", now, step.command.c_str(), step.args.c_str());
        float x = 0, y = 0;
        int button = 1;
        if (step.command == "console")
            xr_engine::ExecuteConsole(step.args.c_str());
        else if (step.command == "key")
        {
            // `Left Shift 200`: only a trailing number is the duration, the rest (with its spaces) is the SDL key name
            std::string name = step.args;
            double ms = 80;
            const size_t last_space = name.find_last_of(' ');
            if (last_space != std::string::npos)
            {
                char* end = nullptr;
                const double value = strtod(name.c_str() + last_space + 1, &end);
                if (end && end != name.c_str() + last_space + 1 && *end == '\0')
                {
                    ms = value;
                    name.resize(last_space);
                }
            }
            PushKey(name, true);
            ScriptStep release;
            release.args = name;
            release.release_at = now + ms * 1e-3;
            app.pending_releases.push_back(release);
        }
        else if (step.command == "padon")
            AttachVirtualPad(app);
        else if (step.command == "pad")
        {
            std::string name = step.args;
            double ms = 80;
            const size_t last_space = name.find_last_of(' ');
            if (last_space != std::string::npos)
            {
                ms = atof(name.c_str() + last_space + 1);
                name.resize(last_space);
            }
            SetPadButton(app, name, true);
            ScriptStep release;
            release.args = name;
            release.pad = true;
            release.release_at = now + ms * 1e-3;
            app.pending_releases.push_back(release);
        }
        else if (step.command == "paddown")
            SetPadButton(app, step.args, true);
        else if (step.command == "padup")
            SetPadButton(app, step.args, false);
        else if (step.command == "padaxis")
            SetPadAxis(app, step.args);
        else if (step.command == "keydown")
            PushKey(step.args, true);
        else if (step.command == "keyup")
            PushKey(step.args, false);
        else if (step.command == "move" && sscanf(step.args.c_str(), "%f %f", &x, &y) == 2)
            PushMotion(app, x, y);
        else if (step.command == "relmove" && sscanf(step.args.c_str(), "%f %f", &x, &y) == 2)
            PushMotion(app, app.pointer_x + x, app.pointer_y + y);
        else if (step.command == "click" && sscanf(step.args.c_str(), "%f %f %d", &x, &y, &button) >= 2)
        {
            PushMotion(app, x, y);
            PushButton(button, true, x, y);
            PushButton(button, false, x, y);
        }
        else if (step.command == "waitlevel")
        {
            app.waiting_level = true;
            app.wait_started_s = now;
            app.wait_step_at = step.at;
            std::istringstream words(step.args);
            double limit = 0;
            app.wait_level_name.clear();
            words >> limit >> app.wait_level_name;
            app.wait_limit_s = limit > 0 ? limit : 240.0;
            return true;
        }
        else if (step.command == "ballast")
        {
            const long chunks = atol(step.args.c_str()) / 64;
            for (long i = 0; i < chunks; ++i)
            {
                void* block = malloc(64u << 20);
                if (!block)
                    break;
                memset(block, 0x5a, 64u << 20);
                app.ballast.push_back(block);
            }
            AppleShell::Log("anomaly: script: ballast %zu MB held, footprint %.0f MB, available %.0f MB", app.ballast.size() * 64,
                double(AppleShell::ProcessFootprint()) / 1048576.0, double(AppleShell::AvailableMemory()) / 1048576.0);
        }
        else if (step.command == "unballast")
        {
            for (void* block : app.ballast)
                free(block);
            app.ballast.clear();
            AppleShell::Log("anomaly: script: ballast freed, available %.0f MB", double(AppleShell::AvailableMemory()) / 1048576.0);
        }
        else if (step.command == "log")
            AppleShell::Log("anomaly: script: %s", step.args.c_str());
        else if (step.command == "quit")
            return false;
        else
            AppleShell::Log("anomaly: script: cannot run '%s %s'", step.command.c_str(), step.args.c_str());
    }
    return true;
}

// The way a user quits: the engine disconnects from the level and quits from its own event queue. Returns when the app should
// give up waiting for that (the engine did not finish in 30 s).
bool RequestQuit(App& app)
{
    if (!app.quit_requested_ns)
    {
        app.quit_requested_ns = SDL_GetTicksNS();
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
        return false;
    }
    return double(SDL_GetTicksNS() - app.quit_requested_ns) * 1e-9 > 30.0;
}

Uint64 FileTimeNs(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return 0;
    return Uint64(st.st_mtimespec.tv_sec) * 1000000000ull + Uint64(st.st_mtimespec.tv_nsec);
}

// The game is about to lose the screen (WillEnterBackground: also Control Center, alerts, the app switcher): save it, so that a
// jetsam kill or a force quit while it sits in the background loses nothing. The engine is still active at this point (this
// listener runs before the engine's own pause), so it runs frames here until the save file is written; GPU work is still allowed
// until DidEnterBackground. A save is skipped while loading, in the main menu, within 30 s of the previous one, and when the
// frames this needs would be re-entered.
void SaveForBackground(App& app)
{
    if (app.saving || !app.engine_up || !xr_engine::InGame())
        return;
    const Uint64 start_ns = SDL_GetTicksNS();
    if (app.last_background_save_ns && double(start_ns - app.last_background_save_ns) * 1e-9 < 30.0)
        return;
    app.saving = true;
    app.last_background_save_ns = start_ns;
    const std::string file = AppleShell::GetPaths().saves + "/ipad_autosave.scop";
    const Uint64 before = FileTimeNs(file);
    xr_engine::HoldActive(true); // the window's minimize event is pumped by the first Frame() and would stop the engine
    xr_engine::ExecuteConsole("save ipad_autosave");
    bool written = false;
    for (int frame = 0; frame < 200 && double(SDL_GetTicksNS() - start_ns) * 1e-9 < 3.0; ++frame)
    {
        if (!xr_engine::Frame())
            break;
        if (FileTimeNs(file) != before)
        {
            written = true;
            break;
        }
    }
    xr_engine::HoldActive(false);
    AppleShell::Log("anomaly: background save %s in %.2f s", written ? "written" : "NOT written", double(SDL_GetTicksNS() - start_ns) * 1e-9);
    app.saving = false;
}

bool StartEngine(App& app)
{
    // before the engine registers its own listener: the listeners run in the order they were added
    if (!app.lifecycle_listener)
    {
        AppleShell::Listener l;
        l.on_pause = [] { if (g_app) SaveForBackground(*g_app); };
        app.lifecycle_listener = AppleShell::AddListener(std::move(l));
    }
    xr_engine::InitParams params;
    params.argc = app.argc;
    params.argv = app.argv;
    params.background = app.unattended;
    params.offscreen = app.unattended && !app.show_window;
    std::string error;
    const Uint64 t0 = SDL_GetTicksNS();
    const xr_engine::InitResult result = xr_engine::Init(params, error);
    if (result != xr_engine::InitResult::Ok)
    {
        AppleShell::Log("anomaly: engine init failed (%d): %s", int(result), error.c_str());
        fprintf(stderr, "anomaly: engine init failed: %s\n", error.c_str());
        return false;
    }
    app.engine_start_ns = SDL_GetTicksNS();
    {
        // the engine's UI cursor starts in the middle of the window and follows motion deltas (relative mouse mode)
        int w = 0, h = 0;
        SDL_GetWindowSize(static_cast<SDL_Window*>(xr_engine::WindowHandle()), &w, &h);
        app.pointer_x = float(w) * 0.5f;
        app.pointer_y = float(h) * 0.5f;
    }
    AppleShell::Log("anomaly: engine up in %.2f s", double(app.engine_start_ns - t0) * 1e-9);
    app.engine_up = true;
    return true;
}

void CloseLauncher(App& app)
{
    app.launcher.reset();
    if (app.renderer)
        SDL_DestroyRenderer(app.renderer);
    if (app.window)
        SDL_DestroyWindow(app.window);
    app.renderer = nullptr;
    app.window = nullptr;
}

bool OpenLauncher(App& app)
{
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL;
#if defined(SDL_PLATFORM_IOS)
    flags |= SDL_WINDOW_FULLSCREEN;
#else
    flags |= SDL_WINDOW_RESIZABLE;
#endif
    if (!SDL_CreateWindowAndRenderer("Anomaly", 1280, 800, flags, &app.window, &app.renderer))
    {
        AppleShell::Log("anomaly: launcher window failed: %s", SDL_GetError());
        return false;
    }
    AppleShell::Launcher::Config config;
    config.allow_quit = true;
    app.launcher = std::make_unique<AppleShell::Launcher>(app.window, app.renderer, config);
    return true;
}
} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv)
{
    SDL_SetAppMetadata("Anomaly", "0.1", "com.andriishafar.anomaly");
    auto* app = new App;
    g_app = app;
    *appstate = app;
    app->argc = argc;
    app->argv = argv;

    bool want_launcher = false, unattended_flag = false, foreground = false;
    const char* script_path = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--show-launcher"))
            want_launcher = true;
        else if (!strcmp(argv[i], "--unattended"))
            unattended_flag = true;
        else if (!strcmp(argv[i], "--foreground"))
            foreground = true;
        else if (!strcmp(argv[i], "--show-window"))
            app->show_window = true;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)
            sscanf(argv[++i], "%dx%d", &app->unattended_width, &app->unattended_height);
        else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc)
            app->exit_after_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc)
            script_path = argv[++i];
    }
    app->unattended = !foreground && (unattended_flag || script_path || app->exit_after_s > 0);
#if defined(SDL_PLATFORM_IOS)
    // Unattended mode is for the Mac (no focus, no window, silent sound). On the iPad a scripted run stays an ordinary foreground run:
    // the engine must pause when the app leaves the screen (GPU work in the background kills the app), so `g_always_active` is wrong there.
    app->unattended = false;
#endif
    if (app->unattended)
    {
        // before SDL_Init: SDL reads the hints when it registers the application and creates windows
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
        setenv("ALSOFT_DRIVERS", "null", 0); // OpenAL Soft's silent device, read when the engine opens its first one
        anomaly_app::BeginUnattendedActivity();
        anomaly_app::SetBackgroundActivationPolicy(); // before SDL creates the NSApplication and its first window
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
    {
        fprintf(stderr, "anomaly: SDL_Init failed: %s\n", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    AppleShell::InitParams shell;
    shell.app_name = "Anomaly";
    shell.argc = argc;
    shell.argv = argv;
    if (!AppleShell::Init(shell))
    {
        fprintf(stderr, "anomaly: AppleShell::Init failed\n");
        return SDL_APP_FAILURE;
    }
    const AppleShell::Paths& paths = AppleShell::GetPaths();
    // a relative script path is looked up next to the shell's files too (iPad: Documents/, where devicectl copies it)
    if (script_path)
    {
        std::string resolved = script_path;
        if (resolved[0] != '/' && !std::ifstream(resolved))
            resolved = paths.shell_dir + "/" + script_path;
        if (!LoadScript(*app, resolved.c_str()))
            return SDL_APP_FAILURE;
    }
    AppleShell::Log("anomaly: data root %s", paths.data_root.c_str());
    AppleShell::Log("anomaly: appdata   %s", paths.appdata.c_str());
    if (app->unattended)
        PrepareUnattendedConfig(*app, paths);

    const bool data_ok = AppleShell::ValidateGameData(paths.data_root).state == AppleShell::DataState::Ok;
    if (data_ok && !want_launcher)
        return StartEngine(*app) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
    if (app->unattended)
    {
        // nobody is there to read a launcher: fail instead of waiting for a click
        AppleShell::Log("anomaly: unattended: the game data at %s is not usable, not showing the launcher", paths.data_root.c_str());
        fprintf(stderr, "anomaly: unattended: the game data at %s is not usable\n", paths.data_root.c_str());
        return SDL_APP_FAILURE;
    }
    return OpenLauncher(*app) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto* app = static_cast<App*>(appstate);
    if (app->engine_up)
    {
        const Uint64 frame_start_ns = SDL_GetTicksNS();
        if (!xr_engine::Frame())
            return SDL_APP_SUCCESS;
        // one line in the shell log every 10 s: frames per second of the main loop, the CPU time of a frame (time inside
        // Frame(): the rest of the frame interval is the wait for the GPU and the display), and the memory the OS charges us
        ++app->fps_frames;
        const Uint64 now_ns = SDL_GetTicksNS();
        app->frame_cpu_ns += now_ns - frame_start_ns;
        app->frame_cpu_max_ns = std::max<Uint64>(app->frame_cpu_max_ns, now_ns - frame_start_ns);
        if (now_ns - app->footprint_sample_ns >= 100000000ull)
        {
            app->footprint_sample_ns = now_ns;
            app->footprint_peak = std::max<Uint64>(app->footprint_peak, AppleShell::ProcessFootprint());
        }
        if (!app->fps_window_ns)
            app->fps_window_ns = now_ns;
        else if (now_ns - app->fps_window_ns >= 10000000000ull)
        {
            const double mb = 1.0 / (1024.0 * 1024.0);
            const Uint64 gpu_ns = mtl_d3d11::GpuTimeNsTotal(), presents = mtl_d3d11::PresentCountTotal();
            const double gpu_ms = presents > app->present_mark ? double(gpu_ns - app->gpu_ns_mark) * 1e-6 / double(presents - app->present_mark) : 0.0;
            app->gpu_ns_mark = gpu_ns;
            app->present_mark = presents;
            AppleShell::Log("anomaly: %.1f fps (%u frames in %.1f s), frame cpu %.1f ms avg %.1f max, gpu %.1f ms, footprint %.0f MB (peak %.0f), available %.0f MB, thermal %s",
                app->fps_frames * 1e9 / double(now_ns - app->fps_window_ns), app->fps_frames,
                double(now_ns - app->fps_window_ns) * 1e-9, double(app->frame_cpu_ns) * 1e-6 / app->fps_frames,
                double(app->frame_cpu_max_ns) * 1e-6, gpu_ms, double(AppleShell::ProcessFootprint()) * mb, double(app->footprint_peak) * mb,
                double(AppleShell::AvailableMemory()) * mb, AppleShell::ToString(AppleShell::CurrentThermalState()));
            // what the renderer cost over the same window: GPU time per frame, CPU time per frame (wall time minus the time
            // blocked on the GPU), draws and passes per frame, and the Metal memory
            mtl_d3d11::RenderStats st;
            if (mtl_d3d11::GetRenderStats(&st))
            {
                const double wall = double(now_ns - app->fps_window_ns) * 1e-9;
                const double frames = double(st.frames - app->last_stats.frames);
                if (frames > 0)
                    AppleShell::Log(
                        "anomaly: metal: gpu %.2f ms/frame, cpu %.2f ms/frame (blocked %.2f), %.0f draws, %.1f passes/frame; "
                        "textures %.0f MB (render targets %.0f MB), buffers %.0f MB, allocated %.0f MB",
                        (st.gpuSeconds - app->last_stats.gpuSeconds) * 1e3 / frames,
                        (wall - (st.blockedSeconds - app->last_stats.blockedSeconds)) * 1e3 / frames,
                        (st.blockedSeconds - app->last_stats.blockedSeconds) * 1e3 / frames,
                        double(st.draws - app->last_stats.draws) / frames,
                        double(st.renderPasses - app->last_stats.renderPasses) / frames, st.textureBytes / 1048576.0,
                        st.renderTargetBytes / 1048576.0, st.bufferBytes / 1048576.0, st.deviceAllocatedBytes / 1048576.0);
                app->last_stats = st;
            }
            app->fps_window_ns = now_ns;
            app->fps_frames = 0;
            app->frame_cpu_ns = app->frame_cpu_max_ns = app->footprint_peak = 0;
        }
        if (!app->quit_requested_ns && !app->script.empty() && !RunScript(*app))
            AppleShell::Log("anomaly: script asked to quit");
        else if (!app->quit_requested_ns && app->exit_after_s > 0 &&
                 double(SDL_GetTicksNS() - app->engine_start_ns) * 1e-9 >= app->exit_after_s)
            AppleShell::Log("anomaly: --exit-after %.1f s reached", app->exit_after_s);
        else if (!app->quit_requested_ns)
            return SDL_APP_CONTINUE;
        // a quit was just asked for, or is under way
        if (RequestQuit(*app))
        {
            AppleShell::Log("anomaly: the engine did not quit in 30 s, leaving");
            return SDL_APP_SUCCESS;
        }
        return SDL_APP_CONTINUE;
    }
    if (app->launcher)
    {
        AppleShell::Update();
        app->launcher->Render();
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    auto* app = static_cast<App*>(appstate);
    if (app->engine_up)
    {
        if (event->type == SDL_EVENT_TERMINATING)
        {
            // the system is ending the app (rare: a suspended app is killed without being told): leave the level through the engine's
            // own disconnect, for as long as the system allows
            AppleShell::Log("anomaly: terminating, disconnecting");
            const Uint64 t0 = SDL_GetTicksNS();
            RequestQuit(*app);
            while (double(SDL_GetTicksNS() - t0) * 1e-9 < 4.0 && xr_engine::Frame())
                ;
            AppleShell::Log("anomaly: disconnect %s", double(SDL_GetTicksNS() - t0) * 1e-9 < 4.0 ? "finished" : "timed out");
            return SDL_APP_SUCCESS;
        }
        // SDL3's callback loop has already taken the event out of the queue the engine would pump
        xr_engine::HandleEvent(*event);
        return SDL_APP_CONTINUE;
    }
    if (event->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;
    if (!app->launcher)
        return SDL_APP_CONTINUE;
    switch (app->launcher->HandleEvent(*event))
    {
    case AppleShell::Launcher::Action::StartGame:
        CloseLauncher(*app);
        return StartEngine(*app) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
    case AppleShell::Launcher::Action::Quit:
        return SDL_APP_SUCCESS;
    default:
        return SDL_APP_CONTINUE;
    }
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    auto* app = static_cast<App*>(appstate);
    if (!app)
        return;
    if (app->engine_up)
        xr_engine::Shutdown();
    CloseLauncher(*app);
    AppleShell::Log("anomaly: quit (%s)", result == SDL_APP_SUCCESS ? "ok" : "failure");
    AppleShell::Shutdown();
    delete app;
}
