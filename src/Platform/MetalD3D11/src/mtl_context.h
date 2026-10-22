// Immediate context of the D3D11-on-Metal shim.
//
// Render-pass strategy (the part that decides performance on a tile GPU):
//  * OMSetRenderTargets only records the wanted attachment set. A Metal render encoder is created lazily by
//    the first draw, and kept while consecutive draws want the same set. R4's "unbind (0,0,0), set, draw"
//    sequences therefore cost nothing.
//  * Clears issued while the target is not part of the open pass become load actions of the next pass that
//    attaches it ("folded" clears). A clear of an attachment of the open pass ends that pass, because the
//    previous contents have to be stored first. A folded clear that is never followed by a pass using the
//    target is turned into an empty clear pass as soon as something reads the texture (SRV, copy, ...).
//  * Copies, resolves, uploads and read-backs run in blit encoders and end the open render pass.
#pragma once

#import "mtl_internal.h"

#include <deque>

namespace mtl_d3d11
{
class Annotation;

struct StageBindings
{
    Ref<Buffer> cb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
    Ref<ShaderResourceView> srv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    Ref<SamplerState> smp[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
};

struct PendingClear
{
    Ref<RenderTargetView> rtv;
    Ref<DepthStencilView> dsv;
    ResourceCore* core = nullptr;
    uint32_t level = 0, slice = 0, plane = 0;
    float color[4] = {};
    bool clearDepth = false, clearStencil = false;
    float depth = 1.0f;
    uint8_t stencil = 0;
};

struct PassAttachment
{
    ResourceCore* core = nullptr;
    uint32_t level = 0, slice = 0, plane = 0;
    uint32_t format = 0;
    bool operator==(const PassAttachment& o) const
    {
        return core == o.core && level == o.level && slice == o.slice && plane == o.plane && format == o.format;
    }
    bool operator!=(const PassAttachment& o) const { return !(*this == o); }
    bool used() const { return core != nullptr; }
};

struct PassState
{
    bool active = false;
    PassAttachment color[8];
    PassAttachment depth;
    MTLPixelFormat colorFormat[8] = {};
    MTLPixelFormat depthFormat = MTLPixelFormatInvalid;
    MTLPixelFormat stencilFormat = MTLPixelFormatInvalid;
    NSUInteger samples = 1;
    uint32_t width = 0, height = 0;
    uint32_t draws = 0;
};

enum DirtyBits : uint32_t
{
    D_PIPELINE = 1u << 0,
    D_DEPTHSTENCIL = 1u << 1,
    D_RASTER = 1u << 2,
    D_VIEWPORT = 1u << 3,
    D_SCISSOR = 1u << 4,
    D_BLENDCOLOR = 1u << 5,
    D_VERTEXBUFFERS = 1u << 6,
    D_ALL = 0xffffffffu
};

class Context final : public ID3D11DeviceContext1
{
public:
    explicit Context(Device* dev);
    ~Context();
    bool init();

    // IUnknown: the immediate context shares the device's reference count.
    HRESULT QueryInterface(MtlREFIID riid, void** pp) override;
    ULONG AddRef() override;
    ULONG Release() override;
    void GetDevice(ID3D11Device** pp) override;

    void IASetInputLayout(ID3D11InputLayout*) override;
    void IASetVertexBuffers(UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*) override;
    void IASetIndexBuffer(ID3D11Buffer*, DXGI_FORMAT, UINT) override;
    void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY) override;

    void VSSetShader(ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT) override;
    void VSSetConstantBuffers(UINT, UINT, ID3D11Buffer* const*) override;
    void VSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView* const*) override;
    void VSSetSamplers(UINT, UINT, ID3D11SamplerState* const*) override;
    void PSSetShader(ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT) override;
    void PSSetConstantBuffers(UINT, UINT, ID3D11Buffer* const*) override;
    void PSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView* const*) override;
    void PSSetSamplers(UINT, UINT, ID3D11SamplerState* const*) override;
    void GSSetShader(ID3D11GeometryShader*, ID3D11ClassInstance* const*, UINT) override;
    void GSSetConstantBuffers(UINT, UINT, ID3D11Buffer* const*) override;
    void GSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView* const*) override;
    void GSSetSamplers(UINT, UINT, ID3D11SamplerState* const*) override;
    void HSSetShader(ID3D11HullShader*, ID3D11ClassInstance* const*, UINT) override;
    void HSSetConstantBuffers(UINT, UINT, ID3D11Buffer* const*) override;
    void HSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView* const*) override;
    void HSSetSamplers(UINT, UINT, ID3D11SamplerState* const*) override;
    void DSSetShader(ID3D11DomainShader*, ID3D11ClassInstance* const*, UINT) override;
    void DSSetConstantBuffers(UINT, UINT, ID3D11Buffer* const*) override;
    void DSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView* const*) override;
    void DSSetSamplers(UINT, UINT, ID3D11SamplerState* const*) override;
    void CSSetShader(ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT) override;
    void CSSetConstantBuffers(UINT, UINT, ID3D11Buffer* const*) override;
    void CSSetShaderResources(UINT, UINT, ID3D11ShaderResourceView* const*) override;
    void CSSetSamplers(UINT, UINT, ID3D11SamplerState* const*) override;
    void CSSetUnorderedAccessViews(UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*) override;

    void RSSetState(ID3D11RasterizerState*) override;
    void RSSetViewports(UINT, const D3D11_VIEWPORT*) override;
    void RSSetScissorRects(UINT, const D3D11_RECT*) override;
    void OMSetRenderTargets(UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*) override;
    void OMGetRenderTargets(UINT, ID3D11RenderTargetView**, ID3D11DepthStencilView**) override;
    void OMSetBlendState(ID3D11BlendState*, const FLOAT[4], UINT) override;
    void OMSetDepthStencilState(ID3D11DepthStencilState*, UINT) override;

    void Draw(UINT, UINT) override;
    void DrawIndexed(UINT, UINT, INT) override;
    void DrawInstanced(UINT, UINT, UINT, UINT) override;
    void DrawIndexedInstanced(UINT, UINT, UINT, INT, UINT) override;
    void Dispatch(UINT, UINT, UINT) override;

    HRESULT Map(ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*) override;
    void Unmap(ID3D11Resource*, UINT) override;
    void UpdateSubresource(ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT) override;
    void CopySubresourceRegion(ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*) override;
    void CopyResource(ID3D11Resource*, ID3D11Resource*) override;
    void ResolveSubresource(ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT) override;
    void GenerateMips(ID3D11ShaderResourceView*) override;
    void ClearRenderTargetView(ID3D11RenderTargetView*, const FLOAT[4]) override;
    void ClearDepthStencilView(ID3D11DepthStencilView*, UINT, FLOAT, UINT8) override;
    void ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView*, const UINT[4]) override;
    void ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView*, const FLOAT[4]) override;

    void Begin(ID3D11Asynchronous*) override;
    void End(ID3D11Asynchronous*) override;
    HRESULT GetData(ID3D11Asynchronous*, void*, UINT, UINT) override;

    void ClearState() override;
    void Flush() override;

    // ---- shim internals ------------------------------------------------------------------------------
    void uploadTexture(TextureCore* t, uint32_t mip, uint32_t slice, const void* data, size_t rowPitch, size_t slicePitch);
    // Swap chain support
    id<MTLCommandBuffer> commandBuffer();
    uint64_t serial() const { return serial_; }
    // Ends the open encoder, lets `preCommit` add work (e.g. a capture blit), presents the drawable, commits and
    // starts the next frame (rings advance, the CPU is throttled to `framesInFlight`).
    void present(id<CAMetalDrawable> drawable, void (^preCommit)(id<MTLCommandBuffer>));
    id<MTLBlitCommandEncoder> blitEncoder();
    // Work encoded outside the shim (MetalFX): lands pending clears of the textures it touches, ends the open encoder and returns
    // the command buffer. The next draw opens a new render pass that loads what is there.
    id<MTLCommandBuffer> externalCommandBuffer(ResourceCore* reads, ResourceCore* writes);
    void flushAllClearsFor(ResourceCore* core) { flushClears(core); }
    void waitIdle();
    Device* device() const { return dev_; }
    uint32_t passEpoch() const { return passEpoch_; }
    Stats stats;
    // Debug: log the next `n` draws (shader names, render targets, depth/stencil/blend state, viewport).
    void traceDraws(int n) { traceDraws_ = n; }

private:
    friend class Annotation;
    // command buffer / encoders
    enum class Enc { None, Render, Compute, Blit };
    void ensureCommandBuffer();
    void endEncoder();
    void commit(bool wait);
    id<MTLBlitCommandEncoder> blit(bool breaksPass);
    id<MTLComputeCommandEncoder> computeEncoder();
    void endFrame();

    // render pass
    bool ensureRenderPass();
    void beginRenderPass(const PassState& want);
    bool wantedPass(PassState* out);
    PassAttachment attachmentOf(RenderTargetView* v) const;
    void materializeClear(size_t index);
    void flushClears(ResourceCore* core);
    void flushClearsForShader(const ShaderReflection& refl, const StageBindings& sb);
    PendingClear* findClear(ResourceCore* core, uint32_t level, uint32_t slice, uint32_t plane);

    // draw
    bool prepareDraw(bool indexed, D3D11_PRIMITIVE_TOPOLOGY* topo);
    id<MTLRenderPipelineState> pipeline(MTLPrimitiveTopologyClass cls);
    void applyFixedState();
    void bindRenderStage(int stage, const ShaderReflection& refl, const StageBindings& sb);
    void bindVertexBuffers();
    void resetEncoderCaches();
    id<MTLTexture> placeholderFor(const ResourceBinding& rb);
    bool sameAsAttachment(ResourceCore* core) const;
    void unbindConflictingSRVs();
    void renameBuffer(BufferCore* b);
    void renameTexture(TextureCore* t);
    void setSRVs(StageBindings& sb, UINT start, UINT n, ID3D11ShaderResourceView* const* views);
    void setCBs(StageBindings& sb, UINT start, UINT n, ID3D11Buffer* const* bufs);
    void setSamplers(StageBindings& sb, UINT start, UINT n, ID3D11SamplerState* const* s);
    void harvestQueries(uint64_t completed);
    void copyTextureSubresource(TextureCore* dst, UINT dstSub, UINT dx, UINT dy, UINT dz, TextureCore* src, UINT srcSub,
                                const D3D11_BOX* box);
    id<MTLTexture> resolveTexture(TextureCore* t);

    Device* dev_;
    id<MTLCommandBuffer> cb_ = nil;
    uint64_t serial_ = 0;   // serial of cb_ (or of the next buffer when cb_ is null)
    uint64_t committed_ = 0;
    Enc enc_ = Enc::None;
    id<MTLRenderCommandEncoder> re_ = nil;
    id<MTLComputeCommandEncoder> ce_ = nil;
    id<MTLBlitCommandEncoder> be_ = nil;
    PassState pass_;
    std::deque<uint64_t> frameSerials_;
    Ring constantRing_, uploadRing_;
    std::vector<id<MTLBuffer>> visBufs_;
    uint32_t visSlot_ = 0;
    uint32_t passEpoch_ = 0;
    int traceDraws_ = 0;
    void traceDraw();

    // D3D state
    Ref<InputLayout> layout_;
    struct VBSlot { Ref<Buffer> buf; UINT stride = 0, offset = 0; } vb_[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    Ref<Buffer> ib_;
    DXGI_FORMAT ibFormat_ = DXGI_FORMAT_R16_UINT;
    UINT ibOffset_ = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    Ref<VertexShader> vs_;
    Ref<PixelShader> ps_;
    Ref<ComputeShader> cs_;
    StageBindings bind_[kShaderStageCount];
    Ref<UnorderedAccessView> uav_[D3D11_PS_CS_UAV_REGISTER_COUNT];
    Ref<RasterizerState> rs_;
    Ref<BlendState> blend_;
    Ref<DepthStencilState> dss_;
    float blendFactor_[4] = {1, 1, 1, 1};
    UINT sampleMask_ = 0xffffffffu;
    UINT stencilRef_ = 0;
    D3D11_VIEWPORT viewport_ = {};
    bool viewportSet_ = false;
    D3D11_RECT scissor_ = {};
    bool scissorSet_ = false;
    Ref<RenderTargetView> rtv_[8];
    Ref<DepthStencilView> dsv_;
    std::vector<PendingClear> clears_;
    uint32_t dirty_ = D_ALL;

    // encoder-side caches (valid for the open render/compute encoder)
    __unsafe_unretained id<MTLBuffer> encBuf_[2][31];
    uint32_t encBufOff_[2][31];
    __unsafe_unretained id<MTLTexture> encTex_[2][128];
    __unsafe_unretained id<MTLSamplerState> encSmp_[2][16];
    __unsafe_unretained id<MTLRenderPipelineState> encPso_ = nil;
    __unsafe_unretained id<MTLDepthStencilState> encDss_ = nil;
    id<MTLDepthStencilState> defaultDss_ = nil;

    std::unordered_map<PipelineKey, id<MTLRenderPipelineState>, PipelineKeyHash> psoCache_;
    // pixel shader variants for vertex shaders that do not write every interpolant the pixel shader reads
    std::unordered_map<uint64_t, id<MTLFunction>> fragmentVariants_;
    id<MTLFunction> fragmentFunctionFor(VertexShader* vs, PixelShader* ps);

    // queries
    Query* activeOcclusion_ = nullptr;
    uint32_t queryEpochInEncoder_ = ~0u;
    std::vector<Ref<Query>> pendingQueries_;

    Annotation* annotation_ = nullptr;
};

} // namespace mtl_d3d11
