#include "stdafx.h"
#include "MonitorList.h"

#ifndef XR_PLATFORM_APPLE

bool CRenderDevice::on_message(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
	switch (uMsg)
	{
	case WM_SYSKEYDOWN:
		{
			return true;
		}
	case WM_ACTIVATE:
		{
#ifdef INGAME_EDITOR
        if (editor())
        {
            Device.b_is_Active = TRUE;
            break;
        }
#endif // #ifdef INGAME_EDITOR
			OnWM_Activate(wParam, lParam);
			return (false);
		}
	case WM_SETCURSOR:
		{
#ifdef INGAME_EDITOR
        if (editor())
            break;
#endif // #ifdef INGAME_EDITOR

			result = 1;
			return (true);
		}
	case WM_SYSCOMMAND:
		{
#ifdef INGAME_EDITOR
        if (editor())
            break;
#endif // #ifdef INGAME_EDITOR

			// Prevent moving/sizing and power loss in fullscreen mode
			switch (wParam)
			{
			case SC_MOVE:
			case SC_SIZE:
			case SC_MAXIMIZE:
			case SC_MONITORPOWER:
				result = 1;
				return (true);
			}
			return (false);
		}
	case WM_CLOSE:
		{
			Engine.Event.Defer("KERNEL:disconnect");
			Engine.Event.Defer("KERNEL:quit");								 
#ifdef INGAME_EDITOR
        if (editor())
            break;
#endif // #ifdef INGAME_EDITOR

			result = 0;
			return (true);
		}
	case WM_HOTKEY: // prevent 'ding' sounds caused by Alt+key combinations
	case WM_SYSCHAR:
		result = 0;
		return true;

	case WM_CHAR:
		Device.imgui().InputChar(wParam);
		return false;

	case WM_INPUTLANGCHANGE:
		Device.imgui().UpdateInputLang();
		return false;

	case WM_DISPLAYCHANGE:
		InterlockedExchange(&g_monitor_list_dirty, 1);
		return false;
	}

	return (false);
}

//-----------------------------------------------------------------------------
// Name: WndProc()
// Desc: Static msg handler which passes messages to the application class.
//-----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	LRESULT result;
	if (Device.on_message(hWnd, uMsg, wParam, lParam, result))
		return (result);

	return (DefWindowProc(hWnd, uMsg, wParam, lParam));
}
#else // XR_PLATFORM_APPLE

// The Apple counterpart of WndProc: SDL events instead of window messages. xr_window::PumpEvents() hands every event
// here after the registered handlers (input) have seen it.
#include <SDL3/SDL.h>

void CRenderDevice::on_sdl_event(const SDL_Event& e)
{
	SDL_Window* const window = xr_window::Get();
	switch (e.type)
	{
	case SDL_EVENT_QUIT:
		// macOS Cmd+Q / the dock; on iPadOS the app is going away, the shell's Terminating listener covers that
		Engine.Event.Defer("KERNEL:disconnect");
		Engine.Event.Defer("KERNEL:quit");
		break;

	case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		if (!window || e.window.windowID != SDL_GetWindowID(window))
			break;
		Engine.Event.Defer("KERNEL:disconnect");
		Engine.Event.Defer("KERNEL:quit");
		break;

	case SDL_EVENT_WINDOW_FOCUS_GAINED:
	case SDL_EVENT_WINDOW_FOCUS_LOST:
	case SDL_EVENT_WINDOW_MINIMIZED:
	case SDL_EVENT_WINDOW_RESTORED:
	case SDL_EVENT_WINDOW_SHOWN:
	case SDL_EVENT_WINDOW_HIDDEN:
		if (window && e.window.windowID == SDL_GetWindowID(window))
			SyncActiveState();
		break;

	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		// A resizable macOS window: follow the new size with the render resolution.
		if (window && e.window.windowID == SDL_GetWindowID(window) && b_is_Ready &&
			xr_window::CurrentMode() == xr_window::Windowed &&
			(u32(e.window.data1) != dwWidth || u32(e.window.data2) != dwHeight))
		{
			psCurrentVidMode[0] = u32(e.window.data1);
			psCurrentVidMode[1] = u32(e.window.data2);
			m_bResizePending = true;
		}
		break;

	case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
	case SDL_EVENT_DISPLAY_ADDED:
	case SDL_EVENT_DISPLAY_REMOVED:
	case SDL_EVENT_DISPLAY_MOVED:
	case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
		__atomic_store_n(&g_monitor_list_dirty, 1, __ATOMIC_SEQ_CST);
		break;
	}
}

// The window is active when it is visible, focused (macOS; on iPadOS the shell's pause is the authority and focus only
// follows it) and the app is not paused by the system. Also called after the device is created, because the first
// focus event predates it.
void CRenderDevice::SyncActiveState()
{
	SDL_Window* window = xr_window::Get();
	if (!window)
		return;
	const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
#if defined(XR_PLATFORM_IOS)
	const bool focused = true;
#else
	const bool focused = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
#endif
	const bool visible = !(flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));
	OnAppActive(focused && visible && !m_bSystemPaused);
}

void CRenderDevice::OnSystemPause(bool paused)
{
	m_bSystemPaused = paused;
	SyncActiveState();
}

void CRenderDevice::OnMemoryPressure(int level)
{
	Msg("* memory pressure: level %d (1 = warning, 2 = critical)", level);
	// Resources the engine can drop without the renderer's help; the renderer's own caches are its session's business.
	Memory.mem_compact();
}

#endif // XR_PLATFORM_APPLE
