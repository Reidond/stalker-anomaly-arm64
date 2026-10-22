// AppleWindow.cpp: window-system glue for CHW on Apple platforms (see AppleWindow.h).
#include "stdafx.h"
#pragma hdrstop

#if defined(XR_PLATFORM_APPLE)

#include "AppleWindow.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <mtl_d3d11/mtl_d3d11.h>

#include <map>

namespace xr_apple_window
{
namespace
{
std::map<HWND, SDL_MetalView> g_views;

SDL_Window* ToWindow(HWND hwnd) { return static_cast<SDL_Window*>(hwnd); }

float PixelDensity(SDL_Window* window)
{
    const float density = SDL_GetWindowPixelDensity(window);
    return density > 0.f ? density : 1.f;
}
} // namespace

bool AttachMetalLayer(HWND hwnd)
{
    SDL_Window* window = ToWindow(hwnd);
    if (!window)
    {
        Msg("! xr_apple_window: no window handle");
        return false;
    }

    if (xr_window::IsOffscreen())
    {
        // no layer: the shim makes an offscreen swap chain of the size CHW asks for
        mtl_d3d11::RegisterOffscreenWindow(hwnd);
        u32 width = 0, height = 0;
        if (GetDisplaySize(hwnd, width, height))
            mtl_d3d11::SetDisplayMode(width, height, 60);
        return true;
    }

    auto it = g_views.find(hwnd);
    if (it == g_views.end())
    {
        if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_METAL))
        {
            Msg("! xr_apple_window: the window was not created with SDL_WINDOW_METAL");
            return false;
        }
        SDL_MetalView view = SDL_Metal_CreateView(window);
        if (!view)
        {
            Msg("! xr_apple_window: SDL_Metal_CreateView failed: %s", SDL_GetError());
            return false;
        }
        it = g_views.emplace(hwnd, view).first;
    }

    void* layer = SDL_Metal_GetLayer(it->second);
    if (!layer)
    {
        Msg("! xr_apple_window: SDL_Metal_GetLayer failed: %s", SDL_GetError());
        return false;
    }
    mtl_d3d11::RegisterWindowLayer(hwnd, layer);

    // IDXGIOutput::GetDisplayModeList reports the desktop mode of the display the window is on
    u32 width = 0, height = 0;
    if (GetDisplaySize(hwnd, width, height))
    {
        const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
        const UINT hz = (mode && mode->refresh_rate > 0.f) ? UINT(mode->refresh_rate + 0.5f) : 60u;
        mtl_d3d11::SetDisplayMode(width, height, hz);
    }
    return true;
}

void DetachMetalLayer(HWND hwnd)
{
    auto it = g_views.find(hwnd);
    if (it == g_views.end())
        return;
    SDL_Metal_DestroyView(it->second);
    g_views.erase(it);
}

bool GetClientSize(HWND hwnd, u32& width, u32& height)
{
    int w = 0, h = 0;
    if (!ToWindow(hwnd) || !SDL_GetWindowSizeInPixels(ToWindow(hwnd), &w, &h))
        return false;
    width = u32(w);
    height = u32(h);
    return true;
}

bool GetDisplaySize(HWND hwnd, u32& width, u32& height)
{
    SDL_Window* window = ToWindow(hwnd);
    SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(display);
    if (!mode)
        return false;
    width = u32(mode->w * mode->pixel_density + 0.5f);
    height = u32(mode->h * mode->pixel_density + 0.5f);
    return true;
}

void ApplyWindowMode(HWND hwnd, u32 screenmode, u32 width, u32 height, bool move_window)
{
    SDL_Window* window = ToWindow(hwnd);
    if (!window || xr_window::IsOffscreen())
        return;
#if defined(XR_PLATFORM_IOS)
    // the window always covers the screen
    (void)screenmode; (void)width; (void)height; (void)move_window;
#else
    if (screenmode == 2)
    {
        SDL_SetWindowFullscreen(window, true);
        return;
    }
    SDL_SetWindowFullscreen(window, false);
    if (!move_window)
        return;
    SDL_SetWindowBordered(window, screenmode == 0);
    if (width && height)
    {
        const float density = PixelDensity(window);
        SDL_SetWindowSize(window, int(width / density + 0.5f), int(height / density + 0.5f));
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    SDL_ShowWindow(window);
    if (!xr_window::IsBackground())
        SDL_RaiseWindow(window);
#endif
}

void Raise(HWND hwnd)
{
    if (ToWindow(hwnd) && !xr_window::IsOffscreen() && !xr_window::IsBackground())
        SDL_RaiseWindow(ToWindow(hwnd));
}

void Minimize(HWND hwnd)
{
#if !defined(XR_PLATFORM_IOS)
    if (ToWindow(hwnd) && !xr_window::IsOffscreen())
        SDL_MinimizeWindow(ToWindow(hwnd));
#else
    (void)hwnd;
#endif
}

void Restore(HWND hwnd)
{
#if !defined(XR_PLATFORM_IOS)
    if (ToWindow(hwnd) && !xr_window::IsOffscreen())
        SDL_RestoreWindow(ToWindow(hwnd));
#else
    (void)hwnd;
#endif
}
} // namespace xr_apple_window

#endif // XR_PLATFORM_APPLE
