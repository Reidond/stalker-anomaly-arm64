// IDXGISwapChain on a CAMetalLayer, plus a minimal DXGI factory/adapter/output.
#import "mtl_swapchain.h"
#import "mtl_context.h"

namespace mtl_d3d11
{
namespace
{
MTLPixelFormat layerFormatFor(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM: return MTLPixelFormatRGB10A2Unorm;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return MTLPixelFormatRGBA16Float;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
    default: return MTLPixelFormatBGRA8Unorm; // CAMetalLayer has no RGBA8; channel order is a storage detail
    }
}
} // namespace

SwapChain::SwapChain(Device* dev, const SwapChainDesc& desc) : dev_(dev), cfg_(desc) { dev_->AddRef(); }

SwapChain::~SwapChain()
{
    if (backBuffer_)
        backBuffer_->Release();
    dev_->Release();
}

bool SwapChain::init()
{
    if (!cfg_.metalLayer)
        return false;
    layer_ = (__bridge CAMetalLayer*)cfg_.metalLayer;
    pixelFormat_ = layerFormatFor(cfg_.format);
    layer_.device = dev_->mtl;
    layer_.pixelFormat = pixelFormat_;
    layer_.framebufferOnly = !cfg_.allowReadback;
    layer_.maximumDrawableCount = cfg_.bufferCount >= 3 ? 3 : 2;
#if TARGET_OS_OSX
    layer_.displaySyncEnabled = cfg_.vsync;
#endif
    if (cfg_.width && cfg_.height)
        layer_.drawableSize = CGSizeMake(cfg_.width, cfg_.height);
    UINT w = UINT(layer_.drawableSize.width), h = UINT(layer_.drawableSize.height);
    if (!w || !h)
        return false;
    backBuffer_ = NewBackBuffer(dev_, this, w, h, cfg_.format, pixelFormat_);
    desc1_.Width = w;
    desc1_.Height = h;
    desc1_.Format = cfg_.format;
    desc1_.SampleDesc = {1, 0};
    desc1_.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc1_.BufferCount = cfg_.bufferCount;
    desc1_.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc1_.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    return true;
}

HRESULT SwapChain::QueryInterface(MtlREFIID riid, void** pp)
{
    if (!pp)
        return E_POINTER;
    if (riid == mtl_uuidof<IUnknown>() || riid == mtl_uuidof<IDXGIObject>() || riid == mtl_uuidof<IDXGIDeviceSubObject>() ||
        riid == mtl_uuidof<IDXGISwapChain>() || riid == mtl_uuidof<IDXGISwapChain1>() || riid == mtl_uuidof<IDXGISwapChain3>())
    {
        *pp = static_cast<IDXGISwapChain3*>(this);
        AddRef();
        return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
}

HRESULT SwapChain::GetDevice(MtlREFIID riid, void** pp) { return dev_->QueryInterface(riid, pp); }

id<MTLTexture> SwapChain::currentTexture()
{
    if (!drawable_)
        drawable_ = [layer_ nextDrawable];
    return drawable_ ? drawable_.texture : nil;
}

HRESULT SwapChain::Present(UINT syncInterval, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST)
        return S_OK;
#if TARGET_OS_OSX
    layer_.displaySyncEnabled = syncInterval != 0;
#endif
    Context* ctx = dev_->context;
    id<CAMetalDrawable> d = drawable_;
    drawable_ = nil;
    void (^pre)(id<MTLCommandBuffer>) = nil;
    // a drawable acquired before RequestCapture is still framebufferOnly: the capture waits for the next one
    if (captureRequested_ && d && !d.texture.framebufferOnly)
    {
        captureRequested_ = false;
        layer_.framebufferOnly = !cfg_.allowReadback;
        id<MTLTexture> src = d.texture;
        const NSUInteger w = src.width, h = src.height;
        id<MTLBuffer> buf = [dev_->mtl newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        const bool bgra = src.pixelFormat == MTLPixelFormatBGRA8Unorm || src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
        pre = ^(id<MTLCommandBuffer> cb) {
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:src
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:buf
                   destinationOffset:0
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:w * h * 4];
            [be endEncoding];
            [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
                CaptureResult r;
                r.width = uint32_t(w);
                r.height = uint32_t(h);
                r.rgba.resize(w * h * 4);
                const uint8_t* p = static_cast<const uint8_t*>(buf.contents);
                for (size_t i = 0; i < w * h; ++i)
                {
                    r.rgba[i * 4 + 0] = bgra ? p[i * 4 + 2] : p[i * 4 + 0];
                    r.rgba[i * 4 + 1] = p[i * 4 + 1];
                    r.rgba[i * 4 + 2] = bgra ? p[i * 4 + 0] : p[i * 4 + 2];
                    r.rgba[i * 4 + 3] = 255;
                }
                {
                    std::lock_guard<std::mutex> lk(captureMutex_);
                    capture_ = std::move(r);
                    captureReady_ = true;
                }
                captureCv_.notify_all();
            }];
        };
    }
    ctx->present(d, pre);
    return S_OK;
}

void SwapChain::requestCapture()
{
    layer_.framebufferOnly = NO;
    captureRequested_ = true;
}

bool SwapChain::pollCapture(CaptureResult* out)
{
    std::lock_guard<std::mutex> lk(captureMutex_);
    if (!captureReady_)
        return false;
    *out = std::move(capture_);
    captureReady_ = false;
    return true;
}

bool SwapChain::waitCapture(CaptureResult* out)
{
    std::unique_lock<std::mutex> lk(captureMutex_);
    if (!captureCv_.wait_for(lk, std::chrono::seconds(5), [&] { return captureReady_; }))
        return false;
    *out = std::move(capture_);
    captureReady_ = false;
    return true;
}

HRESULT SwapChain::GetBuffer(UINT index, MtlREFIID riid, void** pp)
{
    if (!pp)
        return E_POINTER;
    *pp = nullptr;
    if (index != 0)
        return DXGI_ERROR_INVALID_CALL;
    if (riid == mtl_uuidof<ID3D11Texture2D>() || riid == mtl_uuidof<ID3D11Resource>() || riid == mtl_uuidof<IUnknown>())
    {
        backBuffer_->AddRef();
        *pp = static_cast<ID3D11Texture2D*>(backBuffer_);
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT SwapChain::GetFullscreenState(MtlBOOL* f, IDXGIOutput** o)
{
    if (f)
        *f = FALSE;
    if (o)
        *o = nullptr;
    return S_OK;
}

HRESULT SwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* p)
{
    std::memset(p, 0, sizeof(*p));
    p->BufferDesc.Width = desc1_.Width;
    p->BufferDesc.Height = desc1_.Height;
    p->BufferDesc.Format = desc1_.Format;
    p->BufferDesc.RefreshRate = {60, 1};
    p->SampleDesc = desc1_.SampleDesc;
    p->BufferUsage = desc1_.BufferUsage;
    p->BufferCount = desc1_.BufferCount;
    p->Windowed = TRUE;
    p->SwapEffect = desc1_.SwapEffect;
    return S_OK;
}

HRESULT SwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* p)
{
    *p = desc1_;
    return S_OK;
}

HRESULT SwapChain::ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT)
{
    drawable_ = nil;
    if (fmt != DXGI_FORMAT_UNKNOWN)
    {
        desc1_.Format = fmt;
        pixelFormat_ = layerFormatFor(fmt);
        layer_.pixelFormat = pixelFormat_;
        backBuffer_->dxgi = fmt;
        backBuffer_->mtlFormat = pixelFormat_;
    }
    if (count)
        desc1_.BufferCount = count;
    if (w && h)
        layer_.drawableSize = CGSizeMake(w, h);
    desc1_.Width = UINT(layer_.drawableSize.width);
    desc1_.Height = UINT(layer_.drawableSize.height);
    backBuffer_->width = desc1_.Width;
    backBuffer_->height = desc1_.Height;
    return S_OK;
}

HRESULT SwapChain::GetContainingOutput(IDXGIOutput** pp)
{
    IDXGIFactory5* f = nullptr;
    HRESULT hr = CreateFactory(mtl_uuidof<IDXGIFactory5>(), reinterpret_cast<void**>(&f));
    if (FAILED(hr))
        return hr;
    IDXGIAdapter1* a = nullptr;
    f->EnumAdapters1(0, &a);
    hr = a ? a->EnumOutputs(0, pp) : E_FAIL;
    if (a)
        a->Release();
    f->Release();
    return hr;
}

HRESULT SwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE cs, UINT* support)
{
    // HDR10 would need CAMetalLayer EDR + PQ colour space tagging; only SDR is wired up for now.
    *support = cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT : 0;
    return S_OK;
}

HRESULT SwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE cs)
{
    return cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 ? S_OK : E_INVALIDARG;
}

HRESULT CreateSwapChain(ID3D11Device* pDevice, const SwapChainDesc* desc, IDXGISwapChain1** pp)
{
    if (!pDevice || !desc || !pp)
        return E_INVALIDARG;
    ID3D11Device1* d1 = nullptr;
    if (FAILED(pDevice->QueryInterface(&d1)))
        return E_INVALIDARG;
    Device* dev = static_cast<Device*>(d1);
    d1->Release();
    auto* sc = new SwapChain(dev, *desc);
    if (!sc->init())
    {
        sc->Release();
        return E_FAIL;
    }
    *pp = sc;
    return S_OK;
}

void RequestCapture(IDXGISwapChain1* sc) { static_cast<SwapChain*>(static_cast<IDXGISwapChain3*>(sc))->requestCapture(); }
bool WaitForCapture(IDXGISwapChain1* sc, CaptureResult* out)
{
    return static_cast<SwapChain*>(static_cast<IDXGISwapChain3*>(sc))->waitCapture(out);
}
bool PollCapture(IDXGISwapChain1* sc, CaptureResult* out)
{
    return static_cast<SwapChain*>(static_cast<IDXGISwapChain3*>(sc))->pollCapture(out);
}

// TextureCore::current for back buffers is defined here because it needs the swap chain.
id<MTLTexture> BackBufferCurrent(TextureCore* t) { return static_cast<SwapChain*>(t->swapChain)->currentTexture(); }
} // namespace mtl_d3d11
