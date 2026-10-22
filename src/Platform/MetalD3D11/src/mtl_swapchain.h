// Swap chain on a CAMetalLayer. The back buffer is a stable ID3D11Texture2D whose Metal texture is the
// current drawable; the drawable is acquired lazily the first time a pass attaches it.
#pragma once

#import "mtl_internal.h"

namespace mtl_d3d11
{
class SwapChain final : public ComObject<IDXGISwapChain3>
{
public:
    SwapChain(Device* dev, const SwapChainDesc& desc);
    ~SwapChain() override;
    bool init();

    HRESULT QueryInterface(MtlREFIID riid, void** pp) override;
    HRESULT GetParent(MtlREFIID, void**) override { return E_NOINTERFACE; }
    HRESULT GetDevice(MtlREFIID riid, void** pp) override;
    HRESULT Present(UINT SyncInterval, UINT Flags) override;
    HRESULT GetBuffer(UINT Buffer, MtlREFIID riid, void** ppSurface) override;
    HRESULT SetFullscreenState(MtlBOOL, IDXGIOutput*) override { return S_OK; }
    HRESULT GetFullscreenState(MtlBOOL* p, IDXGIOutput** o) override;
    HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC* p) override;
    HRESULT ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT Flags) override;
    HRESULT ResizeTarget(const DXGI_MODE_DESC*) override { return S_OK; }
    HRESULT GetContainingOutput(IDXGIOutput** pp) override;
    HRESULT GetDesc1(DXGI_SWAP_CHAIN_DESC1* p) override;
    UINT GetCurrentBackBufferIndex() override { return 0; }
    HRESULT CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE cs, UINT* support) override;
    HRESULT SetColorSpace1(DXGI_COLOR_SPACE_TYPE cs) override;

    // Used by the context when a pass attaches the back buffer.
    id<MTLTexture> currentTexture();
    MTLPixelFormat layerFormat() const { return pixelFormat_; }

    // Capture support (tools/tests)
    void requestCapture();
    bool waitCapture(CaptureResult* out);
    bool pollCapture(CaptureResult* out);

private:
    Device* dev_;
    SwapChainDesc cfg_;
    CAMetalLayer* layer_ = nil;
    id<CAMetalDrawable> drawable_ = nil;
    id<MTLTexture> offscreen_ = nil; // SwapChainDesc::offscreen: the back buffer when there is no layer
    id<MTLTexture> makeOffscreen(UINT w, UINT h);
    Texture2D* backBuffer_ = nullptr;
    DXGI_SWAP_CHAIN_DESC1 desc1_ = {};
    MTLPixelFormat pixelFormat_ = MTLPixelFormatBGRA8Unorm;
    bool captureRequested_ = false;
    // The blit's completion handler runs on a Metal thread, possibly after the swap chain is gone: it owns the state
    struct CaptureState
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool ready = false;
        CaptureResult result;
    };
    std::shared_ptr<CaptureState> capture_ = std::make_shared<CaptureState>();
};

HRESULT CreateFactory(MtlREFIID riid, void** ppFactory);
} // namespace mtl_d3d11
