// The Anomaly app: SDL3 callbacks around xr_engine::Init / Frame / Shutdown (src/xrEngine/xr_engine_app.h).
//
//   anomaly [--data-root <path>] [--exit-after <seconds>] [--script <file>] [--no-launcher] [--show-launcher]
//           [--launcher-script <file>] [--unattended [--size <w>x<h>] [--show-window]] [--foreground] [engine flags...]
//
// Everything that is not one of the app's own flags goes to the engine as its command line (the same flags the Windows
// build takes, e.g. -nointro, -ltx, -start, -nosound), after the contents of commandline.txt (xrEngine, XRENGINE.md).
// The launcher (src/Platform/Launcher, SwiftUI: Play, the mods, adding mods; also the game-data status and how to copy the data
// when it is missing or incomplete) is the first screen of every ordinary start. --script, --exit-after, --unattended and
// --no-launcher (or ANOMALY_NO_LAUNCHER=1) skip it and go straight to the engine, which mounts the mods of modlist.json as they
// are; --show-launcher shows it anyway. --launcher-script <file> drives the launcher with the script's pad commands (padon, pad,
// paddown, padup, padaxis, log, snapshot <name>, quit; the time is seconds since the launcher came up) and shows it, also on an
// unattended run (then its window is hidden); Play then continues into the engine in the same process.
//
// --unattended (implied by --script and --exit-after, unless --foreground is given) is for runs nobody watches, on a Mac
// somebody is working on: the app stays in the background (SDL's MAC_BACKGROUND_APP and ACTIVATE_WHEN_* hints, set before
// SDL_Init: no dock icon, no focus, no new Space), the engine renders offscreen at --size (default 1280x720 pixels; no window
// is ever shown, --show-window gives it a windowed one in the background), does not pause when unfocused, joystick events are not
// dropped for want of focus (the script pad), sound goes to OpenAL Soft's null device, and App Nap is off so frame pacing stays real. The engine
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
//   <t> touch <x> <y>               a finger tap (the touch pseudo-mouse: a click in a dialog, ignored in the world)
//   <t> click <x> <y> [button]      move, press and release (1 left, 2 middle, 3 right)
//   <t> waitlevel [seconds [level]] hold the timeline until a level (that one, if named) is loaded and playing (default
//                                   limit 240 s); the steps after it run with t counted from that moment, so a scene can
//                                   follow a load of any length
//   <t> padon [n]                   attach virtual pad n (1: a DualSense, 2: an Xbox Series pad; default 1) and drive it from now on;
//                                   again after `padoff` it is a reconnect (same guid, a new SDL instance)
//   <t> padoff [n]                  detach virtual pad n (default: the one being driven): a controller that powers off
//   <t> padsel <n>                  drive virtual pad n with the commands below
//   <t> pad <button> [ms]           press a button for ms (80): cross circle square triangle l1 r1 l3 r3 create options touchpad ps
//                                   up down left right
//   <t> paddown <button> | padup <button>
//   <t> padaxis <axis> <value>      lx ly rx ry l2 r2, -1..1 (triggers 0..1); stays until changed
//   <t> padtouch <x> <y> | padtouchup   the DualSense's touchpad finger (0..1 each way), for the trackpad cursor
//   The virtual pads log what the game asks of them (`script pad n: rumble|triggers rumble|led ...`, on change) in the shell log.
//   <t> lifecycle background|foreground|lowmemory
//                                   the app events iPadOS sends (WillEnter/DidEnter Background or Foreground, UIKit's memory warning), pushed
//                                   into SDL's queue: the shell's pause, background save and resume run, but the OS does not suspend the app
//   <t> audiointerrupt began|ended  iOS: post the audio-session interruption a call or an alarm would (sound pauses, then resumes)
//   <t> ballast <MB>                allocate and touch that much memory (to provoke the memory warnings), in 64 MB chunks
//   <t> unballast                   free it again
//   <t> log <text...>               a line in the shell log
//   <t> delete <path>               remove one file, or one empty folder, below the game folder (the data root): a relative path with no `..`, whose
//                                   first part is gamedata or db (any case); links are not followed, nothing is removed recursively. For test
//                                   runs that leave the game folder as they found it (tools/addons_test): a device has no way to delete a file from outside
//   <t> mods_delete <id>            remove <mods root>/<id> (only a plain folder name directly inside the mods root; symlinks are not followed)
//   <t> mods_clear                  remove every entry of the mods root; put it last, the mounted mods are still in use by the engine
//   <t> quit                        leave the app

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <filesystem>

#include <Platform/Apple/FramePacer.h>
#include <Platform/Apple/GraphicsProfile.h>
#include <Platform/Apple/Launcher.h>
#include <Platform/Apple/Shell.h>
#include <Platform/Launcher/AnomalyLauncher.h>

#include <mtl_d3d11/mtl_stats.h>

#include "Unattended.h"
#include "xrEngine/xr_engine_app.h"

#include <dispatch/dispatch.h>
#include <signal.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
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
    int pad_index = 0;      // ... of this virtual pad (the one `padsel` had picked when the press was made)
};

struct App
{
    // the launcher's own window, while it is up: the SwiftUI launcher is hosted in it (swift_launcher), or, when that cannot be
    // done, the shell's plain launcher draws into it (renderer, launcher)
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<AppleShell::Launcher> launcher;
    void* swift_launcher = nullptr;
    std::vector<SDL_Gamepad*> launcher_pads; // opened while the launcher is up (its pad input)
    std::vector<ScriptStep> launcher_script; // --launcher-script
    size_t launcher_script_next = 0;
    std::vector<ScriptStep> launcher_releases;
    Uint64 launcher_start_ns = 0;

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
    AppleShell::FramePacer pacer; // frame rate cap (gfx_fps): when the next frame may start
    // The time between the starts of consecutive frames in the current 10 s window, in the buckets of kIntervalEdgesMs (logged with the
    // frame rate: a locked 40 fps on a 120 Hz panel is all in the 22 to 28 ms bucket, 40 fps on a 60 Hz one is split in two)
    static constexpr int kIntervalBuckets = 12;
    unsigned interval_hist[kIntervalBuckets] = {};
    Uint64 last_frame_start_ns = 0;
    Uint64 interval_max_ns = 0;

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
    // script `padon`: up to two virtual pads (0: DualSense, 1: Xbox Series), `padsel` picks the one the pad commands drive
    struct VirtualPad
    {
        SDL_Joystick* joystick = nullptr;
        SDL_JoystickID id = 0;
        Uint16 rumble_low = 0, rumble_high = 0, rumble_left = 0, rumble_right = 0;
        Uint8 led[3] = {0, 0, 0};
    };
    VirtualPad virtual_pads[2];
    int pad_sel = 0;

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

bool LoadScript(std::vector<ScriptStep>& steps, const char* path)
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
        steps.push_back(step);
    }
    std::stable_sort(steps.begin(), steps.end(), [](const ScriptStep& a, const ScriptStep& b) { return a.at < b.at; });
    AppleShell::Log("anomaly: script %s, %zu steps", path, steps.size());
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

void PushMotion(App& app, float x, float y, SDL_MouseID which = 0)
{
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.which = which;
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

void PushButton(int button, bool down, float x, float y, SDL_MouseID which = 0)
{
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.which = which;
    e.button.timestamp = SDL_GetTicksNS();
    e.button.windowID = EngineWindowID();
    e.button.button = Uint8(button);
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = x;
    e.button.y = y;
    SDL_PushEvent(&e);
}

// Script `lifecycle`: the events SDL turns UIKit's notifications into. The shell's watcher sees them as it sees the real ones.
void PushAppEvents(std::initializer_list<Uint32> types)
{
    for (Uint32 type : types)
    {
        SDL_Event e;
        SDL_zero(e);
        e.type = type;
        e.common.timestamp = SDL_GetTicksNS();
        SDL_PushEvent(&e);
    }
}

// Script pad: a virtual gamepad that SDL reports like a DualSense (the way tools/xrinput_smoke drives the controller layer).
// Its rumble, trigger rumble and light bar callbacks log what the game sends (on change), which is how a Mac run checks the
// feedback calls: the real motors are the user's to judge.
App::VirtualPad& SelectedPad(App& app) { return app.virtual_pads[app.pad_sel]; }

bool PadRumble(void* user, Uint16 low, Uint16 high)
{
    auto* pad = static_cast<App::VirtualPad*>(user);
    if (pad->rumble_low != low || pad->rumble_high != high)
    {
        pad->rumble_low = low;
        pad->rumble_high = high;
        AppleShell::Log("anomaly: script pad %d: rumble low %u (%.2f) high %u (%.2f)", int(pad->id), unsigned(low), low / 65535.0,
            unsigned(high), high / 65535.0);
    }
    return true;
}

bool PadRumbleTriggers(void* user, Uint16 left, Uint16 right)
{
    auto* pad = static_cast<App::VirtualPad*>(user);
    if (pad->rumble_left != left || pad->rumble_right != right)
    {
        pad->rumble_left = left;
        pad->rumble_right = right;
        AppleShell::Log("anomaly: script pad %d: trigger rumble left %u right %u", int(pad->id), unsigned(left), unsigned(right));
    }
    return true;
}

bool PadSetLed(void* user, Uint8 r, Uint8 g, Uint8 b)
{
    auto* pad = static_cast<App::VirtualPad*>(user);
    if (pad->led[0] != r || pad->led[1] != g || pad->led[2] != b)
    {
        pad->led[0] = r;
        pad->led[1] = g;
        pad->led[2] = b;
        AppleShell::Log("anomaly: script pad %d: led %u %u %u", int(pad->id), unsigned(r), unsigned(g), unsigned(b));
    }
    return true;
}

bool AttachVirtualPad(App& app, int index)
{
    App::VirtualPad& pad = app.virtual_pads[index];
    app.pad_sel = index;
    if (pad.joystick)
        return true;
    static const SDL_VirtualJoystickTouchpadDesc touchpad = {2, {0, 0, 0}};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = 0xFFFFFFFFu;
    desc.axis_mask = 0xFFFFFFFFu;
    if (index == 0)
    {
        desc.name = "anomaly script pad";
        desc.vendor_id = 0x054C;
        desc.product_id = 0x0CE6; // DualSense
        desc.ntouchpads = 1;
        desc.touchpads = &touchpad;
    }
    else
    {
        desc.name = "anomaly script pad 2";
        desc.vendor_id = 0x045E;
        desc.product_id = 0x0B13; // Xbox Series
    }
    desc.userdata = &pad;
    desc.Rumble = PadRumble;
    desc.RumbleTriggers = PadRumbleTriggers;
    desc.SetLED = PadSetLed;
    pad.id = SDL_AttachVirtualJoystick(&desc);
    pad.joystick = pad.id ? SDL_OpenJoystick(pad.id) : nullptr;
    pad.rumble_low = pad.rumble_high = pad.rumble_left = pad.rumble_right = 0;
    pad.led[0] = pad.led[1] = pad.led[2] = 0;
    AppleShell::Log("anomaly: script: virtual pad %d %s", index + 1, pad.joystick ? "attached" : SDL_GetError());
    return pad.joystick != nullptr;
}

void DetachVirtualPad(App& app, int index)
{
    App::VirtualPad& pad = app.virtual_pads[index];
    if (!pad.joystick)
        return;
    SDL_CloseJoystick(pad.joystick);
    SDL_DetachVirtualJoystick(pad.id);
    AppleShell::Log("anomaly: script: virtual pad %d detached", index + 1);
    pad.joystick = nullptr;
    pad.id = 0;
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

// `index` is the virtual pad (0 or 1); -1 is the one being driven. A release for a pad that was detached in the meantime has nothing left to do.
void SetPadButton(App& app, const std::string& name, bool down, int index = -1)
{
    const int button = PadButtonFromName(name);
    SDL_Joystick* joystick = app.virtual_pads[index < 0 ? app.pad_sel : index].joystick;
    if (!joystick || button < 0)
    {
        if (down || button < 0)
            AppleShell::Log("anomaly: script: no pad or unknown button '%s'", name.c_str());
        return;
    }
    SDL_SetJoystickVirtualButton(joystick, button, down);
}

void SetPadAxis(App& app, const std::string& args)
{
    char name[16] = {};
    float value = 0;
    if (!SelectedPad(app).joystick || sscanf(args.c_str(), "%15s %f", name, &value) != 2)
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
            SDL_SetJoystickVirtualAxis(SelectedPad(app).joystick, entry.axis, Sint16(clamped * 32767.f));
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
                SetPadButton(app, app.pending_releases[i].args, false, app.pending_releases[i].pad_index);
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
            AttachVirtualPad(app, atoi(step.args.c_str()) == 2 ? 1 : 0);
        else if (step.command == "padoff")
            DetachVirtualPad(app, step.args.empty() ? app.pad_sel : (atoi(step.args.c_str()) == 2 ? 1 : 0));
        else if (step.command == "padsel")
            app.pad_sel = atoi(step.args.c_str()) == 2 ? 1 : 0;
        else if (step.command == "padtouch" && sscanf(step.args.c_str(), "%f %f", &x, &y) == 2)
            SDL_SetJoystickVirtualTouchpad(SelectedPad(app).joystick, 0, 0, true, x, y, 1.f);
        else if (step.command == "padtouchup")
            SDL_SetJoystickVirtualTouchpad(SelectedPad(app).joystick, 0, 0, false, 0.f, 0.f, 0.f);
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
            release.pad_index = app.pad_sel;
            release.release_at = now + ms * 1e-3;
            app.pending_releases.push_back(release);
        }
        else if (step.command == "paddown")
            SetPadButton(app, step.args, true);
        else if (step.command == "padup")
            SetPadButton(app, step.args, false);
        else if (step.command == "padaxis")
            SetPadAxis(app, step.args);
        else if (step.command == "lifecycle")
        {
            if (step.args == "background")
                PushAppEvents({SDL_EVENT_WILL_ENTER_BACKGROUND, SDL_EVENT_DID_ENTER_BACKGROUND});
            else if (step.args == "foreground")
                PushAppEvents({SDL_EVENT_WILL_ENTER_FOREGROUND, SDL_EVENT_DID_ENTER_FOREGROUND});
            else if (step.args == "lowmemory")
                PushAppEvents({SDL_EVENT_LOW_MEMORY});
            else
                AppleShell::Log("anomaly: script: lifecycle needs background, foreground or lowmemory");
        }
        else if (step.command == "audiointerrupt")
            anomaly_app::PostAudioInterruption(step.args.compare(0, 5, "began") == 0);
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
        else if (step.command == "touch" && sscanf(step.args.c_str(), "%f %f", &x, &y) == 2)
        {
            // a finger tap as SDL reports it on iPadOS: the touch pseudo-mouse moves to the finger, presses and lets go
            PushMotion(app, x, y, SDL_TOUCH_MOUSEID);
            PushButton(1, true, x, y, SDL_TOUCH_MOUSEID);
            PushButton(1, false, x, y, SDL_TOUCH_MOUSEID);
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
        else if (step.command == "delete")
        {
            namespace fs = std::filesystem;
            const fs::path rel(step.args);
            bool allowed = !step.args.empty() && rel.is_relative();
            for (const fs::path& part : rel)
                if (part == ".." || part == ".")
                    allowed = false;
            std::string top = allowed && !rel.empty() ? rel.begin()->string() : std::string();
            for (char& c : top)
                c = char(tolower((unsigned char)c));
            if (top != "gamedata" && top != "db")
                allowed = false;
            const fs::path target = fs::path(AppleShell::GetPaths().data_root) / rel;
            std::error_code ec;
            const fs::file_status status = fs::symlink_status(target, ec);
            if (!allowed)
                AppleShell::Log("anomaly: script: delete '%s' refused: a relative path below gamedata or db is required", step.args.c_str());
            else if (ec || !fs::exists(status))
                AppleShell::Log("anomaly: script: delete %s: not there", step.args.c_str());
            else if (fs::is_symlink(status))
                AppleShell::Log("anomaly: script: delete %s refused: a link", step.args.c_str());
            else if (fs::remove(target, ec))
                AppleShell::Log("anomaly: script: removed %s", step.args.c_str());
            else
                AppleShell::Log("anomaly: script: kept %s: %s", step.args.c_str(), ec ? ec.message().c_str() : "not removed");
        }
        else if (step.command == "mods_delete" || step.command == "mods_clear")
        {
            // test runs that leave the mods folder as they found it (the iPad has no way to delete a file from outside): these act on the
            // shell's own mods root only, and only when it is a folder named Mods with a modlist.json or nothing but mod folders in it
            const AppleShell::ModsRemoval result =
                step.command == "mods_clear" ? AppleShell::ClearModsRoot() : AppleShell::RemoveModFolder(step.args);
            for (const std::string& path : result.removed)
                AppleShell::Log("anomaly: script: removed %s", path.c_str());
            for (const std::string& why : result.refused)
                AppleShell::Log("anomaly: script: %s refused: %s", step.command.c_str(), why.c_str());
            AppleShell::Log("anomaly: script: %s: %zu removed, %zu refused", step.command.c_str(), result.removed.size(), result.refused.size());
        }
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

// SIGTERM (what `devicectl ... terminate`, the system and a `kill` send). SDL turns it into SDL_EVENT_QUIT, but only when its event pump runs,
// and an app in the background is not iterated at its display link's pace. While the app is paused in the background the engine cannot
// answer a quit either (it does not run its event queue, and frames are not allowed): the disconnect it would quit through never
// happens. (Once the system has suspended the app nothing runs at all and the system's SIGKILL, 31 s later, is what ends it.) The game
// was saved when the app left the screen, so the main queue ends it at once through the same exit as a normal quit, also when the
// signal arrives while the app is waking up. Otherwise SDL's own behaviour: a quit event.
void OnTerminateSignal(void*)
{
    App* const app = g_app;
    if (app && app->engine_up && AppleShell::IsPaused())
    {
        AppleShell::Log("anomaly: SIGTERM while in the background, leaving without the disconnect");
        g_app = nullptr;
        SDL_AppQuit(app, SDL_APP_SUCCESS);
        exit(0);
    }
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

dispatch_source_t g_terminate_source = nullptr;

void InstallTerminateSource()
{
    signal(SIGTERM, SIG_IGN); // the dispatch source sees it, the default action (and SDL's handler) does not
    g_terminate_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler_f(g_terminate_source, OnTerminateSignal);
    dispatch_resume(g_terminate_source);
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

// ---- the launcher -------------------------------------------------------------------------------------------------------------

// Where the mods live (the shared contract): iPad Documents/Mods, Mac ~/Library/Application Support/Anomaly/Mods; ANOMALY_MODS_ROOT
// overrides both, and a run with ANOMALY_STATE_ROOT uses <state>/Mods. Takes AppleShell::Paths::mods_root when the shell has one.
template <class P>
auto ModsRootOf(const P& paths, int) -> decltype(paths.mods_root, std::string())
{
    return paths.mods_root;
}

template <class P>
std::string ModsRootOf(const P& paths, long)
{
    if (const char* env = getenv("ANOMALY_MODS_ROOT"); env && *env)
        return env;
#if defined(SDL_PLATFORM_IOS)
    return paths.shell_dir + "/Mods"; // Documents, which the Files app shows
#else
    if (const char* state = getenv("ANOMALY_STATE_ROOT"); state && *state)
        return std::string(state) + "/Mods";
    return paths.app_support + "/Mods";
#endif
}

void JsonString(std::string& out, const std::string& text)
{
    out += '"';
    for (const unsigned char c : text)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buffer[8];
                snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            }
            else
                out += char(c);
        }
    }
    out += '"';
}

// The launcher asks for the game-data check again (Rescan, and every 2 s while it is not usable): the shell's validation as JSON.
char* DataStatusJson(void*)
{
    const AppleShell::GameDataStatus status = AppleShell::ValidateGameData(AppleShell::GetPaths().data_root);
    std::string json = "{\"state\":";
    JsonString(json, status.state == AppleShell::DataState::Ok ? "ok" : status.state == AppleShell::DataState::Incomplete ? "incomplete" : "notfound");
    json += ",\"summary\":";
    JsonString(json, status.summary);
    json += ",\"help\":";
    JsonString(json, status.help);
    json += ",\"root\":";
    JsonString(json, status.root);
    json += ",\"files_ok\":" + std::to_string(status.files_ok) + ",\"files_expected\":" + std::to_string(status.files_expected) + "}";
    return strdup(json.c_str());
}

std::string LauncherConfigJson(const AppleShell::Paths& paths)
{
    const std::string mods_root = ModsRootOf(paths, 0);
    const AppleShell::DeviceInfo device = AppleShell::QueryDeviceInfo();
    std::string json = "{\"mods_root\":";
    JsonString(json, mods_root);
    json += ",\"downloads_dir\":";
    JsonString(json, mods_root + "/.downloads");
#if defined(SDL_PLATFORM_IOS)
    json += ",\"platform\":\"ios\",\"allow_quit\":false";
#else
    json += ",\"platform\":\"macos\",\"allow_quit\":true";
#endif
    json += ",\"version\":";
    JsonString(json, "0.1");
    json += ",\"device_line\":";
    JsonString(json, device.model + " · " + device.os);
    json += ",\"notices\":[";
    bool first = true;
    auto notice = [&](const char* text) {
        json += first ? "" : ",";
        first = false;
        JsonString(json, text);
    };
    if (AppleShell::PreviousCrashLogExists())
        notice("The previous run crashed: the report is in anomaly-crash.prev.log");
    else if (AppleShell::PreviousSessionEndedUnexpectedly())
        notice("The previous session ended unexpectedly (crash, memory kill or force quit)");
    json += "]";
    if (const char* auto_play = getenv("ANOMALY_LAUNCHER_AUTOPLAY_MS"); auto_play && *auto_play)
        json += ",\"auto_play_ms\":" + std::to_string(atoi(auto_play));
    json += "}";
    return json;
}

void CloseLauncher(App& app)
{
    if (app.swift_launcher)
    {
        // the SwiftUI views come out of the window first; the window is destroyed after, and the engine makes its own
        anomaly_launcher_close(app.swift_launcher);
        app.swift_launcher = nullptr;
    }
    for (SDL_Gamepad* pad : app.launcher_pads)
        SDL_CloseGamepad(pad);
    app.launcher_pads.clear();
    if (!app.unattended)
        SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS); // only the launcher took pad input while another app was in front
    app.launcher.reset();
    if (app.renderer)
        SDL_DestroyRenderer(app.renderer);
    if (app.window)
        SDL_DestroyWindow(app.window);
    app.renderer = nullptr;
    app.window = nullptr;
}

// The SwiftUI launcher, hosted in a plain SDL window (no renderer, no Metal view: SwiftUI draws). If that fails, the shell's own
// launcher (SDL renderer, debug text) takes the window.
bool OpenLauncher(App& app)
{
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(SDL_PLATFORM_IOS)
    flags |= SDL_WINDOW_FULLSCREEN;
#else
    flags |= SDL_WINDOW_RESIZABLE;
    if (app.unattended)
        flags |= SDL_WINDOW_HIDDEN; // a scripted run (--launcher-script): nobody is there to see the window
#endif
    // the launcher takes pad input even while another app is in front (a Mac window that is not the key window)
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    app.window = SDL_CreateWindow("Anomaly", 1280, 800, flags);
    if (!app.window)
    {
        AppleShell::Log("anomaly: launcher window failed: %s", SDL_GetError());
        return false;
    }
#if !defined(SDL_PLATFORM_IOS)
    SDL_SetWindowMinimumSize(app.window, 960, 600);
#endif
    const SDL_PropertiesID properties = SDL_GetWindowProperties(app.window);
#if defined(SDL_PLATFORM_IOS)
    void* native = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
#else
    void* native = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#endif
    const std::string config = LauncherConfigJson(AppleShell::GetPaths());
    app.swift_launcher = native ? anomaly_launcher_open(native, config.c_str(), DataStatusJson, nullptr) : nullptr;
    if (app.swift_launcher)
    {
        app.launcher_start_ns = SDL_GetTicksNS();
        AppleShell::Log("anomaly: launcher up, mods in %s", ModsRootOf(AppleShell::GetPaths(), 0).c_str());
        return true;
    }
    AppleShell::Log("anomaly: the SwiftUI launcher could not be hosted, using the plain launcher");
    SDL_DestroyWindow(app.window);
    app.window = nullptr;
    SDL_WindowFlags plain = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL;
#if defined(SDL_PLATFORM_IOS)
    plain |= SDL_WINDOW_FULLSCREEN;
#else
    plain |= SDL_WINDOW_RESIZABLE;
#endif
    if (!SDL_CreateWindowAndRenderer("Anomaly", 1280, 800, plain, &app.window, &app.renderer))
    {
        AppleShell::Log("anomaly: launcher window failed: %s", SDL_GetError());
        return false;
    }
    AppleShell::Launcher::Config plain_config;
    plain_config.allow_quit = true;
    app.launcher = std::make_unique<AppleShell::Launcher>(app.window, app.renderer, plain_config);
    return true;
}

// SDL's gamepad and key events as the launcher's buttons. Returns true when the event was the launcher's.
bool LauncherEvent(App& app, const SDL_Event& event)
{
    auto send = [&](int button, bool down, bool keyboard) { anomaly_launcher_button(app.swift_launcher, button, down ? 1 : 0, keyboard ? 1 : 0); };
    switch (event.type)
    {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (SDL_Gamepad* pad = SDL_OpenGamepad(event.gdevice.which))
            app.launcher_pads.push_back(pad);
        return true;
    case SDL_EVENT_GAMEPAD_REMOVED:
        for (auto it = app.launcher_pads.begin(); it != app.launcher_pads.end(); ++it)
            if (SDL_GetGamepadID(*it) == event.gdevice.which)
            {
                SDL_CloseGamepad(*it);
                app.launcher_pads.erase(it);
                break;
            }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    {
        static const struct { SDL_GamepadButton sdl; int button; } map[] = {
            {SDL_GAMEPAD_BUTTON_DPAD_UP, ANOMALY_LAUNCHER_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ANOMALY_LAUNCHER_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ANOMALY_LAUNCHER_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ANOMALY_LAUNCHER_RIGHT},
            {SDL_GAMEPAD_BUTTON_SOUTH, ANOMALY_LAUNCHER_CONFIRM}, {SDL_GAMEPAD_BUTTON_EAST, ANOMALY_LAUNCHER_BACK},
            {SDL_GAMEPAD_BUTTON_WEST, ANOMALY_LAUNCHER_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, ANOMALY_LAUNCHER_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_START, ANOMALY_LAUNCHER_OPTIONS},
        };
        for (const auto& entry : map)
            if (event.gbutton.button == entry.sdl)
                send(entry.button, event.gbutton.down, false);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX)
            anomaly_launcher_axis(app.swift_launcher, 0, float(event.gaxis.value) / 32767.0f);
        else if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)
            anomaly_launcher_axis(app.swift_launcher, 1, float(event.gaxis.value) / 32767.0f);
        return true;
#if defined(SDL_PLATFORM_IOS)
    // a hardware keyboard on the iPad (on the Mac the launcher reads the keyboard itself, so a key is not handled twice)
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        if (event.key.repeat)
            return true;
        int button = -1;
        switch (event.key.key)
        {
        case SDLK_UP: button = ANOMALY_LAUNCHER_UP; break;
        case SDLK_DOWN: button = ANOMALY_LAUNCHER_DOWN; break;
        case SDLK_LEFT: button = ANOMALY_LAUNCHER_LEFT; break;
        case SDLK_RIGHT: button = ANOMALY_LAUNCHER_RIGHT; break;
        case SDLK_RETURN:
        case SDLK_SPACE: button = ANOMALY_LAUNCHER_CONFIRM; break;
        case SDLK_ESCAPE: button = ANOMALY_LAUNCHER_BACK; break;
        case SDLK_I: button = ANOMALY_LAUNCHER_TRIANGLE; break;
        case SDLK_P: button = ANOMALY_LAUNCHER_OPTIONS; break;
        default: break;
        }
        if (button >= 0)
            send(button, event.key.down, true);
        return true;
    }
#endif
    default:
        return false;
    }
}

// --launcher-script: the pad commands of the engine script, run against the launcher (t = seconds since it came up), so a pad
// can be driven through its screens with nobody holding one. `snapshot <name>` saves a PNG of the launcher next to the shell log.
void RunLauncherScript(App& app)
{
    const double now = double(SDL_GetTicksNS() - app.launcher_start_ns) * 1e-9;
    for (size_t i = 0; i < app.launcher_releases.size();)
    {
        if (now >= app.launcher_releases[i].release_at)
        {
            SetPadButton(app, app.launcher_releases[i].args, false, app.launcher_releases[i].pad_index);
            app.launcher_releases.erase(app.launcher_releases.begin() + long(i));
        }
        else
            ++i;
    }
    while (app.launcher_script_next < app.launcher_script.size() && app.launcher_script[app.launcher_script_next].at <= now)
    {
        const ScriptStep& step = app.launcher_script[app.launcher_script_next++];
        AppleShell::Log("anomaly: launcher script t=%.1f %s %s", now, step.command.c_str(), step.args.c_str());
        if (step.command == "padon")
            AttachVirtualPad(app, atoi(step.args.c_str()) == 2 ? 1 : 0);
        else if (step.command == "padoff")
            DetachVirtualPad(app, step.args.empty() ? app.pad_sel : (atoi(step.args.c_str()) == 2 ? 1 : 0));
        else if (step.command == "padsel")
            app.pad_sel = atoi(step.args.c_str()) == 2 ? 1 : 0;
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
            release.pad_index = app.pad_sel;
            release.release_at = now + ms * 1e-3;
            app.launcher_releases.push_back(release);
        }
        else if (step.command == "paddown")
            SetPadButton(app, step.args, true);
        else if (step.command == "padup")
            SetPadButton(app, step.args, false);
        else if (step.command == "padaxis")
            SetPadAxis(app, step.args);
        else if (step.command == "snapshot")
        {
            const std::string file = AppleShell::GetPaths().shell_dir + "/" + (step.args.empty() ? "launcher" : step.args) + ".png";
            AppleShell::Log("anomaly: launcher snapshot %s: %s", file.c_str(),
                app.swift_launcher && anomaly_launcher_snapshot(app.swift_launcher, file.c_str()) ? "saved" : "failed");
        }
        else if (step.command == "log")
            AppleShell::Log("anomaly: launcher script: %s", step.args.c_str());
        else if (step.command == "quit")
        {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        else
            AppleShell::Log("anomaly: launcher script: cannot run '%s %s'", step.command.c_str(), step.args.c_str());
    }
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

    bool want_launcher = false, unattended_flag = false, foreground = false, no_launcher = false;
    const char* script_path = nullptr;
    const char* launcher_script_path = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--show-launcher"))
            want_launcher = true;
        else if (!strcmp(argv[i], "--no-launcher"))
            no_launcher = true;
        else if (!strcmp(argv[i], "--launcher-script") && i + 1 < argc)
            launcher_script_path = argv[++i];
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
    // The launcher is the first screen of an ordinary start. Runs nobody watches (--script, --exit-after, --unattended) and
    // --no-launcher / ANOMALY_NO_LAUNCHER go straight to the engine, which mounts the mods of modlist.json as they are.
    // --show-launcher and --launcher-script show it regardless.
    if (const char* env = getenv("ANOMALY_NO_LAUNCHER"); env && *env && *env != '0')
        no_launcher = true;
    const bool automated = script_path || app->exit_after_s > 0 || unattended_flag;
    bool show_launcher = want_launcher || launcher_script_path || (!no_launcher && !automated);
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
        // the script's virtual pad is a joystick, and SDL drops joystick events while no window has the keyboard focus (always, here):
        // the pad would follow the script for a few frames and then stay at whatever it showed last
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
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
    auto resolve_script = [&](const char* name) {
        std::string resolved = name;
        if (resolved[0] != '/' && !std::ifstream(resolved))
            resolved = paths.shell_dir + "/" + name;
        return resolved;
    };
    if (script_path && !LoadScript(app->script, resolve_script(script_path).c_str()))
        return SDL_APP_FAILURE;
    if (launcher_script_path && !LoadScript(app->launcher_script, resolve_script(launcher_script_path).c_str()))
        return SDL_APP_FAILURE;
    AppleShell::Log("anomaly: build %s %s", __DATE__, __TIME__); // which binary a log belongs to (this file's compile time)
    AppleShell::Log("anomaly: data root %s", paths.data_root.c_str());
    AppleShell::Log("anomaly: appdata   %s", paths.appdata.c_str());
    if (app->unattended)
        PrepareUnattendedConfig(*app, paths);

    InstallTerminateSource();
    const bool data_ok = AppleShell::ValidateGameData(paths.data_root).state == AppleShell::DataState::Ok;
    // the shell reads the same flags (and commandline.txt): a run that asked to skip the launcher uses modlist.json as it is
    if (AppleShell::SkipLauncher() && !want_launcher && !launcher_script_path)
        show_launcher = false;
    if (data_ok && !show_launcher)
        return StartEngine(*app) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
    if ((app->unattended || !show_launcher) && !launcher_script_path)
    {
        // nobody is there to read a launcher: fail instead of waiting for a click
        AppleShell::Log("anomaly: %s: the game data at %s is not usable, not showing the launcher", app->unattended ? "unattended" : "no launcher",
                        paths.data_root.c_str());
        fprintf(stderr, "anomaly: the game data at %s is not usable (no launcher was asked for)\n", paths.data_root.c_str());
        return SDL_APP_FAILURE;
    }
    return OpenLauncher(*app) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto* app = static_cast<App*>(appstate);
    if (app->engine_up)
    {
        Uint64 frame_start_ns = SDL_GetTicksNS();
        // The frame rate cap of the device profile (gfx_fps; the engine's own limiter is a busy-wait and is compiled out on iOS).
        // Only in the game: the menus and loading run at the display's rate. On the iPad the display link calls at the display's rate, so
        // a tick that comes too early is skipped; on a Mac the loop is not paced by anything but the present, so it sleeps.
        const int cap = AppleShell::Graphics::EffectiveFrameCap();
        if (cap > 0 && xr_engine::InGame())
        {
#if defined(SDL_PLATFORM_IOS)
            const Uint64 skip_over_ns = 5000000ull; // the next display link tick is closer to the target than this one
#else
            const Uint64 skip_over_ns = 0;
#endif
            const AppleShell::FramePacer::Step step = app->pacer.Begin(frame_start_ns, 1000000000ull / Uint64(cap), skip_over_ns);
            if (!step.run)
                return SDL_APP_CONTINUE;
            if (step.sleep_ns)
            {
                SDL_DelayPrecise(step.sleep_ns);
                frame_start_ns = SDL_GetTicksNS();
            }
        }
        else
            app->pacer.Reset();
        if (app->last_frame_start_ns && frame_start_ns > app->last_frame_start_ns)
        {
            const Uint64 gap_ns = frame_start_ns - app->last_frame_start_ns;
            static const double edges_ms[App::kIntervalBuckets - 1] = {6, 11, 14, 19, 22, 28, 31, 37, 45, 55, 100};
            int bucket = 0;
            while (bucket < App::kIntervalBuckets - 1 && double(gap_ns) * 1e-6 >= edges_ms[bucket])
                ++bucket;
            ++app->interval_hist[bucket];
            app->interval_max_ns = std::max(app->interval_max_ns, gap_ns);
        }
        app->last_frame_start_ns = frame_start_ns;
        bool keep_going = true;
        AppleShell::WithAutoreleasePool([&] { keep_going = xr_engine::Frame(); });
        if (!keep_going)
        {
            // the last frame: the level is gone, what follows is xr_engine::Shutdown() (the "quit (ok)" line ends it)
            AppleShell::Log("anomaly: the engine asked to quit");
            return SDL_APP_SUCCESS;
        }
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
            {
                // the share of frames by time since the previous frame started: <6 | 6-11 (8.3) | 11-14 | 14-19 (16.7) | 19-22 | 22-28 (25) |
                // 28-31 | 31-37 (33.3) | 37-45 | 45-55 (50) | 55-100 | >=100 ms
                unsigned total = 0;
                for (unsigned n : app->interval_hist)
                    total += n;
                if (total)
                {
                    char text[256];
                    int len = 0;
                    for (int i = 0; i < App::kIntervalBuckets; ++i)
                        len += snprintf(text + len, sizeof(text) - len, "%s%.1f", i ? " " : "", 100.0 * app->interval_hist[i] / total);
                    AppleShell::Log("anomaly: frame intervals (%% of frames, ms: <6 6-11 11-14 14-19 19-22 22-28 28-31 31-37 37-45 45-55 55-100 >=100): %s; longest %.1f ms",
                        text, double(app->interval_max_ns) * 1e-6);
                }
                memset(app->interval_hist, 0, sizeof(app->interval_hist));
                app->interval_max_ns = 0;
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
    if (app->swift_launcher)
    {
        AppleShell::Update();
        if (!app->launcher_script.empty())
            RunLauncherScript(*app);
        switch (anomaly_launcher_result(app->swift_launcher))
        {
        case ANOMALY_LAUNCHER_PLAY:
            // The launcher has written modlist.json. Its views leave the window and the window goes; the engine then creates its own
            // (SDL window, Metal layer), in this same process.
            AppleShell::Log("anomaly: Play pressed in the launcher, starting the engine");
            CloseLauncher(*app);
            AppleShell::WriteFsGameLtx(); // the mod list changed since the shell started: the engine mounts what this file says
            return StartEngine(*app) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
        case ANOMALY_LAUNCHER_QUIT:
            return SDL_APP_SUCCESS;
        default:
            break;
        }
    }
    else if (app->launcher)
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
        if (event->type == SDL_EVENT_QUIT && AppleShell::IsPaused())
        {
            // A SIGTERM (or RequestQuit) while the app sits in the background: the engine is paused, it does not run its event
            // queue, so the disconnect it would quit through never happens, and RequestQuit's 30 s limit was all that ended the app
            // (31 s). Frames cannot run in the background either (GPU work there kills the app). The game was saved when the app
            // left the screen (SaveForBackground), so leave at once: SDL_AppQuit runs xr_engine::Shutdown().
            AppleShell::Log("anomaly: quit while in the background, leaving without the disconnect");
            return SDL_APP_SUCCESS;
        }
        // SDL3's callback loop has already taken the event out of the queue the engine would pump
        xr_engine::HandleEvent(*event);
        return SDL_APP_CONTINUE;
    }
    if (event->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;
    if (app->swift_launcher)
    {
        LauncherEvent(*app, *event);
        return SDL_APP_CONTINUE;
    }
    if (!app->launcher)
        return SDL_APP_CONTINUE;
    switch (app->launcher->HandleEvent(*event))
    {
    case AppleShell::Launcher::Action::StartGame:
        CloseLauncher(*app);
        AppleShell::WriteFsGameLtx(); // the data root or the mod list may have changed since the shell started: the engine mounts what this file says
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
