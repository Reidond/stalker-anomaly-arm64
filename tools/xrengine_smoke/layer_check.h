// Draws a colour into the engine window through the D3D11-on-Metal shim, with the same calls dx10HW.cpp makes:
// D3D11CreateDevice, then IDXGIFactory2::CreateSwapChainForHwnd(device, <the engine's window handle>). The engine
// registered the window's CAMetalLayer under that handle, so this proves the handoff the renderer relies on.
// No engine headers in here: the shim's D3D11 names would clash with the engine's D3D9 vocabulary.
#pragma once

class LayerCheck
{
public:
    LayerCheck();
    ~LayerCheck();
    // `hwnd` is xr_engine::WindowHandle(); the layer must already exist (xr_engine::WindowMetalLayer()).
    bool Init(void* hwnd, int width, int height, const char** error);
    // Clears the back buffer to a colour that changes with `frame` and presents it.
    void Frame(unsigned frame);
    bool Ready() const { return ready_; }

private:
    struct Impl;
    Impl* impl_;
    bool ready_ = false;
};
