// AppleWindow.h: what CHW needs from the window system on Apple platforms.
//
// Device.m_hWnd is the SDL_Window* the engine created with SDL_WINDOW_METAL (the HWND typedef is void*). The engine
// shell owns the window; this file only asks it for the Metal layer, the pixel size and the window mode, so the
// DXGI code in dx10HW.cpp stays close to the Windows original.
#pragma once

#if defined(XR_PLATFORM_APPLE)

namespace xr_apple_window
{
// Creates (once per window) the SDL Metal view, registers its CAMetalLayer with the Metal D3D11 shim and tells the
// shim which display mode the output reports. Call before IDXGIFactory2::CreateSwapChainForHwnd.
bool AttachMetalLayer(HWND hwnd);
void DetachMetalLayer(HWND hwnd);

// Drawable size of the window in pixels.
bool GetClientSize(HWND hwnd, u32& width, u32& height);
// Size and position (in pixels / points of the desktop) of the display that holds the window.
bool GetDisplaySize(HWND hwnd, u32& width, u32& height);

// g_screenmode: 0 = windowed, 1 = borderless, 2 = fullscreen. A no-op on iOS, where the window always covers the screen.
void ApplyWindowMode(HWND hwnd, u32 screenmode, u32 width, u32 height, bool move_window);
void Raise(HWND hwnd);
void Minimize(HWND hwnd);
void Restore(HWND hwnd);
} // namespace xr_apple_window

#endif // XR_PLATFORM_APPLE
