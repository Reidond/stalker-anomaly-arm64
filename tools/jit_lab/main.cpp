// jit_lab: runs the LuaJIT benchmarks and the stability run, and shows the log on screen.
//
// Arguments (also read from the JITLAB_ARGS environment variable, split on spaces):
//   --scale=N --stability=SECONDS --region-mb=N --wait-ms=N --keep-debugger --probe-all
//   --no-brk --no-jit --verbose --no-bench --interp-stability --require-jit --stikdebug --exit --headless
// The full log is also written to jit_lab.log (Documents on iPadOS, the pref folder on macOS).

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <TargetConditionals.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "lab.h"

namespace
{
struct App
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Thread* worker = nullptr;

    std::mutex mutex;
    std::deque<std::string> lines;
    FILE* log_file = nullptr;

    jitlab::Config config;
    jitlab::Result result;
    std::atomic<bool> done{false};
    bool exit_when_done = false;
    bool headless = false;
};

constexpr size_t kMaxLines = 400;

void Log(App* app, const std::string& line)
{
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
    std::lock_guard<std::mutex> lock(app->mutex);
    if (app->log_file)
    {
        std::fprintf(app->log_file, "%s\n", line.c_str());
        std::fflush(app->log_file);
    }
    app->lines.push_back(line);
    while (app->lines.size() > kMaxLines)
        app->lines.pop_front();
}

int WorkerMain(void* data)
{
    auto* app = static_cast<App*>(data);
    app->result = jitlab::Run(app->config, [app](const std::string& line) { Log(app, line); });
    app->done = true;
    return 0;
}

std::vector<std::string> CollectArgs(int argc, char** argv)
{
    std::vector<std::string> args(argv, argv + argc);
    if (const char* env = std::getenv("JITLAB_ARGS"))
    {
        std::istringstream stream(env);
        std::string word;
        while (stream >> word)
            args.push_back(word);
    }
    return args;
}

std::string LogPath()
{
#if TARGET_OS_IOS
    const char* dir = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
#else
    const char* dir = SDL_GetPrefPath("andriishafar", "jit_lab");
#endif
    return std::string(dir ? dir : "") + "jit_lab.log";
}
} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    auto* app = new App;
    *appstate = app;

    std::vector<std::string> args = CollectArgs(argc, argv);
    std::vector<char*> raw;
    for (auto& a : args)
        raw.push_back(a.data());
    app->config = jitlab::ParseArgs(int(raw.size()), raw.data(), [app](const std::string& line) { Log(app, line); });
    bool stikdebug = false;
    for (auto& a : args)
    {
        if (a == "--exit")
            app->exit_when_done = true;
        else if (a == "--headless")
            app->headless = true;
        else if (a == "--stikdebug")
            stikdebug = true;
    }

    app->log_file = std::fopen(LogPath().c_str(), "w");
    Log(app, "log file: " + LogPath());

    if (app->headless)
    {
        // No window, no worker thread: run on the main thread's 8 MB stack and exit.
        app->result = jitlab::Run(app->config, [app](const std::string& line) { Log(app, line); });
        app->done = true;
        return app->result.passed ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }

    std::string stikdebug_url;
    if (stikdebug)
    {
        // Ask StikDebug to attach to this process and run its universal.js. The app then waits for
        // the debugger inside luajit_txm_init(). (Not tested on a device: StikDebug is not installed
        // on the test iPad.)
        if (app->config.wait_ms < 60000)
            app->config.wait_ms = 60000;
        stikdebug_url = std::string("stikdebug://enable-jit?bundle-id=") + JITLAB_BUNDLE_ID +
            "&pid=" + std::to_string(getpid()) + "&script-name=universal.js";
        Log(app, "opening " + stikdebug_url);
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        Log(app, std::string("SDL_Init failed: ") + SDL_GetError());
        return SDL_APP_FAILURE;
    }
    // A long run must not be interrupted by the screen locking.
    SDL_DisableScreenSaver();
    if (!SDL_CreateWindowAndRenderer("JIT Lab", 1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
            &app->window, &app->renderer))
    {
        Log(app, std::string("SDL_CreateWindowAndRenderer failed: ") + SDL_GetError());
        return SDL_APP_FAILURE;
    }

    if (!stikdebug_url.empty() && !SDL_OpenURL(stikdebug_url.c_str()))
        Log(app, std::string("could not open StikDebug: ") + SDL_GetError());

    // The JIT recorder and assembler recurse deeply: give the worker a generous stack.
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, reinterpret_cast<void*>(WorkerMain));
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "jitlab");
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, app);
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 16 * 1024 * 1024);
    app->worker = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!app->worker)
    {
        Log(app, std::string("could not start the worker thread: ") + SDL_GetError());
        return SDL_APP_FAILURE;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    (void)appstate;
    if (event->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto* app = static_cast<App*>(appstate);
    if (app->done && app->exit_when_done)
        return app->result.passed ? SDL_APP_SUCCESS : SDL_APP_FAILURE;

    int width = 0, height = 0;
    SDL_GetCurrentRenderOutputSize(app->renderer, &width, &height);
    const float scale = width >= 1800 ? 2.0f : 1.0f;
    SDL_SetRenderScale(app->renderer, scale, scale);
    SDL_SetRenderDrawColor(app->renderer, 14, 16, 20, 255);
    SDL_RenderClear(app->renderer);

    const int rows = int(float(height) / scale / 10.0f) - 2;
    {
        std::lock_guard<std::mutex> lock(app->mutex);
        const size_t first = app->lines.size() > size_t(rows) ? app->lines.size() - size_t(rows) : 0;
        SDL_SetRenderDrawColor(app->renderer, 220, 224, 230, 255);
        float y = 6;
        for (size_t i = first; i < app->lines.size(); ++i, y += 10)
            SDL_RenderDebugText(app->renderer, 6, y, app->lines[i].c_str());
    }
    SDL_SetRenderDrawColor(app->renderer, app->done ? (app->result.passed ? 90 : 240) : 200,
        app->done ? (app->result.passed ? 220 : 90) : 200, app->done ? 120 : 90, 255);
    SDL_RenderDebugText(app->renderer, 6, float(height) / scale - 14, app->done ? "finished" : "running...");
    SDL_RenderPresent(app->renderer);
    SDL_Delay(50);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    (void)result;
    auto* app = static_cast<App*>(appstate);
    if (!app)
        return;
    if (app->worker && !app->done)
    {
        // Leave a running benchmark behind: the process is exiting anyway.
        SDL_DetachThread(app->worker);
    }
    else if (app->worker)
    {
        SDL_WaitThread(app->worker, nullptr);
    }
    if (app->log_file)
        std::fclose(app->log_file);
    if (app->renderer)
        SDL_DestroyRenderer(app->renderer);
    if (app->window)
        SDL_DestroyWindow(app->window);
}
