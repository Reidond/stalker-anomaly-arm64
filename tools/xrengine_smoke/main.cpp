// xrEngine smoke test (macOS): links xrCore and xrEngine, brings the engine up through the Apple shell and the SDL3
// window, and runs engine init as far as the other modules allow.
//
//   xrengine_smoke [--data-root <path>] [--exit-after <seconds>] [--full] [--no-window-draw]
//
// Default: xr_engine::Init(stop_before_renderer): SDL + AppleShell, game-data validation, command line, Core and the
// file system on the generated fsgame.ltx, settings (system.ltx), the engine, the SDL window, input, the console and
// user.ltx. Then it stays up, drawing a changing colour into the window through the D3D11-on-Metal shim with
// CreateSwapChainForHwnd(Device.m_hWnd), until the window is closed or --exit-after elapses. --full tries the real
// thing (renderer, game, device): it needs every other module linked in.
//
// The exit code is 0 when every stage passed. Output is prefixed "xrengine_smoke:" so a script can grep for it.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <Platform/Apple/Shell.h>

#include "../../src/xrEngine/xr_engine_app.h"
#include "layer_check.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace
{
struct Smoke
{
    bool full = false;
    bool draw = true;
    double exit_after_s = 0;
    Uint64 start_ns = 0;
    unsigned frame = 0;
    int failures = 0;
    bool engine_up = false;
    std::unique_ptr<LayerCheck> layer;
};

void Report(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Report(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    printf("xrengine_smoke: %s\n", line);
    fflush(stdout);
    AppleShell::Log("xrengine_smoke: %s", line);
}

void Check(Smoke& s, bool ok, const char* what)
{
    Report("%s %s", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++s.failures;
}
} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv)
{
    auto* s = new Smoke;
    *appstate = s;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--full"))
            s->full = true;
        else if (!strcmp(argv[i], "--no-window-draw"))
            s->draw = false;
        else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc)
            s->exit_after_s = atof(argv[++i]);
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        Report("FAIL SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    AppleShell::InitParams shell;
    shell.argc = argc;
    shell.argv = argv;
    if (!AppleShell::Init(shell))
    {
        Report("FAIL AppleShell::Init");
        return SDL_APP_FAILURE;
    }
    const AppleShell::Paths& paths = AppleShell::GetPaths();
    Report("data root %s", paths.data_root.c_str());
    Report("appdata   %s", paths.appdata.c_str());
    Report("fsgame    %s", paths.fsgame_ltx.c_str());

    xr_engine::InitParams params;
    params.argc = argc;
    params.argv = argv;
    params.ignore_commandline_file = true;
    params.stop_before_renderer = !s->full;
    std::string error;
    const xr_engine::InitResult result = xr_engine::Init(params, error);
    const bool expected = s->full ? result == xr_engine::InitResult::Ok
                                  : result == xr_engine::InitResult::StoppedBeforeRenderer;
    Check(*s, expected, s->full ? "engine init (full)" : "engine init up to the renderer load");
    if (!expected)
    {
        Report("init result %d: %s", int(result), error.c_str());
        return SDL_APP_FAILURE;
    }
    Report("command line: '%s'", xr_engine::CommandLine());
    s->engine_up = true;

    if (s->draw && !s->full)
    {
        void* layer = xr_engine::WindowMetalLayer();
        Check(*s, layer != nullptr, "the window has a CAMetalLayer");
        s->layer = std::make_unique<LayerCheck>();
        int w = 0, h = 0;
        SDL_Window* window = static_cast<SDL_Window*>(xr_engine::WindowHandle());
        SDL_GetWindowSizeInPixels(window, &w, &h);
        const char* layer_error = "";
        const bool ok = layer && s->layer->Init(xr_engine::WindowHandle(), w, h, &layer_error);
        Check(*s, ok, "D3D11-on-Metal swap chain on the engine window (CreateSwapChainForHwnd)");
        if (!ok)
            Report("  %s", layer_error);
        else
            Report("window %dx%d px, drawing", w, h);
        SDL_SetWindowTitle(window, "xrEngine smoke: engine initialised, renderer not loaded");
    }
    s->start_ns = SDL_GetTicksNS();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto* s = static_cast<Smoke*>(appstate);
    if (!xr_engine::Frame())
    {
        Report("quit requested");
        return s->failures ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
    }
    if (s->layer)
        s->layer->Frame(s->frame);
    ++s->frame;
    if (s->exit_after_s > 0 && double(SDL_GetTicksNS() - s->start_ns) * 1e-9 >= s->exit_after_s)
    {
        Report("%u frames in %.1f s", s->frame, double(SDL_GetTicksNS() - s->start_ns) * 1e-9);
        return s->failures ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    (void)appstate;
    (void)event; // xr_engine::Frame() drains the SDL queue
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    auto* s = static_cast<Smoke*>(appstate);
    if (!s)
        return;
    s->layer.reset(); // before the window goes
    if (s->engine_up)
        xr_engine::Shutdown();
    Report("%s", s->failures ? "FAILED" : "OK");
    AppleShell::Shutdown();
    delete s;
}
