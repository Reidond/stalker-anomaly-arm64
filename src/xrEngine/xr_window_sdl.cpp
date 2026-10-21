// The SDL3 window of the engine on Apple platforms: creation, screen modes, the CAMetalLayer, the Win32-shaped
// subset on top of it and the event pump. Declarations and the design notes are in xr_window.h.
#include "stdafx.h"

#ifdef XR_PLATFORM_APPLE

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <Platform/Apple/Shell.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace mtl_d3d11
{
// src/Platform/MetalD3D11/include/mtl_d3d11/mtl_d3d11.h; declared here to keep the shim's D3D11 headers out of this file.
void RegisterWindowLayer(void* hwnd, void* caMetalLayer);
} // namespace mtl_d3d11

namespace
{
struct HandlerEntry
{
	xr_window::EventHandler handler;
	void* user;
};

struct WindowState
{
	SDL_Window* window = nullptr;
	SDL_MetalView view = nullptr;
	void* layer = nullptr;
	xr_window::Mode mode = xr_window::Borderless;
	bool quit = false;
	bool cursor_visible = true;
	std::vector<HandlerEntry> handlers;
};

WindowState g_state;

#if defined(XR_PLATFORM_IOS)
constexpr bool kFixedFullScreen = true; // an iPad app owns the whole screen
#else
constexpr bool kFixedFullScreen = false;
#endif

float density_of(SDL_Window* window)
{
	const float d = window ? SDL_GetWindowPixelDensity(window) : 0.f;
	return d > 0.f ? d : 1.f;
}

float display_density(SDL_DisplayID display)
{
	const SDL_DisplayMode* mode = display ? SDL_GetDesktopDisplayMode(display) : nullptr;
	return (mode && mode->pixel_density > 0.f) ? mode->pixel_density : 1.f;
}

SDL_DisplayID resolve_display(unsigned display_id)
{
	if (display_id)
	{
		int count = 0;
		SDL_DisplayID* displays = SDL_GetDisplays(&count);
		bool found = false;
		for (int i = 0; displays && i < count; ++i)
			found |= displays[i] == display_id;
		SDL_free(displays);
		if (found)
			return display_id;
	}
	SDL_DisplayID d = g_state.window ? SDL_GetDisplayForWindow(g_state.window) : 0;
	return d ? d : SDL_GetPrimaryDisplay();
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Win32-shaped subset
// ---------------------------------------------------------------------------------------------------------------------
BOOL GetClientRect(HWND window, RECT* rect)
{
	SDL_Window* w = (SDL_Window*)window;
	int pw = 0, ph = 0;
	if (!w || !SDL_GetWindowSizeInPixels(w, &pw, &ph))
		return FALSE;
	rect->left = 0;
	rect->top = 0;
	rect->right = pw;
	rect->bottom = ph;
	return TRUE;
}

BOOL GetWindowRect(HWND window, RECT* rect) { return GetClientRect(window, rect); }

BOOL ClientToScreen(HWND window, POINT* point) { return window != nullptr; }
BOOL ScreenToClient(HWND window, POINT* point) { return window != nullptr; }

int MapWindowPoints(HWND from, HWND to, POINT* points, UINT count)
{
	// client and screen coordinates are the same space here
	return 0;
}

BOOL GetCursorPos(POINT* point)
{
	float x = 0.f, y = 0.f;
	SDL_GetMouseState(&x, &y);
	const float d = density_of(g_state.window);
	point->x = (LONG)std::lround(x * d);
	point->y = (LONG)std::lround(y * d);
	return TRUE;
}

BOOL SetCursorPos(int x, int y)
{
	if (!g_state.window)
		return FALSE;
	const float d = density_of(g_state.window);
	SDL_WarpMouseInWindow(g_state.window, float(x) / d, float(y) / d);
	return TRUE;
}

BOOL ClipCursor(const RECT* rect)
{
	xr_window::ConfineCursor(rect);
	return TRUE;
}

int ShowCursor(BOOL show)
{
	xr_window::SetCursorVisible(show != FALSE);
	return show ? 0 : -1;
}

BOOL IsIconic(HWND window)
{
	SDL_Window* w = (SDL_Window*)window;
	return w && (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED) ? TRUE : FALSE;
}

BOOL SetForegroundWindow(HWND window)
{
	SDL_Window* w = (SDL_Window*)window;
	if (!w)
		return FALSE;
	SDL_RaiseWindow(w);
	return TRUE;
}

HWND SetActiveWindow(HWND window)
{
	SetForegroundWindow(window);
	return window;
}

HMONITOR MonitorFromPoint(POINT point, DWORD flags)
{
	// "Screen" coordinates are window-local, so there is nothing to hit-test: the window's display, else the primary.
	if (flags == MONITOR_DEFAULTTONULL)
		return nullptr;
	return xr_monitor_from_display(resolve_display(0));
}

HMONITOR MonitorFromWindow(HWND window, DWORD flags)
{
	SDL_Window* w = (SDL_Window*)window;
	SDL_DisplayID d = w ? SDL_GetDisplayForWindow(w) : 0;
	if (!d && flags != MONITOR_DEFAULTTONULL)
		d = SDL_GetPrimaryDisplay();
	return xr_monitor_from_display(d);
}

BOOL GetMonitorInfoA(HMONITOR monitor, MONITORINFO* info)
{
	const SDL_DisplayID display = xr_display_from_monitor(monitor);
	SDL_Rect bounds = {};
	if (!display || !info || !SDL_GetDisplayBounds(display, &bounds))
		return FALSE;
	const float d = display_density(display);
	info->rcMonitor.left = (LONG)std::lround(bounds.x * d);
	info->rcMonitor.top = (LONG)std::lround(bounds.y * d);
	info->rcMonitor.right = info->rcMonitor.left + (LONG)std::lround(bounds.w * d);
	info->rcMonitor.bottom = info->rcMonitor.top + (LONG)std::lround(bounds.h * d);
	SDL_Rect usable = bounds;
	SDL_GetDisplayUsableBounds(display, &usable);
	info->rcWork.left = (LONG)std::lround(usable.x * d);
	info->rcWork.top = (LONG)std::lround(usable.y * d);
	info->rcWork.right = info->rcWork.left + (LONG)std::lround(usable.w * d);
	info->rcWork.bottom = info->rcWork.top + (LONG)std::lround(usable.h * d);
	info->dwFlags = display == SDL_GetPrimaryDisplay() ? MONITORINFOF_PRIMARY : 0;
	return TRUE;
}

// ---------------------------------------------------------------------------------------------------------------------
// xr_window
// ---------------------------------------------------------------------------------------------------------------------
namespace xr_window
{
bool Create(const char* title, int width, int height)
{
	if (g_state.window)
		return true;

	if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO))
	{
		Msg("! xr_window: SDL video init failed: %s", SDL_GetError());
		return false;
	}

	// The game is landscape only (Info.plist says so too; this keeps SDL's own rotation handling in line).
	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
	// Keep the engine's cursor handling in charge: a touch is a pointer, not a gesture to hide the cursor.
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");

	SDL_WindowFlags flags = SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (kFixedFullScreen)
		flags |= SDL_WINDOW_FULLSCREEN;
	else
		flags |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN; // shown by SetMode, once its size is known

	g_state.window = SDL_CreateWindow(title, width, height, flags);
	if (!g_state.window)
	{
		Msg("! xr_window: SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}
	g_state.quit = false;

	int pw = 0, ph = 0;
	SDL_GetWindowSizeInPixels(g_state.window, &pw, &ph);
	Msg("* xr_window: created %dx%d px (density %.1f)", pw, ph, density_of(g_state.window));
	return true;
}

void Destroy()
{
	if (!g_state.window)
		return;
	if (g_state.view)
	{
		SDL_Metal_DestroyView(g_state.view);
		g_state.view = nullptr;
		g_state.layer = nullptr;
	}
	SDL_DestroyWindow(g_state.window);
	g_state.window = nullptr;
}

SDL_Window* Get() { return g_state.window; }

void SetMode(Mode mode, unsigned render_width, unsigned render_height, unsigned display_id)
{
	SDL_Window* w = g_state.window;
	if (!w)
		return;

	if (kFixedFullScreen)
	{
		g_state.mode = Borderless;
		SDL_ShowWindow(w);
		return;
	}

	const SDL_DisplayID display = resolve_display(display_id);
	SDL_Rect bounds = {};
	SDL_GetDisplayBounds(display, &bounds);

	if (mode == Windowed)
	{
		SDL_SetWindowFullscreen(w, false);
		SDL_SetWindowBordered(w, true);
		SDL_SetWindowResizable(w, true);
		const float d = display_density(display);
		SDL_Rect usable = bounds;
		SDL_GetDisplayUsableBounds(display, &usable);
		int pts_w = render_width ? int(std::lround(render_width / d)) : 1280;
		int pts_h = render_height ? int(std::lround(render_height / d)) : 720;
		pts_w = std::min(pts_w, std::max(usable.w, 320));
		pts_h = std::min(pts_h, std::max(usable.h, 240));
		SDL_SetWindowSize(w, pts_w, pts_h);
		SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
	}
	else
	{
		// Exclusive fullscreen would switch the display mode; on macOS a borderless desktop-size fullscreen window
		// is the same thing to the player and keeps the Metal layer's scaling, so both modes use it.
		SDL_SetWindowPosition(w, bounds.x, bounds.y);
		SDL_SetWindowFullscreenMode(w, nullptr);
		SDL_SetWindowFullscreen(w, true);
	}
	g_state.mode = mode;
	SDL_ShowWindow(w);
	SDL_RaiseWindow(w);
	SDL_SyncWindow(w);
}

Mode CurrentMode() { return g_state.mode; }

void* MetalLayer()
{
	if (!g_state.window)
		return nullptr;
	if (!g_state.layer)
	{
		g_state.view = SDL_Metal_CreateView(g_state.window);
		if (!g_state.view)
		{
			Msg("! xr_window: SDL_Metal_CreateView failed: %s", SDL_GetError());
			return nullptr;
		}
		g_state.layer = SDL_Metal_GetLayer(g_state.view);
		if (g_state.layer)
			mtl_d3d11::RegisterWindowLayer(g_state.window, g_state.layer);
	}
	return g_state.layer;
}

void GetDrawableSize(int& width, int& height)
{
	width = height = 0;
	if (g_state.window)
		SDL_GetWindowSizeInPixels(g_state.window, &width, &height);
}

float PixelDensity() { return density_of(g_state.window); }

unsigned CurrentDisplay() { return resolve_display(0); }

void GetDisplayPixelSize(unsigned display_id, unsigned& width, unsigned& height)
{
	const SDL_DisplayID display = resolve_display(display_id);
	const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(display);
	if (!mode)
	{
		width = height = 0;
		return;
	}
	const float d = mode->pixel_density > 0.f ? mode->pixel_density : 1.f;
	width = (unsigned)std::lround(mode->w * d);
	height = (unsigned)std::lround(mode->h * d);
}

float DisplayRefreshHz(unsigned display_id)
{
	const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(resolve_display(display_id));
	return (mode && mode->refresh_rate > 0.f) ? mode->refresh_rate : 60.f;
}

void AddEventHandler(EventHandler handler, void* user)
{
	g_state.handlers.push_back({handler, user});
}

void RemoveEventHandler(EventHandler handler, void* user)
{
	auto it = std::remove_if(g_state.handlers.begin(), g_state.handlers.end(),
	                         [&](const HandlerEntry& e) { return e.handler == handler && e.user == user; });
	g_state.handlers.erase(it, g_state.handlers.end());
}

void PumpEvents()
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		// a handler may add or remove handlers; walk a copy
		const std::vector<HandlerEntry> handlers = g_state.handlers;
		for (const HandlerEntry& e : handlers)
			e.handler(event, e.user);
		Device.on_sdl_event(event);
	}
}

bool QuitRequested() { return g_state.quit; }
void RequestQuit() { g_state.quit = true; }

void SetCursorVisible(bool visible)
{
	g_state.cursor_visible = visible;
	if (visible)
		SDL_ShowCursor();
	else
		SDL_HideCursor();
}

void ConfineCursor(const RECT* client_rect)
{
	if (!g_state.window)
		return;
	if (!client_rect)
	{
		SDL_SetWindowMouseRect(g_state.window, nullptr);
		return;
	}
	const float d = density_of(g_state.window);
	SDL_Rect r;
	r.x = (int)std::lround(client_rect->left / d);
	r.y = (int)std::lround(client_rect->top / d);
	r.w = std::max(1, (int)std::lround((client_rect->right - client_rect->left) / d));
	r.h = std::max(1, (int)std::lround((client_rect->bottom - client_rect->top) / d));
	SDL_SetWindowMouseRect(g_state.window, &r);
}
} // namespace xr_window

#endif // XR_PLATFORM_APPLE
