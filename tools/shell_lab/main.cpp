// Shell lab: runs the Apple app shell on its own so it can be checked on a Mac and on the iPad.
//
// Shows the game-data status, the device memory budget, thermal state, storage, display info and a
// live list of lifecycle / memory / thermal / power events. Two buttons stress the memory callbacks.
//
// Launch arguments (pass them with `devicectl device process launch ... -- <args>`):
//   --data-root <path>   macOS: where the game data lives
//   --print-report       print one machine-readable line on stdout and exit (used by push_gamedata.sh)
//   --exit-after <sec>   quit after that many seconds, so runs on a shared iPad stay short
//   --stress             allocate memory automatically until the budget watchdog reports Critical
//   --screenshot <name>  save a PNG of the launcher after ~1.5 s into the shell directory
//   --crash-after <sec>  crash on purpose (null write) to check the crash file; see anomaly-crash.log

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <Platform/Apple/Launcher.h>
#include <Platform/Apple/Shell.h>

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr size_t kChunkBytes = 128ull << 20;

struct Chunk
{
    void* base;
};

struct LabState
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<AppleShell::Launcher> launcher;
    std::vector<Chunk> chunks;
    AppleShell::ListenerId listener = 0;

    int allocate_button = 0, free_button = 0;
    int pending_chunks = 0; // chunks still to allocate, one per frame
    bool stress = false;
    uint64_t exit_after_ms = 0;
    uint64_t crash_after_ms = 0;
    std::string screenshot_name;
    bool screenshot_requested = false;
    uint64_t start_ticks = 0;
    AppleShell::MemoryLevel level = AppleShell::MemoryLevel::Normal;
};

void AllocateChunk(LabState& lab)
{
    void* p = mmap(nullptr, kChunkBytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED)
    {
        AppleShell::Log("lab: mmap failed");
        lab.pending_chunks = 0;
        return;
    }
    memset(p, 0xA5, kChunkBytes); // touch every page so it counts against the jetsam budget
    lab.chunks.push_back({p});
}

void FreeAll(LabState& lab)
{
    for (Chunk& c : lab.chunks)
        munmap(c.base, kChunkBytes);
    lab.chunks.clear();
    lab.pending_chunks = 0;
    lab.stress = false;
}

void PrintReport()
{
    const AppleShell::Paths& paths = AppleShell::GetPaths();
    const AppleShell::GameDataStatus data = AppleShell::ValidateGameData(paths.data_root);
    const AppleShell::DeviceInfo device = AppleShell::QueryDeviceInfo();
    // The report is space-separated key=value, so values must not contain spaces.
    std::string os = device.os;
    for (char& c : os)
        if (c == ' ')
            c = '_';
    const char* state = data.state == AppleShell::DataState::Ok ? "ok" : data.state == AppleShell::DataState::Incomplete ? "incomplete" : "missing";
    // One line, key=value, easy to grep out of devicectl's console output.
    printf("ANOMALY_REPORT data=%s files_ok=%u files_expected=%u bytes_present=%llu bytes_expected=%llu "
           "storage_free=%llu storage_free_important=%llu storage_total=%llu memory_available=%llu memory_physical=%llu "
           "model=%s os=%s data_root=%s\n",
        state, data.files_ok, data.files_expected, (unsigned long long)data.bytes_present, (unsigned long long)data.bytes_expected,
        (unsigned long long)device.storage_free_raw, (unsigned long long)device.storage_free_important,
        (unsigned long long)device.storage_total, (unsigned long long)device.available_memory,
        (unsigned long long)device.physical_memory, device.model.c_str(), os.c_str(), paths.data_root.c_str());
    fflush(stdout);
}
} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    SDL_SetAppMetadata("Anomaly Shell Lab", "0.1", "com.andriishafar.anomaly");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    AppleShell::InitParams params;
    params.app_name = "Anomaly Shell Lab";
    params.argc = argc;
    params.argv = argv;
    AppleShell::Init(params);

    auto* lab = new LabState;
    *appstate = lab;
    lab->start_ticks = SDL_GetTicks();

    bool report = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--print-report"))
            report = true;
        else if (!strcmp(argv[i], "--stress"))
            lab->stress = true;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc)
            lab->screenshot_name = argv[++i];
        else if (!strcmp(argv[i], "--crash-after") && i + 1 < argc)
            lab->crash_after_ms = uint64_t(atof(argv[++i]) * 1000.0);
        else if (!strcmp(argv[i], "--exit-after") && i + 1 < argc)
            lab->exit_after_ms = uint64_t(atof(argv[++i]) * 1000.0);
    }
    if (report)
    {
        PrintReport();
        return SDL_APP_SUCCESS;
    }

    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL;
#if defined(SDL_PLATFORM_IOS)
    flags |= SDL_WINDOW_FULLSCREEN;
#else
    flags |= SDL_WINDOW_RESIZABLE;
#endif
    if (!SDL_CreateWindowAndRenderer("Anomaly Shell Lab", 1280, 800, flags, &lab->window, &lab->renderer))
    {
        AppleShell::Log("window/renderer creation failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetRenderVSync(lab->renderer, 1);

    AppleShell::Launcher::Config config;
    config.title = "STALKER Anomaly 1.5.3 - app shell lab";
#if !defined(SDL_PLATFORM_IOS)
    config.allow_quit = true;
#endif
    lab->launcher = std::make_unique<AppleShell::Launcher>(lab->window, lab->renderer, config);
    lab->allocate_button = lab->launcher->AddButton("Alloc 512 MiB");
    lab->free_button = lab->launcher->AddButton("Free memory");

    AppleShell::Listener listener;
    listener.on_memory = [lab](const AppleShell::MemoryEvent& e) { lab->level = e.level; };
    lab->listener = AppleShell::AddListener(std::move(listener));

    const AppleShell::DisplayInfo display = AppleShell::QueryDisplayInfo(lab->window);
    AppleShell::Log("display %s: native %dx%d, drawable %dx%d, window %.0fx%.0f pt, scale %.1f (native %.1f), %.0f Hz (max %.0f, ProMotion %s), "
                    "safe area L%.0f T%.0f R%.0f B%.0f pt",
        display.name.c_str(), display.native_width, display.native_height, display.drawable_width, display.drawable_height,
        display.window_width, display.window_height, display.scale, display.native_scale, display.refresh_hz, display.max_refresh_hz,
        display.promotion ? "yes" : "no", display.safe_area_points.left, display.safe_area_points.top, display.safe_area_points.right,
        display.safe_area_points.bottom);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    auto& lab = *static_cast<LabState*>(appstate);
    if (event->type == SDL_EVENT_QUIT)
        return SDL_APP_SUCCESS;

    switch (lab.launcher->HandleEvent(*event))
    {
    case AppleShell::Launcher::Action::Quit:
        return SDL_APP_SUCCESS;
    case AppleShell::Launcher::Action::StartGame:
        AppleShell::Log("lab: Start pressed - the engine would take over here");
        lab.launcher->SetExtraText("Start pressed: data is valid, the engine would take over here.");
        break;
    case AppleShell::Launcher::Action::Custom:
        if (lab.launcher->CustomButtonId() == lab.allocate_button)
            lab.pending_chunks += 4; // 512 MB
        else if (lab.launcher->CustomButtonId() == lab.free_button)
            FreeAll(lab);
        break;
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    auto& lab = *static_cast<LabState*>(appstate);
    if (lab.exit_after_ms && SDL_GetTicks() - lab.start_ticks > lab.exit_after_ms)
        return SDL_APP_SUCCESS;

    if (lab.crash_after_ms && SDL_GetTicks() - lab.start_ticks > lab.crash_after_ms)
    {
        AppleShell::Log("lab: crashing on purpose");
        *static_cast<volatile int*>(nullptr) = 1;
    }

    AppleShell::Update();

    // Allocate one chunk per frame so the memory callbacks can be watched as the budget shrinks.
    // Stop well before the kill: the point is to see Warning and Critical, not to be jetsammed.
    if (lab.stress && lab.pending_chunks == 0 && lab.level != AppleShell::MemoryLevel::Critical)
        lab.pending_chunks = 1;
    if (lab.pending_chunks > 0)
    {
        const AppleShell::DeviceInfo device = AppleShell::QueryDeviceInfo();
        const bool ios_budget = device.has_memory_budget;
        const uint64_t floor_bytes = ios_budget ? (192ull << 20) : 0;
        const bool mac_cap = !ios_budget && lab.chunks.size() * kChunkBytes >= (2048ull << 20);
        if ((ios_budget && device.available_memory < floor_bytes + kChunkBytes) || mac_cap)
        {
            lab.pending_chunks = 0;
            lab.stress = false;
            AppleShell::Log("lab: allocation stopped at %zu MB (safety floor)", lab.chunks.size() * (kChunkBytes >> 20));
        }
        else
        {
            AllocateChunk(lab);
            --lab.pending_chunks;
        }
    }
    lab.launcher->SetExtraText("Stress allocation: " + AppleShell::FormatBytes(lab.chunks.size() * kChunkBytes) + "  (level " +
        AppleShell::ToString(lab.level) + ")");

    if (!lab.screenshot_name.empty() && !lab.screenshot_requested && SDL_GetTicks() - lab.start_ticks > 1500)
    {
        lab.launcher->SaveScreenshotNextFrame(AppleShell::GetPaths().shell_dir + "/" + lab.screenshot_name);
        lab.screenshot_requested = true;
    }
    lab.launcher->Render();
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult /*result*/)
{
    auto* lab = static_cast<LabState*>(appstate);
    if (lab)
    {
        FreeAll(*lab);
        if (lab->listener)
            AppleShell::RemoveListener(lab->listener);
        lab->launcher.reset();
        delete lab;
    }
    AppleShell::Shutdown();
}
