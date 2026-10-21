// Minimal DXGI factory / adapter / output so that engine code written against DXGI keeps compiling and
// running: one adapter (the Metal device), one output. Window handles are mapped to CAMetalLayers.
#import "mtl_swapchain.h"

#include <map>

namespace mtl_d3d11
{
namespace
{
std::mutex g_layerMutex;
std::map<void*, void*>& layerRegistry()
{
    static std::map<void*, void*> m;
    return m;
}

struct DisplayConfig
{
    std::mutex m;
    UINT width = 1920, height = 1080, refresh = 60;
};
DisplayConfig& display()
{
    static DisplayConfig d;
    return d;
}

void widen(const char* s, WCHAR* out, size_t n)
{
    size_t i = 0;
    for (; s[i] && i + 1 < n; ++i)
        out[i] = WCHAR(uint8_t(s[i]));
    out[i] = 0;
}

class Output final : public ComObject<IDXGIOutput>
{
public:
    HRESULT GetParent(MtlREFIID, void**) override { return E_NOINTERFACE; }
    HRESULT GetDesc(DXGI_OUTPUT_DESC* d) override
    {
        std::memset(d, 0, sizeof(*d));
        widen("Display 1", d->DeviceName, 32);
        std::lock_guard<std::mutex> lk(display().m);
        d->DesktopCoordinates = {0, 0, LONG(display().width), LONG(display().height)};
        d->AttachedToDesktop = TRUE;
        d->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        d->Monitor = reinterpret_cast<HMONITOR>(uintptr_t(1));
        return S_OK;
    }
    HRESULT GetDisplayModeList(DXGI_FORMAT, UINT, UINT* n, DXGI_MODE_DESC* modes) override
    {
        std::lock_guard<std::mutex> lk(display().m);
        const UINT refresh[] = {display().refresh, 30};
        const int count = display().refresh > 30 ? 2 : 1;
        if (!modes)
        {
            *n = UINT(count);
            return S_OK;
        }
        UINT out = 0;
        for (int i = 0; i < count && out < *n; ++i, ++out)
        {
            modes[out] = {display().width, display().height, {refresh[i], 1}, DXGI_FORMAT_R8G8B8A8_UNORM,
                          DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE, DXGI_MODE_SCALING_UNSPECIFIED};
        }
        *n = out;
        return out < UINT(count) ? DXGI_ERROR_MORE_DATA : S_OK;
    }
    HRESULT FindClosestMatchingMode(const DXGI_MODE_DESC* m, DXGI_MODE_DESC* c, IUnknown*) override
    {
        *c = *m;
        return S_OK;
    }
    HRESULT GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* c) override
    {
        std::memset(c, 0, sizeof(*c));
        c->MaxConvertedValue = 1.0f;
        c->NumGammaControlPoints = 0; // gamma ramps are not available; the renderer should use a shader
        return S_OK;
    }
    HRESULT SetGammaControl(const DXGI_GAMMA_CONTROL*) override { return S_OK; }
    HRESULT GetGammaControl(DXGI_GAMMA_CONTROL* g) override
    {
        std::memset(g, 0, sizeof(*g));
        return S_OK;
    }
};

class Adapter final : public ComObject<IDXGIAdapter1>
{
public:
    HRESULT GetParent(MtlREFIID, void**) override { return E_NOINTERFACE; }
    HRESULT EnumOutputs(UINT i, IDXGIOutput** pp) override
    {
        if (i != 0)
        {
            *pp = nullptr;
            return DXGI_ERROR_NOT_FOUND;
        }
        *pp = new Output();
        return S_OK;
    }
    HRESULT GetDesc(DXGI_ADAPTER_DESC* d) override
    {
        std::memset(d, 0, sizeof(*d));
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        widen(dev ? dev.name.UTF8String : "Apple GPU", d->Description, 128);
        d->VendorId = 0x106B;
        d->DeviceId = 0;
        if (dev)
        {
            d->DedicatedVideoMemory = size_t(dev.recommendedMaxWorkingSetSize);
            d->SharedSystemMemory = size_t(dev.recommendedMaxWorkingSetSize);
        }
        return S_OK;
    }
    HRESULT GetDesc1(DXGI_ADAPTER_DESC1* d) override { return GetDesc(d); }
};

class Factory final : public ComObject<IDXGIFactory5>
{
public:
    HRESULT QueryInterface(MtlREFIID riid, void** pp) override
    {
        if (riid == mtl_uuidof<IDXGIFactory>() || riid == mtl_uuidof<IDXGIFactory1>() || riid == mtl_uuidof<IDXGIFactory2>() ||
            riid == mtl_uuidof<IDXGIFactory5>() || riid == mtl_uuidof<IUnknown>() || riid == mtl_uuidof<IDXGIObject>())
        {
            *pp = static_cast<IDXGIFactory5*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT GetParent(MtlREFIID, void**) override { return E_NOINTERFACE; }
    HRESULT EnumAdapters(UINT i, IDXGIAdapter** pp) override
    {
        IDXGIAdapter1* a = nullptr;
        HRESULT hr = EnumAdapters1(i, &a);
        *pp = a;
        return hr;
    }
    HRESULT EnumAdapters1(UINT i, IDXGIAdapter1** pp) override
    {
        if (i != 0)
        {
            *pp = nullptr;
            return DXGI_ERROR_NOT_FOUND;
        }
        *pp = new Adapter();
        return S_OK;
    }
    HRESULT CreateSwapChainForHwnd(IUnknown* pDevice, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1** pp) override
    {
        void* layer = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_layerMutex);
            auto it = layerRegistry().find(hwnd);
            if (it != layerRegistry().end())
                layer = it->second;
        }
        if (!layer && hwnd && [(__bridge id)hwnd isKindOfClass:[CAMetalLayer class]])
            layer = hwnd;
        if (!layer)
        {
            Log("CreateSwapChainForHwnd: window %p has no registered CAMetalLayer (call RegisterWindowLayer)", hwnd);
            return E_INVALIDARG;
        }
        ID3D11Device* dev = nullptr;
        if (FAILED(pDevice->QueryInterface(mtl_uuidof<ID3D11Device>(), reinterpret_cast<void**>(&dev))))
            return E_INVALIDARG;
        SwapChainDesc sc;
        sc.metalLayer = layer;
        sc.width = d->Width;
        sc.height = d->Height;
        sc.format = d->Format;
        sc.bufferCount = d->BufferCount;
        sc.allowReadback = (d->BufferUsage & DXGI_USAGE_SHADER_INPUT) != 0;
        HRESULT hr = CreateSwapChain(dev, &sc, pp);
        dev->Release();
        return hr;
    }
    HRESULT CheckFeatureSupport(DXGI_FEATURE f, void* data, UINT size) override
    {
        if (f == DXGI_FEATURE_PRESENT_ALLOW_TEARING && data && size >= sizeof(MtlBOOL))
        {
            *static_cast<MtlBOOL*>(data) = FALSE;
            return S_OK;
        }
        return E_INVALIDARG;
    }
};
} // namespace

void RegisterWindowLayer(void* hwnd, void* layer)
{
    std::lock_guard<std::mutex> lk(g_layerMutex);
    layerRegistry()[hwnd] = layer;
}

HRESULT CreateFactory(MtlREFIID riid, void** pp)
{
    Factory* f = new Factory();
    HRESULT hr = f->QueryInterface(riid, pp);
    f->Release();
    return hr;
}

void SetDisplayMode(UINT width, UINT height, UINT refreshHz)
{
    std::lock_guard<std::mutex> lk(display().m);
    display().width = width;
    display().height = height;
    display().refresh = refreshHz;
}
} // namespace mtl_d3d11

HRESULT CreateDXGIFactory2(UINT, MtlREFIID riid, void** pp) { return mtl_d3d11::CreateFactory(riid, pp); }
