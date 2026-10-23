#pragma once
// The engine side of the device graphics profile (src/Platform/Apple/GraphicsProfile.h, docs/ios-port/SETTINGS.md): the lock on
// console commands, the profile applied after user.ltx, and the console commands `gfx_mode`, `gfx_fps` and `gfx_profile`.
// Apple builds only; nothing here is compiled or called on Windows.

#include "../Common/Platform.h"

#ifdef XR_PLATFORM_APPLE

#ifndef ENGINE_API
# define ENGINE_API
#endif

namespace xr_gfx
{
// Registers gfx_mode, gfx_fps, gfx_profile (CCC_Register).
ENGINE_API void RegisterCommands();

// The lock (CConsole::ExecuteCommand): true when `command` is locked by the active profile and the caller is not the profile
// itself. The first refusal of each command is logged; the refused line does nothing.
ENGINE_API bool Refuse(const char* command, const char* args);

// execUserScript(): user.ltx is about to run / has run. While it runs the lock refuses stale values silently and counts them; at the
// end the profile is applied (once, after the player's file) and the summary is logged.
ENGINE_API void BeginUserConfig();
ENGINE_API void EndUserConfig();

// Runs the active profile's settings through the console with the lock off ("why" goes to the log).
ENGINE_API void ApplyProfile(const char* why);

// default_controls is the game's own list of starting values (configs/default_controls.ltx, also the options menu's "Default" button).
// It runs with this scope: it may set the locked settings the profile gives no value, so they start where the stock game starts,
// and it leaves the profile's own settings alone.
struct ENGINE_API DefaultsScope
{
	explicit DefaultsScope(const char* command);
	~DefaultsScope();
	bool active;
};

// For scripts (console_registrator_script.cpp): the lock is on / this command is locked.
ENGINE_API bool LocksActive();
ENGINE_API bool IsLocked(const char* command);
ENGINE_API const char* ProfileId();
ENGINE_API bool IsMac();
// The frame rate caps the display allows (30, 60, and 120 on a 120 Hz display).
ENGINE_API void FrameCaps(xr_vector<int>& out);
} // namespace xr_gfx

#endif // XR_PLATFORM_APPLE
