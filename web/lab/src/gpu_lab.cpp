// Item 1: WebGPU on a worker through emdawnwebgpu (webgpu.h), drawing to an OffscreenCanvas.
//
// Runs on the app thread, one step per SDL_AppIterate. Asynchronous results (adapter, device,
// buffer maps, async pipelines) arrive as spontaneous callbacks between frames, the way the engine
// will have to consume them: nothing here ever waits on the GPU.
//
// Tests, in order: features and limits, 4-target G-buffer plus composite (pixel-checked), BC1 and
// BC3 decode (pixel-checked), occlusion query latency and value kind, createRenderPipelineAsync,
// dynamic-buffer upload cost, and presentation while the worker is blocked.
#include "gpu_lab.h"

#include <webgpu/webgpu.h>

#include <emscripten.h>
#include <emscripten/threading.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "lab_report.h"

namespace lab::gpu
{
namespace
{
WGPUStringView SV(const char* s) { return WGPUStringView{s, WGPU_STRLEN}; }

std::string Str(WGPUStringView v)
{
    if (!v.data)
        return {};
    return v.length == WGPU_STRLEN ? std::string(v.data) : std::string(v.data, v.length);
}

struct FeatureInfo
{
    WGPUFeatureName id;
    const char* name;
    bool wanted; // the brief's list, requested on the device when the adapter has it
};

const FeatureInfo kFeatures[] = {
    {WGPUFeatureName_CoreFeaturesAndLimits, "core-features-and-limits", false},
    {WGPUFeatureName_DepthClipControl, "depth-clip-control", true},
    {WGPUFeatureName_Depth32FloatStencil8, "depth32float-stencil8", true},
    {WGPUFeatureName_TextureCompressionBC, "texture-compression-bc", true},
    {WGPUFeatureName_TextureCompressionBCSliced3D, "texture-compression-bc-sliced-3d", true},
    {WGPUFeatureName_TextureCompressionETC2, "texture-compression-etc2", false},
    {WGPUFeatureName_TextureCompressionASTC, "texture-compression-astc", false},
    {WGPUFeatureName_TextureCompressionASTCSliced3D, "texture-compression-astc-sliced-3d", false},
    {WGPUFeatureName_TimestampQuery, "timestamp-query", true},
    {WGPUFeatureName_IndirectFirstInstance, "indirect-first-instance", false},
    {WGPUFeatureName_ShaderF16, "shader-f16", false},
    {WGPUFeatureName_RG11B10UfloatRenderable, "rg11b10ufloat-renderable", true},
    {WGPUFeatureName_BGRA8UnormStorage, "bgra8unorm-storage", false},
    {WGPUFeatureName_Float32Filterable, "float32-filterable", true},
    {WGPUFeatureName_Float32Blendable, "float32-blendable", false},
    {WGPUFeatureName_ClipDistances, "clip-distances", true},
    {WGPUFeatureName_DualSourceBlending, "dual-source-blending", false},
    {WGPUFeatureName_Subgroups, "subgroups", false},
    {WGPUFeatureName_TextureFormatsTier1, "texture-formats-tier1", true},
    {WGPUFeatureName_TextureFormatsTier2, "texture-formats-tier2", true},
    {WGPUFeatureName_PrimitiveIndex, "primitive-index", false},
    {WGPUFeatureName_TextureComponentSwizzle, "texture-component-swizzle", false},
    {WGPUFeatureName_Unorm16TextureFormats, "unorm16-texture-formats", false},
    {WGPUFeatureName_MultiDrawIndirect, "multi-draw-indirect", false},
};

struct Test;

struct State
{
    WGPUInstance instance = nullptr;
    WGPUSurface surface = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUTextureFormat surface_format = WGPUTextureFormat_BGRA8Unorm;
    std::string canvas;
    int width = 960;
    int height = 540;

    enum class Phase
    {
        Idle,
        WaitAdapter,
        WaitDevice,
        Ready,
        Failed,
    } phase = Phase::Idle;

    uint64_t frame = 0;
    double start_ms = 0;
    int errors = 0;
    std::string last_error;
    std::vector<WGPUFeatureName> device_features;

    WGPUTexture gbuf[4] = {};
    WGPUTextureView gbuf_view[4] = {};
    WGPUTextureFormat gbuf_format[4] = {};
    WGPURenderPipeline gbuf_pipeline = nullptr;
    WGPURenderPipeline comp_pipeline = nullptr;
    WGPUBindGroup comp_bg = nullptr;
    WGPUBuffer comp_ubo = nullptr;

    std::vector<std::unique_ptr<Test>> tests;
    size_t test_index = 0;
    bool test_started = false;
    int pending_block_ms = 0;
} g;

bool HasFeature(WGPUFeatureName f)
{
    for (WGPUFeatureName d : g.device_features)
        if (d == f)
            return true;
    return false;
}

const char* FeatureName(WGPUFeatureName f)
{
    for (const FeatureInfo& info : kFeatures)
        if (info.id == f)
            return info.name;
    return nullptr;
}

const char* FormatName(WGPUTextureFormat f)
{
    switch (f)
    {
    case WGPUTextureFormat_BGRA8Unorm: return "bgra8unorm";
    case WGPUTextureFormat_RGBA8Unorm: return "rgba8unorm";
    case WGPUTextureFormat_RGBA16Float: return "rgba16float";
    case WGPUTextureFormat_RG11B10Ufloat: return "rg11b10ufloat";
    default: return "other";
    }
}

WGPUShaderModule MakeShader(const std::string& wgsl, const char* label)
{
    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = WGPUStringView{wgsl.c_str(), wgsl.size()};
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    desc.nextInChain = &src.chain;
    desc.label = SV(label);
    return wgpuDeviceCreateShaderModule(g.device, &desc);
}

WGPUBuffer MakeBuffer(uint64_t size, WGPUBufferUsage usage, const char* label)
{
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.size = size;
    desc.usage = usage;
    desc.label = SV(label);
    return wgpuDeviceCreateBuffer(g.device, &desc);
}

WGPUTexture MakeTexture(uint32_t w, uint32_t h, WGPUTextureFormat format, WGPUTextureUsage usage, const char* label)
{
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.size = {w, h, 1};
    desc.format = format;
    desc.usage = usage;
    desc.label = SV(label);
    return wgpuDeviceCreateTexture(g.device, &desc);
}

void Submit(WGPUCommandEncoder enc)
{
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g.queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
}

// A MAP_READ buffer whose map callback records when the data became readable.
struct Readback
{
    WGPUBuffer buffer = nullptr;
    uint64_t size = 0;
    bool pending = false;
    bool ready = false;
    WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Success;
    uint64_t submit_frame = 0, done_frame = 0;
    double submit_ms = 0, done_ms = 0;

    void Create(uint64_t bytes, const char* label)
    {
        size = bytes;
        buffer = MakeBuffer(bytes, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, label);
    }
    bool Free() const { return !pending && !ready; }
    // Call after the submit that writes the buffer.
    void Map()
    {
        pending = true;
        submit_frame = g.frame;
        submit_ms = NowMs();
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.callback = [](WGPUMapAsyncStatus st, WGPUStringView, void* u1, void*) {
            auto* self = static_cast<Readback*>(u1);
            self->status = st;
            self->pending = false;
            self->ready = true;
            self->done_frame = g.frame;
            self->done_ms = NowMs();
        };
        cb.userdata1 = this;
        wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, size, cb);
    }
    const uint8_t* Data() const { return static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(buffer, 0, size)); }
    void Unmap()
    {
        wgpuBufferUnmap(buffer);
        ready = false;
    }
};

void ClearTexture(WGPUCommandEncoder enc, WGPUTextureView view, double r, double gr, double b)
{
    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = view;
    ca.loadOp = WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue = {r, gr, b, 1.0};
    WGPURenderPassDescriptor pd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pd.colorAttachmentCount = 1;
    pd.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &pd);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

bool AcquireSurface(WGPUTexture* tex, WGPUTextureView* view)
{
    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(g.surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
        return false;
    *tex = st.texture;
    *view = wgpuTextureCreateView(st.texture, nullptr);
    return true;
}

// Clears the canvas texture to one colour and submits (used by the blocked-worker test).
void ClearSurface(double r, double gr, double b)
{
    WGPUTexture tex;
    WGPUTextureView view;
    if (!AcquireSurface(&tex, &view))
        return;
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
    ClearTexture(enc, view, r, gr, b);
    Submit(enc);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(tex);
}

// ---- Tests ---------------------------------------------------------------------------------------

struct Test
{
    virtual ~Test() = default;
    virtual void Start() {}
    // Returns true when finished.
    virtual bool Step() = 0;
    // Extra commands recorded into the scene's encoder after the composite pass.
    virtual void AfterScene(WGPUCommandEncoder, WGPUTexture) {}
    // True while the test draws the canvas itself.
    virtual bool OwnsCanvas() const { return false; }
};

const char* kFullscreenVs = R"(
struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> VOut {
  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var o: VOut;
  o.pos = vec4f(p[i], 0.0, 1.0);
  o.uv = p[i] * vec2f(0.5, -0.5) + vec2f(0.5);
  return o;
}
)";

// G-buffer values; the composite shows one target per quadrant and the MRT test checks them.
const float kGbufValues[4][3] = {{0.2f, 0.4f, 0.6f}, {0.8f, 0.6f, 0.4f}, {0.1f, 0.9f, 0.3f}, {0.5f, 0.25f, 0.75f}};

struct MrtTest : Test
{
    Readback rb;
    int stage = 0;
    int wait = 0;
    void Start() override { rb.Create(4 * 256, "mrt readback"); }
    void AfterScene(WGPUCommandEncoder enc, WGPUTexture tex) override
    {
        if (stage != 1)
            return;
        for (int q = 0; q < 4; ++q)
        {
            const uint32_t x = (q & 1) ? g.width * 3 / 4 : g.width / 4;
            const uint32_t y = (q & 2) ? g.height * 3 / 4 : g.height / 4;
            WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            src.texture = tex;
            src.origin = {x, y, 0};
            WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
            dst.buffer = rb.buffer;
            dst.layout.offset = uint64_t(q) * 256;
            dst.layout.bytesPerRow = 256;
            dst.layout.rowsPerImage = 1;
            WGPUExtent3D size = {1, 1, 1};
            wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &size);
        }
        stage = 2;
    }
    bool Step() override
    {
        if (stage == 0 && ++wait > 3)
            stage = 1; // next scene copies the four quadrant centres
        else if (stage == 2)
        {
            rb.Map();
            stage = 3;
        }
        else if (stage == 3 && rb.ready)
        {
            const uint8_t* px = rb.Data();
            const bool bgra = g.surface_format == WGPUTextureFormat_BGRA8Unorm;
            bool ok = rb.status == WGPUMapAsyncStatus_Success;
            std::string got;
            for (int q = 0; q < 4 && px; ++q)
            {
                const uint8_t* p = px + q * 256;
                const int r = bgra ? p[2] : p[0], gg = p[1], b = bgra ? p[0] : p[2];
                const int er = int(std::lround(kGbufValues[q][0] * 255)), eg = int(std::lround(kGbufValues[q][1] * 255)),
                          eb = int(std::lround(kGbufValues[q][2] * 255));
                const bool match = std::abs(r - er) <= 2 && std::abs(gg - eg) <= 2 && std::abs(b - eb) <= 2;
                ok = ok && match;
                got += Fmt("%s[%d,%d,%d vs %d,%d,%d]", q ? " " : "", r, gg, b, er, eg, eb);
            }
            rb.Unmap();
            const std::string formats = Fmt("%s, %s, %s, %s", FormatName(g.gbuf_format[0]), FormatName(g.gbuf_format[1]),
                                            FormatName(g.gbuf_format[2]), FormatName(g.gbuf_format[3]));
            Report("gpu.mrt", ok && g.errors == 0 ? "pass" : "fail",
                   Fmt("4 targets (%s) and a composite to the %s canvas; quadrant pixels %s; %d WebGPU errors so far",
                       formats.c_str(), FormatName(g.surface_format), ok ? "match" : "differ", g.errors),
                   Json().Str("targets", formats).Str("pixels", got).Int("errors", g.errors).Done());
            return true;
        }
        return false;
    }
};

struct BcTest : Test
{
    Readback rb;
    bool done = false;
    void Start() override
    {
        if (!HasFeature(WGPUFeatureName_TextureCompressionBC))
        {
            Report("gpu.bc", "fail", "texture-compression-bc is not available on this adapter");
            done = true;
            return;
        }
        // 8x8 textures = 2x2 blocks: red, green / blue, white. BC3 alpha per block 255, 128 / 64, 0.
        const uint16_t colors[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
        const uint8_t alphas[4] = {255, 128, 64, 0};
        uint8_t bc1[32] = {}, bc3[64] = {};
        for (int i = 0; i < 4; ++i)
        {
            uint8_t* b1 = bc1 + i * 8; // c0, c1 = 0, indices 0 -> c0 everywhere
            b1[0] = colors[i] & 0xFF;
            b1[1] = colors[i] >> 8;
            uint8_t* b3 = bc3 + i * 16; // a0, a1 = 0, indices 0 -> a0; then a BC1 colour block
            b3[0] = alphas[i];
            std::memcpy(b3 + 8, b1, 8);
        }
        WGPUTexture t1 = MakeTexture(8, 8, WGPUTextureFormat_BC1RGBAUnorm, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, "bc1");
        WGPUTexture t3 = MakeTexture(8, 8, WGPUTextureFormat_BC3RGBAUnorm, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst, "bc3");
        auto upload = [](WGPUTexture t, const uint8_t* data, size_t size, uint32_t row) {
            WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            dst.texture = t;
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = row;
            layout.rowsPerImage = 8;
            WGPUExtent3D size3 = {8, 8, 1};
            wgpuQueueWriteTexture(g.queue, &dst, data, size, &layout, &size3);
        };
        upload(t1, bc1, sizeof(bc1), 16);
        upload(t3, bc3, sizeof(bc3), 32);

        const std::string wgsl = std::string(kFullscreenVs) + R"(
@group(0) @binding(0) var t1: texture_2d<f32>;
@group(0) @binding(1) var t3: texture_2d<f32>;
@fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {
  let ip = vec2i(p.xy);
  if (ip.x < 8) { return textureLoad(t1, ip, 0); }
  return textureLoad(t3, ip - vec2i(8, 0), 0);
}
)";
        WGPUShaderModule sm = MakeShader(wgsl, "bc decode");
        WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
        ct.format = WGPUTextureFormat_RGBA8Unorm;
        WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
        fs.module = sm;
        fs.entryPoint = SV("fs");
        fs.targetCount = 1;
        fs.targets = &ct;
        WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        pd.vertex.module = sm;
        pd.vertex.entryPoint = SV("vs");
        pd.fragment = &fs;
        WGPURenderPipeline pipe = wgpuDeviceCreateRenderPipeline(g.device, &pd);

        WGPUTextureView v1 = wgpuTextureCreateView(t1, nullptr), v3 = wgpuTextureCreateView(t3, nullptr);
        WGPUBindGroupEntry e[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        e[0].binding = 0;
        e[0].textureView = v1;
        e[1].binding = 1;
        e[1].textureView = v3;
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = wgpuRenderPipelineGetBindGroupLayout(pipe, 0);
        bgd.entryCount = 2;
        bgd.entries = e;
        WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g.device, &bgd);

        WGPUTexture target = MakeTexture(16, 8, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc, "bc target");
        WGPUTextureView tv = wgpuTextureCreateView(target, nullptr);
        rb.Create(256 * 8, "bc readback");

        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = tv;
        ca.loadOp = WGPULoadOp_Clear;
        ca.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
        wgpuRenderPassEncoderSetPipeline(pass, pipe);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        src.texture = target;
        WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        dst.buffer = rb.buffer;
        dst.layout.bytesPerRow = 256;
        dst.layout.rowsPerImage = 8;
        WGPUExtent3D size = {16, 8, 1};
        wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &size);
        Submit(enc);
        rb.Map();
    }
    bool Step() override
    {
        if (done)
            return true;
        if (!rb.ready)
            return false;
        const uint8_t* px = rb.Data();
        const uint8_t expect_rgb[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
        const int expect_a3[4] = {255, 128, 64, 0};
        bool ok1 = px != nullptr, ok3 = px != nullptr;
        std::string got;
        for (int i = 0; i < 4 && px; ++i)
        {
            const int bx = i & 1, by = i >> 1;
            const uint8_t* p1 = px + (by * 4 + 1) * 256 + (bx * 4 + 1) * 4;
            const uint8_t* p3 = px + (by * 4 + 2) * 256 + (8 + bx * 4 + 2) * 4;
            for (int c = 0; c < 3; ++c)
            {
                ok1 = ok1 && std::abs(p1[c] - expect_rgb[i][c]) <= 1;
                ok3 = ok3 && std::abs(p3[c] - expect_rgb[i][c]) <= 1;
            }
            ok1 = ok1 && p1[3] == 255;
            ok3 = ok3 && std::abs(p3[3] - expect_a3[i]) <= 1;
            got += Fmt("%sbc1(%d,%d,%d,%d) bc3(%d,%d,%d,%d)", i ? " " : "", p1[0], p1[1], p1[2], p1[3], p3[0], p3[1], p3[2], p3[3]);
        }
        rb.Unmap();
        Report("gpu.bc", ok1 && ok3 ? "pass" : "fail",
               Fmt("BC1 decode %s, BC3 decode (colour and alpha) %s, sampled with textureLoad", ok1 ? "matches" : "differs",
                   ok3 ? "matches" : "differs"),
               Json().Bool("bc1", ok1).Bool("bc3", ok3).Str("pixels", got).Done());
        return true;
    }
};

struct OcclusionTest : Test
{
    static constexpr int kIterations = 60;
    static constexpr int kQueries = 4;
    WGPUQuerySet qs = nullptr;
    WGPUBuffer resolve = nullptr, ubo = nullptr;
    WGPUBindGroup bg = nullptr;
    WGPURenderPipeline pipe = nullptr;
    WGPUTextureView color = nullptr, depth = nullptr;
    Readback rbs[8];
    int issued = 0, received = 0;
    std::vector<double> latency_frames, latency_ms;
    uint64_t first_values[kQueries] = {};
    bool values_consistent = true;

    void Start() override
    {
        // Draws into 64x64: full screen at z=0.5 (4096 samples), a 16x16 square in front (256), a
        // full-screen quad behind (0) and a single pixel in front (1).
        struct Draw
        {
            float rect[4];
            float z[4];
        };
        const float px = 2.0f / 64.0f;
        const Draw draws[kQueries] = {
            {{-1, -1, 1, 1}, {0.5f}},
            {{-1 + 24 * px, -1 + 24 * px, -1 + 40 * px, -1 + 40 * px}, {0.25f}},
            {{-1, -1, 1, 1}, {0.75f}},
            {{-1 + 10 * px, -1 + 10 * px, -1 + 11 * px, -1 + 11 * px}, {0.1f}},
        };
        ubo = MakeBuffer(256 * kQueries, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "occlusion ubo");
        for (int i = 0; i < kQueries; ++i)
            wgpuQueueWriteBuffer(g.queue, ubo, uint64_t(i) * 256, &draws[i], sizeof(Draw));

        const char* wgsl = R"(
struct D { rect: vec4f, z: vec4f };
@group(0) @binding(0) var<uniform> d: D;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  var c = array<vec2f, 6>(vec2f(0,0), vec2f(1,0), vec2f(0,1), vec2f(0,1), vec2f(1,0), vec2f(1,1));
  let p = mix(d.rect.xy, d.rect.zw, c[i]);
  return vec4f(p, d.z.x, 1.0);
}
@fragment fn fs() -> @location(0) vec4f { return vec4f(1.0, 0.5, 0.0, 1.0); }
)";
        WGPUShaderModule sm = MakeShader(wgsl, "occlusion");
        WGPUBindGroupLayoutEntry le = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        le.binding = 0;
        le.visibility = WGPUShaderStage_Vertex;
        le.buffer.type = WGPUBufferBindingType_Uniform;
        le.buffer.hasDynamicOffset = true;
        le.buffer.minBindingSize = 32;
        WGPUBindGroupLayoutDescriptor bld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bld.entryCount = 1;
        bld.entries = &le;
        WGPUBindGroupLayout bgl = wgpuDeviceCreateBindGroupLayout(g.device, &bld);
        WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &bgl;
        WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(g.device, &pld);

        WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
        ct.format = WGPUTextureFormat_RGBA8Unorm;
        WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
        fs.module = sm;
        fs.entryPoint = SV("fs");
        fs.targetCount = 1;
        fs.targets = &ct;
        WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
        ds.format = WGPUTextureFormat_Depth32Float;
        ds.depthWriteEnabled = WGPUOptionalBool_True;
        ds.depthCompare = WGPUCompareFunction_Less;
        WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        pd.layout = pl;
        pd.vertex.module = sm;
        pd.vertex.entryPoint = SV("vs");
        pd.fragment = &fs;
        pd.depthStencil = &ds;
        pipe = wgpuDeviceCreateRenderPipeline(g.device, &pd);

        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = 0;
        e.buffer = ubo;
        e.size = 32;
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = bgl;
        bgd.entryCount = 1;
        bgd.entries = &e;
        bg = wgpuDeviceCreateBindGroup(g.device, &bgd);

        color = wgpuTextureCreateView(MakeTexture(64, 64, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_RenderAttachment, "occ color"), nullptr);
        depth = wgpuTextureCreateView(MakeTexture(64, 64, WGPUTextureFormat_Depth32Float, WGPUTextureUsage_RenderAttachment, "occ depth"), nullptr);

        WGPUQuerySetDescriptor qd = WGPU_QUERY_SET_DESCRIPTOR_INIT;
        qd.type = WGPUQueryType_Occlusion;
        qd.count = kQueries;
        qs = wgpuDeviceCreateQuerySet(g.device, &qd);
        resolve = MakeBuffer(8 * kQueries, WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc, "occ resolve");
        for (Readback& rb : rbs)
            rb.Create(8 * kQueries, "occ readback");
    }

    void Issue(Readback& rb)
    {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = color;
        ca.loadOp = WGPULoadOp_Clear;
        ca.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDepthStencilAttachment da = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        da.view = depth;
        da.depthLoadOp = WGPULoadOp_Clear;
        da.depthStoreOp = WGPUStoreOp_Store;
        da.depthClearValue = 1.0f;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        rp.depthStencilAttachment = &da;
        rp.occlusionQuerySet = qs;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
        wgpuRenderPassEncoderSetPipeline(pass, pipe);
        for (uint32_t q = 0; q < kQueries; ++q)
        {
            const uint32_t offset = q * 256;
            wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 1, &offset);
            wgpuRenderPassEncoderBeginOcclusionQuery(pass, q);
            wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
            wgpuRenderPassEncoderEndOcclusionQuery(pass);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        wgpuCommandEncoderResolveQuerySet(enc, qs, 0, kQueries, resolve, 0);
        wgpuCommandEncoderCopyBufferToBuffer(enc, resolve, 0, rb.buffer, 0, 8 * kQueries);
        Submit(enc);
        rb.Map();
        ++issued;
    }

    bool Step() override
    {
        for (Readback& rb : rbs)
        {
            if (!rb.ready)
                continue;
            const uint8_t* data = rb.Data();
            uint64_t v[kQueries] = {};
            if (data)
                std::memcpy(v, data, sizeof(v));
            rb.Unmap();
            if (received == 0)
                std::memcpy(first_values, v, sizeof(v));
            else if (std::memcmp(first_values, v, sizeof(v)) != 0)
                values_consistent = false;
            // A result mapped between frames F and F+1 is usable in frame F+1.
            latency_frames.push_back(double(rb.done_frame + 1 - rb.submit_frame));
            latency_ms.push_back(rb.done_ms - rb.submit_ms);
            ++received;
        }
        if (issued < kIterations)
            for (Readback& rb : rbs)
                if (rb.Free())
                {
                    Issue(rb);
                    break;
                }
        if (received < kIterations)
            return false;

        const uint64_t* v = first_values;
        const bool counts = v[0] == 4096 && v[1] == 256 && v[2] == 0 && v[3] == 1;
        const bool boolean = !counts && v[0] != 0 && v[1] != 0 && v[2] == 0 && v[3] != 0 && v[0] == v[1] && v[1] == v[3];
        const bool sane = counts || boolean || (v[0] != 0 && v[1] != 0 && v[2] == 0 && v[3] != 0);
        const Stats lf = Summarize(latency_frames), lm = Summarize(latency_ms);
        Report("gpu.occlusion", sane && values_consistent ? "pass" : "fail",
               Fmt("values %llu/%llu/%llu/%llu for 4096/256/0/1 samples: %s; readable after %.0f frames median (min %.0f, max %.0f), %.1f ms median",
                   (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3],
                   counts ? "sample counts" : (boolean ? "zero/non-zero only" : "other non-zero values"), lf.median, lf.min, lf.max,
                   lm.median),
               Json()
                   .Nums("values", {double(v[0]), double(v[1]), double(v[2]), double(v[3])})
                   .Nums("expected_samples", {4096, 256, 0, 1})
                   .Str("kind", counts ? "count" : (boolean ? "boolean" : "other"))
                   .Bool("consistent", values_consistent)
                   .Raw("latency_frames", StatsJson(lf))
                   .Raw("latency_ms", StatsJson(lm))
                   .Done());
        return true;
    }
};

struct PipelineTest : Test
{
    static constexpr int kCount = 64;
    double t_start = 0, t_issued = 0, t_last = 0;
    uint64_t frame_start = 0;
    int done = 0, failed = 0;
    std::vector<WGPURenderPipeline> async_pipes;
    int stage = 0;
    double sync_call_ms = 0, sync_first_use_ms = -1, async_first_use_ms = -1;
    double first_use_t0 = 0;
    std::vector<WGPURenderPipeline> sync_pipes;
    WGPUTextureView target = nullptr;

    WGPURenderPipelineDescriptor Desc(int k, WGPUShaderModule& sm, WGPUColorTargetState& ct, WGPUFragmentState& fs)
    {
        // A distinct module per variant so nothing is served from a cache.
        sm = MakeShader(std::string(kFullscreenVs) + Fmt("@fragment fn fs() -> @location(0) vec4f { return vec4f(%f, %f, 0.5, 1.0); }\n",
                                                          (k % 97) / 97.0, (k % 13) / 13.0),
                        "variant");
        ct = WGPU_COLOR_TARGET_STATE_INIT;
        ct.format = WGPUTextureFormat_RGBA8Unorm;
        fs = WGPU_FRAGMENT_STATE_INIT;
        fs.module = sm;
        fs.entryPoint = SV("fs");
        fs.targetCount = 1;
        fs.targets = &ct;
        WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        pd.vertex.module = sm;
        pd.vertex.entryPoint = SV("vs");
        pd.fragment = &fs;
        return pd;
    }

    void Start() override
    {
        target = wgpuTextureCreateView(MakeTexture(4, 4, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_RenderAttachment, "pipe target"), nullptr);
        async_pipes.assign(kCount, nullptr);
        frame_start = g.frame;
        t_start = NowMs();
        for (int k = 0; k < kCount; ++k)
        {
            WGPUShaderModule sm;
            WGPUColorTargetState ct;
            WGPUFragmentState fs;
            WGPURenderPipelineDescriptor pd = Desc(k, sm, ct, fs);
            WGPUCreateRenderPipelineAsyncCallbackInfo cb = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
            cb.mode = WGPUCallbackMode_AllowSpontaneous;
            cb.callback = [](WGPUCreatePipelineAsyncStatus st, WGPURenderPipeline p, WGPUStringView msg, void* u1, void* u2) {
                auto* self = static_cast<PipelineTest*>(u1);
                const int k = int(reinterpret_cast<intptr_t>(u2));
                if (st == WGPUCreatePipelineAsyncStatus_Success)
                    self->async_pipes[k] = p;
                else
                {
                    ++self->failed;
                    Log("createRenderPipelineAsync %d failed: %s", k, Str(msg).c_str());
                }
                ++self->done;
                self->t_last = NowMs();
            };
            cb.userdata1 = this;
            cb.userdata2 = reinterpret_cast<void*>(intptr_t(k));
            wgpuDeviceCreateRenderPipelineAsync(g.device, &pd, cb);
            wgpuShaderModuleRelease(sm);
        }
        t_issued = NowMs();
    }

    // Draws once with every pipeline and times submit -> onSubmittedWorkDone.
    void FirstUse(const std::vector<WGPURenderPipeline>& pipes, double* out)
    {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = target;
        ca.loadOp = WGPULoadOp_Clear;
        ca.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
        for (WGPURenderPipeline p : pipes)
            if (p)
            {
                wgpuRenderPassEncoderSetPipeline(pass, p);
                wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
            }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        first_use_t0 = NowMs();
        Submit(enc);
        struct Ctx
        {
            PipelineTest* self;
            double* out;
        };
        WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* u1, void* u2) {
            auto* self = static_cast<PipelineTest*>(u1);
            *static_cast<double*>(u2) = NowMs() - self->first_use_t0;
        };
        cb.userdata1 = this;
        cb.userdata2 = out;
        wgpuQueueOnSubmittedWorkDone(g.queue, cb);
    }

    bool Step() override
    {
        if (stage == 0 && done == kCount)
        {
            stage = 1;
            FirstUse(async_pipes, &async_first_use_ms);
        }
        else if (stage == 1 && async_first_use_ms >= 0)
        {
            // Same number of new variants, created synchronously.
            const double t0 = NowMs();
            for (int k = 0; k < kCount; ++k)
            {
                WGPUShaderModule sm;
                WGPUColorTargetState ct;
                WGPUFragmentState fs;
                WGPURenderPipelineDescriptor pd = Desc(kCount + k, sm, ct, fs);
                sync_pipes.push_back(wgpuDeviceCreateRenderPipeline(g.device, &pd));
                wgpuShaderModuleRelease(sm);
            }
            sync_call_ms = NowMs() - t0;
            FirstUse(sync_pipes, &sync_first_use_ms);
            stage = 2;
        }
        else if (stage == 2 && sync_first_use_ms >= 0)
        {
            const double total = t_last - t_start;
            const uint64_t frames = g.frame - frame_start;
            Report("gpu.pipeline_async", failed == 0 ? "pass" : "fail",
                   Fmt("%d pipelines: async issue %.1f ms, all callbacks after %.0f ms; first use %.1f ms. Synchronous: %.1f ms of calls, first use %.1f ms",
                       kCount, t_issued - t_start, total, async_first_use_ms, sync_call_ms, sync_first_use_ms),
                   Json()
                       .Int("count", kCount)
                       .Int("failed", failed)
                       .Num("async_issue_ms", t_issued - t_start)
                       .Num("async_all_done_ms", total)
                       .Int("frames_elapsed", int64_t(frames))
                       .Num("async_first_use_ms", async_first_use_ms)
                       .Num("sync_create_calls_ms", sync_call_ms)
                       .Num("sync_first_use_ms", sync_first_use_ms)
                       .Done());
            for (WGPURenderPipeline p : async_pipes)
                if (p)
                    wgpuRenderPipelineRelease(p);
            for (WGPURenderPipeline p : sync_pipes)
                wgpuRenderPipelineRelease(p);
            return true;
        }
        return false;
    }
};

// Dynamic buffers shaped like the engine's: per draw a 32-byte constant block in a 256-byte slot and
// six vertices, filled on the CPU into rings and uploaded with one writeBuffer per ring per frame.
struct UploadTest : Test
{
    struct Phase
    {
        int draws;
        bool per_draw_writes;
        int frames;
    };
    std::vector<Phase> phases = {{1000, false, 70}, {2000, false, 70}, {4000, false, 70}, {8000, false, 70}, {4000, true, 70}};
    static constexpr int kMaxDraws = 8192;
    static constexpr int kVertexBytes = 6 * 16;
    size_t phase = 0;
    int frame_in_phase = 0;
    double last_frame_ms = 0;
    std::vector<uint8_t> ubo_ring, vbo_ring;
    WGPUBuffer ubo = nullptr, vbo = nullptr;
    WGPUBindGroup bg = nullptr;
    WGPURenderPipeline pipe = nullptr;
    WGPUTextureView target = nullptr;
    std::vector<double> cpu_ms, fill_ms, interval_ms;
    std::string results = "[";
    std::string summary;

    void Start() override
    {
        ubo_ring.assign(size_t(kMaxDraws) * 256, 0);
        vbo_ring.assign(size_t(kMaxDraws) * kVertexBytes, 0);
        ubo = MakeBuffer(ubo_ring.size(), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "upload ubo");
        vbo = MakeBuffer(vbo_ring.size(), WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst, "upload vbo");
        const char* wgsl = R"(
struct U { place: vec4f, color: vec4f };
@group(0) @binding(0) var<uniform> u: U;
struct VO { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@location(0) v: vec4f) -> VO {
  var o: VO;
  o.pos = vec4f(v.xy * u.place.zw + u.place.xy, 0.0, 1.0);
  o.uv = v.zw;
  return o;
}
@fragment fn fs(i: VO) -> @location(0) vec4f { return u.color * vec4f(i.uv, 1.0, 1.0); }
)";
        WGPUShaderModule sm = MakeShader(wgsl, "upload");
        WGPUBindGroupLayoutEntry le = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        le.binding = 0;
        le.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        le.buffer.type = WGPUBufferBindingType_Uniform;
        le.buffer.hasDynamicOffset = true;
        le.buffer.minBindingSize = 32;
        WGPUBindGroupLayoutDescriptor bld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        bld.entryCount = 1;
        bld.entries = &le;
        WGPUBindGroupLayout bgl = wgpuDeviceCreateBindGroupLayout(g.device, &bld);
        WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &bgl;
        WGPUVertexAttribute attr = WGPU_VERTEX_ATTRIBUTE_INIT;
        attr.format = WGPUVertexFormat_Float32x4;
        attr.shaderLocation = 0;
        WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        vbl.arrayStride = 16;
        vbl.attributeCount = 1;
        vbl.attributes = &attr;
        WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
        ct.format = WGPUTextureFormat_RGBA8Unorm;
        WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
        fs.module = sm;
        fs.entryPoint = SV("fs");
        fs.targetCount = 1;
        fs.targets = &ct;
        WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        pd.layout = wgpuDeviceCreatePipelineLayout(g.device, &pld);
        pd.vertex.module = sm;
        pd.vertex.entryPoint = SV("vs");
        pd.vertex.bufferCount = 1;
        pd.vertex.buffers = &vbl;
        pd.fragment = &fs;
        pipe = wgpuDeviceCreateRenderPipeline(g.device, &pd);
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = 0;
        e.buffer = ubo;
        e.size = 32;
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = bgl;
        bgd.entryCount = 1;
        bgd.entries = &e;
        bg = wgpuDeviceCreateBindGroup(g.device, &bgd);
        target = wgpuTextureCreateView(MakeTexture(512, 512, WGPUTextureFormat_RGBA8Unorm, WGPUTextureUsage_RenderAttachment, "upload target"), nullptr);
    }

    void RunFrame(const Phase& ph)
    {
        const double t0 = NowMs();
        const float t = float(g.frame % 1000) * 0.01f;
        for (int i = 0; i < ph.draws; ++i)
        {
            float* u = reinterpret_cast<float*>(&ubo_ring[size_t(i) * 256]);
            u[0] = -0.95f + 1.9f * float(i % 97) / 97.0f + 0.01f * std::sin(t + i);
            u[1] = -0.95f + 1.9f * float((i / 97) % 89) / 89.0f;
            u[2] = 0.01f;
            u[3] = 0.01f;
            u[4] = float(i & 255) / 255.0f;
            u[5] = float((i >> 3) & 255) / 255.0f;
            u[6] = 0.5f;
            u[7] = 1.0f;
            float* v = reinterpret_cast<float*>(&vbo_ring[size_t(i) * kVertexBytes]);
            const float quad[6][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {-1, 1, 0, 1}, {-1, 1, 0, 1}, {1, -1, 1, 0}, {1, 1, 1, 1}};
            std::memcpy(v, quad, sizeof(quad));
        }
        const double t1 = NowMs();
        if (!ph.per_draw_writes)
        {
            wgpuQueueWriteBuffer(g.queue, ubo, 0, ubo_ring.data(), size_t(ph.draws) * 256);
            wgpuQueueWriteBuffer(g.queue, vbo, 0, vbo_ring.data(), size_t(ph.draws) * kVertexBytes);
        }
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = target;
        ca.loadOp = WGPULoadOp_Clear;
        ca.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
        wgpuRenderPassEncoderSetPipeline(pass, pipe);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vbo, 0, WGPU_WHOLE_SIZE);
        for (int i = 0; i < ph.draws; ++i)
        {
            if (ph.per_draw_writes)
            {
                wgpuQueueWriteBuffer(g.queue, ubo, uint64_t(i) * 256, &ubo_ring[size_t(i) * 256], 32);
                wgpuQueueWriteBuffer(g.queue, vbo, uint64_t(i) * kVertexBytes, &vbo_ring[size_t(i) * kVertexBytes], kVertexBytes);
            }
            const uint32_t offset = uint32_t(i) * 256;
            wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 1, &offset);
            wgpuRenderPassEncoderDraw(pass, 6, 1, uint32_t(i) * 6, 0);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        Submit(enc);
        const double t2 = NowMs();
        fill_ms.push_back(t1 - t0);
        cpu_ms.push_back(t2 - t0);
    }

    bool Step() override
    {
        const double now = NowMs();
        const Phase& ph = phases[phase];
        if (frame_in_phase >= 10) // the first frames warm up and are not counted
        {
            interval_ms.push_back(now - last_frame_ms);
            RunFrame(ph);
        }
        else
        {
            std::vector<double> discard;
            RunFrame(ph);
            cpu_ms.clear();
            fill_ms.clear();
        }
        last_frame_ms = now;
        if (++frame_in_phase < ph.frames)
            return false;

        const Stats c = Summarize(cpu_ms), f = Summarize(fill_ms), iv = Summarize(interval_ms);
        if (results.size() > 1)
            results += ",";
        results += Json()
                       .Int("draws", ph.draws)
                       .Str("upload", ph.per_draw_writes ? "writeBuffer per draw" : "one writeBuffer per ring per frame")
                       .Raw("cpu_ms", StatsJson(c))
                       .Raw("fill_ms", StatsJson(f))
                       .Raw("frame_interval_ms", StatsJson(iv))
                       .Done();
        summary += Fmt("%s%d draws%s: %.2f ms (p95 %.2f)", summary.empty() ? "" : "; ", ph.draws, ph.per_draw_writes ? " per-draw writes" : "",
                       c.median, c.p95);
        Log("upload %d draws %s: cpu median %.2f ms p95 %.2f, fill %.2f ms, frame interval %.1f ms", ph.draws,
            ph.per_draw_writes ? "per-draw" : "batched", c.median, c.p95, f.median, iv.median);
        cpu_ms.clear();
        fill_ms.clear();
        interval_ms.clear();
        frame_in_phase = 0;
        if (++phase < phases.size())
            return false;
        Report("gpu.upload", "pass", "CPU ms per frame (fill + encode + submit), median: " + summary, results + "]");
        return true;
    }
};

// Does a frame reach the screen while the worker is stuck in a long call? The worker shows magenta,
// then submits cyan and blocks; while blocked it submits yellow, green, blue and white. The page
// samples the canvas and counts its own animation frames during the block.
struct BlockTest : Test
{
    int block_ms;
    int stage = 0;
    uint64_t f0 = 0;
    explicit BlockTest(int ms) : block_ms(ms) {}
    bool OwnsCanvas() const override { return stage < 3; }
    bool Step() override
    {
        if (stage == 0)
        {
            ClearSurface(1, 0, 1);
            MAIN_THREAD_ASYNC_EM_ASM({ if (globalThis.labBlockArm) globalThis.labBlockArm(); });
            f0 = g.frame;
            stage = 1;
        }
        else if (stage == 1)
        {
            ClearSurface(1, 0, 1);
            if (g.frame - f0 >= 30)
                stage = 2;
        }
        else if (stage == 2)
        {
            ClearSurface(0, 1, 1);
            const double start = EpochMs();
            MAIN_THREAD_ASYNC_EM_ASM({ if (globalThis.labBlockStart) globalThis.labBlockStart($0, $1); }, start, block_ms);
            const double colors[4][3] = {{1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}};
            for (int i = 0; i < 4; ++i)
            {
                emscripten_thread_sleep(block_ms / 4.0); // Atomics.wait: the worker is blocked
                ClearSurface(colors[i][0], colors[i][1], colors[i][2]);
            }
            const double end = EpochMs();
            MAIN_THREAD_ASYNC_EM_ASM({ if (globalThis.labBlockEnd) globalThis.labBlockEnd($0, $1); }, start, end);
            f0 = g.frame;
            stage = 3;
        }
        else if (stage == 3)
            return g.frame - f0 > 30;
        return false;
    }
};

// ---- Scene -------------------------------------------------------------------------------------

void CreateScene()
{
    const bool rg11 = HasFeature(WGPUFeatureName_RG11B10UfloatRenderable);
    const WGPUTextureFormat formats[4] = {WGPUTextureFormat_RGBA16Float, WGPUTextureFormat_RGBA16Float, WGPUTextureFormat_RGBA8Unorm,
                                          rg11 ? WGPUTextureFormat_RG11B10Ufloat : WGPUTextureFormat_RGBA16Float};
    for (int i = 0; i < 4; ++i)
    {
        g.gbuf_format[i] = formats[i];
        g.gbuf[i] = MakeTexture(g.width, g.height, formats[i], WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding, "gbuffer");
        g.gbuf_view[i] = wgpuTextureCreateView(g.gbuf[i], nullptr);
    }
    std::string gbuf_wgsl = std::string(kFullscreenVs) +
                            "struct G { @location(0) a: vec4f, @location(1) b: vec4f, @location(2) c: vec4f, @location(3) d: vec4f };\n"
                            "@fragment fn fs(in: VOut) -> G {\n  var o: G;\n";
    for (int i = 0; i < 4; ++i)
        gbuf_wgsl += Fmt("  o.%c = vec4f(%f, %f, %f, 1.0);\n", 'a' + i, kGbufValues[i][0], kGbufValues[i][1], kGbufValues[i][2]);
    gbuf_wgsl += "  return o;\n}\n";
    WGPUShaderModule gsm = MakeShader(gbuf_wgsl, "gbuffer");
    WGPUColorTargetState cts[4];
    for (int i = 0; i < 4; ++i)
    {
        cts[i] = WGPU_COLOR_TARGET_STATE_INIT;
        cts[i].format = formats[i];
    }
    WGPUFragmentState gfs = WGPU_FRAGMENT_STATE_INIT;
    gfs.module = gsm;
    gfs.entryPoint = SV("fs");
    gfs.targetCount = 4;
    gfs.targets = cts;
    WGPURenderPipelineDescriptor gpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    gpd.vertex.module = gsm;
    gpd.vertex.entryPoint = SV("vs");
    gpd.fragment = &gfs;
    g.gbuf_pipeline = wgpuDeviceCreateRenderPipeline(g.device, &gpd);

    const std::string comp_wgsl = std::string(kFullscreenVs) + R"(
@group(0) @binding(0) var t0: texture_2d<f32>;
@group(0) @binding(1) var t1: texture_2d<f32>;
@group(0) @binding(2) var t2: texture_2d<f32>;
@group(0) @binding(3) var t3: texture_2d<f32>;
@group(0) @binding(4) var<uniform> u: vec4f; // time, width, height
@fragment fn fs(@builtin(position) p: vec4f) -> @location(0) vec4f {
  let ip = vec2i(p.xy);
  let w = i32(u.y);
  let h = i32(u.z);
  var c: vec3f;
  if (ip.y < h / 2) {
    if (ip.x < w / 2) { c = textureLoad(t0, ip, 0).rgb; } else { c = textureLoad(t1, ip, 0).rgb; }
  } else {
    if (ip.x < w / 2) { c = textureLoad(t2, ip, 0).rgb; } else { c = textureLoad(t3, ip, 0).rgb; }
  }
  if (ip.y >= h - 24) {
    let x = fract(u.x * 0.25) * f32(w);
    c = select(vec3f(0.05), vec3f(1.0), abs(f32(ip.x) - x) < 40.0);
  }
  return vec4f(c, 1.0);
}
)";
    WGPUShaderModule csm = MakeShader(comp_wgsl, "composite");
    WGPUColorTargetState cct = WGPU_COLOR_TARGET_STATE_INIT;
    cct.format = g.surface_format;
    WGPUFragmentState cfs = WGPU_FRAGMENT_STATE_INIT;
    cfs.module = csm;
    cfs.entryPoint = SV("fs");
    cfs.targetCount = 1;
    cfs.targets = &cct;
    WGPURenderPipelineDescriptor cpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    cpd.vertex.module = csm;
    cpd.vertex.entryPoint = SV("vs");
    cpd.fragment = &cfs;
    g.comp_pipeline = wgpuDeviceCreateRenderPipeline(g.device, &cpd);

    g.comp_ubo = MakeBuffer(16, WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst, "composite ubo");
    WGPUBindGroupEntry e[5];
    for (int i = 0; i < 5; ++i)
    {
        e[i] = WGPU_BIND_GROUP_ENTRY_INIT;
        e[i].binding = uint32_t(i);
        if (i < 4)
            e[i].textureView = g.gbuf_view[i];
    }
    e[4].buffer = g.comp_ubo;
    e[4].size = 16;
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = wgpuRenderPipelineGetBindGroupLayout(g.comp_pipeline, 0);
    bgd.entryCount = 5;
    bgd.entries = e;
    g.comp_bg = wgpuDeviceCreateBindGroup(g.device, &bgd);
}

void RenderScene(Test* test)
{
    WGPUTexture tex;
    WGPUTextureView view;
    if (!AcquireSurface(&tex, &view))
    {
        ++g.errors;
        g.last_error = "getCurrentTexture failed";
        return;
    }
    const float u[4] = {float((NowMs() - g.start_ms) / 1000.0), float(g.width), float(g.height), 0};
    wgpuQueueWriteBuffer(g.queue, g.comp_ubo, 0, u, sizeof(u));
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);

    WGPURenderPassColorAttachment ca[4];
    for (int i = 0; i < 4; ++i)
    {
        ca[i] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca[i].view = g.gbuf_view[i];
        ca[i].loadOp = WGPULoadOp_Clear;
        ca[i].storeOp = WGPUStoreOp_Store;
        ca[i].clearValue = {0, 0, 0, 1};
    }
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 4;
    rp.colorAttachments = ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, g.gbuf_pipeline);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPURenderPassColorAttachment sca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    sca.view = view;
    sca.loadOp = WGPULoadOp_Clear;
    sca.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor srp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    srp.colorAttachmentCount = 1;
    srp.colorAttachments = &sca;
    pass = wgpuCommandEncoderBeginRenderPass(enc, &srp);
    wgpuRenderPassEncoderSetPipeline(pass, g.comp_pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, g.comp_bg, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    if (test)
        test->AfterScene(enc, tex);
    Submit(enc);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(tex);
}

// ---- Setup -------------------------------------------------------------------------------------

void ReportAdapter()
{
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    wgpuAdapterGetInfo(g.adapter, &info);
    WGPUSupportedFeatures sf = WGPU_SUPPORTED_FEATURES_INIT;
    wgpuAdapterGetFeatures(g.adapter, &sf);
    WGPULimits lim = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(g.adapter, &lim);

    std::string all = "[", wanted_json = "{", missing;
    for (size_t i = 0; i < sf.featureCount; ++i)
    {
        const char* n = FeatureName(sf.features[i]);
        all += Fmt("%s\"%s\"", i ? "," : "", n ? n : Fmt("0x%x", unsigned(sf.features[i])).c_str());
    }
    all += "]";
    int wanted_have = 0, wanted_total = 0;
    for (const FeatureInfo& f : kFeatures)
    {
        if (!f.wanted)
            continue;
        bool have = false;
        for (size_t i = 0; i < sf.featureCount; ++i)
            have = have || sf.features[i] == f.id;
        wanted_json += Fmt("%s\"%s\":%s", wanted_total ? "," : "", f.name, have ? "true" : "false");
        ++wanted_total;
        if (have)
        {
            ++wanted_have;
            g.device_features.push_back(f.id);
        }
        else
            missing += Fmt("%s%s", missing.empty() ? "" : ", ", f.name);
    }
    wanted_json += "}";
    const std::string limits = Json()
                                   .Int("maxTextureDimension2D", lim.maxTextureDimension2D)
                                   .Int("maxTextureArrayLayers", lim.maxTextureArrayLayers)
                                   .Int("maxBindGroups", lim.maxBindGroups)
                                   .Int("maxColorAttachments", lim.maxColorAttachments)
                                   .Int("maxColorAttachmentBytesPerSample", lim.maxColorAttachmentBytesPerSample)
                                   .Int("maxSampledTexturesPerShaderStage", lim.maxSampledTexturesPerShaderStage)
                                   .Int("maxSamplersPerShaderStage", lim.maxSamplersPerShaderStage)
                                   .Int("maxUniformBuffersPerShaderStage", lim.maxUniformBuffersPerShaderStage)
                                   .Int("maxDynamicUniformBuffersPerPipelineLayout", lim.maxDynamicUniformBuffersPerPipelineLayout)
                                   .Int("maxUniformBufferBindingSize", int64_t(lim.maxUniformBufferBindingSize))
                                   .Int("maxStorageBufferBindingSize", int64_t(lim.maxStorageBufferBindingSize))
                                   .Int("maxBufferSize", int64_t(lim.maxBufferSize))
                                   .Int("minUniformBufferOffsetAlignment", lim.minUniformBufferOffsetAlignment)
                                   .Int("maxVertexBuffers", lim.maxVertexBuffers)
                                   .Int("maxVertexAttributes", lim.maxVertexAttributes)
                                   .Int("maxInterStageShaderVariables", lim.maxInterStageShaderVariables)
                                   .Int("maxComputeWorkgroupStorageSize", lim.maxComputeWorkgroupStorageSize)
                                   .Int("maxComputeInvocationsPerWorkgroup", lim.maxComputeInvocationsPerWorkgroup)
                                   .Done();
    const std::string desc = Fmt("%s %s %s (%s)", Str(info.vendor).c_str(), Str(info.architecture).c_str(), Str(info.device).c_str(),
                                 Str(info.description).c_str());
    Report("gpu.adapter", wanted_have == wanted_total ? "pass" : "info",
           Fmt("%d of %d listed features present%s%s; adapter %s", wanted_have, wanted_total, missing.empty() ? "" : "; missing: ",
               missing.c_str(), desc.c_str()),
           Json().Str("adapter", desc).Raw("wanted", wanted_json).Raw("all_features", all).Raw("limits", limits).Done());
    wgpuSupportedFeaturesFreeMembers(sf);
    wgpuAdapterInfoFreeMembers(info);
}

void OnDevice(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void*, void*)
{
    if (status != WGPURequestDeviceStatus_Success)
    {
        g.phase = State::Phase::Failed;
        Report("gpu.worker", "fail", "requestDevice failed: " + Str(message));
        return;
    }
    g.device = device;
    g.queue = wgpuDeviceGetQueue(device);

    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    wgpuSurfaceGetCapabilities(g.surface, g.adapter, &caps);
    if (caps.formatCount > 0)
        g.surface_format = caps.formats[0];
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
    cfg.device = device;
    cfg.format = g.surface_format;
    cfg.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    cfg.width = uint32_t(g.width);
    cfg.height = uint32_t(g.height);
    cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
    cfg.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(g.surface, &cfg);

    CreateScene();
    g.tests.emplace_back(new MrtTest);
    g.tests.emplace_back(new BcTest);
    g.tests.emplace_back(new OcclusionTest);
    g.tests.emplace_back(new PipelineTest);
    g.tests.emplace_back(new UploadTest);
    g.tests.emplace_back(new BlockTest(1600));
    g.phase = State::Phase::Ready;
    g.start_ms = NowMs();
    Report("gpu.worker", "pass",
           Fmt("webgpu.h device created on a worker thread (main browser thread: %s); drawing to %s as %s %dx%d",
               emscripten_is_main_browser_thread() ? "yes" : "no", g.canvas.c_str(), FormatName(g.surface_format), g.width, g.height),
           Json().Bool("on_main_browser_thread", emscripten_is_main_browser_thread()).Str("surface_format", FormatName(g.surface_format)).Done());
}

void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void*, void*)
{
    if (status != WGPURequestAdapterStatus_Success)
    {
        g.phase = State::Phase::Failed;
        Report("gpu.worker", "fail", "requestAdapter failed: " + Str(message));
        return;
    }
    g.adapter = adapter;
    ReportAdapter();

    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.requiredFeatureCount = g.device_features.size();
    dd.requiredFeatures = g.device_features.data();
    dd.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    dd.deviceLostCallbackInfo.callback = [](WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView msg, void*, void*) {
        if (reason != WGPUDeviceLostReason_Destroyed)
            Report("gpu.device_lost", "fail", "device lost: " + Str(msg));
    };
    dd.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType type, WGPUStringView msg, void*, void*) {
        ++g.errors;
        g.last_error = Str(msg);
        if (g.errors <= 10)
            Log("WebGPU error %d: %s", int(type), g.last_error.c_str());
    };
    WGPURequestDeviceCallbackInfo cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = OnDevice;
    g.phase = State::Phase::WaitDevice;
    wgpuAdapterRequestDevice(adapter, &dd, cb);
}
} // namespace

void Init(const char* canvas_selector, int width, int height)
{
    g.canvas = canvas_selector;
    g.width = width;
    g.height = height;
    g.instance = wgpuCreateInstance(nullptr);
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector src = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    src.selector = SV(canvas_selector);
    WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
    sd.nextInChain = &src.chain;
    g.surface = wgpuInstanceCreateSurface(g.instance, &sd);
    if (!g.surface)
    {
        g.phase = State::Phase::Failed;
        Report("gpu.worker", "fail", Fmt("no WebGPU context on %s from the worker", canvas_selector));
        return;
    }
    WGPURequestAdapterOptions opts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    opts.powerPreference = WGPUPowerPreference_HighPerformance;
    opts.compatibleSurface = g.surface;
    WGPURequestAdapterCallbackInfo cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = OnAdapter;
    g.phase = State::Phase::WaitAdapter;
    wgpuInstanceRequestAdapter(g.instance, &opts, cb);
}

void RequestBlockTest(int block_ms) { g.pending_block_ms = block_ms; }

bool Done() { return g.phase == State::Phase::Failed || (g.phase == State::Phase::Ready && g.test_index >= g.tests.size()); }

void Frame()
{
    if (g.instance)
        wgpuInstanceProcessEvents(g.instance);
    if (g.phase != State::Phase::Ready)
        return;
    ++g.frame;

    if (g.pending_block_ms > 0 && g.test_index >= g.tests.size())
    {
        g.tests.emplace_back(new BlockTest(g.pending_block_ms));
        g.pending_block_ms = 0;
    }
    Test* test = g.test_index < g.tests.size() ? g.tests[g.test_index].get() : nullptr;
    if (test && !g.test_started)
    {
        test->Start();
        g.test_started = true;
    }
    if (!test || !test->OwnsCanvas())
        RenderScene(test);
    if (test && test->Step())
    {
        ++g.test_index;
        g.test_started = false;
    }
}
} // namespace lab::gpu
