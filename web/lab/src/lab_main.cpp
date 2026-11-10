// Browser lab entry point. Built with -sPROXY_TO_PTHREAD, so SDL_main (and everything below) runs on
// a pthread, not the page's main thread, the way the engine will. SDL3's main callbacks drive one
// frame per requestAnimationFrame on that worker.
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <emscripten.h>
#include <emscripten/threading.h>

#include "gpu_lab.h"
#include "input_lab.h"
#include "lab_report.h"
#include "storage_lab.h"

namespace
{
constexpr int kWidth = 960;
constexpr int kHeight = 540;

// Commands from the page: a single-producer (page) single-consumer (app thread) ring.
constexpr int kCmdCapacity = 64;
int32_t g_cmd_write = 0;
int32_t g_cmd_read = 0;
int32_t g_cmds[kCmdCapacity][2];

SDL_Window* g_window = nullptr;
} // namespace

// Called on the page's main thread.
extern "C" EMSCRIPTEN_KEEPALIVE void lab_command(int cmd, int arg)
{
    const int32_t w = __atomic_load_n(&g_cmd_write, __ATOMIC_ACQUIRE);
    if (w - __atomic_load_n(&g_cmd_read, __ATOMIC_ACQUIRE) >= kCmdCapacity)
        return;
    g_cmds[w % kCmdCapacity][0] = cmd;
    g_cmds[w % kCmdCapacity][1] = arg;
    __atomic_store_n(&g_cmd_write, w + 1, __ATOMIC_RELEASE);
}

SDL_AppResult SDL_AppInit(void**, int, char**)
{
    lab::Report("thread.app", emscripten_is_main_browser_thread() ? "fail" : "pass",
                lab::Fmt("app code runs on %s (PROXY_TO_PTHREAD), %d hardware threads reported",
                         emscripten_is_main_browser_thread() ? "the page's main thread" : "a worker pthread",
                         emscripten_num_logical_cores()));

    SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, "#canvas");
    const double t0 = lab::NowMs();
    if (!SDL_Init(SDL_INIT_VIDEO))
        lab::Report("input.sdl_init", "fail", std::string("SDL_Init(VIDEO) on the worker failed: ") + SDL_GetError());
    else
    {
        g_window = SDL_CreateWindow("Anomaly browser lab", kWidth, kHeight, 0);
        if (!g_window)
            lab::Report("input.sdl_init", "fail", std::string("SDL_CreateWindow on the worker failed: ") + SDL_GetError());
        else
        {
            const int v = SDL_GetVersion();
            lab::Report("input.sdl_init", "pass",
                        lab::Fmt("SDL %d.%d.%d video driver '%s' initialised on the worker in %.1f ms; window bound to #canvas",
                                 SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), SDL_GetCurrentVideoDriver(),
                                 lab::NowMs() - t0));
        }
    }
    lab::input::Init(g_window);
    lab::gpu::Init("#canvas", kWidth, kHeight);
    lab::storage::Start();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void*, SDL_Event* event)
{
    lab::input::OnSdlEvent(*event);
    return SDL_APP_CONTINUE; // the lab never quits on its own (Esc and window close are test inputs)
}

SDL_AppResult SDL_AppIterate(void*)
{
    for (;;)
    {
        const int32_t r = __atomic_load_n(&g_cmd_read, __ATOMIC_ACQUIRE);
        if (r == __atomic_load_n(&g_cmd_write, __ATOMIC_ACQUIRE))
            break;
        const int cmd = g_cmds[r % kCmdCapacity][0], arg = g_cmds[r % kCmdCapacity][1];
        __atomic_store_n(&g_cmd_read, r + 1, __ATOMIC_RELEASE);
        if (cmd == 5)
            lab::gpu::RequestBlockTest(arg > 0 ? arg : 3000);
        else
            lab::input::Command(cmd, arg);
    }
    lab::input::Frame();
    lab::gpu::Frame();
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void*, SDL_AppResult) {}
