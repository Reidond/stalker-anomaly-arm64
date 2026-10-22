// ID3D11Device implementation: resource, view, state and shader creation.
#import "mtl_internal.h"
#include <chrono>
#import "mtl_context.h"

#include <algorithm>
#include <cmath>

namespace mtl_d3d11
{
// ---------------------------------------------------------------------------------------------------------
// Ring
// ---------------------------------------------------------------------------------------------------------
void Ring::init(id<MTLDevice> dev, size_t chunkBytes, uint32_t slabCount, const char* label)
{
    dev_ = dev;
    label_ = label;
    chunkBytes_ = chunkBytes;
    slabs_.assign(slabCount, Slab());
    for (Slab& s : slabs_)
        s.chunks.reserve(8);
    cur_ = 0;
    chunk_ = 0;
    head_ = 0;
    used_ = 0;
}

size_t Ring::totalBytes() const
{
    size_t n = 0;
    for (const Slab& s : slabs_)
        n += s.chunks.size() * chunkBytes_;
    return n;
}

bool Ring::alloc(size_t bytes, size_t align, RingAlloc* out)
{
    if (bytes > chunkBytes_)
        return false;
    Slab& slab = slabs_[cur_];
    size_t start = (head_ + align - 1) & ~(align - 1);
    if (slab.chunks.empty() || start + bytes > chunkBytes_)
    {
        if (!slab.chunks.empty())
            ++chunk_;
        if (chunk_ >= slab.chunks.size())
        {
            id<MTLBuffer> b = [dev_ newBufferWithLength:chunkBytes_
                                                options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined |
                                                        MTLResourceHazardTrackingModeUntracked];
            b.label = [NSString stringWithFormat:@"%s slab %u chunk %zu", label_.c_str(), cur_, slab.chunks.size()];
            slab.chunks.push_back(b);
            chunk_ = slab.chunks.size() - 1;
        }
        head_ = 0;
        start = 0;
    }
    id<MTLBuffer> buf = slab.chunks[chunk_];
    out->buf = buf;
    out->offset = start;
    out->ptr = static_cast<uint8_t*>(buf.contents) + start;
    out->dedicated = false;
    head_ = start + bytes;
    used_ += bytes;
    return true;
}

uint64_t Ring::advance()
{
    cur_ = (cur_ + 1) % uint32_t(slabs_.size());
    chunk_ = 0;
    head_ = 0;
    used_ = 0;
    return slabs_[cur_].serial;
}

size_t PipelineKeyHash::operator()(const PipelineKey& k) const { return size_t(HashBytes(&k, sizeof(k))); }

// ---------------------------------------------------------------------------------------------------------
// Resource plumbing
// ---------------------------------------------------------------------------------------------------------
BufferCore::~BufferCore() = default;
TextureCore::~TextureCore() = default;

id<MTLTexture> BackBufferCurrent(TextureCore* t);
id<MTLTexture> TextureCore::current(Context*) { return isBackBuffer ? BackBufferCurrent(this) : tex; }

DeviceChildBase::DeviceChildBase(Device* d) : dev(d) { d->AddRef(); }
DeviceChildBase::~DeviceChildBase() { dev->Release(); }

template <class I>
void ChildObject<I>::GetDevice(ID3D11Device** pp)
{
    dev->AddRef();
    *pp = dev;
}

Buffer::Buffer(Device* d) : BufferCore(d) { d->AddRef(); }
Buffer::~Buffer()
{
    dev->bufferBytes -= std::min<uint64_t>(dev->bufferBytes, byteWidth);
    dev->Release();
}
void Buffer::GetDevice(ID3D11Device** pp)
{
    dev->AddRef();
    *pp = dev;
}
void Buffer::GetDesc(D3D11_BUFFER_DESC* p)
{
    p->ByteWidth = byteWidth;
    p->Usage = usage;
    p->BindFlags = bindFlags;
    p->CPUAccessFlags = cpuAccess;
    p->MiscFlags = miscFlags;
    p->StructureByteStride = structureStride;
}

template <class I, class Desc>
TextureImpl<I, Desc>::TextureImpl(Device* d, ResKind k) : TextureCore(d, k) { d->AddRef(); }
template <class I, class Desc>
TextureImpl<I, Desc>::~TextureImpl()
{
    if (!isBackBuffer)
    {
        size_t bytes = 0;
        const FormatInfo& fi = GetFormatInfo(dxgi);
        for (UINT m = 0; m < mips; ++m)
            bytes += SliceBytes(fi, Max1(width >> m), Max1(height >> m)) * Max1(depth >> m) * arraySize * samples;
        dev->textureBytes -= std::min<uint64_t>(dev->textureBytes, bytes);
        if (bindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS))
            dev->renderTargetBytes -= std::min<uint64_t>(dev->renderTargetBytes, bytes);
    }
    dev->Release();
}
template <class I, class Desc>
void TextureImpl<I, Desc>::GetDevice(ID3D11Device** pp)
{
    dev->AddRef();
    *pp = dev;
}
template <>
void Texture1D::GetDesc(D3D11_TEXTURE1D_DESC* p)
{
    *p = {width, mips, arraySize, dxgi, usage, bindFlags, cpuAccess, miscFlags};
}
template <>
void Texture2D::GetDesc(D3D11_TEXTURE2D_DESC* p)
{
    *p = {width, height, mips, arraySize, dxgi, {samples, sampleQuality}, usage, bindFlags, cpuAccess, miscFlags};
}
template <>
void Texture3D::GetDesc(D3D11_TEXTURE3D_DESC* p)
{
    *p = {width, height, depth, mips, dxgi, usage, bindFlags, cpuAccess, miscFlags};
}
template class TextureImpl<ID3D11Texture1D, D3D11_TEXTURE1D_DESC>;
template class TextureImpl<ID3D11Texture2D, D3D11_TEXTURE2D_DESC>;
template class TextureImpl<ID3D11Texture3D, D3D11_TEXTURE3D_DESC>;
template class ChildObject<ID3D11ShaderResourceView>;
template class ChildObject<ID3D11RenderTargetView>;
template class ChildObject<ID3D11DepthStencilView>;
template class ChildObject<ID3D11UnorderedAccessView>;
template class ChildObject<ID3D11SamplerState>;
template class ChildObject<ID3D11DepthStencilState>;
template class ChildObject<ID3D11BlendState>;
template class ChildObject<ID3D11RasterizerState>;
template class ChildObject<ID3D11InputLayout>;
template class ChildObject<ID3D11Query>;
template class ChildObject<ID3D11VertexShader>;
template class ChildObject<ID3D11PixelShader>;
template class ChildObject<ID3D11GeometryShader>;
template class ChildObject<ID3D11HullShader>;
template class ChildObject<ID3D11DomainShader>;
template class ChildObject<ID3D11ComputeShader>;

ResourceCore* CoreOf(ID3D11Resource* r)
{
    if (!r)
        return nullptr;
    D3D11_RESOURCE_DIMENSION d;
    r->GetType(&d);
    switch (d)
    {
    case D3D11_RESOURCE_DIMENSION_BUFFER: return static_cast<Buffer*>(static_cast<ID3D11Buffer*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return static_cast<Texture1D*>(static_cast<ID3D11Texture1D*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return static_cast<Texture2D*>(static_cast<ID3D11Texture2D*>(r));
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: return static_cast<Texture3D*>(static_cast<ID3D11Texture3D*>(r));
    default: return nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------------------------------------
ShaderResourceView::ShaderResourceView(Device* d, ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC& dsc)
    : ChildObject(d), res(r), core(CoreOf(r)), desc(dsc)
{
    res->AddRef();
}
ShaderResourceView::~ShaderResourceView() { res->Release(); }
void ShaderResourceView::GetResource(ID3D11Resource** pp)
{
    res->AddRef();
    *pp = res;
}
id<MTLTexture> ShaderResourceView::texture(Context* ctx) const
{
    if (view)
        return view;
    return static_cast<TextureCore*>(core)->current(ctx);
}

RenderTargetView::RenderTargetView(Device* d, ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC& dsc)
    : ChildObject(d), res(r), tex(static_cast<TextureCore*>(CoreOf(r))), desc(dsc)
{
    res->AddRef();
}
RenderTargetView::~RenderTargetView() { res->Release(); }
void RenderTargetView::GetResource(ID3D11Resource** pp)
{
    res->AddRef();
    *pp = res;
}
id<MTLTexture> RenderTargetView::texture(Context* ctx) const { return view ? view : tex->current(ctx); }

DepthStencilView::DepthStencilView(Device* d, ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC& dsc)
    : ChildObject(d), res(r), tex(static_cast<TextureCore*>(CoreOf(r))), desc(dsc)
{
    res->AddRef();
}
DepthStencilView::~DepthStencilView() { res->Release(); }
void DepthStencilView::GetResource(ID3D11Resource** pp)
{
    res->AddRef();
    *pp = res;
}
id<MTLTexture> DepthStencilView::texture(Context* ctx) const { return tex->current(ctx); }

UnorderedAccessView::UnorderedAccessView(Device* d, ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC& dsc)
    : ChildObject(d), res(r), core(CoreOf(r)), desc(dsc)
{
    res->AddRef();
}
UnorderedAccessView::~UnorderedAccessView() { res->Release(); }
void UnorderedAccessView::GetResource(ID3D11Resource** pp)
{
    res->AddRef();
    *pp = res;
}
id<MTLTexture> UnorderedAccessView::texture(Context* ctx) const
{
    if (view)
        return view;
    return static_cast<TextureCore*>(core)->current(ctx);
}

// ---------------------------------------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------------------------------------
namespace
{
std::atomic<Device*> g_latestDevice{nullptr};
}
Device* LatestDevice() { return g_latestDevice.load(); }

Device::Device(const DeviceDesc& d) : desc(d) { g_latestDevice = this; }

Device::~Device()
{
    // The immediate context shares this object's reference count, so it is never referenced from outside now.
    delete context;
    context = nullptr;
}

HRESULT Device::QueryInterface(MtlREFIID riid, void** pp)
{
    if (!pp)
        return E_POINTER;
    if (riid == mtl_uuidof<IUnknown>() || riid == mtl_uuidof<ID3D11Device>() || riid == mtl_uuidof<ID3D11Device1>())
    {
        *pp = static_cast<ID3D11Device1*>(this);
        AddRef();
        return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
}

void Device::waitForSerial(uint64_t s)
{
    if (completedSerial.load(std::memory_order_acquire) >= s)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::unique_lock<std::mutex> lk(fenceMutex);
        fenceCv.wait(lk, [&] { return completedSerial.load(std::memory_order_acquire) >= s; });
    }
    blockedNanos += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

void Device::noteCompleted(uint64_t s)
{
    {
        std::lock_guard<std::mutex> lk(fenceMutex);
        uint64_t cur = completedSerial.load();
        if (s > cur)
            completedSerial.store(s, std::memory_order_release);
    }
    fenceCv.notify_all();
}

id<MTLBuffer> Device::newBuffer(size_t bytes, bool writeCombined, const char* label)
{
    MTLResourceOptions o = MTLResourceStorageModeShared;
    if (writeCombined)
        o |= MTLResourceCPUCacheModeWriteCombined;
    id<MTLBuffer> b = [mtl newBufferWithLength:std::max<size_t>(bytes, 16) options:o];
    if (label)
        b.label = [NSString stringWithUTF8String:label];
    return b;
}

id<MTLTexture> Device::newTexture(MTLTextureDescriptor* d, const char* label)
{
    id<MTLTexture> t = [mtl newTextureWithDescriptor:d];
    if (t && label)
        t.label = [NSString stringWithUTF8String:label];
    return t;
}

bool Device::init()
{
    mtl = desc.mtlDevice ? (__bridge id<MTLDevice>)desc.mtlDevice : MTLCreateSystemDefaultDevice();
    if (!mtl)
    {
        Log("no Metal device available");
        return false;
    }
    if (!mtl.hasUnifiedMemory)
    {
        Log("this shim requires unified memory (Apple silicon)");
        return false;
    }
    queue = [mtl newCommandQueueWithMaxCommandBufferCount:64];
    queue.label = @"MetalD3D11";
    supportsBC = [mtl supportsBCTextureCompression];
    supportsDepth24Stencil8 = false; // not on Apple GPUs

    // Placeholders bound when the shader samples a slot the app left empty (D3D reads zeros).
    auto mk = [&](MTLTextureType type, MTLPixelFormat fmt, NSUInteger w, NSUInteger h, NSUInteger depth, NSUInteger samples) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor new];
        d.textureType = type;
        d.pixelFormat = fmt;
        d.width = w;
        d.height = h;
        d.depth = depth;
        d.sampleCount = samples;
        d.mipmapLevelCount = 1;
        d.usage = MTLTextureUsageShaderRead | (samples > 1 ? MTLTextureUsageRenderTarget : 0);
        d.storageMode = samples > 1 ? MTLStorageModePrivate : MTLStorageModeShared;
        id<MTLTexture> t = [mtl newTextureWithDescriptor:d];
        t.label = @"D3D11 placeholder";
        return t;
    };
    dummyTexture2D = mk(MTLTextureType2D, MTLPixelFormatRGBA8Unorm, 1, 1, 1, 1);
    dummyTexture3D = mk(MTLTextureType3D, MTLPixelFormatRGBA8Unorm, 1, 1, 1, 1);
    dummyDepth2D = mk(MTLTextureType2D, MTLPixelFormatDepth32Float, 1, 1, 1, 1);
    {
        MTLTextureDescriptor* d = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         size:1
                                                                                    mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        dummyTextureCube = [mtl newTextureWithDescriptor:d];
    }
    if ([mtl supportsTextureSampleCount:4])
        dummyTexture2DMS = mk(MTLTextureType2DMultisample, MTLPixelFormatRGBA8Unorm, 1, 1, 1, 4);
    const uint8_t zero[4] = {0, 0, 0, 0};
    for (id<MTLTexture> t in @[dummyTexture2D, dummyTexture3D, dummyTextureCube])
        if (t.textureType == MTLTextureType2D || t.textureType == MTLTextureType3D)
            [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:zero bytesPerRow:4 bytesPerImage:4];
    for (int f = 0; f < 6; ++f)
        [dummyTextureCube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:f withBytes:zero bytesPerRow:4 bytesPerImage:4];
    {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        dummySampler = [mtl newSamplerStateWithDescriptor:sd];
    }
    {
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = NO;
        noDepthState = [mtl newDepthStencilStateWithDescriptor:dd];
    }
    dummyBuffer = newBuffer(64 * 1024, false, "D3D11 placeholder cbuffer");
    std::memset(dummyBuffer.contents, 0, dummyBuffer.length);
    zeroVertexBuffer = newBuffer(256, false, "D3D11 zero vertex data");
    std::memset(zeroVertexBuffer.contents, 0, zeroVertexBuffer.length);

    context = new Context(this);
    if (!context->init())
        return false;
    return true;
}

void Device::GetImmediateContext(ID3D11DeviceContext** pp)
{
    context->AddRef();
    *pp = context;
}

void Device::GetImmediateContext1(ID3D11DeviceContext1** pp)
{
    context->AddRef();
    *pp = context;
}

// ---------------------------------------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------------------------------------
HRESULT Device::CreateBuffer(const D3D11_BUFFER_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out)
{
    if (!d || !out || d->ByteWidth == 0)
        return E_INVALIDARG;
    if (d->Usage == D3D11_USAGE_IMMUTABLE && !init)
        return E_INVALIDARG;
    Buffer* b = new Buffer(this);
    b->byteWidth = d->ByteWidth;
    b->usage = d->Usage;
    b->bindFlags = d->BindFlags;
    b->cpuAccess = d->CPUAccessFlags;
    b->miscFlags = d->MiscFlags;
    b->structureStride = d->StructureByteStride;
    const bool wc = d->Usage == D3D11_USAGE_DYNAMIC && !(d->CPUAccessFlags & D3D11_CPU_ACCESS_READ);
    id<MTLBuffer> mb = newBuffer(d->ByteWidth, wc, nullptr);
    if (!mb)
    {
        b->Release();
        return E_OUTOFMEMORY;
    }
    b->cur.buf = mb;
    b->cur.offset = 0;
    b->cur.ptr = static_cast<uint8_t*>(mb.contents);
    if (init && init->pSysMem)
        std::memcpy(b->cur.ptr, init->pSysMem, d->ByteWidth);
    else
        std::memset(b->cur.ptr, 0, d->ByteWidth);
    bufferBytes += d->ByteWidth;
    *out = b;
    return S_OK;
}

// ---------------------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------------------
namespace
{
struct TexParams
{
    ResKind kind;
    UINT w, h, d, mips, arr;
    DXGI_FORMAT fmt;
    UINT samples, quality;
    D3D11_USAGE usage;
    UINT bind, cpu, misc;
};

MTLTextureType textureTypeFor(const TexParams& p)
{
    switch (p.kind)
    {
    case ResKind::Tex1D: return p.arr > 1 ? MTLTextureType1DArray : MTLTextureType1D;
    case ResKind::Tex3D: return MTLTextureType3D;
    default:
        if (p.samples > 1)
            return p.arr > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
        if (p.misc & D3D11_RESOURCE_MISC_TEXTURECUBE)
            return p.arr > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
        return p.arr > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    }
}
} // namespace

HRESULT Device::createTexture(ResKind kind, const void* descIn, const D3D11_SUBRESOURCE_DATA* init, TextureCore* t)
{
    TexParams p{};
    p.kind = kind;
    p.d = 1;
    p.arr = 1;
    p.samples = 1;
    switch (kind)
    {
    case ResKind::Tex1D:
    {
        auto* d = static_cast<const D3D11_TEXTURE1D_DESC*>(descIn);
        p.w = d->Width; p.h = 1; p.mips = d->MipLevels; p.arr = d->ArraySize ? d->ArraySize : 1; p.fmt = d->Format;
        p.usage = d->Usage; p.bind = d->BindFlags; p.cpu = d->CPUAccessFlags; p.misc = d->MiscFlags;
        break;
    }
    case ResKind::Tex2D:
    {
        auto* d = static_cast<const D3D11_TEXTURE2D_DESC*>(descIn);
        p.w = d->Width; p.h = d->Height; p.mips = d->MipLevels; p.arr = d->ArraySize ? d->ArraySize : 1; p.fmt = d->Format;
        p.samples = d->SampleDesc.Count ? d->SampleDesc.Count : 1; p.quality = d->SampleDesc.Quality;
        p.usage = d->Usage; p.bind = d->BindFlags; p.cpu = d->CPUAccessFlags; p.misc = d->MiscFlags;
        break;
    }
    default:
    {
        auto* d = static_cast<const D3D11_TEXTURE3D_DESC*>(descIn);
        p.w = d->Width; p.h = d->Height; p.d = d->Depth; p.mips = d->MipLevels; p.fmt = d->Format;
        p.usage = d->Usage; p.bind = d->BindFlags; p.cpu = d->CPUAccessFlags; p.misc = d->MiscFlags;
        break;
    }
    }
    if (p.w == 0 || p.h == 0 || p.d == 0)
        return E_INVALIDARG;
    if (p.mips == 0)
        p.mips = MipCountFor(p.w, p.h, p.d);
    if (p.usage == D3D11_USAGE_IMMUTABLE && !init)
        return E_INVALIDARG;
    const FormatInfo& fi = GetFormatInfo(p.fmt);
    if (fi.dxgi == DXGI_FORMAT_UNKNOWN)
        return E_INVALIDARG;
    const bool cube = (p.misc & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
    if (cube && (p.arr % 6) != 0)
        return E_INVALIDARG;

    t->usage = p.usage;
    t->bindFlags = p.bind;
    t->cpuAccess = p.cpu;
    t->miscFlags = p.misc;
    t->width = p.w; t->height = p.h; t->depth = p.d; t->mips = p.mips; t->arraySize = p.arr;
    t->samples = p.samples; t->sampleQuality = p.quality; t->dxgi = p.fmt; t->cube = cube;
    t->dim = kind == ResKind::Tex1D ? D3D11_RESOURCE_DIMENSION_TEXTURE1D
             : kind == ResKind::Tex3D ? D3D11_RESOURCE_DIMENSION_TEXTURE3D
                                      : D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    t->layout.clear();

    // Subresource layout (used by STAGING and for uploads).
    size_t total = 0;
    for (UINT s = 0; s < p.arr; ++s)
        for (UINT m = 0; m < p.mips; ++m)
        {
            SubresourceLayout l;
            l.width = Max1(p.w >> m);
            l.height = Max1(p.h >> m);
            l.depth = Max1(p.d >> m);
            l.rowPitch = (RowPitch(fi, l.width) + 15) & ~size_t(15);
            const size_t rows = fi.compressed ? (l.height + fi.blockSize - 1) / fi.blockSize : l.height;
            l.slicePitch = l.rowPitch * rows;
            l.offset = (total + 255) & ~size_t(255);
            total = l.offset + l.slicePitch * l.depth;
            t->layout.push_back(l);
        }

    const size_t totalSub = t->layout.size();
    if (init)
    {
        // validated by callers: init has one entry per subresource
        (void)totalSub;
    }

    const size_t texBytes = [&] {
        size_t b = 0;
        for (UINT m = 0; m < p.mips; ++m)
            b += SliceBytes(fi, Max1(p.w >> m), Max1(p.h >> m)) * Max1(p.d >> m);
        return b * p.arr * p.samples;
    }();

    if (p.usage == D3D11_USAGE_STAGING)
    {
        t->staging = newBuffer(total, false, "D3D11 staging texture");
        if (!t->staging)
            return E_OUTOFMEMORY;
        std::memset(t->staging.contents, 0, total);
        if (init)
            for (size_t i = 0; i < t->layout.size(); ++i)
            {
                const SubresourceLayout& l = t->layout[i];
                const uint8_t* src = static_cast<const uint8_t*>(init[i].pSysMem);
                const size_t rows = fi.compressed ? (l.height + fi.blockSize - 1) / fi.blockSize : l.height;
                const size_t srcRow = init[i].SysMemPitch ? init[i].SysMemPitch : RowPitch(fi, l.width);
                const size_t srcSlice = init[i].SysMemSlicePitch ? init[i].SysMemSlicePitch : srcRow * rows;
                for (UINT z = 0; z < l.depth; ++z)
                    for (size_t r = 0; r < rows; ++r)
                        std::memcpy(static_cast<uint8_t*>(t->staging.contents) + l.offset + z * l.slicePitch + r * l.rowPitch,
                                    src + z * srcSlice + r * srcRow, std::min(srcRow, l.rowPitch));
            }
        textureBytes += texBytes;
        if (p.bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS))
            renderTargetBytes += texBytes;
        return S_OK;
    }

    MTLPixelFormat pf = TextureStorageFormat(p.fmt, p.bind);
    if (pf == MTLPixelFormatInvalid || (fi.compressed && !supportsBC))
        return E_INVALIDARG;
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType = textureTypeFor(p);
    td.pixelFormat = pf;
    td.width = p.w;
    td.height = kind == ResKind::Tex1D ? 1 : p.h;
    td.depth = kind == ResKind::Tex3D ? p.d : 1;
    td.mipmapLevelCount = p.samples > 1 ? 1 : p.mips;
    td.sampleCount = p.samples;
    td.arrayLength = (td.textureType == MTLTextureTypeCube) ? 1 : (td.textureType == MTLTextureTypeCubeArray ? p.arr / 6 : p.arr);
    if (p.samples > 1 && ![mtl supportsTextureSampleCount:p.samples])
        return E_INVALIDARG;
    MTLTextureUsage u = 0;
    if (p.bind & D3D11_BIND_SHADER_RESOURCE) u |= MTLTextureUsageShaderRead;
    if (p.bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) u |= MTLTextureUsageRenderTarget;
    if (p.bind & D3D11_BIND_UNORDERED_ACCESS) u |= MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    if (fi.typeless && !fi.depth) u |= MTLTextureUsagePixelFormatView;
    // Blit mip generation wants a renderable texture; D3DX11FilterTexture-style callers create these empty.
    if (p.mips > 1 && p.samples == 1 && !fi.compressed && !fi.depth && p.usage == D3D11_USAGE_DEFAULT &&
        ((p.misc & D3D11_RESOURCE_MISC_GENERATE_MIPS) || !init))
        u |= MTLTextureUsageRenderTarget;
    if (fi.depth && fi.stencil && (p.bind & D3D11_BIND_SHADER_RESOURCE)) u |= MTLTextureUsagePixelFormatView;
    if (u == 0) u = MTLTextureUsageShaderRead;
    td.usage = u;

    const bool dynamic = p.usage == D3D11_USAGE_DYNAMIC;
    const bool gpuOnly = (p.bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS)) ||
                         p.samples > 1 || (p.usage == D3D11_USAGE_DEFAULT && !init);
    td.storageMode = (gpuOnly && !dynamic) ? MTLStorageModePrivate : MTLStorageModeShared;
    if (p.usage == D3D11_USAGE_DEFAULT && init && !gpuOnly)
        td.storageMode = MTLStorageModeShared; // DEFAULT with initial data and no GPU writes: upload in place

    t->mtlFormat = pf;
    t->tex = newTexture(td, nullptr);
    if (!t->tex)
        return E_OUTOFMEMORY;
    if (dynamic)
    {
        t->renamePool.push_back(t->tex);
        t->renameLastUse.push_back(0);
        for (int i = 1; i < 3; ++i)
        {
            t->renamePool.push_back(newTexture(td, nullptr));
            t->renameLastUse.push_back(0);
        }
    }
    textureBytes += texBytes * (dynamic ? 3 : 1);
    if (p.bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS))
        renderTargetBytes += texBytes;

    if (init)
    {
        for (UINT s = 0; s < p.arr; ++s)
            for (UINT m = 0; m < p.mips; ++m)
            {
                const size_t idx = size_t(m) + size_t(s) * p.mips;
                const SubresourceLayout& l = t->layout[idx];
                if (!init[idx].pSysMem)
                    continue;
                const size_t rowPitch = init[idx].SysMemPitch ? init[idx].SysMemPitch : RowPitch(fi, l.width);
                const size_t rows = fi.compressed ? (l.height + fi.blockSize - 1) / fi.blockSize : l.height;
                const size_t slicePitch = init[idx].SysMemSlicePitch ? init[idx].SysMemSlicePitch : rowPitch * rows;
                if (td.storageMode == MTLStorageModeShared)
                {
                    [t->tex replaceRegion:MTLRegionMake3D(0, 0, 0, l.width, l.height, l.depth)
                              mipmapLevel:m
                                    slice:s
                                withBytes:init[idx].pSysMem
                              bytesPerRow:rowPitch
                            bytesPerImage:slicePitch];
                }
                else
                {
                    context->uploadTexture(t, m, s, init[idx].pSysMem, rowPitch, slicePitch);
                }
            }
    }
    return S_OK;
}

HRESULT Device::CreateTexture1D(const D3D11_TEXTURE1D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture1D** out)
{
    if (!d || !out)
        return E_INVALIDARG;
    Texture1D* t = new Texture1D(this, ResKind::Tex1D);
    HRESULT hr = createTexture(ResKind::Tex1D, d, init, t);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    *out = t;
    return S_OK;
}

HRESULT Device::CreateTexture2D(const D3D11_TEXTURE2D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out)
{
    if (!d || !out)
        return E_INVALIDARG;
    Texture2D* t = new Texture2D(this, ResKind::Tex2D);
    HRESULT hr = createTexture(ResKind::Tex2D, d, init, t);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    *out = t;
    return S_OK;
}

HRESULT Device::CreateTexture3D(const D3D11_TEXTURE3D_DESC* d, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture3D** out)
{
    if (!d || !out)
        return E_INVALIDARG;
    Texture3D* t = new Texture3D(this, ResKind::Tex3D);
    HRESULT hr = createTexture(ResKind::Tex3D, d, init, t);
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    *out = t;
    return S_OK;
}

Texture2D* NewBackBuffer(Device* dev, SwapChain* sc, UINT w, UINT h, DXGI_FORMAT fmt, MTLPixelFormat mtlFmt)
{
    Texture2D* t = new Texture2D(dev, ResKind::Tex2D);
    t->isBackBuffer = true;
    t->swapChain = sc;
    t->width = w;
    t->height = h;
    t->dxgi = fmt;
    t->mtlFormat = mtlFmt;
    t->bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    t->dim = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    return t;
}

// ---------------------------------------------------------------------------------------------------------
// View creation
// ---------------------------------------------------------------------------------------------------------
HRESULT Device::CreateShaderResourceView(ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* d,
                                         ID3D11ShaderResourceView** out)
{
    if (!r || !out)
        return E_INVALIDARG;
    ResourceCore* core = CoreOf(r);
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    if (core->kind == ResKind::Buffer)
    {
        if (d)
            desc = *d;
        else
        {
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        }
        auto* v = new ShaderResourceView(this, r, desc);
        v->buffer = static_cast<BufferCore*>(core)->cur.buf;
        *out = v;
        return S_OK;
    }
    TextureCore* t = static_cast<TextureCore*>(core);
    if (t->usage == D3D11_USAGE_STAGING || !(t->bindFlags & D3D11_BIND_SHADER_RESOURCE))
        return E_INVALIDARG;
    if (d)
        desc = *d;
    else
    {
        desc.Format = t->dxgi;
        desc.ViewDimension = t->cube ? D3D11_SRV_DIMENSION_TEXTURECUBE
                             : t->dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? D3D11_SRV_DIMENSION_TEXTURE3D
                             : t->dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D ? (t->arraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE1DARRAY : D3D11_SRV_DIMENSION_TEXTURE1D)
                             : t->samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS
                             : t->arraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY
                                                : D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MostDetailedMip = 0;
        desc.Texture2D.MipLevels = (UINT)-1;
    }
    if (desc.Format == DXGI_FORMAT_UNKNOWN)
        desc.Format = t->dxgi;

    auto* v = new ShaderResourceView(this, r, desc);
    if (t->isBackBuffer)
    {
        *out = v;
        return S_OK;
    }
    bool stencilOnly = false;
    MTLPixelFormat vf = ViewFormat(desc.Format, &stencilOnly);
    if (GetFormatInfo(desc.Format).depth && !stencilOnly)
        vf = t->mtlFormat; // depth plane: bind the depth texture itself
    v->stencilView = stencilOnly;

    UINT mostDetailed = 0, mipCount = t->mips, firstSlice = 0, sliceCount = t->arraySize;
    switch (desc.ViewDimension)
    {
    case D3D11_SRV_DIMENSION_TEXTURE1D:
    case D3D11_SRV_DIMENSION_TEXTURE2D:
    case D3D11_SRV_DIMENSION_TEXTURE3D:
    case D3D11_SRV_DIMENSION_TEXTURECUBE:
        mostDetailed = desc.Texture2D.MostDetailedMip;
        mipCount = desc.Texture2D.MipLevels;
        break;
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        mostDetailed = desc.Texture2DArray.MostDetailedMip;
        mipCount = desc.Texture2DArray.MipLevels;
        firstSlice = desc.Texture2DArray.FirstArraySlice;
        sliceCount = desc.Texture2DArray.ArraySize;
        break;
    default: break;
    }
    if (mipCount == (UINT)-1 || mipCount == 0)
        mipCount = t->mips - mostDetailed;
    const bool fullRange = mostDetailed == 0 && mipCount == t->mips && firstSlice == 0 && sliceCount == t->arraySize;
    const bool sameFormat = vf == t->mtlFormat;
    if (!fullRange || !sameFormat || stencilOnly)
    {
        MTLTextureType tt = t->tex.textureType;
        id<MTLTexture> base = t->tex;
        if (base)
        {
            NSRange levels = NSMakeRange(mostDetailed, mipCount);
            NSRange slices = NSMakeRange(firstSlice, sliceCount);
            if (tt == MTLTextureTypeCube || tt == MTLTextureType3D || tt == MTLTextureType2D || tt == MTLTextureType1D ||
                tt == MTLTextureType2DMultisample)
                slices = NSMakeRange(0, 1);
            if (tt == MTLTextureTypeCube)
                slices = NSMakeRange(0, 6);
            if (tt == MTLTextureTypeCubeArray)
                slices = NSMakeRange(firstSlice, sliceCount);
            v->view = [base newTextureViewWithPixelFormat:vf textureType:tt levels:levels slices:slices];
            if (!v->view)
            {
                v->Release();
                return E_FAIL;
            }
        }
    }
    *out = v;
    return S_OK;
}

HRESULT Device::CreateRenderTargetView(ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* d,
                                       ID3D11RenderTargetView** out)
{
    if (!r || !out)
        return E_INVALIDARG;
    ResourceCore* core = CoreOf(r);
    if (core->kind == ResKind::Buffer)
        return E_NOTIMPL;
    TextureCore* t = static_cast<TextureCore*>(core);
    if (!(t->bindFlags & D3D11_BIND_RENDER_TARGET))
        return E_INVALIDARG;
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    if (d)
        desc = *d;
    else
    {
        desc.Format = t->dxgi;
        desc.ViewDimension = t->dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D ? D3D11_RTV_DIMENSION_TEXTURE3D
                             : t->samples > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                             : t->arraySize > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY
                                                : D3D11_RTV_DIMENSION_TEXTURE2D;
        if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
            desc.Texture2DArray.ArraySize = t->arraySize;
    }
    if (desc.Format == DXGI_FORMAT_UNKNOWN)
        desc.Format = t->dxgi;
    auto* v = new RenderTargetView(this, r, desc);
    MTLPixelFormat vf = t->isBackBuffer ? t->mtlFormat : ViewFormat(desc.Format, nullptr);
    v->format = vf;
    switch (desc.ViewDimension)
    {
    case D3D11_RTV_DIMENSION_TEXTURE2D: v->level = desc.Texture2D.MipSlice; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
        v->level = desc.Texture2DArray.MipSlice;
        v->slice = desc.Texture2DArray.FirstArraySlice;
        break;
    case D3D11_RTV_DIMENSION_TEXTURE3D:
        v->level = desc.Texture3D.MipSlice;
        v->plane = desc.Texture3D.FirstWSlice;
        break;
    default: break;
    }
    if (!t->isBackBuffer && vf != t->mtlFormat && t->tex)
    {
        v->view = [t->tex newTextureViewWithPixelFormat:vf];
        if (!v->view)
        {
            v->Release();
            return E_FAIL;
        }
    }
    *out = v;
    return S_OK;
}

HRESULT Device::CreateDepthStencilView(ID3D11Resource* r, const D3D11_DEPTH_STENCIL_VIEW_DESC* d,
                                       ID3D11DepthStencilView** out)
{
    if (!r || !out)
        return E_INVALIDARG;
    ResourceCore* core = CoreOf(r);
    if (core->kind == ResKind::Buffer)
        return E_INVALIDARG;
    TextureCore* t = static_cast<TextureCore*>(core);
    if (!(t->bindFlags & D3D11_BIND_DEPTH_STENCIL))
        return E_INVALIDARG;
    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
    if (d)
        desc = *d;
    else
    {
        desc.Format = t->dxgi;
        desc.ViewDimension = t->samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS
                             : t->arraySize > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY
                                                : D3D11_DSV_DIMENSION_TEXTURE2D;
    }
    if (desc.Format == DXGI_FORMAT_UNKNOWN)
        desc.Format = t->dxgi;
    auto* v = new DepthStencilView(this, r, desc);
    v->format = t->mtlFormat;
    v->hasStencil = t->mtlFormat == MTLPixelFormatDepth32Float_Stencil8 || t->mtlFormat == MTLPixelFormatStencil8;
    if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D)
        v->level = desc.Texture2D.MipSlice;
    else if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY)
    {
        v->level = desc.Texture2DArray.MipSlice;
        v->slice = desc.Texture2DArray.FirstArraySlice;
    }
    *out = v;
    return S_OK;
}

HRESULT Device::CreateUnorderedAccessView(ID3D11Resource* r, const D3D11_UNORDERED_ACCESS_VIEW_DESC* d,
                                          ID3D11UnorderedAccessView** out)
{
    if (!r || !out)
        return E_INVALIDARG;
    ResourceCore* core = CoreOf(r);
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
    if (d)
        desc = *d;
    else if (core->kind == ResKind::Buffer)
        desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    auto* v = new UnorderedAccessView(this, r, desc);
    if (core->kind == ResKind::Buffer)
    {
        v->buffer = static_cast<BufferCore*>(core)->cur.buf;
    }
    else
    {
        TextureCore* t = static_cast<TextureCore*>(core);
        if (!(t->bindFlags & D3D11_BIND_UNORDERED_ACCESS))
        {
            v->Release();
            return E_INVALIDARG;
        }
        const DXGI_FORMAT f = desc.Format == DXGI_FORMAT_UNKNOWN ? t->dxgi : desc.Format;
        MTLPixelFormat vf = ViewFormat(f, nullptr);
        const bool sub = desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D && desc.Texture2D.MipSlice != 0;
        if (vf != t->mtlFormat || sub)
        {
            UINT mip = desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D ? desc.Texture2D.MipSlice : 0;
            v->view = [t->tex newTextureViewWithPixelFormat:vf
                                                textureType:t->tex.textureType
                                                     levels:NSMakeRange(mip, 1)
                                                     slices:NSMakeRange(0, t->tex.arrayLength)];
        }
    }
    *out = v;
    return S_OK;
}

// ---------------------------------------------------------------------------------------------------------
// State objects
// ---------------------------------------------------------------------------------------------------------
namespace
{
MTLCompareFunction mapCompare(D3D11_COMPARISON_FUNC f)
{
    switch (f)
    {
    case D3D11_COMPARISON_NEVER: return MTLCompareFunctionNever;
    case D3D11_COMPARISON_LESS: return MTLCompareFunctionLess;
    case D3D11_COMPARISON_EQUAL: return MTLCompareFunctionEqual;
    case D3D11_COMPARISON_LESS_EQUAL: return MTLCompareFunctionLessEqual;
    case D3D11_COMPARISON_GREATER: return MTLCompareFunctionGreater;
    case D3D11_COMPARISON_NOT_EQUAL: return MTLCompareFunctionNotEqual;
    case D3D11_COMPARISON_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation mapStencilOp(D3D11_STENCIL_OP o)
{
    switch (o)
    {
    case D3D11_STENCIL_OP_ZERO: return MTLStencilOperationZero;
    case D3D11_STENCIL_OP_REPLACE: return MTLStencilOperationReplace;
    case D3D11_STENCIL_OP_INCR_SAT: return MTLStencilOperationIncrementClamp;
    case D3D11_STENCIL_OP_DECR_SAT: return MTLStencilOperationDecrementClamp;
    case D3D11_STENCIL_OP_INVERT: return MTLStencilOperationInvert;
    case D3D11_STENCIL_OP_INCR: return MTLStencilOperationIncrementWrap;
    case D3D11_STENCIL_OP_DECR: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

MTLSamplerAddressMode mapAddress(D3D11_TEXTURE_ADDRESS_MODE m)
{
    switch (m)
    {
    case D3D11_TEXTURE_ADDRESS_WRAP: return MTLSamplerAddressModeRepeat;
    case D3D11_TEXTURE_ADDRESS_MIRROR: return MTLSamplerAddressModeMirrorRepeat;
    case D3D11_TEXTURE_ADDRESS_BORDER: return MTLSamplerAddressModeClampToBorderColor;
    case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeClampToEdge;
    }
}
} // namespace

HRESULT Device::CreateSamplerState(const D3D11_SAMPLER_DESC* d, ID3D11SamplerState** out)
{
    if (!d || !out)
        return E_INVALIDARG;
    auto* s = new SamplerState(this, *d);
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    const UINT f = d->Filter;
    const bool aniso = (f & 0x55) == 0x55;
    const bool cmp = (f & 0x80) != 0 && (f & 0x100) == 0;
    sd.mipFilter = (f & 0x1) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    sd.magFilter = (f & 0x4) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.minFilter = (f & 0x10) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.maxAnisotropy = aniso ? std::min<NSUInteger>(std::max<UINT>(d->MaxAnisotropy, 1), 16) : 1;
    sd.sAddressMode = mapAddress(d->AddressU);
    sd.tAddressMode = mapAddress(d->AddressV);
    sd.rAddressMode = mapAddress(d->AddressW);
    sd.lodMinClamp = std::max(d->MinLOD, 0.0f);
    sd.lodMaxClamp = std::min(d->MaxLOD, 1000.0f);
    if (cmp)
        sd.compareFunction = mapCompare(d->ComparisonFunc);
    if (d->AddressU == D3D11_TEXTURE_ADDRESS_BORDER || d->AddressV == D3D11_TEXTURE_ADDRESS_BORDER ||
        d->AddressW == D3D11_TEXTURE_ADDRESS_BORDER)
    {
        const float* c = d->BorderColor;
        if (c[3] <= 0.0f)
            sd.borderColor = MTLSamplerBorderColorTransparentBlack;
        else if (c[0] > 0.5f && c[1] > 0.5f && c[2] > 0.5f)
            sd.borderColor = MTLSamplerBorderColorOpaqueWhite;
        else
            sd.borderColor = MTLSamplerBorderColorOpaqueBlack;
    }
    // MipLODBias has no Metal sampler equivalent; applying it needs a bias in the shader.
    sd.normalizedCoordinates = YES;
    s->mtl = [mtl newSamplerStateWithDescriptor:sd];
    if (!s->mtl)
    {
        s->Release();
        return E_FAIL;
    }
    *out = s;
    return S_OK;
}

HRESULT Device::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* d, ID3D11DepthStencilState** out)
{
    if (!d || !out)
        return E_INVALIDARG;
    auto* s = new DepthStencilState(this, *d);
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = d->DepthEnable ? mapCompare(d->DepthFunc) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = d->DepthEnable && d->DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
    if (d->StencilEnable)
    {
        auto fill = [&](MTLStencilDescriptor* sd, const D3D11_DEPTH_STENCILOP_DESC& o) {
            sd.stencilFailureOperation = mapStencilOp(o.StencilFailOp);
            sd.depthFailureOperation = mapStencilOp(o.StencilDepthFailOp);
            sd.depthStencilPassOperation = mapStencilOp(o.StencilPassOp);
            sd.stencilCompareFunction = mapCompare(o.StencilFunc);
            sd.readMask = d->StencilReadMask;
            sd.writeMask = d->StencilWriteMask;
        };
        dd.frontFaceStencil = [MTLStencilDescriptor new];
        dd.backFaceStencil = [MTLStencilDescriptor new];
        fill(dd.frontFaceStencil, d->FrontFace);
        fill(dd.backFaceStencil, d->BackFace);
    }
    s->mtl = [mtl newDepthStencilStateWithDescriptor:dd];
    if (!s->mtl)
    {
        s->Release();
        return E_FAIL;
    }
    if (d->StencilEnable)
    {
        dd.frontFaceStencil = nil;
        dd.backFaceStencil = nil;
        s->mtlDepthOnly = [mtl newDepthStencilStateWithDescriptor:dd];
    }
    else
        s->mtlDepthOnly = s->mtl;
    *out = s;
    return S_OK;
}

HRESULT Device::CreateBlendState(const D3D11_BLEND_DESC* d, ID3D11BlendState** out)
{
    if (!d || !out)
        return E_INVALIDARG;
    auto* s = new BlendState(this, *d);
    // Hash only the fields that reach the pipeline (normalised so equal states hash equally).
    D3D11_BLEND_DESC n = *d;
    const int count = n.IndependentBlendEnable ? 8 : 1;
    for (int i = count; i < 8; ++i)
        n.RenderTarget[i] = n.RenderTarget[0];
    s->hash = HashBytes(&n, sizeof(n));
    *out = s;
    return S_OK;
}

HRESULT Device::CreateRasterizerState(const D3D11_RASTERIZER_DESC* d, ID3D11RasterizerState** out)
{
    if (!d || !out)
        return E_INVALIDARG;
    *out = new RasterizerState(this, *d);
    return S_OK;
}

HRESULT Device::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* elems, UINT n, const void*, SIZE_T, ID3D11InputLayout** out)
{
    if (!elems || !out || n == 0)
        return E_INVALIDARG;
    auto* l = new InputLayout(this);
    uint64_t h = 1469598103934665603ull;
    uint32_t running[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    for (UINT i = 0; i < n; ++i)
    {
        InputElement e;
        for (const char* c = elems[i].SemanticName; *c; ++c)
            e.semantic.push_back(char(toupper(*c)));
        e.semanticIndex = elems[i].SemanticIndex;
        e.format = elems[i].Format;
        e.slot = elems[i].InputSlot;
        if (e.slot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT || VertexFormatFor(e.format) == MTLVertexFormatInvalid)
        {
            l->Release();
            return E_INVALIDARG;
        }
        e.offset = elems[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? running[e.slot] : elems[i].AlignedByteOffset;
        e.cls = elems[i].InputSlotClass;
        e.stepRate = elems[i].InstanceDataStepRate;
        running[e.slot] = e.offset + GetFormatInfo(e.format).bytesPerBlock;
        h = HashBytes(e.semantic.data(), e.semantic.size(), h);
        h = HashBytes(&e.semanticIndex, 4, h);
        h = HashBytes(&e.format, 4, h);
        h = HashBytes(&e.slot, 4, h);
        h = HashBytes(&e.offset, 4, h);
        h = HashBytes(&e.cls, 4, h);
        h = HashBytes(&e.stepRate, 4, h);
        l->elements.push_back(std::move(e));
    }
    l->hash = h;
    *out = l;
    return S_OK;
}

// ---------------------------------------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------------------------------------
id<MTLLibrary> Device::newLibraryFromMsl(NSString* source, NSError** err)
{
    MTLCompileOptions* opts = [MTLCompileOptions new];
    if (@available(macOS 15.0, iOS 18.0, *))
        opts.mathMode = desc.fastMath ? MTLMathModeFast : MTLMathModeSafe;
    else
        opts.fastMathEnabled = desc.fastMath;
    opts.languageVersion = MTLLanguageVersion3_0;
    return [mtl newLibraryWithSource:source options:opts error:err];
}

template <class Shader, class Iface>
HRESULT Device::createShader(const void* bytecode, SIZE_T size, ShaderStage expected, Iface** out)
{
    if (!bytecode || !out)
        return E_INVALIDARG;
    ShaderBlob blob;
    if (!Deserialize(bytecode, size, &blob))
    {
        Log("CreateShader: bytecode is not a MetalD3D11 shader container (%zu bytes)", size);
        return E_INVALIDARG;
    }
    if (blob.reflection.stage != expected)
        return E_INVALIDARG;
    auto* s = new Shader(this);
    s->refl = std::move(blob.reflection);
    NSError* err = nil;
    if (blob.kind == PayloadKind::MslSource)
    {
        NSString* src = [[NSString alloc] initWithBytes:blob.payload.data() length:blob.payload.size() encoding:NSUTF8StringEncoding];
        s->lib = newLibraryFromMsl(src, &err);
        if (blob.reflection.stage == ShaderStage::Vertex || blob.reflection.stage == ShaderStage::Pixel)
        {
            const std::string text(reinterpret_cast<const char*>(blob.payload.data()), blob.payload.size());
            const bool vertex = blob.reflection.stage == ShaderStage::Vertex;
            s->interpolants = ParseInterpolants(text, vertex ? "main0_out" : "main0_in");
            if (!vertex)
                s->msl = text;
        }
    }
    else
    {
        dispatch_data_t data = dispatch_data_create(blob.payload.data(), blob.payload.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        s->lib = [mtl newLibraryWithData:data error:&err];
    }
    if (!s->lib)
    {
        Log("CreateShader: Metal library failed: %s", err ? err.localizedDescription.UTF8String : "(no error)");
        s->Release();
        return E_FAIL;
    }
    s->fn = [s->lib newFunctionWithName:[NSString stringWithUTF8String:s->refl.entryPoint.c_str()]];
    if (!s->fn)
    {
        Log("CreateShader: entry point '%s' not found", s->refl.entryPoint.c_str());
        s->Release();
        return E_FAIL;
    }
    s->id = nextShaderId.fetch_add(1);
    *out = s;
    return S_OK;
}

HRESULT Device::CreateVertexShader(const void* b, SIZE_T n, ID3D11ClassLinkage*, ID3D11VertexShader** out)
{
    return createShader<VertexShader>(b, n, ShaderStage::Vertex, out);
}
HRESULT Device::CreatePixelShader(const void* b, SIZE_T n, ID3D11ClassLinkage*, ID3D11PixelShader** out)
{
    return createShader<PixelShader>(b, n, ShaderStage::Pixel, out);
}
HRESULT Device::CreateComputeShader(const void* b, SIZE_T n, ID3D11ClassLinkage*, ID3D11ComputeShader** out)
{
    return createShader<ComputeShader>(b, n, ShaderStage::Compute, out);
}
HRESULT Device::CreateGeometryShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader** out)
{
    if (out)
        *out = nullptr;
    return E_NOTIMPL; // only the 3D fluid blenders use GS; see METAL-SHIM.md
}
HRESULT Device::CreateHullShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11HullShader** out)
{
    if (out)
        *out = nullptr;
    return E_NOTIMPL; // tessellation is off by default and not implemented
}
HRESULT Device::CreateDomainShader(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader** out)
{
    if (out)
        *out = nullptr;
    return E_NOTIMPL;
}

// ---------------------------------------------------------------------------------------------------------
// Queries and capability checks
// ---------------------------------------------------------------------------------------------------------
HRESULT Device::CreateQuery(const D3D11_QUERY_DESC* d, ID3D11Query** out)
{
    if (!d || !out)
        return E_INVALIDARG;
    if (d->Query != D3D11_QUERY_EVENT && d->Query != D3D11_QUERY_OCCLUSION && d->Query != D3D11_QUERY_OCCLUSION_PREDICATE)
        return E_NOTIMPL;
    *out = new Query(this, *d);
    return S_OK;
}

HRESULT Device::CheckFormatSupport(DXGI_FORMAT f, UINT* support)
{
    if (!support)
        return E_INVALIDARG;
    const FormatInfo& fi = GetFormatInfo(f);
    MTLPixelFormat pf = fi.typeless ? TextureStorageFormat(f, 0) : fi.mtl;
    *support = 0;
    if (pf == MTLPixelFormatInvalid)
        return f == DXGI_FORMAT_R32G32B32_FLOAT ? S_OK : E_FAIL;
    if (fi.compressed && !supportsBC)
        return E_FAIL;
    UINT s = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TEXTURE3D | D3D11_FORMAT_SUPPORT_TEXTURECUBE |
             D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_MIP;
    if (fi.depth)
        s |= D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
    else if (!fi.compressed)
        s |= D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET |
             D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
    *support = s;
    return S_OK;
}

HRESULT Device::CheckMultisampleQualityLevels(DXGI_FORMAT, UINT samples, UINT* levels)
{
    if (!levels)
        return E_INVALIDARG;
    *levels = (samples == 1 || [mtl supportsTextureSampleCount:samples]) ? 1 : 0;
    return S_OK;
}
} // namespace mtl_d3d11

// ---------------------------------------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------------------------------------
namespace mtl_d3d11
{
HRESULT CreateDevice(const DeviceDesc* desc, ID3D11Device** ppDevice, ID3D11DeviceContext** ppContext)
{
    DeviceDesc d = desc ? *desc : DeviceDesc();
    Device* dev = new Device(d);
    if (!dev->init())
    {
        dev->Release();
        return E_FAIL;
    }
    if (ppDevice)
    {
        dev->AddRef();
        *ppDevice = dev;
    }
    if (ppContext)
    {
        dev->context->AddRef(); // forwards to the device's count
        *ppContext = dev->context;
    }
    dev->Release(); // drop the creation reference; the caller's references keep the device alive
    return S_OK;
}

void* GetNativeDevice(ID3D11Device* d) { return (__bridge void*)static_cast<Device*>(static_cast<ID3D11Device1*>(d))->mtl; }
void* GetNativeTexture(ID3D11Resource* r)
{
    TextureCore* t = AsTexture(r);
    return t ? (__bridge void*)t->tex : nullptr;
}
void* GetNativeBuffer(ID3D11Resource* r)
{
    BufferCore* b = AsBuffer(r);
    return b ? (__bridge void*)b->cur.buf : nullptr;
}
void SetDebugName(ID3D11DeviceChild* o, const char* name)
{
    if (!o || !name)
        return;
    if (DeviceChildBase* c = dynamic_cast<DeviceChildBase*>(o))
        c->debugName = name;
    // Resources carry the label on the Metal object.
    if (ID3D11Resource* r = dynamic_cast<ID3D11Resource*>(o))
    {
        if (TextureCore* t = AsTexture(r))
            t->tex.label = [NSString stringWithUTF8String:name];
        else if (BufferCore* b = AsBuffer(r))
            b->cur.buf.label = [NSString stringWithUTF8String:name];
    }
}
} // namespace mtl_d3d11

HRESULT D3D11CreateDevice(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT Flags, const D3D_FEATURE_LEVEL*, UINT, UINT,
                          ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppContext)
{
    mtl_d3d11::DeviceDesc d;
    d.validation = (Flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
    HRESULT hr = mtl_d3d11::CreateDevice(&d, ppDevice, ppContext);
    if (SUCCEEDED(hr) && pFeatureLevel)
        *pFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return hr;
}
