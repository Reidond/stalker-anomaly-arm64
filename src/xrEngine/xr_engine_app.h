#pragma once
// xrEngine as an Apple application: the replacement of WinMain (x_ray.cpp, `xr_engine::*`).
//
// The app (the launcher/`anomaly` target, tools/xrengine_smoke) owns `main`. With SDL3 callbacks it is a few lines:
//
//   SDL_AppInit:     SDL_Init(...); AppleShell::Init(...); [launcher until the user presses Start];
//                    xr_engine::Init(params)            // returns once the first frame can run
//   SDL_AppIterate:  AppleShell::Update(); xr_engine::Frame() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS
//   SDL_AppEvent:    nothing: xr_engine::Frame() pumps the SDL queue itself (input hooks in through
//                    xr_window::AddEventHandler)
//   SDL_AppQuit:     xr_engine::Shutdown(); AppleShell::Shutdown()
//
// Init() does what WinMain_impl did, in the same order: command line, Core and the file system on the shell's generated
// fsgame.ltx, settings, engine, input, console, renderer list, the renderer and game "DLLs" (static libraries here),
// the device (window, swap chain), the application object and the game's persistent object, then the first half of
// the main loop (Device.RunBegin). It registers an AppleShell listener that maps pause/resume, memory pressure and
// thermal state onto the device. Nothing in here calls into the launcher: the caller decides what to show first.

#include "../Common/Platform.h"

#ifdef XR_PLATFORM_APPLE

#include <string>

union SDL_Event;

#ifndef ENGINE_API
# define ENGINE_API
#endif

namespace xr_engine
{
struct InitParams
{
	int argc = 0;
	char** argv = nullptr;
	// Extra command line arguments (as typed in a commandline.txt), appended after argv. Empty: read the file.
	const char* extra_command_line = nullptr;
	// Skip the Documents/commandline.txt lookup (tools that pass everything themselves).
	bool ignore_commandline_file = false;
	// Stop after the console and settings (user.ltx) are up and before the renderer is loaded: what the smoke test
	// uses while the other modules are not linked yet. The window exists then.
	bool stop_before_renderer = false;
};

enum class InitResult
{
	Ok,
	ShellMissing,      // AppleShell::Init has not run
	GameDataInvalid,   // AppleShell::ValidateGameData says the archives are not all there; `error` has the summary
	StoppedBeforeRenderer, // stop_before_renderer was requested and reached
};

// SDL_Init and AppleShell::Init must have run. Fills `error` with a one-line reason when the result is not Ok.
ENGINE_API InitResult Init(const InitParams& params, std::string& error);

// One iteration: events, the shell's per-frame update and the engine's on_idle. False once the game asked to quit.
ENGINE_API bool Frame();

// The second half of the main loop and the teardown WinMain did after Device.Run(). Safe to call without Init.
ENGINE_API void Shutdown();

ENGINE_API bool IsRunning();

// An event the app received in SDL_AppEvent. SDL3's callback loop takes every event out of the queue before the engine's
// frame (Frame() pumps only what is left, which is nothing outside the long loads), so the app has to hand them over.
ENGINE_API void HandleEvent(const SDL_Event& event);

// Runs a console command as if typed into the console (`start server(all/single/alife/new) client(localhost)`, `screenshot`,
// `quit`, ...). Main thread, after Init() returned Ok.
ENGINE_API void ExecuteConsole(const char* command);

// The engine window (an SDL_Window*, also Device.m_hWnd) and its CAMetalLayer, for tools that draw before the renderer
// exists. MetalLayer() registers the layer with the D3D11 shim under the window handle, as the renderer expects.
ENGINE_API void* WindowHandle();
ENGINE_API void* WindowMetalLayer();

// The command line the engine saw (Core.Params), for diagnostics.
ENGINE_API const char* CommandLine();
} // namespace xr_engine

#endif // XR_PLATFORM_APPLE
