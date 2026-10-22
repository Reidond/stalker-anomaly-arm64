#pragma once
// The window system boundary of xrEngine.
//
// Windows: nothing in here; HWND is the real window handle and the engine calls the Win32 API directly.
//
// Apple (XR_PLATFORM_APPLE): the engine window is an SDL3 window. `HWND` is that window's `SDL_Window*`
// (CRenderDevice::m_hWnd), so code that only passes the handle around keeps working. This header provides
//   * namespace xr_window: window creation, screen modes, the CAMetalLayer, cursor control and the SDL event pump
//     (xr_window_sdl.cpp), and
//   * a small Win32-named subset (GetClientRect, ClientToScreen, ClipCursor, MonitorFromWindow, ...) over it, for the
//     call sites in xrEngine, xrGame and the renderer that were written against Win32.
//
// Units: everything Win32-shaped here is in *pixels* of the drawable (SDL reports points; the iPad is 2x). Client
// coordinates have their origin at the window's top left and "screen" coordinates are the same space (a window always
// covers its own screen on iPadOS and the engine only uses screen coordinates in pairs with client ones).

#ifdef XR_PLATFORM_APPLE

struct SDL_Window;
union SDL_Event;

// ---------------------------------------------------------------------------------------------------------------------
// Win32 vocabulary
// ---------------------------------------------------------------------------------------------------------------------
typedef struct tagMONITORINFO
{
	DWORD cbSize;
	RECT rcMonitor; // pixels
	RECT rcWork;
	DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;

#ifndef MONITOR_DEFAULTTONULL
# define MONITOR_DEFAULTTONULL 0x00000000
# define MONITOR_DEFAULTTOPRIMARY 0x00000001
# define MONITOR_DEFAULTTONEAREST 0x00000002
# define MONITORINFOF_PRIMARY 0x00000001
#endif

// An HMONITOR is an SDL_DisplayID stored in a pointer (0 is "no monitor").
inline HMONITOR xr_monitor_from_display(unsigned display_id) { return (HMONITOR)(uintptr_t)display_id; }
inline unsigned xr_display_from_monitor(HMONITOR monitor) { return (unsigned)(uintptr_t)monitor; }

ENGINE_API BOOL GetClientRect(HWND window, RECT* rect);
ENGINE_API BOOL GetWindowRect(HWND window, RECT* rect);
ENGINE_API BOOL ClientToScreen(HWND window, POINT* point);
ENGINE_API BOOL ScreenToClient(HWND window, POINT* point);
// Only the form the engine uses: client points of `from` to screen (to == null) or the other way round.
ENGINE_API int MapWindowPoints(HWND from, HWND to, POINT* points, UINT count);
ENGINE_API BOOL GetCursorPos(POINT* point); // the mouse position in screen (= client) pixels
ENGINE_API BOOL SetCursorPos(int x, int y); // warps the mouse
ENGINE_API BOOL ClipCursor(const RECT* rect); // null releases; confines the mouse to the rectangle otherwise
ENGINE_API int ShowCursor(BOOL show); // a flag here, not Win32's counter
ENGINE_API BOOL IsIconic(HWND window);
ENGINE_API BOOL SetForegroundWindow(HWND window);
ENGINE_API HWND SetActiveWindow(HWND window);
inline void DisableProcessWindowsGhosting() {}

ENGINE_API HMONITOR MonitorFromPoint(POINT point, DWORD flags);
ENGINE_API HMONITOR MonitorFromWindow(HWND window, DWORD flags);
ENGINE_API BOOL GetMonitorInfoA(HMONITOR monitor, MONITORINFO* info);

// ---------------------------------------------------------------------------------------------------------------------
// xr_window
// ---------------------------------------------------------------------------------------------------------------------
namespace xr_window
{
// Same numbering as g_screenmode (`vid_mode`/`screenmode` console command).
enum Mode
{
	Windowed = 0,
	Borderless = 1,
	Fullscreen = 2,
};

// Creates the engine window (SDL_Init(SDL_INIT_VIDEO) must have run): Metal-capable, high pixel density, hidden until
// SetMode. `width`/`height` are points and only matter for a windowed Mac window. On iPadOS the window is always the
// whole screen. Returns false (and logs) on failure. Safe to call twice; the second call returns the first window.
ENGINE_API bool Create(const char* title, int width, int height);

// Background mode (unattended runs, call before Create): the engine never takes focus from the user. It does not raise the
// window, does not enable relative mouse mode or confine the cursor, and does not warp the real cursor; synthetic input
// events (the app's --script) still reach the engine.
ENGINE_API void SetBackground(bool background);
ENGINE_API bool IsBackground();
// Offscreen mode (unattended runs, call before Create): the window is never shown, raised or given a Metal view; the renderer
// draws into an offscreen swap chain of the render resolution (xr_apple_window::AttachMetalLayer), and screenshots read that
// texture back. The window object still exists for input events and the drawable size.
ENGINE_API void SetOffscreen(bool offscreen);
ENGINE_API bool IsOffscreen();
ENGINE_API void Destroy();
ENGINE_API SDL_Window* Get();
inline HWND Handle() { return (HWND)Get(); }

// Applies a screen mode. `render_width`/`render_height` (pixels) are the resolution the renderer will use; they size a
// windowed window (1 pixel per render pixel) and are ignored for the other modes, where the window covers the display
// and the Metal layer scales the render resolution to it. `display_id` is an SDL_DisplayID, 0 for the window's current
// display. Shows the window. iPadOS ignores the mode (always full screen).
ENGINE_API void SetMode(Mode mode, unsigned render_width, unsigned render_height, unsigned display_id = 0);
ENGINE_API Mode CurrentMode();

// The CAMetalLayer of the window (a void* so this header stays free of SDL). The first call creates the SDL Metal view
// and registers the layer with the D3D11 shim under Handle(), so the renderer's CreateSwapChainForHwnd(Device.m_hWnd)
// works as written. Null if the window has no Metal support.
ENGINE_API void* MetalLayer();
ENGINE_API void GetDrawableSize(int& width, int& height); // pixels
ENGINE_API float PixelDensity(); // pixels per point
// The display the window is on, as an SDL_DisplayID, and its size in pixels.
ENGINE_API unsigned CurrentDisplay();
ENGINE_API void GetDisplayPixelSize(unsigned display_id, unsigned& width, unsigned& height);
ENGINE_API float DisplayRefreshHz(unsigned display_id);

// Events. PumpEvents() must run on the main thread, once per frame (and from long blocking loops such as level
// loading). It drains the SDL queue; every event goes to the registered handlers first (input registers one), then
// the engine reacts to the window events itself (close, focus, minimize, resize, display change).
typedef void (*EventHandler)(const SDL_Event& event, void* user);
ENGINE_API void AddEventHandler(EventHandler handler, void* user);
ENGINE_API void RemoveEventHandler(EventHandler handler, void* user);
ENGINE_API void PumpEvents();
// Hands one event, already taken out of the SDL queue, to the handlers and the device: what PumpEvents does per event.
// An app that runs on SDL3's callbacks gets its events through SDL_AppEvent before the engine's frame runs and forwards
// them here (xr_engine::HandleEvent).
ENGINE_API void DispatchEvent(const SDL_Event& event);
// True once the window was closed or the OS asked the app to quit (the engine has queued KERNEL:quit by then).
ENGINE_API bool QuitRequested();
ENGINE_API void RequestQuit();

// Cursor. Visible: draw the OS pointer. Confine: keep it inside the rectangle (client pixels).
ENGINE_API void SetCursorVisible(bool visible);
ENGINE_API void ConfineCursor(const RECT* client_rect);
} // namespace xr_window

#endif // XR_PLATFORM_APPLE
