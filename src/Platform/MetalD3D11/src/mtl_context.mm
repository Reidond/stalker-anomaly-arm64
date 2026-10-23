// ID3D11DeviceContext on Metal: state tracking, lazy render passes, draws, copies, queries.
#import "mtl_context.h"
#import "mtl_swapchain.h"
#include <mtl_d3d11/mtl_stats.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

namespace mtl_d3d11
{
// Process-wide totals for the app's frame-time log (GPU time of every command buffer, frames presented)
static std::atomic<uint64_t> g_gpuTimeNs{0};
static std::atomic<uint64_t> g_presentCount{0};
uint64_t GpuTimeNsTotal() { return g_gpuTimeNs.load(std::memory_order_relaxed); }
uint64_t PresentCountTotal() { return g_presentCount.load(std::memory_order_relaxed); }

namespace
{
constexpr size_t kRingMaxAlloc = 256 * 1024; // larger DISCARD maps use per-buffer rename pools
constexpr uint32_t kVisSlots = 4096;
constexpr int kPoolMax = 8;
constexpr uint32_t kZeroVertexIndex = kVertexBufferBase + kMaxVertexStreams - 1;

MTLBlendFactor mapBlend(D3D11_BLEND b)
{
    switch (b)
    {
    case D3D11_BLEND_ZERO: return MTLBlendFactorZero;
    case D3D11_BLEND_ONE: return MTLBlendFactorOne;
    case D3D11_BLEND_SRC_COLOR: return MTLBlendFactorSourceColor;
    case D3D11_BLEND_INV_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case D3D11_BLEND_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case D3D11_BLEND_INV_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case D3D11_BLEND_DEST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case D3D11_BLEND_INV_DEST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case D3D11_BLEND_DEST_COLOR: return MTLBlendFactorDestinationColor;
    case D3D11_BLEND_INV_DEST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case D3D11_BLEND_SRC_ALPHA_SAT: return MTLBlendFactorSourceAlphaSaturated;
    case D3D11_BLEND_BLEND_FACTOR: return MTLBlendFactorBlendColor;
    case D3D11_BLEND_INV_BLEND_FACTOR: return MTLBlendFactorOneMinusBlendColor;
    case D3D11_BLEND_SRC1_COLOR: return MTLBlendFactorSource1Color;
    case D3D11_BLEND_INV_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
    case D3D11_BLEND_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
    case D3D11_BLEND_INV_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation mapBlendOp(D3D11_BLEND_OP o)
{
    switch (o)
    {
    case D3D11_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
    case D3D11_BLEND_OP_REV_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case D3D11_BLEND_OP_MIN: return MTLBlendOperationMin;
    case D3D11_BLEND_OP_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLColorWriteMask mapWriteMask(UINT8 m)
{
    MTLColorWriteMask r = MTLColorWriteMaskNone;
    if (m & D3D11_COLOR_WRITE_ENABLE_RED) r |= MTLColorWriteMaskRed;
    if (m & D3D11_COLOR_WRITE_ENABLE_GREEN) r |= MTLColorWriteMaskGreen;
    if (m & D3D11_COLOR_WRITE_ENABLE_BLUE) r |= MTLColorWriteMaskBlue;
    if (m & D3D11_COLOR_WRITE_ENABLE_ALPHA) r |= MTLColorWriteMaskAlpha;
    return r;
}

bool mapTopology(D3D11_PRIMITIVE_TOPOLOGY t, MTLPrimitiveType* type, MTLPrimitiveTopologyClass* cls)
{
    switch (t)
    {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: *type = MTLPrimitiveTypePoint; *cls = MTLPrimitiveTopologyClassPoint; return true;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: *type = MTLPrimitiveTypeLine; *cls = MTLPrimitiveTopologyClassLine; return true;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: *type = MTLPrimitiveTypeLineStrip; *cls = MTLPrimitiveTopologyClassLine; return true;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: *type = MTLPrimitiveTypeTriangle; *cls = MTLPrimitiveTopologyClassTriangle; return true;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: *type = MTLPrimitiveTypeTriangleStrip; *cls = MTLPrimitiveTopologyClassTriangle; return true;
    default: return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------------------------------------
class Annotation final : public ID3DUserDefinedAnnotation
{
public:
    explicit Annotation(Context* c) : ctx(c) {}
    HRESULT QueryInterface(MtlREFIID riid, void** pp) override { return ctx->QueryInterface(riid, pp); }
    ULONG AddRef() override { return ctx->AddRef(); }
    ULONG Release() override { return ctx->Release(); }
    INT BeginEvent(LPCWSTR name) override
    {
        ctx->ensureCommandBuffer();
        NSString* s = [[NSString alloc] initWithBytes:name length:wcslen(name) * sizeof(wchar_t) encoding:NSUTF32LittleEndianStringEncoding];
        [ctx->cb_ pushDebugGroup:s ?: @"event"];
        return ++depth;
    }
    INT EndEvent() override
    {
        if (depth > 0 && ctx->cb_)
        {
            [ctx->cb_ popDebugGroup];
            return --depth;
        }
        return -1;
    }
    void SetMarker(LPCWSTR) override {}
    MtlBOOL GetStatus() override { return TRUE; }

private:
    Context* ctx;
    INT depth = 0;
};

Context::Context(Device* dev) : dev_(dev)
{
    std::memset(encBuf_, 0, sizeof(encBuf_));
    std::memset(encBufOff_, 0, sizeof(encBufOff_));
    std::memset(encTex_, 0, sizeof(encTex_));
    std::memset(encSmp_, 0, sizeof(encSmp_));
}

Context::~Context()
{
    waitIdle();
    delete annotation_;
}

bool Context::init()
{
    const uint32_t frames = std::max<uint32_t>(dev_->desc.framesInFlight, 1);
    constantRing_.init(dev_->mtl, dev_->desc.constantRingBytes, frames + 1, "Constant ring");
    uploadRing_.init(dev_->mtl, dev_->desc.uploadRingBytes, frames + 1, "Upload ring");
    for (uint32_t i = 0; i < frames + 1; ++i)
    {
        id<MTLBuffer> b = [dev_->mtl newBufferWithLength:kVisSlots * 8 options:MTLResourceStorageModeShared];
        b.label = [NSString stringWithFormat:@"Visibility %u", i];
        visBufs_.push_back(b);
    }
    MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionLess;
    dd.depthWriteEnabled = YES;
    defaultDss_ = [dev_->mtl newDepthStencilStateWithDescriptor:dd];
    serial_ = 1;
    return true;
}

HRESULT Context::QueryInterface(MtlREFIID riid, void** pp)
{
    if (!pp)
        return E_POINTER;
    if (riid == mtl_uuidof<IUnknown>() || riid == mtl_uuidof<ID3D11DeviceChild>() ||
        riid == mtl_uuidof<ID3D11DeviceContext>() || riid == mtl_uuidof<ID3D11DeviceContext1>())
    {
        *pp = static_cast<ID3D11DeviceContext1*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == mtl_uuidof<ID3DUserDefinedAnnotation>())
    {
        if (!annotation_)
            annotation_ = new Annotation(this);
        *pp = static_cast<ID3DUserDefinedAnnotation*>(annotation_);
        AddRef();
        return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
}

ULONG Context::AddRef() { return dev_->AddRef(); }
ULONG Context::Release() { return dev_->Release(); }
void Context::GetDevice(ID3D11Device** pp)
{
    dev_->AddRef();
    *pp = dev_;
}

// ---------------------------------------------------------------------------------------------------------
// Command buffer and encoder management
// ---------------------------------------------------------------------------------------------------------
void Context::ensureCommandBuffer()
{
    if (cb_)
        return;
    cb_ = [dev_->queue commandBuffer];
    cb_.label = [NSString stringWithFormat:@"D3D11 submission %llu", (unsigned long long)serial_];
}

id<MTLCommandBuffer> Context::commandBuffer()
{
    ensureCommandBuffer();
    return cb_;
}

void Context::endEncoder()
{
    switch (enc_)
    {
    case Enc::Render:
        [re_ endEncoding];
        re_ = nil;
        pass_.active = false;
        break;
    case Enc::Compute:
        [ce_ endEncoding];
        ce_ = nil;
        break;
    case Enc::Blit:
        [be_ endEncoding];
        be_ = nil;
        break;
    case Enc::None: break;
    }
    enc_ = Enc::None;
}

void Context::commit(bool wait)
{
    endEncoder();
    if (!cb_)
        return;
    Device* d = dev_;
    const uint64_t s = serial_;
    Context* self = this;
    [cb_ addCompletedHandler:^(id<MTLCommandBuffer> b) {
        if (b.error)
            Log("command buffer %llu failed: %s", (unsigned long long)s, b.error.localizedDescription.UTF8String);
        if (b.GPUEndTime > b.GPUStartTime && b.GPUStartTime > 0)
            d->gpuNanos += uint64_t((b.GPUEndTime - b.GPUStartTime) * 1e9);
        if (b.GPUEndTime > b.GPUStartTime)
            g_gpuTimeNs.fetch_add(uint64_t((b.GPUEndTime - b.GPUStartTime) * 1e9), std::memory_order_relaxed);
        self->harvestQueries(s);
        d->noteCompleted(s);
    }];
    id<MTLCommandBuffer> cb = cb_;
    cb_ = nil;
    committed_ = s;
    serial_ = s + 1;
    ++stats.commandBuffers;
    [cb commit];
    if (wait)
        [cb waitUntilCompleted];
}

void Context::waitIdle()
{
    commit(false);
    if (committed_)
        dev_->waitForSerial(committed_);
}

void Context::Flush() { commit(false); }

id<MTLBlitCommandEncoder> Context::blit(bool)
{
    if (enc_ == Enc::Blit)
        return be_;
    endEncoder();
    ensureCommandBuffer();
    be_ = [cb_ blitCommandEncoder];
    enc_ = Enc::Blit;
    ++stats.blitPasses;
    return be_;
}

id<MTLBlitCommandEncoder> Context::blitEncoder() { return blit(true); }

id<MTLCommandBuffer> Context::externalCommandBuffer(ResourceCore* reads, ResourceCore* writes)
{
    if (reads)
        flushClears(reads);
    if (writes)
        flushClears(writes);
    if (pass_.active)
        ++stats.passBreaksByCopy;
    endEncoder();
    ensureCommandBuffer();
    if (reads)
        reads->lastUseSerial = serial_;
    if (writes)
        writes->lastUseSerial = writes->lastWriteSerial = serial_;
    return cb_;
}

id<MTLComputeCommandEncoder> Context::computeEncoder()
{
    if (enc_ == Enc::Compute)
        return ce_;
    endEncoder();
    ensureCommandBuffer();
    ce_ = [cb_ computeCommandEncoder];
    enc_ = Enc::Compute;
    ++stats.computePasses;
    resetEncoderCaches();
    return ce_;
}

void Context::resetEncoderCaches()
{
    std::memset(encBuf_, 0, sizeof(encBuf_));
    std::memset(encBufOff_, 0, sizeof(encBufOff_));
    std::memset(encTex_, 0, sizeof(encTex_));
    std::memset(encSmp_, 0, sizeof(encSmp_));
    encPso_ = nil;
    encDss_ = nil;
    dirty_ = D_ALL;
}

void Context::endFrame()
{
    // `committed_` is the serial of the command buffer that carried this frame's tail.
    constantRing_.tagCurrent(committed_);
    uploadRing_.tagCurrent(committed_);
    const uint64_t needC = constantRing_.advance();
    const uint64_t needU = uploadRing_.advance();
    if (needC)
        dev_->waitForSerial(needC);
    if (needU)
        dev_->waitForSerial(needU);
    visSlot_ = 0;
    stats.ringBytesUsed = 0;

    frameSerials_.push_back(committed_);
    while (frameSerials_.size() > std::max<uint32_t>(dev_->desc.framesInFlight, 1))
    {
        dev_->waitForSerial(frameSerials_.front());
        frameSerials_.pop_front();
    }
}

void Context::present(id<CAMetalDrawable> drawable, void (^preCommit)(id<MTLCommandBuffer>))
{
    endEncoder();
    // A clear that was never followed by a draw still has to reach the screen.
    for (size_t i = 0; i < clears_.size();)
    {
        if (clears_[i].core && clears_[i].core->kind != ResKind::Buffer &&
            static_cast<TextureCore*>(clears_[i].core)->isBackBuffer)
            materializeClear(i);
        else
            ++i;
    }
    ensureCommandBuffer();
    if (preCommit)
        preCommit(cb_);
    if (drawable)
    {
        [cb_ presentDrawable:drawable];
        g_presentCount.fetch_add(1, std::memory_order_relaxed);
    }
    commit(false);
    ++dev_->frameCount;
    endFrame();
}

// ---------------------------------------------------------------------------------------------------------
// Render pass management
// ---------------------------------------------------------------------------------------------------------
PassAttachment Context::attachmentOf(RenderTargetView* v) const
{
    PassAttachment a;
    a.core = v->tex;
    a.level = v->level;
    a.slice = v->slice;
    a.plane = v->plane;
    a.format = uint32_t(v->format);
    return a;
}

bool Context::wantedPass(PassState* out)
{
    *out = PassState();
    bool any = false;
    for (int i = 0; i < 8; ++i)
    {
        RenderTargetView* v = rtv_[i].get();
        if (!v)
            continue;
        out->color[i] = attachmentOf(v);
        out->colorFormat[i] = v->format;
        out->samples = v->tex->samples;
        out->width = std::max<uint32_t>(v->tex->width >> v->level, 1);
        out->height = std::max<uint32_t>(v->tex->height >> v->level, 1);
        any = true;
    }
    if (DepthStencilView* d = dsv_.get())
    {
        out->depth.core = d->tex;
        out->depth.level = d->level;
        out->depth.slice = d->slice;
        out->depth.format = uint32_t(d->format);
        out->depthFormat = d->format;
        out->stencilFormat = d->hasStencil ? d->format : MTLPixelFormatInvalid;
        out->samples = d->tex->samples;
        out->width = std::max<uint32_t>(d->tex->width >> d->level, 1);
        out->height = std::max<uint32_t>(d->tex->height >> d->level, 1);
        any = true;
    }
    return any;
}

bool Context::ensureRenderPass()
{
    PassState want;
    if (!wantedPass(&want))
        return false;
    if (pass_.active)
    {
        bool same = pass_.depth == want.depth;
        for (int i = 0; same && i < 8; ++i)
            same = pass_.color[i] == want.color[i];
        if (same)
            return true;
        endEncoder();
        ++stats.passBreaksByTargetChange;
    }
    beginRenderPass(want);
    return true;
}

PendingClear* Context::findClear(ResourceCore* core, uint32_t level, uint32_t slice, uint32_t plane)
{
    if (!core->pendingClears)
        return nullptr;
    for (PendingClear& c : clears_)
        if (c.core == core && c.level == level && c.slice == slice && c.plane == plane)
            return &c;
    return nullptr;
}

void Context::beginRenderPass(const PassState& want)
{
    ensureCommandBuffer();
    endEncoder();
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    std::vector<size_t> consumed;
    for (int i = 0; i < 8; ++i)
    {
        RenderTargetView* v = rtv_[i].get();
        if (!v)
            continue;
        MTLRenderPassColorAttachmentDescriptor* a = rp.colorAttachments[i];
        a.texture = v->texture(this);
        a.level = v->level;
        a.slice = v->slice;
        a.depthPlane = v->plane;
        a.storeAction = MTLStoreActionStore;
        a.loadAction = MTLLoadActionLoad;
        if (PendingClear* pc = findClear(v->tex, v->level, v->slice, v->plane))
        {
            a.loadAction = MTLLoadActionClear;
            a.clearColor = MTLClearColorMake(pc->color[0], pc->color[1], pc->color[2], pc->color[3]);
            consumed.push_back(size_t(pc - clears_.data()));
        }
    }
    if (DepthStencilView* d = dsv_.get())
    {
        id<MTLTexture> t = d->texture(this);
        MTLRenderPassDepthAttachmentDescriptor* da = rp.depthAttachment;
        da.texture = t;
        da.level = d->level;
        da.slice = d->slice;
        da.storeAction = MTLStoreActionStore;
        da.loadAction = MTLLoadActionLoad;
        MTLRenderPassStencilAttachmentDescriptor* sa = nil;
        if (d->hasStencil)
        {
            sa = rp.stencilAttachment;
            sa.texture = t;
            sa.level = d->level;
            sa.slice = d->slice;
            sa.storeAction = MTLStoreActionStore;
            sa.loadAction = MTLLoadActionLoad;
        }
        if (PendingClear* pc = findClear(d->tex, d->level, d->slice, 0))
        {
            if (pc->clearDepth)
            {
                da.loadAction = MTLLoadActionClear;
                da.clearDepth = pc->depth;
            }
            if (pc->clearStencil && sa)
            {
                sa.loadAction = MTLLoadActionClear;
                sa.clearStencil = pc->stencil;
            }
            consumed.push_back(size_t(pc - clears_.data()));
        }
    }
    rp.visibilityResultBuffer = visBufs_[constantRing_.current()];

    std::sort(consumed.begin(), consumed.end(), std::greater<size_t>());
    for (size_t idx : consumed)
    {
        clears_[idx].core->pendingClears--;
        clears_.erase(clears_.begin() + idx);
        ++stats.foldedClears;
    }

    re_ = [cb_ renderCommandEncoderWithDescriptor:rp];
    enc_ = Enc::Render;
    pass_ = want;
    pass_.active = true;
    ++passEpoch_;
    ++stats.renderPasses;
    resetEncoderCaches();
    queryEpochInEncoder_ = ~0u;
    // The zero vertex buffer serves attributes the bound layout does not provide.
    [re_ setVertexBuffer:dev_->zeroVertexBuffer offset:0 atIndex:kZeroVertexIndex];
}

void Context::materializeClear(size_t index)
{
    PendingClear pc = std::move(clears_[index]);
    clears_.erase(clears_.begin() + index);
    pc.core->pendingClears--;
    endEncoder();
    ensureCommandBuffer();
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (pc.rtv)
    {
        MTLRenderPassColorAttachmentDescriptor* a = rp.colorAttachments[0];
        a.texture = pc.rtv->texture(this);
        a.level = pc.level;
        a.slice = pc.slice;
        a.depthPlane = pc.plane;
        a.loadAction = MTLLoadActionClear;
        a.storeAction = MTLStoreActionStore;
        a.clearColor = MTLClearColorMake(pc.color[0], pc.color[1], pc.color[2], pc.color[3]);
    }
    else if (pc.dsv)
    {
        id<MTLTexture> t = pc.dsv->texture(this);
        MTLRenderPassDepthAttachmentDescriptor* da = rp.depthAttachment;
        da.texture = t;
        da.level = pc.level;
        da.slice = pc.slice;
        da.loadAction = pc.clearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
        da.storeAction = MTLStoreActionStore;
        da.clearDepth = pc.depth;
        if (pc.dsv->hasStencil)
        {
            MTLRenderPassStencilAttachmentDescriptor* sa = rp.stencilAttachment;
            sa.texture = t;
            sa.level = pc.level;
            sa.slice = pc.slice;
            sa.loadAction = pc.clearStencil ? MTLLoadActionClear : MTLLoadActionLoad;
            sa.storeAction = MTLStoreActionStore;
            sa.clearStencil = pc.stencil;
        }
    }
    id<MTLRenderCommandEncoder> e = [cb_ renderCommandEncoderWithDescriptor:rp];
    [e endEncoding];
    ++stats.renderPasses;
    ++stats.materializedClears;
}

void Context::flushClears(ResourceCore* core)
{
    if (!core || !core->pendingClears)
        return;
    for (size_t i = 0; i < clears_.size() && core->pendingClears;)
    {
        if (clears_[i].core == core)
            materializeClear(i);
        else
            ++i;
    }
}

void Context::flushClearsForShader(const ShaderReflection& refl, const StageBindings& sb)
{
    for (const ResourceBinding& rb : refl.resources)
    {
        if (rb.type != D3D_SIT_TEXTURE && rb.type != D3D_SIT_UAV_RWTYPED)
            continue;
        if (rb.d3dSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
            continue;
        ShaderResourceView* v = sb.srv[rb.d3dSlot].get();
        if (v && v->core && v->core->pendingClears)
            flushClears(v->core);
    }
}

void Context::ClearRenderTargetView(ID3D11RenderTargetView* pView, const FLOAT color[4])
{
    if (!pView)
        return;
    RenderTargetView* v = static_cast<RenderTargetView*>(pView);
    // The open pass still holds the old contents of this attachment: store them, clear in a new pass.
    if (pass_.active)
    {
        PassAttachment a = attachmentOf(v);
        for (int i = 0; i < 8; ++i)
            if (pass_.color[i] == a)
            {
                endEncoder();
                ++stats.passBreaksByClear;
                break;
            }
    }
    PendingClear* pc = findClear(v->tex, v->level, v->slice, v->plane);
    if (!pc)
    {
        clears_.emplace_back();
        pc = &clears_.back();
        pc->core = v->tex;
        pc->level = v->level;
        pc->slice = v->slice;
        pc->plane = v->plane;
        v->tex->pendingClears++;
    }
    pc->rtv = v;
    pc->dsv = nullptr;
    std::memcpy(pc->color, color, sizeof(pc->color));
}

void Context::ClearDepthStencilView(ID3D11DepthStencilView* pView, UINT flags, FLOAT depth, UINT8 stencil)
{
    if (!pView)
        return;
    DepthStencilView* v = static_cast<DepthStencilView*>(pView);
    if (pass_.active && pass_.depth.core == v->tex && pass_.depth.level == v->level && pass_.depth.slice == v->slice)
    {
        endEncoder();
        ++stats.passBreaksByClear;
    }
    PendingClear* pc = findClear(v->tex, v->level, v->slice, 0);
    if (!pc)
    {
        clears_.emplace_back();
        pc = &clears_.back();
        pc->core = v->tex;
        pc->level = v->level;
        pc->slice = v->slice;
        v->tex->pendingClears++;
    }
    pc->dsv = v;
    pc->rtv = nullptr;
    if (flags & D3D11_CLEAR_DEPTH)
    {
        pc->clearDepth = true;
        pc->depth = depth;
    }
    if (flags & D3D11_CLEAR_STENCIL)
    {
        pc->clearStencil = true;
        pc->stencil = stencil;
    }
}

void Context::ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* pView, const UINT values[4])
{
    UnorderedAccessView* v = static_cast<UnorderedAccessView*>(pView);
    if (!v)
        return;
    if (v->core->kind == ResKind::Buffer)
    {
        flushClears(v->core);
        id<MTLBlitCommandEncoder> b = blit(true);
        [b fillBuffer:static_cast<BufferCore*>(v->core)->cur.buf range:NSMakeRange(0, static_cast<BufferCore*>(v->core)->byteWidth) value:uint8_t(values[0])];
        return;
    }
    Log("ClearUnorderedAccessViewUint on textures is not implemented");
}

void Context::ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* pView, const FLOAT values[4])
{
    (void)pView;
    (void)values;
    Log("ClearUnorderedAccessViewFloat is not implemented");
}

// ---------------------------------------------------------------------------------------------------------
// State setters
// ---------------------------------------------------------------------------------------------------------
void Context::IASetInputLayout(ID3D11InputLayout* l)
{
    layout_ = static_cast<InputLayout*>(l);
    dirty_ |= D_VERTEXBUFFERS;
}

void Context::IASetVertexBuffers(UINT start, UINT n, ID3D11Buffer* const* bufs, const UINT* strides, const UINT* offsets)
{
    for (UINT i = 0; i < n && start + i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++i)
    {
        vb_[start + i].buf = bufs ? static_cast<Buffer*>(bufs[i]) : nullptr;
        vb_[start + i].stride = strides ? strides[i] : 0;
        vb_[start + i].offset = offsets ? offsets[i] : 0;
    }
    dirty_ |= D_VERTEXBUFFERS;
}

void Context::IASetIndexBuffer(ID3D11Buffer* b, DXGI_FORMAT f, UINT offset)
{
    ib_ = static_cast<Buffer*>(b);
    ibFormat_ = f;
    ibOffset_ = offset;
}

void Context::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY t) { topology_ = t; }

void Context::VSSetShader(ID3D11VertexShader* s, ID3D11ClassInstance* const*, UINT) { vs_ = static_cast<VertexShader*>(s); }
void Context::PSSetShader(ID3D11PixelShader* s, ID3D11ClassInstance* const*, UINT) { ps_ = static_cast<PixelShader*>(s); }
void Context::CSSetShader(ID3D11ComputeShader* s, ID3D11ClassInstance* const*, UINT) { cs_ = static_cast<ComputeShader*>(s); }
void Context::GSSetShader(ID3D11GeometryShader* s, ID3D11ClassInstance* const*, UINT)
{
    if (s)
        Log("GSSetShader: geometry shaders are not supported");
}
void Context::HSSetShader(ID3D11HullShader* s, ID3D11ClassInstance* const*, UINT)
{
    if (s)
        Log("HSSetShader: tessellation is not supported");
}
void Context::DSSetShader(ID3D11DomainShader* s, ID3D11ClassInstance* const*, UINT)
{
    if (s)
        Log("DSSetShader: tessellation is not supported");
}

void Context::setCBs(StageBindings& sb, UINT start, UINT n, ID3D11Buffer* const* bufs)
{
    for (UINT i = 0; i < n && start + i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i)
        sb.cb[start + i] = bufs ? static_cast<Buffer*>(bufs[i]) : nullptr;
}

void Context::setSamplers(StageBindings& sb, UINT start, UINT n, ID3D11SamplerState* const* s)
{
    for (UINT i = 0; i < n && start + i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i)
        sb.smp[start + i] = s ? static_cast<SamplerState*>(s[i]) : nullptr;
}

bool Context::sameAsAttachment(ResourceCore* core) const
{
    if (!core)
        return false;
    for (int i = 0; i < 8; ++i)
        if (rtv_[i] && rtv_[i]->tex == core)
            return true;
    return dsv_ && dsv_->tex == core;
}

void Context::setSRVs(StageBindings& sb, UINT start, UINT n, ID3D11ShaderResourceView* const* views)
{
    for (UINT i = 0; i < n && start + i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i)
    {
        ShaderResourceView* v = views ? static_cast<ShaderResourceView*>(views[i]) : nullptr;
        sb.srv[start + i] = v;
        if (!v)
            continue;
        // D3D11 unbinds a render target that is bound as a shader resource.
        for (int r = 0; r < 8; ++r)
            if (rtv_[r] && rtv_[r]->tex == v->core)
                rtv_[r] = nullptr;
        if (dsv_ && dsv_->tex == v->core)
            dsv_ = nullptr;
    }
}

void Context::VSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b) { setCBs(bind_[0], s, n, b); }
void Context::PSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b) { setCBs(bind_[1], s, n, b); }
void Context::GSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b) { setCBs(bind_[2], s, n, b); }
void Context::HSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b) { setCBs(bind_[3], s, n, b); }
void Context::DSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b) { setCBs(bind_[4], s, n, b); }
void Context::CSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b) { setCBs(bind_[5], s, n, b); }
void Context::VSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v) { setSRVs(bind_[0], s, n, v); }
void Context::PSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v) { setSRVs(bind_[1], s, n, v); }
void Context::GSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v) { setSRVs(bind_[2], s, n, v); }
void Context::HSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v) { setSRVs(bind_[3], s, n, v); }
void Context::DSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v) { setSRVs(bind_[4], s, n, v); }
void Context::CSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v) { setSRVs(bind_[5], s, n, v); }
void Context::VSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* v) { setSamplers(bind_[0], s, n, v); }
void Context::PSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* v) { setSamplers(bind_[1], s, n, v); }
void Context::GSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* v) { setSamplers(bind_[2], s, n, v); }
void Context::HSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* v) { setSamplers(bind_[3], s, n, v); }
void Context::DSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* v) { setSamplers(bind_[4], s, n, v); }
void Context::CSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* v) { setSamplers(bind_[5], s, n, v); }

void Context::CSSetUnorderedAccessViews(UINT start, UINT n, ID3D11UnorderedAccessView* const* views, const UINT*)
{
    for (UINT i = 0; i < n && start + i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i)
        uav_[start + i] = views ? static_cast<UnorderedAccessView*>(views[i]) : nullptr;
}

void Context::RSSetState(ID3D11RasterizerState* s)
{
    rs_ = static_cast<RasterizerState*>(s);
    dirty_ |= D_RASTER | D_SCISSOR;
}

void Context::RSSetViewports(UINT n, const D3D11_VIEWPORT* vp)
{
    if (n >= 1 && vp)
    {
        viewport_ = vp[0];
        viewportSet_ = true;
    }
    else
        viewportSet_ = false;
    dirty_ |= D_VIEWPORT;
}

void Context::RSSetScissorRects(UINT n, const D3D11_RECT* r)
{
    if (n >= 1 && r)
    {
        scissor_ = r[0];
        scissorSet_ = true;
    }
    else
        scissorSet_ = false;
    dirty_ |= D_SCISSOR;
}

void Context::OMSetRenderTargets(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    for (UINT i = 0; i < 8; ++i)
        rtv_[i] = (i < n && rtvs) ? static_cast<RenderTargetView*>(rtvs[i]) : nullptr;
    dsv_ = static_cast<DepthStencilView*>(dsv);
    // D3D11 unbinds shader resources that alias the new targets.
    if (n || dsv)
    {
        for (int st = 0; st < kShaderStageCount; ++st)
            for (int s = 0; s < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++s)
            {
                ShaderResourceView* v = bind_[st].srv[s].get();
                if (v && sameAsAttachment(v->core))
                    bind_[st].srv[s] = nullptr;
            }
    }
}

void Context::OMGetRenderTargets(UINT n, ID3D11RenderTargetView** rtvs, ID3D11DepthStencilView** dsv)
{
    if (rtvs)
        for (UINT i = 0; i < n && i < 8; ++i)
        {
            rtvs[i] = rtv_[i].get();
            if (rtvs[i])
                rtvs[i]->AddRef();
        }
    if (dsv)
    {
        *dsv = dsv_.get();
        if (*dsv)
            (*dsv)->AddRef();
    }
}

void Context::OMSetBlendState(ID3D11BlendState* s, const FLOAT f[4], UINT mask)
{
    blend_ = static_cast<BlendState*>(s);
    if (f)
        std::memcpy(blendFactor_, f, sizeof(blendFactor_));
    else
        blendFactor_[0] = blendFactor_[1] = blendFactor_[2] = blendFactor_[3] = 1.0f;
    sampleMask_ = mask;
    dirty_ |= D_BLENDCOLOR;
}

void Context::OMSetDepthStencilState(ID3D11DepthStencilState* s, UINT ref)
{
    dss_ = static_cast<DepthStencilState*>(s);
    stencilRef_ = ref;
    dirty_ |= D_DEPTHSTENCIL;
}

void Context::ClearState()
{
    endEncoder();
    layout_ = nullptr;
    for (auto& v : vb_)
        v = VBSlot();
    ib_ = nullptr;
    vs_ = nullptr;
    ps_ = nullptr;
    cs_ = nullptr;
    for (auto& b : bind_)
        b = StageBindings();
    for (auto& u : uav_)
        u = nullptr;
    rs_ = nullptr;
    blend_ = nullptr;
    dss_ = nullptr;
    for (auto& r : rtv_)
        r = nullptr;
    dsv_ = nullptr;
    viewportSet_ = false;
    scissorSet_ = false;
    dirty_ = D_ALL;
}

// ---------------------------------------------------------------------------------------------------------
// Pipeline state
// ---------------------------------------------------------------------------------------------------------
// D3D gives a pixel shader zero for a TEXCOORD the vertex shader never wrote; Metal fails the pipeline instead. The
// stock set has 9 such vertex/pixel pairs (water_*, model_distort*, the scope lens, ...). The pixel function is rebuilt
// from its MSL with those inputs declared as zero locals.
id<MTLFunction> Context::fragmentFunctionFor(VertexShader* vs, PixelShader* ps)
{
    if (ps->interpolants.empty() || ps->msl.empty())
        return ps->fn;
    const uint64_t key = (vs->id << 32) ^ ps->id;
    auto it = fragmentVariants_.find(key);
    if (it != fragmentVariants_.end())
        return it->second;

    std::vector<Interpolant> missing;
    std::vector<InterpolantPair> mismatched;
    for (const Interpolant& in : ps->interpolants)
    {
        const Interpolant* written = nullptr;
        for (const Interpolant& out : vs->interpolants)
            if (out.location == in.location)
            {
                written = &out;
                break;
            }
        if (!written)
            missing.push_back(in);
        else if (written->type != in.type)
            mismatched.push_back({in, *written});
    }
    id<MTLFunction> fn = ps->fn;
    if (!missing.empty() || !mismatched.empty())
    {
        NSError* err = nil;
        std::string text = AdaptFragmentInputs(ps->msl, mismatched);
        text = PatchFragmentInputs(text, missing);
        NSString* src = [NSString stringWithUTF8String:text.c_str()];
        id<MTLLibrary> lib = dev_->newLibraryFromMsl(src, &err);
        id<MTLFunction> patched = lib ? [lib newFunctionWithName:[NSString stringWithUTF8String:ps->refl.entryPoint.c_str()]] : nil;
        if (patched)
        {
            fn = patched;
            Log("pixel shader %s: %zu interpolant(s) that vertex shader %s does not write are zero-filled, %zu of another width adapted",
                ps->debugName.c_str(), missing.size(), vs->debugName.c_str(), mismatched.size());
        }
        else
            Log("pixel shader %s: patching %zu unwritten and %zu mismatched input(s) failed: %s", ps->debugName.c_str(), missing.size(),
                mismatched.size(), err ? err.localizedDescription.UTF8String : "?");
    }
    fragmentVariants_.emplace(key, fn);
    return fn;
}

id<MTLRenderPipelineState> Context::pipeline(MTLPrimitiveTopologyClass cls)
{
    PipelineKey k;
    std::memset(&k, 0, sizeof(k));
    k.vs = vs_->id;
    k.ps = ps_ ? ps_->id : 0;
    k.layout = layout_ ? layout_->hash : 0;
    uint64_t sh = 1469598103934665603ull;
    if (layout_)
    {
        uint32_t seen = 0;
        for (const InputElement& e : layout_->elements)
            if (!(seen & (1u << e.slot)))
            {
                seen |= 1u << e.slot;
                sh = HashBytes(&vb_[e.slot].stride, 4, sh);
            }
    }
    k.strides = sh;
    static const uint64_t kDefaultBlend = [] {
        D3D11_BLEND_DESC d{};
        for (auto& rt : d.RenderTarget)
        {
            rt.SrcBlend = rt.DestBlend = D3D11_BLEND_ONE;
            rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ONE;
            rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
        return HashBytes(&d, sizeof(d));
    }();
    k.blend = blend_ ? blend_->hash : kDefaultBlend;
    for (int i = 0; i < 8; ++i)
        k.colorFormat[i] = uint32_t(pass_.colorFormat[i]);
    k.depthFormat = uint32_t(pass_.depthFormat);
    k.stencilFormat = uint32_t(pass_.stencilFormat);
    k.samples = uint8_t(pass_.samples);
    k.topologyClass = uint8_t(cls);

    auto it = psoCache_.find(k);
    if (it != psoCache_.end())
    {
        ++stats.pipelineHits;
        return it->second;
    }

    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.label = [NSString stringWithFormat:@"vs%llu %s, ps%llu %s", (unsigned long long)k.vs, vs_->debugName.c_str(),
                                         (unsigned long long)k.ps, ps_ ? ps_->debugName.c_str() : "-"];
    d.vertexFunction = vs_->fn;
    d.fragmentFunction = ps_ ? fragmentFunctionFor(vs_.get(), ps_.get()) : nil;
    d.rasterSampleCount = pass_.samples;
    d.inputPrimitiveTopology = cls;

    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    uint32_t usedSlots = 0;
    for (const VertexInput& in : vs_->refl.inputs)
    {
        const InputElement* el = nullptr;
        if (layout_)
            for (const InputElement& e : layout_->elements)
                if (e.semanticIndex == in.semanticIndex && e.semantic == in.semantic)
                {
                    el = &e;
                    break;
                }
        MTLVertexAttributeDescriptor* a = vd.attributes[in.attribute];
        if (el)
        {
            a.format = VertexFormatFor(el->format);
            a.offset = el->offset;
            a.bufferIndex = kVertexBufferBase + el->slot;
            usedSlots |= 1u << el->slot;
            MTLVertexBufferLayoutDescriptor* l = vd.layouts[kVertexBufferBase + el->slot];
            UINT stride = vb_[el->slot].stride;
            if (stride == 0)
            {
                for (const InputElement& e2 : layout_->elements)
                    if (e2.slot == el->slot)
                        stride = std::max<UINT>(stride, e2.offset + GetFormatInfo(e2.format).bytesPerBlock);
            }
            l.stride = (stride + 3) & ~3u;
            l.stepFunction = el->cls == D3D11_INPUT_PER_INSTANCE_DATA ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            l.stepRate = el->cls == D3D11_INPUT_PER_INSTANCE_DATA ? std::max<uint32_t>(el->stepRate, 1) : 1;
        }
        else
        {
            // Not provided by the layout: read zeros (D3D would have refused the layout).
            a.format = in.componentType == D3D_REGISTER_COMPONENT_UINT32 ? MTLVertexFormatUInt4
                       : in.componentType == D3D_REGISTER_COMPONENT_SINT32 ? MTLVertexFormatInt4
                                                                            : MTLVertexFormatFloat4;
            a.offset = 0;
            a.bufferIndex = kZeroVertexIndex;
            MTLVertexBufferLayoutDescriptor* l = vd.layouts[kZeroVertexIndex];
            l.stride = 16;
            l.stepFunction = MTLVertexStepFunctionConstant;
            l.stepRate = 0;
        }
    }
    (void)usedSlots;
    d.vertexDescriptor = vd;

    const BlendState* bs = blend_.get();
    for (int i = 0; i < 8; ++i)
    {
        if (pass_.colorFormat[i] == MTLPixelFormatInvalid)
            continue;
        MTLRenderPipelineColorAttachmentDescriptor* a = d.colorAttachments[i];
        a.pixelFormat = pass_.colorFormat[i];
        D3D11_RENDER_TARGET_BLEND_DESC rt{};
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (bs)
            rt = bs->desc.IndependentBlendEnable ? bs->desc.RenderTarget[i] : bs->desc.RenderTarget[0];
        a.writeMask = mapWriteMask(rt.RenderTargetWriteMask);
        a.blendingEnabled = rt.BlendEnable != 0;
        if (rt.BlendEnable)
        {
            a.sourceRGBBlendFactor = mapBlend(rt.SrcBlend);
            a.destinationRGBBlendFactor = mapBlend(rt.DestBlend);
            a.rgbBlendOperation = mapBlendOp(rt.BlendOp);
            a.sourceAlphaBlendFactor = mapBlend(rt.SrcBlendAlpha);
            a.destinationAlphaBlendFactor = mapBlend(rt.DestBlendAlpha);
            a.alphaBlendOperation = mapBlendOp(rt.BlendOpAlpha);
        }
    }
    d.depthAttachmentPixelFormat = pass_.depthFormat;
    d.stencilAttachmentPixelFormat = pass_.stencilFormat;
    d.alphaToCoverageEnabled = bs && bs->desc.AlphaToCoverageEnable != 0;

    NSError* err = nil;
    id<MTLRenderPipelineState> pso = [dev_->mtl newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso)
        Log("pipeline creation failed (%s): %s", d.label.UTF8String, err ? err.localizedDescription.UTF8String : "?");
    ++stats.pipelineCreates;
    psoCache_.emplace(k, pso);
    return pso;
}

// ---------------------------------------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------------------------------------
void Context::applyFixedState()
{
    const uint32_t d = dirty_;
    if (d & D_DEPTHSTENCIL)
    {
        id<MTLDepthStencilState> st;
        if (pass_.depthFormat == MTLPixelFormatInvalid)
            st = dev_->noDepthState;
        else if (dss_)
            st = pass_.stencilFormat != MTLPixelFormatInvalid ? dss_->mtl : dss_->mtlDepthOnly;
        else
            st = defaultDss_;
        if (st != encDss_)
        {
            [re_ setDepthStencilState:st];
            encDss_ = st;
        }
        [re_ setStencilReferenceValue:stencilRef_];
    }
    if (d & D_RASTER)
    {
        D3D11_RASTERIZER_DESC r{};
        if (rs_)
            r = rs_->desc;
        else
        {
            r.FillMode = D3D11_FILL_SOLID;
            r.CullMode = D3D11_CULL_BACK;
            r.DepthClipEnable = TRUE;
        }
        [re_ setCullMode:r.CullMode == D3D11_CULL_NONE ? MTLCullModeNone : r.CullMode == D3D11_CULL_FRONT ? MTLCullModeFront : MTLCullModeBack];
        [re_ setFrontFacingWinding:r.FrontCounterClockwise ? MTLWindingCounterClockwise : MTLWindingClockwise];
        [re_ setTriangleFillMode:r.FillMode == D3D11_FILL_WIREFRAME ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        [re_ setDepthBias:float(r.DepthBias) slopeScale:r.SlopeScaledDepthBias clamp:r.DepthBiasClamp];
        [re_ setDepthClipMode:r.DepthClipEnable ? MTLDepthClipModeClip : MTLDepthClipModeClamp];
    }
    if (d & D_VIEWPORT)
    {
        MTLViewport vp;
        if (viewportSet_)
            vp = {viewport_.TopLeftX, viewport_.TopLeftY, viewport_.Width, viewport_.Height, viewport_.MinDepth, viewport_.MaxDepth};
        else
            vp = {0, 0, double(pass_.width), double(pass_.height), 0, 1};
        [re_ setViewport:vp];
    }
    if (d & (D_SCISSOR | D_VIEWPORT))
    {
        MTLScissorRect sr = {0, 0, pass_.width, pass_.height};
        if (rs_ && rs_->desc.ScissorEnable && scissorSet_)
        {
            int x0 = std::max<int>(scissor_.left, 0), y0 = std::max<int>(scissor_.top, 0);
            int x1 = std::min<int>(scissor_.right, int(pass_.width)), y1 = std::min<int>(scissor_.bottom, int(pass_.height));
            sr = {NSUInteger(x0), NSUInteger(y0), NSUInteger(std::max(x1 - x0, 0)), NSUInteger(std::max(y1 - y0, 0))};
        }
        [re_ setScissorRect:sr];
    }
    if (d & D_BLENDCOLOR)
        [re_ setBlendColorRed:blendFactor_[0] green:blendFactor_[1] blue:blendFactor_[2] alpha:blendFactor_[3]];
    dirty_ = 0;
}

void Context::bindVertexBuffers()
{
    if (!layout_)
        return;
    uint32_t seen = 0;
    for (const InputElement& e : layout_->elements)
    {
        if (seen & (1u << e.slot))
            continue;
        seen |= 1u << e.slot;
        Buffer* b = vb_[e.slot].buf.get();
        id<MTLBuffer> mb = b ? b->cur.buf : dev_->zeroVertexBuffer;
        uint32_t off = b ? uint32_t(b->cur.offset + vb_[e.slot].offset) : 0;
        if (b)
            b->cur.lastUse = serial_;
        const uint32_t idx = kVertexBufferBase + e.slot;
        if (encBuf_[0][idx] != mb || encBufOff_[0][idx] != off)
        {
            [re_ setVertexBuffer:mb offset:off atIndex:idx];
            encBuf_[0][idx] = mb;
            encBufOff_[0][idx] = off;
        }
    }
}

id<MTLTexture> Context::placeholderFor(const ResourceBinding& rb)
{
    if (rb.depthTexture)
        return dev_->dummyDepth2D;
    switch (rb.dimension)
    {
    case D3D_SRV_DIMENSION_TEXTURE3D: return dev_->dummyTexture3D;
    case D3D_SRV_DIMENSION_TEXTURECUBE: return dev_->dummyTextureCube;
    case D3D_SRV_DIMENSION_TEXTURE2DMS: return dev_->dummyTexture2DMS ? dev_->dummyTexture2DMS : dev_->dummyTexture2D;
    default: return dev_->dummyTexture2D;
    }
}

void Context::bindRenderStage(int stage, const ShaderReflection& refl, const StageBindings& sb)
{
    const bool vertex = stage == 0;
    for (const CBuffer& cb : refl.cbuffers)
    {
        if (cb.metalIndex >= 31)
            continue;
        Buffer* b = cb.d3dSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ? sb.cb[cb.d3dSlot].get() : nullptr;
        id<MTLBuffer> mb = b ? b->cur.buf : dev_->dummyBuffer;
        uint32_t off = b ? uint32_t(b->cur.offset) : 0;
        if (b)
            b->cur.lastUse = serial_;
        if (encBuf_[stage][cb.metalIndex] != mb)
        {
            if (vertex) [re_ setVertexBuffer:mb offset:off atIndex:cb.metalIndex];
            else [re_ setFragmentBuffer:mb offset:off atIndex:cb.metalIndex];
            encBuf_[stage][cb.metalIndex] = mb;
            encBufOff_[stage][cb.metalIndex] = off;
        }
        else if (encBufOff_[stage][cb.metalIndex] != off)
        {
            if (vertex) [re_ setVertexBufferOffset:off atIndex:cb.metalIndex];
            else [re_ setFragmentBufferOffset:off atIndex:cb.metalIndex];
            encBufOff_[stage][cb.metalIndex] = off;
        }
    }
    for (const ResourceBinding& rb : refl.resources)
    {
        switch (rb.type)
        {
        case D3D_SIT_TEXTURE:
        {
            if (rb.metalIndex >= 128)
                break;
            ShaderResourceView* v = rb.d3dSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ? sb.srv[rb.d3dSlot].get() : nullptr;
            id<MTLTexture> t = nil;
            if (v && !v->buffer && v->core && !sameAsAttachment(v->core))
            {
                t = v->texture(this);
                TextureCore* tc = static_cast<TextureCore*>(v->core);
                if (tc->usage == D3D11_USAGE_DYNAMIC && tc->renameIndex < tc->renameLastUse.size())
                    tc->renameLastUse[tc->renameIndex] = serial_;
            }
            if (!t)
                t = placeholderFor(rb);
            if (encTex_[stage][rb.metalIndex] != t)
            {
                if (vertex) [re_ setVertexTexture:t atIndex:rb.metalIndex];
                else [re_ setFragmentTexture:t atIndex:rb.metalIndex];
                encTex_[stage][rb.metalIndex] = t;
            }
            break;
        }
        case D3D_SIT_SAMPLER:
        {
            if (rb.metalIndex >= 16)
                break;
            SamplerState* s = rb.d3dSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT ? sb.smp[rb.d3dSlot].get() : nullptr;
            id<MTLSamplerState> ms = s ? s->mtl : dev_->dummySampler;
            if (encSmp_[stage][rb.metalIndex] != ms)
            {
                if (vertex) [re_ setVertexSamplerState:ms atIndex:rb.metalIndex];
                else [re_ setFragmentSamplerState:ms atIndex:rb.metalIndex];
                encSmp_[stage][rb.metalIndex] = ms;
            }
            break;
        }
        case D3D_SIT_STRUCTURED:
        case D3D_SIT_BYTEADDRESS:
        {
            if (rb.metalIndex >= 31)
                break;
            ShaderResourceView* v = rb.d3dSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ? sb.srv[rb.d3dSlot].get() : nullptr;
            id<MTLBuffer> mb = v && v->buffer ? static_cast<BufferCore*>(v->core)->cur.buf : dev_->dummyBuffer;
            if (encBuf_[stage][rb.metalIndex] != mb)
            {
                if (vertex) [re_ setVertexBuffer:mb offset:0 atIndex:rb.metalIndex];
                else [re_ setFragmentBuffer:mb offset:0 atIndex:rb.metalIndex];
                encBuf_[stage][rb.metalIndex] = mb;
                encBufOff_[stage][rb.metalIndex] = 0;
            }
            break;
        }
        default: break;
        }
    }
}

bool Context::prepareDraw(bool, D3D11_PRIMITIVE_TOPOLOGY* topo)
{
    if (!vs_)
        return false;
    MTLPrimitiveType type;
    MTLPrimitiveTopologyClass cls;
    if (!mapTopology(topology_, &type, &cls))
    {
        Log("draw with unsupported topology %d ignored", int(topology_));
        return false;
    }
    *topo = topology_;
    flushClearsForShader(vs_->refl, bind_[0]);
    if (ps_)
        flushClearsForShader(ps_->refl, bind_[1]);
    if (!ensureRenderPass())
        return false;

    if (activeOcclusion_ && queryEpochInEncoder_ != passEpoch())
    {
        if (visSlot_ < kVisSlots)
        {
            id<MTLBuffer> vb = visBufs_[constantRing_.current()];
            const NSUInteger off = NSUInteger(visSlot_) * 8;
            *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(vb.contents) + off) = 0;
            activeOcclusion_->slotPtrs.push_back(reinterpret_cast<const uint64_t*>(static_cast<uint8_t*>(vb.contents) + off));
            [re_ setVisibilityResultMode:MTLVisibilityResultModeCounting offset:off];
            ++visSlot_;
        }
        queryEpochInEncoder_ = passEpoch();
    }

    id<MTLRenderPipelineState> pso = pipeline(cls);
    if (!pso)
        return false;
    if (pso != encPso_)
    {
        [re_ setRenderPipelineState:pso];
        encPso_ = pso;
    }
    applyFixedState();
    bindVertexBuffers();
    bindRenderStage(0, vs_->refl, bind_[0]);
    if (ps_)
        bindRenderStage(1, ps_->refl, bind_[1]);
    ++pass_.draws;
    if (traceDraws_ > 0)
        traceDraw();
    return true;
}

static const char* nameOf(TextureCore* t)
{
    if (t->isBackBuffer)
        return "backbuffer";
    return t->tex && t->tex.label ? t->tex.label.UTF8String : "?";
}

void Context::traceDraw()
{
    --traceDraws_;
    std::string rts;
    for (int i = 0; i < 8; ++i)
        if (RenderTargetView* v = rtv_[i].get())
        {
            char b[160];
            snprintf(b, sizeof b, " rt%d=%s(%ux%u fmt %u)", i, nameOf(v->tex), v->tex->width, v->tex->height, unsigned(v->format));
            rts += b;
        }
    std::string z = "none";
    if (DepthStencilView* d = dsv_.get())
    {
        char zb[96];
        snprintf(zb, sizeof zb, "%s@%p(%ux%u s%u)", nameOf(d->tex), (__bridge void*)d->tex->tex, d->tex->width, d->tex->height,
                 d->tex->samples);
        z = zb;
    }
    char ds[200] = "default";
    if (dss_)
    {
        const D3D11_DEPTH_STENCIL_DESC& s = dss_->desc;
        snprintf(ds, sizeof ds, "z%d/w%d/f%d st%d(f%d r%u m%02x)", s.DepthEnable, int(s.DepthWriteMask), int(s.DepthFunc),
                 s.StencilEnable, int(s.FrontFace.StencilFunc), stencilRef_, s.StencilReadMask);
    }
    char bl[120] = "default";
    if (blend_)
    {
        const D3D11_RENDER_TARGET_BLEND_DESC& r = blend_->desc.RenderTarget[0];
        snprintf(bl, sizeof bl, "%d src%d dst%d op%d wr%x", r.BlendEnable, int(r.SrcBlend), int(r.DestBlend), int(r.BlendOp),
                 r.RenderTargetWriteMask);
    }
    std::string tex;
    if (ps_)
        for (const ResourceBinding& rb : ps_->refl.resources)
            if (rb.type == D3D_SIT_TEXTURE && rb.d3dSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
            {
                ShaderResourceView* v = bind_[1].srv[rb.d3dSlot].get();
                const char* n = !v ? "-" : (v->buffer ? "buffer" : (v->core ? (sameAsAttachment(v->core) ? "ATTACHED" : nameOf(static_cast<TextureCore*>(v->core))) : "?"));
                tex += std::string(" t") + std::to_string(rb.d3dSlot) + "=" + n;
            }
    char sc[96] = "";
    if (rs_ && rs_->desc.ScissorEnable)
        snprintf(sc, sizeof sc, " scissor(%d,%d,%d,%d)%s", int(scissor_.left), int(scissor_.top), int(scissor_.right),
                 int(scissor_.bottom), scissorSet_ ? "" : "[unset]");
    Log("trace draw: vs=%s ps=%s topo %d |%s| zs=%s %s | blend %s | cull %d | vp %.0fx%.0f z %.2f-%.2f%s |%s", vs_->debugName.c_str(),
        ps_ ? ps_->debugName.c_str() : "-", int(topology_), rts.c_str(), z.c_str(), ds, bl, rs_ ? int(rs_->desc.CullMode) : 3, viewport_.Width,
        viewport_.Height, viewport_.MinDepth, viewport_.MaxDepth, sc, tex.c_str());
}

void Context::Draw(UINT count, UINT start) { DrawInstanced(count, 1, start, 0); }

void Context::DrawInstanced(UINT vc, UINT ic, UINT sv, UINT si)
{
    D3D11_PRIMITIVE_TOPOLOGY t;
    if (!prepareDraw(false, &t))
        return;
    MTLPrimitiveType type;
    MTLPrimitiveTopologyClass cls;
    mapTopology(t, &type, &cls);
    [re_ drawPrimitives:type vertexStart:sv vertexCount:vc instanceCount:ic baseInstance:si];
    ++stats.draws;
}

void Context::DrawIndexed(UINT ic, UINT start, INT base) { DrawIndexedInstanced(ic, 1, start, base, 0); }

void Context::DrawIndexedInstanced(UINT ic, UINT inst, UINT start, INT base, UINT si)
{
    if (!ib_)
        return;
    D3D11_PRIMITIVE_TOPOLOGY t;
    if (!prepareDraw(true, &t))
        return;
    MTLPrimitiveType type;
    MTLPrimitiveTopologyClass cls;
    mapTopology(t, &type, &cls);
    const bool wide = ibFormat_ == DXGI_FORMAT_R32_UINT;
    const NSUInteger isz = wide ? 4 : 2;
    ib_->cur.lastUse = serial_;
    [re_ drawIndexedPrimitives:type
                    indexCount:ic
                     indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                   indexBuffer:ib_->cur.buf
             indexBufferOffset:ib_->cur.offset + ibOffset_ + start * isz
                 instanceCount:inst
                    baseVertex:base
                  baseInstance:si];
    ++stats.draws;
}

void Context::Dispatch(UINT x, UINT y, UINT z)
{
    ComputeShader* cs = cs_.get();
    if (!cs)
        return;
    flushClearsForShader(cs->refl, bind_[5]);
    for (const ResourceBinding& rb : cs->refl.resources)
        if (rb.type == D3D_SIT_UAV_RWTYPED && rb.d3dSlot < D3D11_PS_CS_UAV_REGISTER_COUNT && uav_[rb.d3dSlot])
            flushClears(uav_[rb.d3dSlot]->core);
    id<MTLComputeCommandEncoder> e = computeEncoder();
    if (!cs->computePso)
    {
        NSError* err = nil;
        cs->computePso = [dev_->mtl newComputePipelineStateWithFunction:cs->fn error:&err];
        if (!cs->computePso)
        {
            Log("compute pipeline failed: %s", err.localizedDescription.UTF8String);
            return;
        }
    }
    [e setComputePipelineState:cs->computePso];
    const StageBindings& sb = bind_[5];
    for (const CBuffer& cb : cs->refl.cbuffers)
    {
        Buffer* b = cb.d3dSlot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT ? sb.cb[cb.d3dSlot].get() : nullptr;
        if (b)
            b->cur.lastUse = serial_;
        [e setBuffer:b ? b->cur.buf : dev_->dummyBuffer offset:b ? b->cur.offset : 0 atIndex:cb.metalIndex];
    }
    for (const ResourceBinding& rb : cs->refl.resources)
    {
        switch (rb.type)
        {
        case D3D_SIT_TEXTURE:
        {
            ShaderResourceView* v = sb.srv[rb.d3dSlot].get();
            id<MTLTexture> t = (v && !v->buffer) ? v->texture(this) : placeholderFor(rb);
            [e setTexture:t atIndex:rb.metalIndex];
            break;
        }
        case D3D_SIT_SAMPLER:
        {
            SamplerState* s = sb.smp[rb.d3dSlot].get();
            [e setSamplerState:s ? s->mtl : dev_->dummySampler atIndex:rb.metalIndex];
            break;
        }
        case D3D_SIT_UAV_RWTYPED:
        {
            UnorderedAccessView* u = rb.d3dSlot < D3D11_PS_CS_UAV_REGISTER_COUNT ? uav_[rb.d3dSlot].get() : nullptr;
            if (u && u->core->kind != ResKind::Buffer)
            {
                [e setTexture:u->texture(this) atIndex:rb.metalIndex];
                u->core->lastWriteSerial = serial_;
            }
            break;
        }
        case D3D_SIT_UAV_RWSTRUCTURED:
        case D3D_SIT_UAV_RWBYTEADDRESS:
        {
            UnorderedAccessView* u = rb.d3dSlot < D3D11_PS_CS_UAV_REGISTER_COUNT ? uav_[rb.d3dSlot].get() : nullptr;
            [e setBuffer:u && u->buffer ? u->buffer : dev_->dummyBuffer offset:0 atIndex:rb.metalIndex];
            break;
        }
        default: break;
        }
    }
    const uint32_t* tg = cs->refl.threadgroupSize;
    [e dispatchThreadgroups:MTLSizeMake(x, y, z) threadsPerThreadgroup:MTLSizeMake(tg[0], tg[1], tg[2])];
    ++stats.dispatches;
    // Subsequent draws re-establish their own encoder caches.
}

// ---------------------------------------------------------------------------------------------------------
// Mapping, buffers and copies
// ---------------------------------------------------------------------------------------------------------
void Context::renameBuffer(BufferCore* b)
{
    const size_t size = (b->byteWidth + 15) & ~size_t(15);
    if (size <= kRingMaxAlloc)
    {
        RingAlloc a;
        if (constantRing_.alloc(size, 256, &a))
        {
            BufferBacking nb;
            nb.buf = a.buf;
            nb.offset = a.offset;
            nb.ptr = a.ptr;
            nb.lastUse = serial_;
            nb.ring = true;
            if (!b->cur.ring)
            {
                b->cur.lastUse = std::max(b->cur.lastUse, serial_);
                if (b->pool.size() < kPoolMax)
                    b->pool.push_back(b->cur);
            }
            b->cur = nb;
            return;
        }
    }
    BufferBacking old = b->cur;
    if (!old.ring)
    {
        old.lastUse = std::max(old.lastUse, serial_);
        b->pool.push_back(old);
    }
    const uint64_t completed = dev_->completedSerial.load(std::memory_order_acquire);
    for (size_t i = 0; i < b->pool.size(); ++i)
        if (b->pool[i].lastUse <= completed && b->pool[i].buf)
        {
            b->cur = b->pool[i];
            b->pool.erase(b->pool.begin() + i);
            b->cur.lastUse = serial_;
            return;
        }
    if (b->pool.size() < kPoolMax)
    {
        BufferBacking nb;
        nb.buf = dev_->newBuffer(b->byteWidth, true, nullptr);
        nb.ptr = static_cast<uint8_t*>(nb.buf.contents);
        nb.lastUse = serial_;
        dev_->bufferBytes += b->byteWidth;
        b->cur = nb;
        return;
    }
    // Every backing is still in flight: wait for the oldest one.
    size_t oldest = 0;
    for (size_t i = 1; i < b->pool.size(); ++i)
        if (b->pool[i].lastUse < b->pool[oldest].lastUse)
            oldest = i;
    if (b->pool[oldest].lastUse >= serial_)
        commit(false);
    dev_->waitForSerial(b->pool[oldest].lastUse);
    b->cur = b->pool[oldest];
    b->pool.erase(b->pool.begin() + oldest);
    b->cur.lastUse = serial_;
}

void Context::renameTexture(TextureCore* t)
{
    if (t->renamePool.empty())
        return;
    const uint64_t completed = dev_->completedSerial.load(std::memory_order_acquire);
    const size_t n = t->renamePool.size();
    for (size_t k = 1; k <= n; ++k)
    {
        const size_t i = (t->renameIndex + k) % n;
        if (t->renameLastUse[i] <= completed)
        {
            t->renameIndex = i;
            t->tex = t->renamePool[i];
            return;
        }
    }
    // All in flight: wait for the oldest.
    size_t oldest = 0;
    for (size_t i = 1; i < n; ++i)
        if (t->renameLastUse[i] < t->renameLastUse[oldest])
            oldest = i;
    if (t->renameLastUse[oldest] >= serial_)
        commit(false);
    dev_->waitForSerial(t->renameLastUse[oldest]);
    t->renameIndex = oldest;
    t->tex = t->renamePool[oldest];
}

HRESULT Context::Map(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT, D3D11_MAPPED_SUBRESOURCE* out)
{
    ResourceCore* core = CoreOf(res);
    if (!core || !out)
        return E_INVALIDARG;
    if (core->kind == ResKind::Buffer)
    {
        BufferCore* b = static_cast<BufferCore*>(core);
        if (b->usage == D3D11_USAGE_DEFAULT || b->usage == D3D11_USAGE_IMMUTABLE)
            return E_INVALIDARG;
        if (b->usage == D3D11_USAGE_DYNAMIC)
        {
            if (type == D3D11_MAP_WRITE_DISCARD)
                renameBuffer(b);
            else if (type != D3D11_MAP_WRITE_NO_OVERWRITE && type != D3D11_MAP_WRITE)
                return E_INVALIDARG;
        }
        else // STAGING
        {
            if (type & D3D11_MAP_READ)
            {
                // Make sure GPU writes (CopyResource into this buffer) have landed.
                if (b->lastWriteSerial)
                {
                    if (b->lastWriteSerial >= serial_)
                    {
                        commit(false);
                        ++stats.passBreaksByReadback;
                    }
                    dev_->waitForSerial(b->lastWriteSerial);
                }
            }
        }
        out->pData = b->cur.ptr;
        out->RowPitch = out->DepthPitch = b->byteWidth;
        return S_OK;
    }
    TextureCore* t = static_cast<TextureCore*>(core);
    if (sub >= t->subresourceCount())
        return E_INVALIDARG;
    const SubresourceLayout& l = t->layout[sub];
    if (t->usage == D3D11_USAGE_STAGING)
    {
        if ((type & D3D11_MAP_READ) && t->lastWriteSerial)
        {
            if (t->lastWriteSerial >= serial_)
            {
                commit(false);
                ++stats.passBreaksByReadback;
            }
            dev_->waitForSerial(t->lastWriteSerial);
        }
        out->pData = static_cast<uint8_t*>(t->staging.contents) + l.offset;
        out->RowPitch = UINT(l.rowPitch);
        out->DepthPitch = UINT(l.slicePitch);
        return S_OK;
    }
    if (t->usage == D3D11_USAGE_DYNAMIC && type == D3D11_MAP_WRITE_DISCARD)
    {
        t->dynLayout = l;
        t->dynShadow.assign(l.slicePitch * l.depth, 0);
        out->pData = t->dynShadow.data();
        out->RowPitch = UINT(l.rowPitch);
        out->DepthPitch = UINT(l.slicePitch);
        return S_OK;
    }
    return E_INVALIDARG;
}

void Context::Unmap(ID3D11Resource* res, UINT sub)
{
    ResourceCore* core = CoreOf(res);
    if (!core || core->kind == ResKind::Buffer)
        return;
    TextureCore* t = static_cast<TextureCore*>(core);
    if (t->usage == D3D11_USAGE_DYNAMIC && !t->dynShadow.empty())
    {
        renameTexture(t);
        const FormatInfo& fi = GetFormatInfo(t->dxgi);
        const UINT mip = sub % t->mips, slice = sub / t->mips;
        const SubresourceLayout& l = t->dynLayout;
        (void)fi;
        [t->tex replaceRegion:MTLRegionMake3D(0, 0, 0, l.width, l.height, l.depth)
                  mipmapLevel:mip
                        slice:slice
                    withBytes:t->dynShadow.data()
                  bytesPerRow:l.rowPitch
                bytesPerImage:l.slicePitch];
        t->dynShadow.clear();
    }
}

void Context::uploadTexture(TextureCore* t, uint32_t mip, uint32_t slice, const void* data, size_t rowPitch, size_t slicePitch)
{
    const SubresourceLayout& l = t->layout[mip + size_t(slice) * t->mips];
    const size_t total = slicePitch * l.depth;
    RingAlloc a;
    id<MTLBuffer> buf = nil;
    size_t off = 0;
    if (uploadRing_.alloc(total, 256, &a))
    {
        std::memcpy(a.ptr, data, total);
        buf = a.buf;
        off = a.offset;
    }
    else
    {
        buf = dev_->newBuffer(total, true, "upload");
        std::memcpy(buf.contents, data, total);
    }
    flushClears(t);
    id<MTLBlitCommandEncoder> b = blit(true);
    [b copyFromBuffer:buf
               sourceOffset:off
          sourceBytesPerRow:rowPitch
        sourceBytesPerImage:slicePitch
                 sourceSize:MTLSizeMake(l.width, l.height, l.depth)
                  toTexture:t->tex
           destinationSlice:slice
           destinationLevel:mip
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    t->lastWriteSerial = serial_;
}

void Context::UpdateSubresource(ID3D11Resource* res, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch)
{
    ResourceCore* core = CoreOf(res);
    if (!core || !data)
        return;
    flushClears(core);
    if (core->kind == ResKind::Buffer)
    {
        BufferCore* b = static_cast<BufferCore*>(core);
        const UINT begin = box ? box->left : 0;
        const UINT end = box ? box->right : b->byteWidth;
        const size_t n = end - begin;
        if (b->usage == D3D11_USAGE_DYNAMIC || b->usage == D3D11_USAGE_STAGING)
        {
            std::memcpy(b->cur.ptr + begin, data, n);
            return;
        }
        RingAlloc a;
        id<MTLBuffer> src;
        size_t off = 0;
        if (uploadRing_.alloc(n, 16, &a))
        {
            std::memcpy(a.ptr, data, n);
            src = a.buf;
            off = a.offset;
        }
        else
        {
            src = dev_->newBuffer(n, true, "upload");
            std::memcpy(src.contents, data, n);
        }
        id<MTLBlitCommandEncoder> be = blit(true);
        [be copyFromBuffer:src sourceOffset:off toBuffer:b->cur.buf destinationOffset:b->cur.offset + begin size:n];
        b->lastWriteSerial = serial_;
        b->cur.lastUse = serial_;
        return;
    }
    TextureCore* t = static_cast<TextureCore*>(core);
    const UINT mip = sub % t->mips, slice = sub / t->mips;
    const FormatInfo& fi = GetFormatInfo(t->dxgi);
    UINT w = Max1(t->width >> mip), h = Max1(t->height >> mip), d = Max1(t->depth >> mip), x = 0, y = 0, z = 0;
    if (box)
    {
        x = box->left; y = box->top; z = box->front;
        w = box->right - box->left; h = box->bottom - box->top; d = box->back - box->front;
    }
    const size_t rp = rowPitch ? rowPitch : RowPitch(fi, w);
    const size_t rows = fi.compressed ? (h + fi.blockSize - 1) / fi.blockSize : h;
    const size_t sp = depthPitch ? depthPitch : rp * rows;
    if (t->usage == D3D11_USAGE_STAGING)
    {
        const SubresourceLayout& l = t->layout[sub];
        for (UINT zz = 0; zz < d; ++zz)
            for (size_t r = 0; r < rows; ++r)
                std::memcpy(static_cast<uint8_t*>(t->staging.contents) + l.offset + (z + zz) * l.slicePitch +
                                (y / (fi.compressed ? fi.blockSize : 1) + r) * l.rowPitch + (x / (fi.compressed ? fi.blockSize : 1)) * fi.bytesPerBlock,
                            static_cast<const uint8_t*>(data) + zz * sp + r * rp, std::min(rp, l.rowPitch));
        return;
    }
    const size_t total = sp * d;
    RingAlloc a;
    id<MTLBuffer> src;
    size_t off = 0;
    if (uploadRing_.alloc(total, 256, &a))
    {
        std::memcpy(a.ptr, data, total);
        src = a.buf;
        off = a.offset;
    }
    else
    {
        src = dev_->newBuffer(total, true, "upload");
        std::memcpy(src.contents, data, total);
    }
    id<MTLBlitCommandEncoder> be = blit(true);
    [be copyFromBuffer:src
               sourceOffset:off
          sourceBytesPerRow:rp
        sourceBytesPerImage:sp
                 sourceSize:MTLSizeMake(w, h, d)
                  toTexture:resolveTexture(t)
           destinationSlice:slice
           destinationLevel:mip
          destinationOrigin:MTLOriginMake(x, y, z)];
    t->lastWriteSerial = serial_;
}

id<MTLTexture> Context::resolveTexture(TextureCore* t) { return t->current(this); }

void Context::copyTextureSubresource(TextureCore* dst, UINT dstSub, UINT dx, UINT dy, UINT dz, TextureCore* src, UINT srcSub,
                                     const D3D11_BOX* box)
{
    const UINT sMip = srcSub % src->mips, sSlice = srcSub / src->mips;
    const UINT dMip = dstSub % dst->mips, dSlice = dstSub / dst->mips;
    const FormatInfo& fi = GetFormatInfo(src->dxgi);
    UINT w = Max1(src->width >> sMip), h = Max1(src->height >> sMip), d = Max1(src->depth >> sMip), sx = 0, sy = 0, sz = 0;
    if (box)
    {
        sx = box->left; sy = box->top; sz = box->front;
        w = box->right - box->left; h = box->bottom - box->top; d = box->back - box->front;
    }
    id<MTLBlitCommandEncoder> be = blit(true);
    const bool srcStaging = src->usage == D3D11_USAGE_STAGING;
    const bool dstStaging = dst->usage == D3D11_USAGE_STAGING;
    if (srcStaging && dstStaging)
    {
        const SubresourceLayout& ls = src->layout[srcSub];
        const SubresourceLayout& ld = dst->layout[dstSub];
        const size_t rows = fi.compressed ? (h + fi.blockSize - 1) / fi.blockSize : h;
        const size_t n = std::min(ls.rowPitch, ld.rowPitch);
        for (UINT z = 0; z < d; ++z)
            for (size_t r = 0; r < rows; ++r)
                std::memcpy(static_cast<uint8_t*>(dst->staging.contents) + ld.offset + z * ld.slicePitch + r * ld.rowPitch,
                            static_cast<uint8_t*>(src->staging.contents) + ls.offset + z * ls.slicePitch + r * ls.rowPitch, n);
        return;
    }
    if (srcStaging)
    {
        const SubresourceLayout& ls = src->layout[srcSub];
        const size_t bx = fi.compressed ? sx / fi.blockSize : sx, by = fi.compressed ? sy / fi.blockSize : sy;
        [be copyFromBuffer:src->staging
                   sourceOffset:ls.offset + sz * ls.slicePitch + by * ls.rowPitch + bx * fi.bytesPerBlock
              sourceBytesPerRow:ls.rowPitch
            sourceBytesPerImage:ls.slicePitch
                     sourceSize:MTLSizeMake(w, h, d)
                      toTexture:resolveTexture(dst)
               destinationSlice:dSlice
               destinationLevel:dMip
              destinationOrigin:MTLOriginMake(dx, dy, dz)];
        dst->lastWriteSerial = serial_;
        return;
    }
    if (dstStaging)
    {
        const SubresourceLayout& ld = dst->layout[dstSub];
        const size_t bx = fi.compressed ? dx / fi.blockSize : dx, by = fi.compressed ? dy / fi.blockSize : dy;
        [be copyFromTexture:resolveTexture(src)
                    sourceSlice:sSlice
                    sourceLevel:sMip
                   sourceOrigin:MTLOriginMake(sx, sy, sz)
                     sourceSize:MTLSizeMake(w, h, d)
                       toBuffer:dst->staging
              destinationOffset:ld.offset + dz * ld.slicePitch + by * ld.rowPitch + bx * fi.bytesPerBlock
         destinationBytesPerRow:ld.rowPitch
       destinationBytesPerImage:ld.slicePitch];
        dst->lastWriteSerial = serial_;
        return;
    }
    [be copyFromTexture:resolveTexture(src)
            sourceSlice:sSlice
            sourceLevel:sMip
           sourceOrigin:MTLOriginMake(sx, sy, sz)
             sourceSize:MTLSizeMake(w, h, d)
              toTexture:resolveTexture(dst)
       destinationSlice:dSlice
       destinationLevel:dMip
      destinationOrigin:MTLOriginMake(dx, dy, dz)];
    dst->lastWriteSerial = serial_;
}

void Context::CopyResource(ID3D11Resource* pDst, ID3D11Resource* pSrc)
{
    ResourceCore* d = CoreOf(pDst);
    ResourceCore* s = CoreOf(pSrc);
    if (!d || !s || d == s)
        return;
    flushClears(d);
    flushClears(s);
    if (pass_.active)
        ++stats.passBreaksByCopy;
    if (d->kind == ResKind::Buffer && s->kind == ResKind::Buffer)
    {
        BufferCore* db = static_cast<BufferCore*>(d);
        BufferCore* sb = static_cast<BufferCore*>(s);
        id<MTLBlitCommandEncoder> be = blit(true);
        [be copyFromBuffer:sb->cur.buf sourceOffset:sb->cur.offset toBuffer:db->cur.buf destinationOffset:db->cur.offset size:std::min(db->byteWidth, sb->byteWidth)];
        db->lastWriteSerial = serial_;
        sb->cur.lastUse = db->cur.lastUse = serial_;
        return;
    }
    if (d->kind == ResKind::Buffer || s->kind == ResKind::Buffer)
        return;
    TextureCore* dt = static_cast<TextureCore*>(d);
    TextureCore* st = static_cast<TextureCore*>(s);
    const UINT n = std::min(dt->subresourceCount(), st->subresourceCount());
    for (UINT i = 0; i < n; ++i)
        copyTextureSubresource(dt, i, 0, 0, 0, st, i, nullptr);
}

void Context::CopySubresourceRegion(ID3D11Resource* pDst, UINT dstSub, UINT dx, UINT dy, UINT dz, ID3D11Resource* pSrc, UINT srcSub,
                                    const D3D11_BOX* box)
{
    ResourceCore* d = CoreOf(pDst);
    ResourceCore* s = CoreOf(pSrc);
    if (!d || !s)
        return;
    flushClears(d);
    flushClears(s);
    if (pass_.active)
        ++stats.passBreaksByCopy;
    if (d->kind == ResKind::Buffer && s->kind == ResKind::Buffer)
    {
        BufferCore* db = static_cast<BufferCore*>(d);
        BufferCore* sb = static_cast<BufferCore*>(s);
        const UINT begin = box ? box->left : 0;
        const UINT end = box ? box->right : sb->byteWidth;
        id<MTLBlitCommandEncoder> be = blit(true);
        [be copyFromBuffer:sb->cur.buf sourceOffset:sb->cur.offset + begin toBuffer:db->cur.buf destinationOffset:db->cur.offset + dx size:end - begin];
        db->lastWriteSerial = serial_;
        return;
    }
    if (d->kind == ResKind::Buffer || s->kind == ResKind::Buffer)
        return;
    copyTextureSubresource(static_cast<TextureCore*>(d), dstSub, dx, dy, dz, static_cast<TextureCore*>(s), srcSub, box);
}

void Context::ResolveSubresource(ID3D11Resource* pDst, UINT dstSub, ID3D11Resource* pSrc, UINT srcSub, DXGI_FORMAT)
{
    TextureCore* d = AsTexture(pDst);
    TextureCore* s = AsTexture(pSrc);
    if (!d || !s || s->samples < 2)
        return;
    flushClears(d);
    flushClears(s);
    endEncoder();
    ensureCommandBuffer();
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor* a = rp.colorAttachments[0];
    a.texture = resolveTexture(s);
    a.level = srcSub % s->mips;
    a.slice = srcSub / s->mips;
    a.loadAction = MTLLoadActionLoad;
    a.storeAction = MTLStoreActionStoreAndMultisampleResolve;
    a.resolveTexture = resolveTexture(d);
    a.resolveLevel = dstSub % d->mips;
    a.resolveSlice = dstSub / d->mips;
    id<MTLRenderCommandEncoder> e = [cb_ renderCommandEncoderWithDescriptor:rp];
    [e endEncoding];
    ++stats.renderPasses;
    ++stats.passBreaksByCopy;
    d->lastWriteSerial = serial_;
}

void Context::GenerateMips(ID3D11ShaderResourceView* pView)
{
    ShaderResourceView* v = static_cast<ShaderResourceView*>(pView);
    if (!v || v->buffer)
        return;
    flushClears(v->core);
    id<MTLBlitCommandEncoder> be = blit(true);
    [be generateMipmapsForTexture:v->texture(this)];
}

// ---------------------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------------------
void Context::harvestQueries(uint64_t completed)
{
    std::lock_guard<std::mutex> lk(dev_->queryMutex);
    for (size_t i = 0; i < pendingQueries_.size();)
    {
        Query* q = pendingQueries_[i].get();
        if (q->serial <= completed)
        {
            if (q->desc.Query != D3D11_QUERY_EVENT)
            {
                uint64_t sum = 0;
                for (const uint64_t* p : q->slotPtrs)
                    sum += *p;
                q->result = sum;
            }
            q->done = true;
            pendingQueries_.erase(pendingQueries_.begin() + i);
        }
        else
            ++i;
    }
}

void Context::Begin(ID3D11Asynchronous* pAsync)
{
    Query* q = static_cast<Query*>(static_cast<ID3D11Query*>(pAsync));
    if (!q || q->desc.Query == D3D11_QUERY_EVENT)
        return;
    std::lock_guard<std::mutex> lk(dev_->queryMutex);
    q->active = true;
    q->ended = false;
    q->done = false;
    q->result = 0;
    q->slotPtrs.clear();
    activeOcclusion_ = q;
    queryEpochInEncoder_ = ~0u;
}

void Context::End(ID3D11Asynchronous* pAsync)
{
    Query* q = static_cast<Query*>(static_cast<ID3D11Query*>(pAsync));
    if (!q)
        return;
    ensureCommandBuffer();
    if (q->desc.Query != D3D11_QUERY_EVENT && activeOcclusion_ == q)
    {
        if (enc_ == Enc::Render && queryEpochInEncoder_ == passEpoch())
            [re_ setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        activeOcclusion_ = nullptr;
    }
    std::lock_guard<std::mutex> lk(dev_->queryMutex);
    q->active = false;
    q->ended = true;
    q->done = false;
    q->serial = serial_;
    pendingQueries_.emplace_back(q);
}

HRESULT Context::GetData(ID3D11Asynchronous* pAsync, void* data, UINT size, UINT flags)
{
    Query* q = static_cast<Query*>(static_cast<ID3D11Query*>(pAsync));
    if (!q)
        return E_INVALIDARG;
    bool in_flight;
    {
        std::lock_guard<std::mutex> lk(dev_->queryMutex);
        if (!q->ended)
            return S_FALSE;
        in_flight = !q->done && q->serial >= serial_; // done is set by the completion handler's thread, under this lock
    }
    if (in_flight && !(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH))
        commit(false); // the query lives in the buffer being recorded: hand it to the GPU
    bool done;
    {
        std::lock_guard<std::mutex> lk(dev_->queryMutex);
        done = q->done;
    }
    if (!done)
    {
        std::this_thread::yield();
        return S_FALSE;
    }
    if (data)
    {
        if (q->desc.Query == D3D11_QUERY_EVENT && size >= 4)
            *static_cast<MtlBOOL*>(data) = TRUE;
        else if (q->desc.Query == D3D11_QUERY_OCCLUSION && size >= 8)
            *static_cast<uint64_t*>(data) = q->result;
        else if (q->desc.Query == D3D11_QUERY_OCCLUSION_PREDICATE && size >= 4)
            *static_cast<MtlBOOL*>(data) = q->result != 0;
    }
    return S_OK;
}

// ---------------------------------------------------------------------------------------------------------
// Debug annotation
// ---------------------------------------------------------------------------------------------------------
void GetStats(ID3D11DeviceContext* pContext, Stats* out)
{
    Context* c = static_cast<Context*>(static_cast<ID3D11DeviceContext1*>(pContext));
    *out = c->stats;
    Device* d = c->device();
    out->textureBytes = d->textureBytes;
    out->renderTargetBytes = d->renderTargetBytes;
    out->bufferBytes = d->bufferBytes;
    out->deviceAllocatedBytes = d->mtl.currentAllocatedSize;
    out->frames = d->frameCount;
    out->gpuSeconds = double(d->gpuNanos.load()) * 1e-9;
    out->blockedSeconds = double(d->blockedNanos.load()) * 1e-9;
}

bool GetRenderStats(RenderStats* out)
{
    Stats s;
    if (!GetGlobalStats(&s))
        return false;
    out->frames = s.frames;
    out->draws = s.draws;
    out->renderPasses = s.renderPasses;
    out->commandBuffers = s.commandBuffers;
    out->gpuSeconds = s.gpuSeconds;
    out->blockedSeconds = s.blockedSeconds;
    out->textureBytes = s.textureBytes;
    out->renderTargetBytes = s.renderTargetBytes;
    out->bufferBytes = s.bufferBytes;
    out->deviceAllocatedBytes = s.deviceAllocatedBytes;
    return true;
}

bool GetGlobalStats(Stats* out)
{
    Device* d = LatestDevice();
    if (!d || !d->context)
        return false;
    GetStats(static_cast<ID3D11DeviceContext*>(static_cast<ID3D11DeviceContext1*>(d->context)), out);
    return true;
}

void TraceDraws(ID3D11DeviceContext* pContext, int count)
{
    static_cast<Context*>(static_cast<ID3D11DeviceContext1*>(pContext))->traceDraws(count);
}

void ResetFrameStats(ID3D11DeviceContext* pContext)
{
    Context* c = static_cast<Context*>(static_cast<ID3D11DeviceContext1*>(pContext));
    Stats& s = c->stats;
    s.commandBuffers = s.renderPasses = s.computePasses = s.blitPasses = s.draws = s.dispatches = 0;
    s.passBreaksByTargetChange = s.passBreaksByClear = s.passBreaksByCopy = s.passBreaksByReadback = 0;
    s.foldedClears = s.materializedClears = 0;
}
} // namespace mtl_d3d11
