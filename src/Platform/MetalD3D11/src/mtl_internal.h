// Internal declarations of the D3D11-on-Metal shim (Objective-C++ only). Nothing here is exposed to the engine.
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mtl_d3d11/d3d11_metal.h"
#include "mtl_d3d11/d3d11shader_metal.h"
#include "mtl_d3d11/d3dx11_metal.h"
#include "mtl_d3d11/mtl_d3d11.h"
#include "mtl_d3d11/mtl_shader.h"

namespace mtl_d3d11
{
class Device;
class Context;
class SwapChain;
struct TextureCore;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define MTL_ASSERT(cond, msg)                                                                     \
    do                                                                                            \
    {                                                                                             \
        if (!(cond))                                                                              \
        {                                                                                         \
            ::mtl_d3d11::Log("MetalD3D11 assertion failed: %s (%s:%d): %s", #cond, __FILE__, __LINE__, msg); \
            __builtin_trap();                                                                     \
        }                                                                                         \
    } while (0)

// ---------------------------------------------------------------------------------------------------------
// Reference counting
// ---------------------------------------------------------------------------------------------------------
template <class I>
class ComObject : public I
{
public:
    ULONG AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG Release() override
    {
        ULONG r = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT QueryInterface(MtlREFIID riid, void** pp) override
    {
        if (!pp)
            return E_POINTER;
        if (riid == mtl_uuidof<IUnknown>() || riid == mtl_uuidof<I>())
        {
            *pp = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }

protected:
    ~ComObject() override = default;

private:
    std::atomic<ULONG> refs_{1};
};

// Strong pointer to a ComObject-derived implementation class.
template <class T>
class Ref
{
public:
    Ref() = default;
    Ref(T* p) : p_(p) { if (p_) p_->AddRef(); }
    Ref(const Ref& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Ref(Ref&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Ref() { if (p_) p_->Release(); }
    Ref& operator=(T* p) { reset(p); return *this; }
    Ref& operator=(const Ref& o) { reset(o.p_); return *this; }
    Ref& operator=(Ref&& o) noexcept { if (this != &o) { if (p_) p_->Release(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    void reset(T* p = nullptr) { if (p) p->AddRef(); if (p_) p_->Release(); p_ = p; }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    bool operator==(const Ref& o) const { return p_ == o.p_; }
    bool operator!=(const Ref& o) const { return p_ != o.p_; }

private:
    T* p_ = nullptr;
};

// ---------------------------------------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------------------------------------
struct FormatInfo
{
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    MTLPixelFormat mtl = MTLPixelFormatInvalid; // concrete format for typed DXGI formats
    uint8_t bytesPerBlock = 0;                  // per pixel, or per 4x4 block when compressed
    uint8_t blockSize = 1;                      // 1 for plain formats, 4 for BC
    bool compressed = false;
    bool depth = false;
    bool stencil = false;
    bool typeless = false;
    bool srgb = false;
    bool integer = false;
};

const FormatInfo& GetFormatInfo(DXGI_FORMAT f);
// Storage format of a texture created with DXGI format `f` and the given D3D11 bind flags (resolves typeless).
MTLPixelFormat TextureStorageFormat(DXGI_FORMAT f, UINT bindFlags);
// Format a view of DXGI format `f` should use; sets *stencilOnly for the X24/X32 stencil-plane views.
MTLPixelFormat ViewFormat(DXGI_FORMAT f, bool* stencilOnly);
MTLVertexFormat VertexFormatFor(DXGI_FORMAT f);
uint32_t MipCountFor(uint32_t w, uint32_t h, uint32_t d);
size_t RowPitch(const FormatInfo& fi, uint32_t width);
size_t SliceBytes(const FormatInfo& fi, uint32_t width, uint32_t height);
inline uint32_t Max1(uint32_t v) { return v ? v : 1; }

// ---------------------------------------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------------------------------------
enum class ResKind : uint8_t { Buffer, Tex1D, Tex2D, Tex3D };

struct ResourceCore
{
    ResourceCore(Device* d, ResKind k) : dev(d), kind(k) {}
    virtual ~ResourceCore() = default;
    Device* dev;
    ResKind kind;
    UINT bindFlags = 0;
    UINT cpuAccess = 0;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    uint64_t lastUseSerial = 0;   // newest command buffer that encodes a use of this resource
    uint64_t lastWriteSerial = 0; // newest command buffer that writes it on the GPU (staging read-back)
    uint32_t pendingClears = 0;   // folded-clear entries waiting for a render pass
    std::string debugName;
};

struct BufferBacking
{
    id<MTLBuffer> buf = nil;
    size_t offset = 0;      // into buf (non-zero for ring sub-allocations)
    uint8_t* ptr = nullptr; // CPU address of the first byte
    uint64_t lastUse = 0;   // serial of the newest command buffer using this backing
    bool ring = false;
};

struct BufferCore : ResourceCore
{
    BufferCore(Device* d) : ResourceCore(d, ResKind::Buffer) {}
    ~BufferCore() override;
    UINT byteWidth = 0;
    UINT miscFlags = 0;
    UINT structureStride = 0;
    BufferBacking cur;
    std::vector<BufferBacking> pool; // rename pool for large DYNAMIC buffers
    std::vector<uint8_t> shadow;     // CPU copy for DEFAULT buffers updated via UpdateSubresource
};

struct SubresourceLayout
{
    size_t offset = 0;
    size_t rowPitch = 0;
    size_t slicePitch = 0;
    uint32_t width = 0, height = 0, depth = 0;
};

struct TextureCore : ResourceCore
{
    TextureCore(Device* d, ResKind k) : ResourceCore(d, k) {}
    ~TextureCore() override;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    UINT width = 1, height = 1, depth = 1, mips = 1, arraySize = 1, samples = 1, sampleQuality = 0, miscFlags = 0;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    MTLPixelFormat mtlFormat = MTLPixelFormatInvalid;
    bool cube = false;
    bool isBackBuffer = false;
    SwapChain* swapChain = nullptr;

    id<MTLTexture> tex = nil;              // current backing (null for staging)
    std::vector<id<MTLTexture>> renamePool; // DYNAMIC textures rotate through these
    size_t renameIndex = 0;
    std::vector<uint64_t> renameLastUse;

    // STAGING: all subresources packed into one shared buffer.
    id<MTLBuffer> staging = nil;
    std::vector<SubresourceLayout> layout;
    // DYNAMIC Map(WRITE_DISCARD): CPU shadow written by the app, copied to the backing on Unmap.
    std::vector<uint8_t> dynShadow;
    SubresourceLayout dynLayout;

    virtual id<MTLTexture> current(Context* ctx); // back buffers resolve their drawable here
    UINT subresourceCount() const { return mips * arraySize; }
    UINT calcSubresource(UINT mip, UINT slice) const { return mip + slice * mips; }
};

class Buffer final : public ComObject<ID3D11Buffer>, public BufferCore
{
public:
    explicit Buffer(Device* d);
    ~Buffer() override;
    void GetDevice(ID3D11Device** pp) override;
    void GetType(D3D11_RESOURCE_DIMENSION* p) override { *p = D3D11_RESOURCE_DIMENSION_BUFFER; }
    void SetEvictionPriority(UINT) override {}
    UINT GetEvictionPriority() override { return 0; }
    void GetDesc(D3D11_BUFFER_DESC* p) override;
};

template <class I, class Desc>
class TextureImpl final : public ComObject<I>, public TextureCore
{
public:
    TextureImpl(Device* d, ResKind k);
    ~TextureImpl() override;
    void GetDevice(ID3D11Device** pp) override;
    void GetType(D3D11_RESOURCE_DIMENSION* p) override { *p = dim; }
    void SetEvictionPriority(UINT) override {}
    UINT GetEvictionPriority() override { return 0; }
    void GetDesc(Desc* p) override;
};
typedef TextureImpl<ID3D11Texture1D, D3D11_TEXTURE1D_DESC> Texture1D;
typedef TextureImpl<ID3D11Texture2D, D3D11_TEXTURE2D_DESC> Texture2D;
typedef TextureImpl<ID3D11Texture3D, D3D11_TEXTURE3D_DESC> Texture3D;

ResourceCore* CoreOf(ID3D11Resource* r);
inline BufferCore* AsBuffer(ID3D11Resource* r)
{
    ResourceCore* c = CoreOf(r);
    return c && c->kind == ResKind::Buffer ? static_cast<BufferCore*>(c) : nullptr;
}
inline TextureCore* AsTexture(ID3D11Resource* r)
{
    ResourceCore* c = CoreOf(r);
    return c && c->kind != ResKind::Buffer ? static_cast<TextureCore*>(c) : nullptr;
}

// ---------------------------------------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------------------------------------
class DeviceChildBase
{
public:
    explicit DeviceChildBase(Device* d);
    ~DeviceChildBase();
    Device* dev;
    std::string debugName;
};

template <class I>
class ChildObject : public ComObject<I>, public DeviceChildBase
{
public:
    explicit ChildObject(Device* d) : DeviceChildBase(d) {}
    void GetDevice(ID3D11Device** pp) override;
};

// Identity of one render-pass attachment: a texture subresource.
struct AttachmentKey
{
    void* texture = nullptr; // resolved MTLTexture pointer
    uint32_t level = 0;
    uint32_t slice = 0;
    uint32_t plane = 0;
    bool operator==(const AttachmentKey& o) const
    {
        return texture == o.texture && level == o.level && slice == o.slice && plane == o.plane;
    }
};

class ShaderResourceView final : public ChildObject<ID3D11ShaderResourceView>
{
public:
    ShaderResourceView(Device* d, ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC& desc);
    ~ShaderResourceView() override;
    void GetResource(ID3D11Resource** pp) override;
    void GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* p) override { *p = desc; }
    id<MTLTexture> texture(Context* ctx) const; // current texture to bind
    ID3D11Resource* res;
    ResourceCore* core;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc;
    id<MTLTexture> view = nil; // explicit view when the format or range differs from the base texture
    bool depthView = false;
    bool stencilView = false;
    id<MTLBuffer> buffer = nil;
};

class RenderTargetView final : public ChildObject<ID3D11RenderTargetView>
{
public:
    RenderTargetView(Device* d, ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC& desc);
    ~RenderTargetView() override;
    void GetResource(ID3D11Resource** pp) override;
    void GetDesc(D3D11_RENDER_TARGET_VIEW_DESC* p) override { *p = desc; }
    id<MTLTexture> texture(Context* ctx) const;
    ID3D11Resource* res;
    TextureCore* tex;
    D3D11_RENDER_TARGET_VIEW_DESC desc;
    id<MTLTexture> view = nil; // only when the format differs from the texture's
    MTLPixelFormat format = MTLPixelFormatInvalid;
    uint32_t level = 0, slice = 0, plane = 0;
};

class DepthStencilView final : public ChildObject<ID3D11DepthStencilView>
{
public:
    DepthStencilView(Device* d, ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC& desc);
    ~DepthStencilView() override;
    void GetResource(ID3D11Resource** pp) override;
    void GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC* p) override { *p = desc; }
    id<MTLTexture> texture(Context* ctx) const;
    ID3D11Resource* res;
    TextureCore* tex;
    D3D11_DEPTH_STENCIL_VIEW_DESC desc;
    MTLPixelFormat format = MTLPixelFormatInvalid;
    bool hasStencil = false;
    uint32_t level = 0, slice = 0;
};

class UnorderedAccessView final : public ChildObject<ID3D11UnorderedAccessView>
{
public:
    UnorderedAccessView(Device* d, ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC& desc);
    ~UnorderedAccessView() override;
    void GetResource(ID3D11Resource** pp) override;
    void GetDesc(D3D11_UNORDERED_ACCESS_VIEW_DESC* p) override { *p = desc; }
    id<MTLTexture> texture(Context* ctx) const;
    ID3D11Resource* res;
    ResourceCore* core;
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc;
    id<MTLTexture> view = nil;
    id<MTLBuffer> buffer = nil;
};

// ---------------------------------------------------------------------------------------------------------
// State objects
// ---------------------------------------------------------------------------------------------------------
class SamplerState final : public ChildObject<ID3D11SamplerState>
{
public:
    SamplerState(Device* d, const D3D11_SAMPLER_DESC& desc) : ChildObject(d), desc(desc) {}
    void GetDesc(D3D11_SAMPLER_DESC* p) override { *p = desc; }
    D3D11_SAMPLER_DESC desc;
    id<MTLSamplerState> mtl = nil;
};

class DepthStencilState final : public ChildObject<ID3D11DepthStencilState>
{
public:
    DepthStencilState(Device* d, const D3D11_DEPTH_STENCIL_DESC& desc) : ChildObject(d), desc(desc) {}
    void GetDesc(D3D11_DEPTH_STENCIL_DESC* p) override { *p = desc; }
    D3D11_DEPTH_STENCIL_DESC desc;
    id<MTLDepthStencilState> mtl = nil;
    id<MTLDepthStencilState> mtlDepthOnly = nil; // same depth test, no stencil (pass without a stencil plane)
};

class BlendState final : public ChildObject<ID3D11BlendState>
{
public:
    BlendState(Device* d, const D3D11_BLEND_DESC& desc) : ChildObject(d), desc(desc) {}
    void GetDesc(D3D11_BLEND_DESC* p) override { *p = desc; }
    D3D11_BLEND_DESC desc;
    uint64_t hash = 0;
};

class RasterizerState final : public ChildObject<ID3D11RasterizerState>
{
public:
    RasterizerState(Device* d, const D3D11_RASTERIZER_DESC& desc) : ChildObject(d), desc(desc) {}
    void GetDesc(D3D11_RASTERIZER_DESC* p) override { *p = desc; }
    D3D11_RASTERIZER_DESC desc;
};

struct InputElement
{
    std::string semantic; // upper-case
    uint32_t semanticIndex = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t slot = 0;
    uint32_t offset = 0;
    D3D11_INPUT_CLASSIFICATION cls = D3D11_INPUT_PER_VERTEX_DATA;
    uint32_t stepRate = 0;
};

class InputLayout final : public ChildObject<ID3D11InputLayout>
{
public:
    explicit InputLayout(Device* d) : ChildObject(d) {}
    std::vector<InputElement> elements;
    uint64_t hash = 0;
};

// Shader object shared by all stages.
template <class I>
class ShaderObject final : public ChildObject<I>
{
public:
    explicit ShaderObject(Device* d) : ChildObject<I>(d) {}
    ShaderReflection refl;
    id<MTLLibrary> lib = nil;
    id<MTLFunction> fn = nil;
    id<MTLComputePipelineState> computePso = nil;
    uint64_t id = 0;
};
typedef ShaderObject<ID3D11VertexShader> VertexShader;
typedef ShaderObject<ID3D11PixelShader> PixelShader;
typedef ShaderObject<ID3D11ComputeShader> ComputeShader;
typedef ShaderObject<ID3D11GeometryShader> GeometryShader;
typedef ShaderObject<ID3D11HullShader> HullShader;
typedef ShaderObject<ID3D11DomainShader> DomainShader;

class Query final : public ChildObject<ID3D11Query>
{
public:
    Query(Device* d, const D3D11_QUERY_DESC& desc) : ChildObject(d), desc(desc) {}
    UINT GetDataSize() override { return desc.Query == D3D11_QUERY_OCCLUSION ? 8 : 4; }
    void GetDesc(D3D11_QUERY_DESC* p) override { *p = desc; }
    D3D11_QUERY_DESC desc;
    // Guarded by the device's query mutex.
    bool active = false;  // between Begin and End
    bool ended = false;   // End() was issued
    bool done = false;    // result available
    uint64_t serial = 0;  // command buffer that contains End()
    uint64_t result = 0;
    std::vector<const uint64_t*> slotPtrs; // occlusion: visibility slots written for this query
};

// ---------------------------------------------------------------------------------------------------------
// Pipeline state cache key
// ---------------------------------------------------------------------------------------------------------
struct PipelineKey
{
    uint64_t vs = 0, ps = 0;
    uint64_t layout = 0;
    uint64_t strides = 0; // hash of the bound vertex strides per slot
    uint64_t blend = 0;
    uint32_t colorFormat[8] = {};
    uint32_t depthFormat = 0, stencilFormat = 0;
    uint8_t samples = 1;
    uint8_t topologyClass = 0;
    bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
struct PipelineKeyHash
{
    size_t operator()(const PipelineKey& k) const;
};

uint64_t HashBytes(const void* p, size_t n, uint64_t seed = 1469598103934665603ull);

// ---------------------------------------------------------------------------------------------------------
// Ring allocator: per-frame slabs of shared memory, reused after the GPU has finished with the frame.
// ---------------------------------------------------------------------------------------------------------
struct RingAlloc
{
    id<MTLBuffer> buf = nil;
    size_t offset = 0;
    uint8_t* ptr = nullptr;
    bool dedicated = false;
};

class Ring
{
public:
    void init(id<MTLDevice> dev, size_t chunkBytes, uint32_t slabCount, const char* label);
    // Sub-allocates `bytes` from the current frame's chunks, growing by whole chunks when needed. Fails only
    // when bytes > chunk size; the caller then uses a dedicated buffer.
    bool alloc(size_t bytes, size_t align, RingAlloc* out);
    uint32_t current() const { return cur_; }
    void tagCurrent(uint64_t serial) { slabs_[cur_].serial = serial; }
    // Moves to the next slab; returns the serial that must be complete before it can be reused (0 if free).
    uint64_t advance();
    size_t usedThisFrame() const { return used_; }
    size_t chunkBytes() const { return chunkBytes_; }
    size_t totalBytes() const;

private:
    struct Slab
    {
        std::vector<id<MTLBuffer>> chunks;
        uint64_t serial = 0;
    };
    id<MTLDevice> dev_ = nil;
    std::vector<Slab> slabs_;
    std::string label_;
    size_t chunkBytes_ = 0;
    uint32_t cur_ = 0;
    size_t chunk_ = 0, head_ = 0, used_ = 0;
};

// ---------------------------------------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------------------------------------
class Device final : public ComObject<ID3D11Device1>
{
public:
    Device(const DeviceDesc& desc);
    ~Device() override;
    bool init();

    HRESULT QueryInterface(MtlREFIID riid, void** pp) override;

    HRESULT CreateBuffer(const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**) override;
    HRESULT CreateTexture1D(const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**) override;
    HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**) override;
    HRESULT CreateTexture3D(const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**) override;
    HRESULT CreateShaderResourceView(ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*,
                                     ID3D11ShaderResourceView**) override;
    HRESULT CreateUnorderedAccessView(ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*,
                                      ID3D11UnorderedAccessView**) override;
    HRESULT CreateRenderTargetView(ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*,
                                   ID3D11RenderTargetView**) override;
    HRESULT CreateDepthStencilView(ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*,
                                   ID3D11DepthStencilView**) override;
    HRESULT CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**) override;
    HRESULT CreateVertexShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**) override;
    HRESULT CreateGeometryShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**) override;
    HRESULT CreatePixelShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**) override;
    HRESULT CreateHullShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11HullShader**) override;
    HRESULT CreateDomainShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader**) override;
    HRESULT CreateComputeShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**) override;
    HRESULT CreateBlendState(const D3D11_BLEND_DESC*, ID3D11BlendState**) override;
    HRESULT CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC*, ID3D11DepthStencilState**) override;
    HRESULT CreateRasterizerState(const D3D11_RASTERIZER_DESC*, ID3D11RasterizerState**) override;
    HRESULT CreateSamplerState(const D3D11_SAMPLER_DESC*, ID3D11SamplerState**) override;
    HRESULT CreateQuery(const D3D11_QUERY_DESC*, ID3D11Query**) override;
    HRESULT CheckFormatSupport(DXGI_FORMAT, UINT*) override;
    HRESULT CheckMultisampleQualityLevels(DXGI_FORMAT, UINT, UINT*) override;
    UINT GetCreationFlags() override { return createFlags; }
    HRESULT GetDeviceRemovedReason() override { return S_OK; }
    void GetImmediateContext(ID3D11DeviceContext** pp) override;
    void GetImmediateContext1(ID3D11DeviceContext1** pp) override;
    D3D_FEATURE_LEVEL GetFeatureLevel() override { return D3D_FEATURE_LEVEL_11_0; }

    template <class Shader, class Iface>
    HRESULT createShader(const void* bytecode, SIZE_T size, ShaderStage expected, Iface** out);

    // Shared state
    DeviceDesc desc;
    UINT createFlags = 0;
    id<MTLDevice> mtl = nil;
    id<MTLCommandQueue> queue = nil;
    Context* context = nullptr; // immediate context (not ref-counted by the device)
    std::atomic<uint64_t> nextShaderId{1};
    std::atomic<uint64_t> textureBytes{0};
    std::atomic<uint64_t> bufferBytes{0};
    bool supportsBC = false;
    bool supportsDepth24Stencil8 = false;
    bool supportsDualSourceBlend = true;
    MTLPixelFormat depthStencilFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLTexture> dummyTexture2D = nil, dummyTexture3D = nil, dummyTextureCube = nil, dummyDepth2D = nil,
                   dummyTexture2DMS = nil;
    id<MTLSamplerState> dummySampler = nil;
    id<MTLBuffer> dummyBuffer = nil, zeroVertexBuffer = nil;
    id<MTLDepthStencilState> noDepthState = nil; // depth test off, no writes (passes without a depth attachment)

    // Serial bookkeeping shared with the context's completion handlers.
    std::mutex fenceMutex;
    std::condition_variable fenceCv;
    std::atomic<uint64_t> completedSerial{0};
    void waitForSerial(uint64_t s);
    void noteCompleted(uint64_t s);

    std::mutex queryMutex;

    id<MTLBuffer> newBuffer(size_t bytes, bool writeCombined, const char* label);
    id<MTLTexture> newTexture(MTLTextureDescriptor* d, const char* label);

private:
    HRESULT createTexture(ResKind kind, const void* descIn, const D3D11_SUBRESOURCE_DATA* init, TextureCore* out);
};

// Back-buffer wrapper for swap chains: a Texture2D whose current() resolves the drawable.
Texture2D* NewBackBuffer(Device* dev, SwapChain* sc, UINT w, UINT h, DXGI_FORMAT fmt, MTLPixelFormat mtlFmt);

} // namespace mtl_d3d11
