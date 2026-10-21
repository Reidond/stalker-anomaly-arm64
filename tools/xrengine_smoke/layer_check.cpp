#include "layer_check.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cmath>

struct LayerCheck::Impl
{
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
};

LayerCheck::LayerCheck() : impl_(new Impl) {}

LayerCheck::~LayerCheck()
{
    if (impl_->rtv) impl_->rtv->Release();
    if (impl_->back) impl_->back->Release();
    if (impl_->swap) impl_->swap->Release();
    if (impl_->context) impl_->context->Release();
    if (impl_->device) impl_->device->Release();
    delete impl_;
}

bool LayerCheck::Init(void* hwnd, int width, int height, const char** error)
{
    static const char* kNone = "";
    *error = kNone;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                 D3D11_SDK_VERSION, &impl_->device, &level, &impl_->context)))
    {
        *error = "D3D11CreateDevice failed";
        return false;
    }
    IDXGIFactory2* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
    {
        *error = "CreateDXGIFactory2 failed";
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsd = {};
    fsd.Windowed = TRUE;
    const HRESULT hr = factory->CreateSwapChainForHwnd(impl_->device, (HWND)hwnd, &sd, &fsd, nullptr, &impl_->swap);
    factory->Release();
    if (FAILED(hr))
    {
        *error = "CreateSwapChainForHwnd failed: the window's layer was not registered with the shim";
        return false;
    }
    impl_->swap->GetBuffer(0, IID_PPV_ARGS(&impl_->back));
    impl_->device->CreateRenderTargetView(impl_->back, nullptr, &impl_->rtv);
    ready_ = impl_->rtv != nullptr;
    if (!ready_)
        *error = "no render target view for the back buffer";
    return ready_;
}

void LayerCheck::Frame(unsigned frame)
{
    if (!ready_)
        return;
    const float t = float(frame) * 0.02f;
    const float color[4] = {0.15f + 0.1f * std::sin(t), 0.18f + 0.1f * std::sin(t * 1.3f + 1.f), 0.22f + 0.1f * std::sin(t * 0.7f + 2.f), 1.f};
    impl_->context->OMSetRenderTargets(1, &impl_->rtv, nullptr);
    impl_->context->ClearRenderTargetView(impl_->rtv, color);
    impl_->swap->Present(1, 0);
}
