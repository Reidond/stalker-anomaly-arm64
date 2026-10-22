// The Anomaly app: SDL3 callbacks around xr_engine::Init / Frame / Shutdown (src/xrEngine/xr_engine_app.h).
//
//   anomaly [--data-root <path>] [--exit-after <seconds>] [--show-launcher] [engine flags...]
//
// Everything that is not one of the app's own flags goes to the engine as its command line (the same flags the Windows
// build takes, e.g. -nointro, -ltx, -start, -nosound), after the contents of commandline.txt (xrEngine, XRENGINE.md).
// The launcher (game-data status and how to copy the data) is shown when the data is missing or incomplete, or with
// --show-launcher.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <Platform/Apple/Launcher.h>
#include <Platform/Apple/Shell.h>

#include "xrEngine/xr_engine_app.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace
{
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
};

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
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--show-launcher"))
            want_launcher = true;
        else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc)
            app->exit_after_s = atof(argv[++i]);
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
        if (app->exit_after_s > 0 && double(SDL_GetTicksNS() - app->engine_start_ns) * 1e-9 >= app->exit_after_s)
        {
            AppleShell::Log("anomaly: --exit-after %.1f s reached", app->exit_after_s);
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
        return SDL_APP_CONTINUE; // xr_engine::Frame() drains the SDL queue itself
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
