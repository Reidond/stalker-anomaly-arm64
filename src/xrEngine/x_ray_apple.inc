// Apple entry point of the engine: what WinMain/WinMain_impl did, driven from an SDL3 app (xr_engine_app.h).
//
// Included by x_ray.cpp (not compiled on its own): it needs the file-local helpers there (InitSettings, InitInput,
// InitConsole, InitEngine, Startup_Create/Destroy, compute_build_id, ...). The Windows-only parts of WinMain are not
// repeated: single-instance mutex, splash dialog, DPI awareness, sticky keys, SEH stack-overflow guard, launcher,
// OpenAutomate, benchmark mode.

#include "xr_engine_app.h"

#include <SDL3/SDL.h>
#include <Platform/Apple/Shell.h>

#include <cstdio>
#include <fstream>
#include <sstream>

extern BOOL DllMainXrPhysics(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved);
extern bool ignore_verify;

namespace
{
struct EngineApp
{
	bool core_created = false;  // Core._initialize has run (the file system is up)
	bool core_ready = false;    // settings/engine/input/console exist
	bool renderer_ready = false; // the renderer and the game are loaded and the device is created
	bool running = false;       // Device.RunBegin() has run and RunEnd() has not
	bool physics_attached = false;
	AppleShell::ListenerId listener = 0;
	std::string command_line;
};

EngineApp g_app;

std::string read_text_file(const std::string& path)
{
	std::ifstream in(path);
	if (!in)
		return {};
	std::string result, line;
	while (std::getline(in, line))
	{
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			line.pop_back();
		size_t first = line.find_first_not_of(" \t");
		if (first == std::string::npos || line[first] == '#')
			continue;
		if (!result.empty())
			result += ' ';
		result += line.substr(first);
	}
	return result;
}

// argv (minus the program name and the shell's own `--data-root <path>`), then the extra text. The engine looks for
// flags with strstr(Core.Params, "-flag"), so the program path (which may contain '-') is left out.
std::string build_command_line(const xr_engine::InitParams& params, const AppleShell::Paths& paths)
{
	std::string result;
	auto append = [&](const std::string& s)
	{
		if (s.empty())
			return;
		if (!result.empty())
			result += ' ';
		result += s;
	};
	for (int i = 1; i < params.argc; ++i)
	{
		if (!strcmp(params.argv[i], "--data-root"))
		{
			++i; // the shell consumed it and its value
			continue;
		}
		const bool has_space = strchr(params.argv[i], ' ') != nullptr;
		append(has_space ? std::string("\"") + params.argv[i] + "\"" : std::string(params.argv[i]));
	}
	if (params.extra_command_line)
		append(params.extra_command_line);
	else if (!params.ignore_commandline_file)
	{
		// iPad: Documents/commandline.txt (visible in the Files app); macOS: the same name in the app support folder.
#if defined(XR_PLATFORM_IOS)
		append(read_text_file(paths.shell_dir + "/commandline.txt"));
#else
		append(read_text_file(paths.app_support + "/commandline.txt"));
#endif
	}
	return result;
}

void on_app_event(const AppleShell::MemoryEvent& e)
{
	if (e.level >= AppleShell::MemoryLevel::Warning)
		Device.OnMemoryPressure(e.level == AppleShell::MemoryLevel::Critical ? 2 : 1);
}

void register_shell_listener()
{
	AppleShell::Listener l;
	l.on_pause = [] { Device.OnSystemPause(true); };
	l.on_resume = [] { Device.OnSystemPause(false); };
	l.on_memory = on_app_event;
	l.on_thermal = [](AppleShell::ThermalState now, AppleShell::ThermalState before)
	{
		Msg("* thermal state: %s (was %s)", AppleShell::ToString(now), AppleShell::ToString(before));
	};
	l.on_low_power = [](bool on) { Msg("* low power mode: %s", on ? "on" : "off"); };
	g_app.listener = AppleShell::AddListener(std::move(l));
}
} // namespace

namespace xr_engine
{
InitResult Init(const InitParams& params, std::string& error)
{
	if (!AppleShell::IsInitialized())
	{
		error = "AppleShell::Init has not run";
		return InitResult::ShellMissing;
	}
	if (g_app.core_ready)
		return g_app.renderer_ready ? InitResult::Ok : InitResult::StoppedBeforeRenderer;

	const AppleShell::Paths& paths = AppleShell::GetPaths();
	const AppleShell::GameDataStatus data = AppleShell::ValidateGameData(paths.data_root);
	if (data.state != AppleShell::DataState::Ok)
	{
		error = data.summary;
		return InitResult::GameDataInvalid;
	}

	xr_window::SetBackground(params.background);
	g_app.command_line = build_command_line(params, paths);
	AppleShell::Log("engine: command line '%s'", g_app.command_line.c_str());

	// Core._initialize, xrDebug and memory_monitor read the flags through GetCommandLine()
	xr_set_command_line(g_app.command_line.c_str());

	// ODE's allocator hooks (DllMain of the physics DLL on Windows)
	DllMainXrPhysics(NULL, 1 /*DLL_PROCESS_ATTACH*/, NULL);
	g_app.physics_attached = true;

	Debug._initialize(false);
	g_bIntroFinished = TRUE; // no intro video: InitEngine() waits for this
	g_sLaunchOnExit_app[0] = NULL;
	g_sLaunchOnExit_params[0] = NULL;
	compute_build_id();
	Core._initialize("xray", NULL, TRUE, paths.fsgame_ltx.c_str());
	g_app.core_created = true;


	InitSettings();
	Msg(XRAY_MONOLITH_VERSION);

	if (pSettings->line_exist("string_table", "no_native_input"))
	{
		xr_strcpy(Core.UserName, sizeof(Core.UserName), "Player");
		xr_strcpy(Core.CompName, sizeof(Core.CompName), "Computer");
	}

	FPU::m24r();
	InitEngine(); // Engine.Initialize(), Device.Initialize(): this creates the window
	InitInput();
	InitConsole();
	g_app.core_ready = true;
	register_shell_listener();

	ignore_verify = !strstr(Core.Params, "-dbgdev");
	Msg("command line %s", Core.Params);

	if (params.stop_before_renderer)
	{
		// user.ltx is normally executed by Startup(), after the renderer is selected; run it here so the console state
		// is the same as it would be at that point.
		execUserScript();
		// there is no device to do it: show the window, 1280x720 points
		const float density = xr_window::PixelDensity();
		xr_window::SetMode(xr_window::Windowed, unsigned(1280 * density), unsigned(720 * density));
		error = "stopped before the renderer was loaded (requested)";
		return InitResult::StoppedBeforeRenderer;
	}

	Engine.External.CreateRendererList();
	{
		CCC_LoadCFG_custom* pTmp = xr_new<CCC_LoadCFG_custom>("renderer ");
		pTmp->Execute(Console->ConfigFile);
		xr_delete(pTmp);
	}
	Engine.External.Initialize();
	Console->Execute("stat_memory");

	Startup_Create();
	Device.RunBegin();
	Device.SyncActiveState(); // the window's first focus event came before the device existed
	g_app.running = true;
	g_app.renderer_ready = true;
	return InitResult::Ok;
}

bool Frame()
{
	if (g_app.running)
	{
		AppleShell::Update();
		Device.RunFrame();
		return !xr_window::QuitRequested();
	}
	// Before the device exists (stop_before_renderer): keep the window alive and notice a close request.
	AppleShell::Update();
	xr_window::PumpEvents();
	return !(Engine.Event.Peek("KERNEL:quit") || xr_window::QuitRequested());
}

void Shutdown()
{
	if (g_app.listener)
	{
		AppleShell::RemoveListener(g_app.listener);
		g_app.listener = 0;
	}
	if (g_app.running)
	{
		g_app.running = false;
		Device.RunEnd();
	}
	if (g_app.renderer_ready)
	{
		g_app.renderer_ready = false;
		Startup_Destroy();
	}
	else if (g_app.core_ready)
	{
		// stopped early: undo what InitEngine/InitInput/InitConsole/InitSettings did. No cfg_save: the early stop is
		// for tools, and must not rewrite the player's user.ltx.
		destroyInput();
		destroySettings();
		Console->Destroy();
		xr_delete(Console);
		destroyEngine();
	}
	g_app.core_ready = false;
	if (g_app.core_created)
	{
		g_app.core_created = false;
		Core._destroy();
	}
	xr_window::Destroy();
}

bool IsRunning() { return g_app.running; }

void HandleEvent(const SDL_Event& event)
{
	if (g_app.core_ready)
		xr_window::DispatchEvent(event);
}

void ExecuteConsole(const char* command)
{
	if (Console && command)
		Console->Execute(command);
}

void* WindowHandle() { return xr_window::Handle(); }

void* WindowMetalLayer() { return xr_window::MetalLayer(); }

const char* CommandLine() { return g_app.command_line.c_str(); }
} // namespace xr_engine
