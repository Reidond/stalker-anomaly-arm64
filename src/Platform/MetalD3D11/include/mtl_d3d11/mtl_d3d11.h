// Entry points of the D3D11-on-Metal shim that have no D3D11 equivalent: device and swap chain creation from
// a CAMetalLayer, window-handle registration, frame statistics and a screenshot capture helper.
//
// Typical start-up (see tools/metal_d3d11_test):
//     SDL_MetalView view = SDL_Metal_CreateView(window);
//     void* layer = SDL_Metal_GetLayer(view);
//     mtl_d3d11::CreateDevice(nullptr, &device, &context);
//     mtl_d3d11::SwapChainDesc sc = {}; sc.metalLayer = layer; ...
//     mtl_d3d11::CreateSwapChain(device, &sc, &swapChain);
//     swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
//     device->CreateRenderTargetView(backBuffer, nullptr, &backBufferRTV);
#pragma once

#include <cstdint>
#include <vector>

#include "d3d11shader_metal.h"
#include "mtl_shader.h"

namespace mtl_d3d11
{
struct DeviceDesc
{
    void* mtlDevice = nullptr;          // id<MTLDevice>, or null for the system default
    bool validation = false;            // extra shim-side checks and logging (also D3D11_CREATE_DEVICE_DEBUG)
    uint32_t framesInFlight = 3;        // command buffers the CPU may run ahead of the GPU
    uint32_t constantRingBytes = 8u << 20; // per-frame ring for DISCARD maps of small dynamic buffers
    uint32_t uploadRingBytes = 32u << 20;  // per-frame ring for UpdateSubresource / texture uploads
    bool fastMath = true;                  // MTLCompileOptions.fastMathEnabled for MSL source payloads
};

HRESULT CreateDevice(const DeviceDesc* desc, ID3D11Device** ppDevice, ID3D11DeviceContext** ppImmediateContext);

struct SwapChainDesc
{
    void* metalLayer = nullptr;           // CAMetalLayer*; null with offscreen
    bool offscreen = false;               // no layer: the back buffer is a private MTLTexture of width x height (unattended runs);
                                          // Present only ends the frame, screenshots come from RequestCapture
    UINT width = 0;                       // 0: use the layer's drawableSize
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM; // R8G8B8A8/B8G8R8A8_UNORM -> BGRA8, R10G10B10A2 -> RGB10A2
    UINT bufferCount = 3;                 // CAMetalLayer.maximumDrawableCount (2 or 3)
    bool allowReadback = false;           // framebufferOnly = NO so the drawable can be captured
    bool vsync = true;                    // macOS only; iOS always syncs to the display
};

HRESULT CreateSwapChain(ID3D11Device* pDevice, const SwapChainDesc* desc, IDXGISwapChain1** ppSwapChain);

// IDXGIFactory2::CreateSwapChainForHwnd receives the engine's window handle. Register the CAMetalLayer that
// belongs to it (SDL_Metal_GetLayer) so the unmodified CHW code can create its swap chain.
void RegisterWindowLayer(void* hwnd, void* caMetalLayer);
// The window has no layer: CreateSwapChainForHwnd then makes an offscreen swap chain (SwapChainDesc::offscreen).
void RegisterOffscreenWindow(void* hwnd);

// What IDXGIOutput reports (desktop size and display-mode list). The platform layer sets it from SDL.
void SetDisplayMode(UINT width, UINT height, UINT refreshHz);

// Captures the next presented frame as tightly packed RGBA8 (converted from the drawable's BGRA8).
// The swap chain must have been created with allowReadback. Returns false if no frame arrived.
struct CaptureResult
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};
// RequestCapture makes the layer's drawables readable (framebufferOnly = NO) and arms the capture for the first
// Present whose drawable allows it, so a swap chain created without allowReadback pays nothing until a screenshot is
// asked for (the capture then lands one or two frames later). WaitForCapture blocks until the frame arrived;
// PollCapture does not (use it from a thread that is also the one presenting).
void RequestCapture(IDXGISwapChain1* pSwapChain);
bool WaitForCapture(IDXGISwapChain1* pSwapChain, CaptureResult* out);
bool PollCapture(IDXGISwapChain1* pSwapChain, CaptureResult* out);

// Encodes a capture as JPEG (format 0) or PNG (format 1) with ImageIO.
bool EncodeCapture(const CaptureResult& image, int format, std::vector<uint8_t>* out);

struct Stats
{
    uint64_t commandBuffers = 0;
    uint64_t renderPasses = 0;
    uint64_t computePasses = 0;
    uint64_t blitPasses = 0;
    uint64_t draws = 0;
    uint64_t dispatches = 0;
    uint64_t pipelineCreates = 0;
    uint64_t pipelineHits = 0;
    uint64_t passBreaksByTargetChange = 0;
    uint64_t passBreaksByClear = 0;
    uint64_t passBreaksByCopy = 0;
    uint64_t passBreaksByReadback = 0;
    uint64_t foldedClears = 0;       // clears turned into a load action
    uint64_t materializedClears = 0; // clears that needed their own empty pass
    uint64_t ringBytesUsed = 0;      // this frame
    uint64_t textureBytes = 0;       // live texture memory (best effort)
    uint64_t bufferBytes = 0;
    uint64_t deviceAllocatedBytes = 0; // MTLDevice.currentAllocatedSize
};
void GetStats(ID3D11DeviceContext* pContext, Stats* out);
void ResetFrameStats(ID3D11DeviceContext* pContext);

// Native handles for tools and tests. Return unretained id<MTLTexture>/id<MTLBuffer>/id<MTLDevice>.
void* GetNativeDevice(ID3D11Device* pDevice);
void* GetNativeTexture(ID3D11Resource* pResource);
void* GetNativeBuffer(ID3D11Resource* pResource);

// Names the object in Xcode's GPU debugger.
void SetDebugName(ID3D11DeviceChild* pObject, const char* name);
} // namespace mtl_d3d11
