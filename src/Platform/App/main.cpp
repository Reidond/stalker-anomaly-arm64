// The Anomaly app: SDL3 callbacks around xr_engine::Init / Frame / Shutdown (src/xrEngine/xr_engine_app.h).
//
//   anomaly [--data-root <path>] [--exit-after <seconds>] [--script <file>] [--show-launcher] [engine flags...]
//
// Everything that is not one of the app's own flags goes to the engine as its command line (the same flags the Windows
// build takes, e.g. -nointro, -ltx, -start, -nosound), after the contents of commandline.txt (xrEngine, XRENGINE.md).
// The launcher (game-data status and how to copy the data) is shown when the data is missing or incomplete, or with
// --show-launcher.
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
//   <t> log <text...>               a line in the shell log
//   <t> quit                        leave the app

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <Platform/Apple/Launcher.h>
#include <Platform/Apple/Shell.h>

#include "xrEngine/xr_engine_app.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct ScriptStep
{
    double at = 0; // seconds since the engine came up
    std::string command;
    std::string args;
    double release_at = -1; // key presses: when the matching key-up is due
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
    Uint64 quit_requested_ns = 0; // set when the app asked the engine to quit; it disconnects and unloads the level first

    // where the scripted pointer is, window points: motion events carry the position and the delta
    float pointer_x = 0, pointer_y = 0;
    std::vector<ScriptStep> script;
    size_t script_next = 0;
    std::vector<ScriptStep> pending_releases;
};

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

// Runs the steps that are due. Returns false once the script asked to quit.
bool RunScript(App& app)
{
    const double now = double(SDL_GetTicksNS() - app.engine_start_ns) * 1e-9;
    for (size_t i = 0; i < app.pending_releases.size();)
    {
        if (now >= app.pending_releases[i].release_at)
        {
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
            std::istringstream a(step.args);
            std::string name;
            std::getline(a, name, ' ');
            double ms = 80;
            a >> ms;
            PushKey(name, true);
            ScriptStep release;
            release.args = name;
            release.release_at = now + ms * 1e-3;
            app.pending_releases.push_back(release);
        }
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

bool StartEngine(App& app)
{
    xr_engine::InitParams params;
    params.argc = app.argc;
    params.argv = app.argv;
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
    *appstate = app;
    app->argc = argc;
    app->argv = argv;

    bool want_launcher = false;
    const char* script_path = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--show-launcher"))
            want_launcher = true;
        else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc)
            app->exit_after_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc)
            script_path = argv[++i];
    }
    if (script_path && !LoadScript(*app, script_path))
        return SDL_APP_FAILURE;

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
    AppleShell::Log("anomaly: data root %s", paths.data_root.c_str());
    AppleShell::Log("anomaly: appdata   %s", paths.appdata.c_str());

    const bool data_ok = AppleShell::ValidateGameData(paths.data_root).state == AppleShell::DataState::Ok;
    if (data_ok && !want_launcher)
        return StartEngine(*app) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
    return OpenLauncher(*app) ? SDL_APP_CONTINUE : SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto* app = static_cast<App*>(appstate);
    if (app->engine_up)
    {
        if (!xr_engine::Frame())
            return SDL_APP_SUCCESS;
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
