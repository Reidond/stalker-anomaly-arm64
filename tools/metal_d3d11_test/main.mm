// metal_d3d11_test: exercises the D3D11-on-Metal shim through the D3D11 API, the way R4 uses it.
//
// Scene (deferred, like R4): a G-buffer pass (MRT + depth) draws textured, depth-tested cubes and a ground
// plane; a lighting pass samples the G-buffer and the depth buffer and accumulates one directional and two
// point lights with additive blending into an HDR render target; a combine pass tone-maps into the back
// buffer; alpha-blended HUD quads, streamed through a dynamic vertex buffer, are depth-tested against the
// G-buffer depth. A row of swatches at the bottom checks individual features with known colours: blending,
// point-sampled textures, an offscreen RT cleared and sampled, MSAA resolve, depth test, compute/UAV,
// staging copy, dynamic texture upload and BC1.
//
// Usage: metal_d3d11_test [--frames N] [--screenshot path.png]
// Exit code 0 when every swatch matches.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/CAMetalLayer.h>

#import <Metal/Metal.h>
#include <TargetConditionals.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <d3dx11.h>
#include <mtl_d3d11/mtl_d3d11.h>

using namespace mtl_d3d11;

namespace
{
// ---------------------------------------------------------------------------------------------------------
// Math (column-major, column-vector convention, left-handed, depth 0..1: what the MSL expects)
// ---------------------------------------------------------------------------------------------------------
struct Mat4
{
    float m[16]; // m[col * 4 + row]
};
Mat4 identity()
{
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}
Mat4 mul(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
Mat4 translate(float x, float y, float z)
{
    Mat4 r = identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}
Mat4 scale(float x, float y, float z)
{
    Mat4 r = identity();
    r.m[0] = x; r.m[5] = y; r.m[10] = z;
    return r;
}
Mat4 rotateY(float a)
{
    Mat4 r = identity();
    r.m[0] = cosf(a); r.m[8] = sinf(a); r.m[2] = -sinf(a); r.m[10] = cosf(a);
    return r;
}
Mat4 perspective(float fovY, float aspect, float zn, float zf)
{
    const float f = 1.0f / tanf(fovY * 0.5f);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = zf / (zf - zn);
    r.m[11] = 1;
    r.m[14] = -zn * zf / (zf - zn);
    return r;
}
Mat4 lookAt(const float e[3], const float t[3])
{
    float z[3] = {t[0] - e[0], t[1] - e[1], t[2] - e[2]};
    float zl = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (float& v : z) v /= zl;
    const float up[3] = {0, 1, 0};
    float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
    float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (float& v : x) v /= xl;
    float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    Mat4 r = identity();
    r.m[0] = x[0]; r.m[4] = x[1]; r.m[8] = x[2];
    r.m[1] = y[0]; r.m[5] = y[1]; r.m[9] = y[2];
    r.m[2] = z[0]; r.m[6] = z[1]; r.m[10] = z[2];
    r.m[12] = -(x[0] * e[0] + x[1] * e[1] + x[2] * e[2]);
    r.m[13] = -(y[0] * e[0] + y[1] * e[1] + y[2] * e[2]);
    r.m[14] = -(z[0] * e[0] + z[1] * e[1] + z[2] * e[2]);
    return r;
}
Mat4 inverse(const Mat4& mat)
{
    const float* m = mat.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r{};
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i)
        r.m[i] = inv[i] * det;
    return r;
}

// ---------------------------------------------------------------------------------------------------------
// Shaders (hand-written MSL; the real pipeline gets these from the HLSL translator)
// ---------------------------------------------------------------------------------------------------------
const char* kMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GVIn  { float3 pos [[attribute(0)]]; float3 nrm [[attribute(1)]]; float2 uv [[attribute(2)]]; };
struct GVOut { float4 pos [[position]]; float3 nrm; float2 uv; };
struct ObjVS { float4x4 mvp; float4x4 model; };
vertex GVOut gbuffer_vs(GVIn in [[stage_in]], constant ObjVS& cb [[buffer(0)]])
{
    GVOut o;
    o.pos = cb.mvp * float4(in.pos, 1.0);
    o.nrm = (cb.model * float4(in.nrm, 0.0)).xyz;
    o.uv = in.uv;
    return o;
}
struct ObjPS { float4 tint; float4 uvScale; };
struct GBufOut { float4 albedo [[color(0)]]; float4 normal [[color(1)]]; };
fragment GBufOut gbuffer_ps(GVOut in [[stage_in]], constant ObjPS& cb [[buffer(0)]],
                            texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
    GBufOut o;
    float4 c = tex.sample(smp, in.uv * cb.uvScale.xy);
    o.albedo = float4(c.rgb * cb.tint.rgb, 1.0);
    o.normal = float4(normalize(in.nrm), 1.0);
    return o;
}

struct QOut { float4 pos [[position]]; float2 uv; };
vertex QOut quad_vs(uint vid [[vertex_id]])
{
    float2 p = float2((vid << 1) & 2, vid & 2);
    QOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

struct LightCB { float4x4 invViewProj; float4 posRadius; float4 color; float4 dir; float4 params; };
fragment float4 light_ps(QOut in [[stage_in]], constant LightCB& cb [[buffer(0)]],
                         texture2d<float> albedoT [[texture(0)]], texture2d<float> normalT [[texture(1)]],
                         texture2d<float> depthT [[texture(2)]])
{
    uint2 pix = uint2(in.pos.xy);
    float d = depthT.read(pix).x;
    if (d >= 1.0)
        return cb.params.x < 0.5 ? float4(mix(float3(0.10, 0.14, 0.24), float3(0.35, 0.40, 0.50), in.uv.y), 1.0)
                                 : float4(0.0);
    float3 albedo = albedoT.read(pix).rgb;
    float3 n = normalize(normalT.read(pix).xyz);
    float4 clip = float4(in.uv.x * 2.0 - 1.0, 1.0 - in.uv.y * 2.0, d, 1.0);
    float4 wp4 = cb.invViewProj * clip;
    float3 wp = wp4.xyz / wp4.w;
    float3 L;
    float att = 1.0;
    float amb = 0.0;
    if (cb.params.x < 0.5)
    {
        L = -normalize(cb.dir.xyz);
        amb = 0.10;
    }
    else
    {
        float3 dv = cb.posRadius.xyz - wp;
        float dist = length(dv);
        L = dv / max(dist, 1e-4);
        att = saturate(1.0 - dist / cb.posRadius.w);
        att *= att;
    }
    float ndl = saturate(dot(n, L));
    return float4(albedo * (cb.color.rgb * (ndl * att) + amb), 1.0);
}

struct CombineCB { float4 fog; float4 misc; };
fragment float4 combine_ps(QOut in [[stage_in]], constant CombineCB& cb [[buffer(0)]],
                           texture2d<float> sceneT [[texture(0)]], texture2d<float> depthT [[texture(1)]],
                           sampler lin [[sampler(0)]])
{
    float3 c = sceneT.sample(lin, in.uv).rgb;
    float d = depthT.read(uint2(in.pos.xy)).x;
    float n = cb.misc.y, f = cb.misc.z;
    float z = n * f / (f - d * (f - n));
    float fog = d < 1.0 ? 1.0 - exp(-z * cb.fog.w) : 0.0;
    c = mix(c, cb.fog.rgb, fog);
    c *= cb.misc.x;
    c = c / (1.0 + c);
    c = pow(c, float3(1.0 / 2.2));
    return float4(c, 1.0);
}

struct UIIn  { float3 pos [[attribute(0)]]; float4 col [[attribute(1)]]; float2 uv [[attribute(2)]]; };
struct UIOut { float4 pos [[position]]; float4 col; float2 uv; };
struct UICB  { float4 screen; };
vertex UIOut ui_vs(UIIn in [[stage_in]], constant UICB& cb [[buffer(0)]])
{
    UIOut o;
    o.pos = float4(in.pos.x * cb.screen.x - 1.0, 1.0 + in.pos.y * cb.screen.y, in.pos.z, 1.0);
    o.col = in.col;
    o.uv = in.uv;
    return o;
}
fragment float4 ui_ps(UIOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
    return in.col * t.sample(s, in.uv);
}

struct ProbeCB { float4 dir; };
fragment float4 cube_ps(UIOut in [[stage_in]], constant ProbeCB& cb [[buffer(0)]], texturecube<float> t [[texture(0)]],
                        sampler s [[sampler(0)]])
{
    return t.sample(s, cb.dir.xyz);
}
fragment float4 vol_ps(UIOut in [[stage_in]], constant ProbeCB& cb [[buffer(0)]], texture3d<float> t [[texture(0)]],
                       sampler s [[sampler(0)]])
{
    return t.sample(s, float3(0.5, 0.5, cb.dir.z));
}

kernel void fill_cs(texture2d<float, access::write> outT [[texture(64)]], constant float4& color [[buffer(0)]],
                    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= outT.get_width() || gid.y >= outT.get_height())
        return;
    outT.write(color, gid);
}
)MSL";

CBufferVariable var(const char* name, uint32_t off, uint32_t size, D3D_SHADER_VARIABLE_CLASS cls, uint32_t rows, uint32_t cols)
{
    CBufferVariable v;
    v.name = name; v.offset = off; v.size = size; v.cls = cls; v.type = D3D_SVT_FLOAT; v.rows = rows; v.columns = cols;
    return v;
}
CBuffer cbuf(const char* name, uint32_t slot, uint32_t size, std::vector<CBufferVariable> vars)
{
    CBuffer c;
    c.name = name; c.d3dSlot = slot; c.metalIndex = slot; c.sizeBytes = size; c.variables = std::move(vars);
    return c;
}
ResourceBinding texBind(const char* name, uint32_t slot)
{
    ResourceBinding r;
    r.name = name; r.type = D3D_SIT_TEXTURE; r.d3dSlot = slot; r.metalIndex = slot;
    return r;
}
ResourceBinding smpBind(const char* name, uint32_t slot)
{
    ResourceBinding r;
    r.name = name; r.type = D3D_SIT_SAMPLER; r.d3dSlot = slot; r.metalIndex = slot;
    return r;
}
VertexInput vin(const char* sem, uint32_t idx, uint32_t attr, uint32_t comps)
{
    VertexInput v;
    v.semantic = sem; v.semanticIndex = idx; v.attribute = attr; v.components = comps;
    return v;
}
std::vector<uint8_t> shaderBytes(ShaderStage st, const char* entry, ShaderReflection r)
{
    ShaderBlob b;
    r.stage = st;
    r.entryPoint = entry;
    b.reflection = std::move(r);
    b.kind = PayloadKind::MslSource;
    b.payload.assign(kMSL, kMSL + strlen(kMSL));
    return Serialize(b);
}

// ---------------------------------------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------------------------------------
struct Vtx { float p[3], n[3], uv[2]; };
struct UIVertex { float x, y, z; uint8_t rgba[4]; float u, v; };
static_assert(sizeof(Vtx) == 32 && sizeof(UIVertex) == 24, "vertex layouts");

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

struct RT
{
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    void release() { SAFE_RELEASE(srv); SAFE_RELEASE(rtv); SAFE_RELEASE(tex); }
};

struct Swatch
{
    const char* name;
    uint8_t expect[3];
    int tol;
};

struct App
{
    SDL_Window* window = nullptr;
    SDL_MetalView view = nullptr;
    int W = 0, H = 0;
    ID3D11Device1* dev = nullptr;
    ID3D11DeviceContext1* ctx = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11Texture2D* backTex = nullptr;
    ID3D11RenderTargetView* backRTV = nullptr;

    // shaders and layouts
    ID3D11VertexShader *gbufVS = nullptr, *quadVS = nullptr, *uiVS = nullptr;
    ID3D11PixelShader *gbufPS = nullptr, *lightPS = nullptr, *combinePS = nullptr, *uiPS = nullptr;
    ID3D11ComputeShader* fillCS = nullptr;
    ID3D11PixelShader *cubePS = nullptr, *volPS = nullptr;
    ID3D11Buffer* cbProbe = nullptr;
    ID3D11Texture2D *texCube = nullptr, *texUpdate = nullptr;
    ID3D11Texture3D* tex3D = nullptr;
    ID3D11ShaderResourceView *srvCube = nullptr, *srv3D = nullptr, *srvUpdate = nullptr;
    ID3D11InputLayout *sceneLayout = nullptr, *uiLayout = nullptr;
    // states
    ID3D11RasterizerState *rsCullBack = nullptr, *rsNoCull = nullptr;
    ID3D11DepthStencilState *dsDepth = nullptr, *dsNoDepth = nullptr, *dsTestOnly = nullptr;
    ID3D11BlendState *bsOpaque = nullptr, *bsAlpha = nullptr, *bsAdditive = nullptr;
    ID3D11SamplerState *smpLinear = nullptr, *smpPoint = nullptr, *smpAniso = nullptr;
    // geometry
    ID3D11Buffer *sceneVB = nullptr, *sceneIB = nullptr, *uiVB = nullptr;
    UINT uiPos = 0;
    static constexpr UINT kUiVBSize = 24 * 6 * 12; // 12 quads: wraps (DISCARD) several times per frame
    // constant buffers
    ID3D11Buffer *cbObjVS = nullptr, *cbObjPS = nullptr, *cbLight = nullptr, *cbCombine = nullptr, *cbUI = nullptr,
                 *cbFill = nullptr;
    // textures
    ID3D11Texture2D *texBrick = nullptr, *texGround = nullptr, *texChecker = nullptr, *texWhite = nullptr, *texBC1 = nullptr,
                    *texStageDst = nullptr, *texDynamic = nullptr, *texUAV = nullptr, *texCopy = nullptr;
    ID3D11ShaderResourceView *srvBrick = nullptr, *srvGround = nullptr, *srvChecker = nullptr, *srvWhite = nullptr,
                             *srvBC1 = nullptr, *srvStageDst = nullptr, *srvDynamic = nullptr, *srvUAV = nullptr,
                             *srvCopy = nullptr;
    ID3D11UnorderedAccessView* uavFill = nullptr;
    ID3D11Texture2D *texDDS = nullptr, *texDDSStaged = nullptr;
    ID3D11ShaderResourceView *srvDDS = nullptr, *srvDDSStaged = nullptr;
    // render targets
    RT albedo, normal, scene, small, resolved;
    ID3D11Texture2D *depthTex = nullptr, *msaaTex = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* depthSRV = nullptr;
    ID3D11RenderTargetView* msaaRTV = nullptr;
    // queries
    ID3D11Query *occHidden = nullptr, *occVisible = nullptr, *event = nullptr;
    bool eventPending = false;

    UINT planeBase = 0, planeFirstIndex = 0;
    int frame = 0, maxFrames = 120, captureFrame = 60;
    std::string screenshot;
    bool captureRequested = false;
    int failures = 0;
    uint64_t occHiddenSamples = ~0ull, occVisibleSamples = 0;
};

#define CHECK_HR(expr)                                                                                       \
    do                                                                                                       \
    {                                                                                                        \
        HRESULT _hr = (expr);                                                                                \
        if (FAILED(_hr))                                                                                     \
        {                                                                                                    \
            SDL_Log("FAILED %s -> 0x%08x (%s:%d)", #expr, (unsigned)_hr, __FILE__, __LINE__);                 \
            return false;                                                                                    \
        }                                                                                                    \
    } while (0)

std::string g_report;
void report(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void report(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SDL_Log("%s", buf);
    g_report += buf;
    g_report += "\n";
}

// ---- geometry --------------------------------------------------------------------------------------------
void buildCube(std::vector<Vtx>& v, std::vector<uint16_t>& idx)
{
    const float N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const float U[6][3] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    for (int f = 0; f < 6; ++f)
    {
        const float* n = N[f];
        const float* u = U[f];
        const float vv[3] = {n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0]};
        const uint16_t base = uint16_t(v.size());
        for (int iv = 0; iv < 2; ++iv)
            for (int iu = 0; iu < 2; ++iu)
            {
                Vtx x{};
                for (int k = 0; k < 3; ++k)
                {
                    x.p[k] = n[k] + u[k] * (iu * 2 - 1) + vv[k] * (iv * 2 - 1);
                    x.n[k] = n[k];
                }
                x.uv[0] = float(iu);
                x.uv[1] = 1.0f - float(iv);
                v.push_back(x);
            }
        // choose the winding whose geometric normal matches the face normal (left-handed, clockwise front)
        auto tri = [&](int a, int b, int c) {
            const Vtx &A = v[base + a], &B = v[base + b], &C = v[base + c];
            float e1[3] = {B.p[0] - A.p[0], B.p[1] - A.p[1], B.p[2] - A.p[2]};
            float e2[3] = {C.p[0] - A.p[0], C.p[1] - A.p[1], C.p[2] - A.p[2]};
            float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            if (cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] > 0)
            {
                idx.push_back(base + a); idx.push_back(base + b); idx.push_back(base + c);
            }
            else
            {
                idx.push_back(base + a); idx.push_back(base + c); idx.push_back(base + b);
            }
        };
        tri(0, 1, 2);
        tri(2, 1, 3);
    }
}

// ---- texture data -------------------------------------------------------------------------------------------
struct MipChain
{
    std::vector<std::vector<uint8_t>> mips;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
};
// RGBA8 pattern with a CPU box-filtered mip chain.
MipChain makePattern(int size, int cell, const uint8_t a[3], const uint8_t b[3], bool bricks)
{
    MipChain c;
    int n = 0;
    for (int s = size; s >= 1; s >>= 1) ++n;
    std::vector<uint8_t> cur(size_t(size) * size * 4);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            bool on = ((x / cell) + (y / cell)) & 1;
            if (bricks)
            {
                const int row = y / cell;
                on = ((x + (row & 1) * cell / 2) % (cell * 2)) < cell * 2 - 3 && (y % cell) > 1;
                on = !on;
            }
            const uint8_t* col = on ? a : b;
            uint8_t* p = &cur[(size_t(y) * size + x) * 4];
            p[0] = col[0]; p[1] = col[1]; p[2] = col[2]; p[3] = 255;
        }
    int s = size;
    for (int m = 0; m < n; ++m)
    {
        c.mips.push_back(cur);
        if (s > 1)
        {
            const int ns = s / 2;
            std::vector<uint8_t> next(size_t(ns) * ns * 4);
            for (int y = 0; y < ns; ++y)
                for (int x = 0; x < ns; ++x)
                    for (int k = 0; k < 4; ++k)
                        next[(size_t(y) * ns + x) * 4 + k] = uint8_t(
                            (cur[(size_t(2 * y) * s + 2 * x) * 4 + k] + cur[(size_t(2 * y) * s + 2 * x + 1) * 4 + k] +
                             cur[(size_t(2 * y + 1) * s + 2 * x) * 4 + k] + cur[(size_t(2 * y + 1) * s + 2 * x + 1) * 4 + k]) / 4);
            cur = std::move(next);
            s = ns;
        }
    }
    s = size;
    for (int m = 0; m < n; ++m, s = std::max(s / 2, 1))
        c.init.push_back({c.mips[m].data(), UINT(s * 4), 0});
    return c;
}

uint16_t rgb565(int r, int g, int b) { return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

// BC1 texture, 64x64 with a full mip chain: alternating 4x4 blocks of two colours.
MipChain makeBC1(int size, const uint8_t a[3], const uint8_t b[3])
{
    MipChain c;
    int s = size;
    std::vector<int> sizes;
    while (true)
    {
        sizes.push_back(s);
        if (s == 1) break;
        s /= 2;
    }
    for (int sz : sizes)
    {
        const int bw = std::max((sz + 3) / 4, 1);
        std::vector<uint8_t> data(size_t(bw) * bw * 8);
        for (int by = 0; by < bw; ++by)
            for (int bx = 0; bx < bw; ++bx)
            {
                const uint8_t* col = ((bx + by) & 1) ? a : b;
                uint16_t c0 = rgb565(col[0], col[1], col[2]);
                uint8_t* blk = &data[(size_t(by) * bw + bx) * 8];
                std::memcpy(blk, &c0, 2);
                std::memcpy(blk + 2, &c0, 2); // equal endpoints: every texel is colour 0
                std::memset(blk + 4, 0, 4);
            }
        c.mips.push_back(std::move(data));
    }
    for (size_t m = 0; m < sizes.size(); ++m)
        c.init.push_back({c.mips[m].data(), UINT(std::max((sizes[m] + 3) / 4, 1) * 8), 0});
    return c;
}

// A legacy-header DXT1 .dds file with a full mip chain of one colour (what Anomaly's textures look like).
std::vector<uint8_t> makeDDS(int size, const uint8_t col[3])
{
    MipChain c = makeBC1(size, col, col);
    std::vector<uint8_t> f(4 + 124, 0);
    auto put = [&](size_t off, uint32_t v) { std::memcpy(&f[off], &v, 4); };
    put(0, 0x20534444);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000);
    put(12, size);
    put(16, size);
    put(20, uint32_t(c.mips[0].size()));
    put(28, uint32_t(c.mips.size()));
    put(4 + 72, 32);                 // DDS_PIXELFORMAT.size
    put(4 + 76, 0x4);                // DDPF_FOURCC
    put(4 + 80, 0x31545844);         // 'DXT1'
    put(4 + 108, 0x1000 | 0x8 | 0x400000);
    for (auto& m : c.mips)
        f.insert(f.end(), m.begin(), m.end());
    return f;
}

// ---- device setup -------------------------------------------------------------------------------------------
bool createRT(App& a, RT& rt, int w, int h, DXGI_FORMAT fmt, UINT samples = 1, bool srv = true)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = samples;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | (srv ? D3D11_BIND_SHADER_RESOURCE : 0);
    CHECK_HR(a.dev->CreateTexture2D(&d, nullptr, &rt.tex));
    CHECK_HR(a.dev->CreateRenderTargetView(rt.tex, nullptr, &rt.rtv));
    if (srv)
        CHECK_HR(a.dev->CreateShaderResourceView(rt.tex, nullptr, &rt.srv));
    return true;
}

bool createBuffer(App& a, ID3D11Buffer** out, UINT size, D3D11_USAGE usage, UINT bind, const void* data = nullptr)
{
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size; d.Usage = usage; d.BindFlags = bind;
    d.CPUAccessFlags = usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : 0;
    D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    CHECK_HR(a.dev->CreateBuffer(&d, data ? &init : nullptr, out));
    return true;
}

bool createPipeline(App& a)
{
    ShaderReflection r;
    // G-buffer
    r = ShaderReflection();
    r.cbuffers = {cbuf("ObjVS", 0, 128, {var("mvp", 0, 64, D3D_SVC_MATRIX_ROWS, 4, 4), var("model", 64, 64, D3D_SVC_MATRIX_ROWS, 4, 4)})};
    r.inputs = {vin("POSITION", 0, 0, 3), vin("NORMAL", 0, 1, 3), vin("TEXCOORD", 0, 2, 2)};
    auto bytes = shaderBytes(ShaderStage::Vertex, "gbuffer_vs", r);
    CHECK_HR(a.dev->CreateVertexShader(bytes.data(), bytes.size(), nullptr, &a.gbufVS));
    // input layout built from the VS input signature, as R4 does
    ID3D10Blob* sig = nullptr;
    CHECK_HR(D3DGetInputSignatureBlob(bytes.data(), bytes.size(), &sig));
    D3D11_INPUT_ELEMENT_DESC scene[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    CHECK_HR(a.dev->CreateInputLayout(scene, 3, sig->GetBufferPointer(), sig->GetBufferSize(), &a.sceneLayout));
    sig->Release();

    r = ShaderReflection();
    r.cbuffers = {cbuf("ObjPS", 0, 32, {var("tint", 0, 16, D3D_SVC_VECTOR, 1, 4), var("uvScale", 16, 16, D3D_SVC_VECTOR, 1, 4)})};
    r.resources = {texBind("s_base", 0), smpBind("smp_base", 0)};
    r.colorOutputs = 2;
    bytes = shaderBytes(ShaderStage::Pixel, "gbuffer_ps", r);
    CHECK_HR(a.dev->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &a.gbufPS));

    r = ShaderReflection();
    bytes = shaderBytes(ShaderStage::Vertex, "quad_vs", r);
    CHECK_HR(a.dev->CreateVertexShader(bytes.data(), bytes.size(), nullptr, &a.quadVS));

    r = ShaderReflection();
    r.cbuffers = {cbuf("LightCB", 0, 128, {var("invViewProj", 0, 64, D3D_SVC_MATRIX_ROWS, 4, 4), var("posRadius", 64, 16, D3D_SVC_VECTOR, 1, 4),
                                           var("color", 80, 16, D3D_SVC_VECTOR, 1, 4), var("dir", 96, 16, D3D_SVC_VECTOR, 1, 4),
                                           var("params", 112, 16, D3D_SVC_VECTOR, 1, 4)})};
    r.resources = {texBind("s_albedo", 0), texBind("s_normal", 1), texBind("s_depth", 2)};
    r.colorOutputs = 1;
    bytes = shaderBytes(ShaderStage::Pixel, "light_ps", r);
    CHECK_HR(a.dev->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &a.lightPS));

    r = ShaderReflection();
    r.cbuffers = {cbuf("CombineCB", 0, 32, {var("fog", 0, 16, D3D_SVC_VECTOR, 1, 4), var("misc", 16, 16, D3D_SVC_VECTOR, 1, 4)})};
    r.resources = {texBind("s_scene", 0), texBind("s_depth", 1), smpBind("smp_rtlinear", 0)};
    r.colorOutputs = 1;
    bytes = shaderBytes(ShaderStage::Pixel, "combine_ps", r);
    CHECK_HR(a.dev->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &a.combinePS));

    r = ShaderReflection();
    r.cbuffers = {cbuf("UICB", 0, 16, {var("screen", 0, 16, D3D_SVC_VECTOR, 1, 4)})};
    r.inputs = {vin("POSITION", 0, 0, 3), vin("COLOR", 0, 1, 4), vin("TEXCOORD", 0, 2, 2)};
    bytes = shaderBytes(ShaderStage::Vertex, "ui_vs", r);
    CHECK_HR(a.dev->CreateVertexShader(bytes.data(), bytes.size(), nullptr, &a.uiVS));
    D3D11_INPUT_ELEMENT_DESC ui[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    CHECK_HR(a.dev->CreateInputLayout(ui, 3, bytes.data(), bytes.size(), &a.uiLayout));

    r = ShaderReflection();
    r.resources = {texBind("s_base", 0), smpBind("smp_base", 0)};
    r.colorOutputs = 1;
    bytes = shaderBytes(ShaderStage::Pixel, "ui_ps", r);
    CHECK_HR(a.dev->CreatePixelShader(bytes.data(), bytes.size(), nullptr, &a.uiPS));

    for (int k = 0; k < 2; ++k)
    {
        r = ShaderReflection();
        r.cbuffers = {cbuf("ProbeCB", 0, 16, {var("dir", 0, 16, D3D_SVC_VECTOR, 1, 4)})};
        ResourceBinding t = texBind("s_probe", 0);
        t.dimension = k == 0 ? D3D_SRV_DIMENSION_TEXTURECUBE : D3D_SRV_DIMENSION_TEXTURE3D;
        r.resources = {t, smpBind("smp_probe", 0)};
        r.colorOutputs = 1;
        bytes = shaderBytes(ShaderStage::Pixel, k == 0 ? "cube_ps" : "vol_ps", r);
        CHECK_HR(a.dev->CreatePixelShader(bytes.data(), bytes.size(), nullptr, k == 0 ? &a.cubePS : &a.volPS));
    }

    r = ShaderReflection();
    r.cbuffers = {cbuf("FillCB", 0, 16, {var("color", 0, 16, D3D_SVC_VECTOR, 1, 4)})};
    ResourceBinding u;
    u.name = "o_image"; u.type = D3D_SIT_UAV_RWTYPED; u.d3dSlot = 0; u.metalIndex = kUavTextureBase;
    r.resources = {u};
    r.threadgroupSize[0] = 8; r.threadgroupSize[1] = 8; r.threadgroupSize[2] = 1;
    bytes = shaderBytes(ShaderStage::Compute, "fill_cs", r);
    CHECK_HR(a.dev->CreateComputeShader(bytes.data(), bytes.size(), nullptr, &a.fillCS));

    // state objects
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_BACK; rs.DepthClipEnable = TRUE;
    CHECK_HR(a.dev->CreateRasterizerState(&rs, &a.rsCullBack));
    rs.CullMode = D3D11_CULL_NONE;
    CHECK_HR(a.dev->CreateRasterizerState(&rs, &a.rsNoCull));

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_LESS;
    CHECK_HR(a.dev->CreateDepthStencilState(&ds, &a.dsDepth));
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    CHECK_HR(a.dev->CreateDepthStencilState(&ds, &a.dsTestOnly));
    ds.DepthEnable = FALSE;
    CHECK_HR(a.dev->CreateDepthStencilState(&ds, &a.dsNoDepth));

    D3D11_BLEND_DESC bd{};
    for (auto& rt : bd.RenderTarget)
    {
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    CHECK_HR(a.dev->CreateBlendState(&bd, &a.bsOpaque));
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    CHECK_HR(a.dev->CreateBlendState(&bd, &a.bsAlpha));
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    CHECK_HR(a.dev->CreateBlendState(&bd, &a.bsAdditive));

    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxAnisotropy = 1;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    CHECK_HR(a.dev->CreateSamplerState(&sd, &a.smpLinear));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    CHECK_HR(a.dev->CreateSamplerState(&sd, &a.smpPoint));
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    CHECK_HR(a.dev->CreateSamplerState(&sd, &a.smpAniso));
    return true;
}

bool createResources(App& a)
{
    std::vector<Vtx> verts;
    std::vector<uint16_t> idx;
    buildCube(verts, idx);
    // ground plane appended after the cube: its indices are relative (0..3) and drawn with a base vertex
    const uint32_t planeBase = uint32_t(verts.size());
    const float S = 14.0f;
    verts.push_back({{-S, 0, -S}, {0, 1, 0}, {0, 0}});
    verts.push_back({{-S, 0, S}, {0, 1, 0}, {0, 1}});
    verts.push_back({{S, 0, -S}, {0, 1, 0}, {1, 0}});
    verts.push_back({{S, 0, S}, {0, 1, 0}, {1, 1}});
    const uint32_t planeFirstIndex = uint32_t(idx.size());
    // clockwise as seen from above (+y)
    uint16_t pl[6] = {0, 1, 2, 2, 1, 3};
    for (uint16_t i : pl) idx.push_back(i);
    a.planeBase = planeBase;
    a.planeFirstIndex = planeFirstIndex;
    if (!createBuffer(a, &a.sceneVB, UINT(verts.size() * sizeof(Vtx)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, verts.data())) return false;
    if (!createBuffer(a, &a.sceneIB, UINT(idx.size() * 2), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, idx.data())) return false;
    if (!createBuffer(a, &a.uiVB, App::kUiVBSize, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER)) return false;
    if (!createBuffer(a, &a.cbObjVS, 128, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER)) return false;
    if (!createBuffer(a, &a.cbObjPS, 32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER)) return false;
    if (!createBuffer(a, &a.cbLight, 128, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER)) return false;
    if (!createBuffer(a, &a.cbCombine, 32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER)) return false;
    if (!createBuffer(a, &a.cbUI, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER)) return false;
    if (!createBuffer(a, &a.cbFill, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER)) return false;
    if (!createBuffer(a, &a.cbProbe, 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER)) return false;

    auto makeTex = [&](ID3D11Texture2D** t, ID3D11ShaderResourceView** s, const MipChain& c, int size, DXGI_FORMAT f) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = size; d.Height = size; d.MipLevels = UINT(c.init.size()); d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CHECK_HR(a.dev->CreateTexture2D(&d, c.init.data(), t));
        CHECK_HR(a.dev->CreateShaderResourceView(*t, nullptr, s));
        return true;
    };
    const uint8_t brickA[3] = {150, 70, 50}, brickB[3] = {200, 190, 170};
    const uint8_t gA[3] = {120, 125, 110}, gB[3] = {80, 85, 75};
    if (!makeTex(&a.texBrick, &a.srvBrick, makePattern(256, 32, brickA, brickB, true), 256, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    if (!makeTex(&a.texGround, &a.srvGround, makePattern(256, 64, gA, gB, false), 256, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    const uint8_t cA[3] = {255, 255, 255}, cB[3] = {40, 40, 200};
    if (!makeTex(&a.texChecker, &a.srvChecker, makePattern(8, 4, cA, cB, false), 8, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    const uint8_t bcA[3] = {255, 128, 0};
    if (!makeTex(&a.texBC1, &a.srvBC1, makeBC1(64, bcA, bcA), 64, DXGI_FORMAT_BC1_UNORM)) return false;
    {
        const uint32_t white = 0xffffffff;
        MipChain c;
        c.init.push_back({&white, 4, 0});
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CHECK_HR(a.dev->CreateTexture2D(&d, c.init.data(), &a.texWhite));
        CHECK_HR(a.dev->CreateShaderResourceView(a.texWhite, nullptr, &a.srvWhite));
    }
    // staging -> default copy
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = 8; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Texture2D* stage = nullptr;
        CHECK_HR(a.dev->CreateTexture2D(&d, nullptr, &stage));
        D3D11_MAPPED_SUBRESOURCE m;
        CHECK_HR(a.ctx->Map(stage, 0, D3D11_MAP_WRITE, 0, &m));
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
            {
                uint8_t* p = static_cast<uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
                p[0] = 0; p[1] = 255; p[2] = 255; p[3] = 255;
            }
        a.ctx->Unmap(stage, 0);
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CHECK_HR(a.dev->CreateTexture2D(&d, nullptr, &a.texStageDst));
        a.ctx->CopyResource(a.texStageDst, stage);
        stage->Release();
        CHECK_HR(a.dev->CreateShaderResourceView(a.texStageDst, nullptr, &a.srvStageDst));
    }
    // dynamic texture
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = 8; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CHECK_HR(a.dev->CreateTexture2D(&d, nullptr, &a.texDynamic));
        CHECK_HR(a.dev->CreateShaderResourceView(a.texDynamic, nullptr, &a.srvDynamic));
    }
    // UAV target for the compute swatch
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = 16; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        CHECK_HR(a.dev->CreateTexture2D(&d, nullptr, &a.texUAV));
        CHECK_HR(a.dev->CreateShaderResourceView(a.texUAV, nullptr, &a.srvUAV));
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        CHECK_HR(a.dev->CreateUnorderedAccessView(a.texUAV, &uv, &a.uavFill));
    }
    // copy destination for the resolved MSAA image
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = 64; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CHECK_HR(a.dev->CreateTexture2D(&d, nullptr, &a.texCopy));
        CHECK_HR(a.dev->CreateShaderResourceView(a.texCopy, nullptr, &a.srvCopy));
    }

    // cube map (6 solid faces), 3D texture (4 solid slices), UpdateSubresource on a DEFAULT texture
    {
        static const uint8_t faces[6][4] = {{200, 50, 50, 255}, {50, 200, 50, 255}, {50, 50, 200, 255},
                                            {200, 200, 50, 255}, {200, 50, 200, 255}, {50, 200, 200, 255}};
        uint8_t texels[6][4 * 4 * 4];
        D3D11_SUBRESOURCE_DATA init[6];
        for (int f = 0; f < 6; ++f)
        {
            for (int i = 0; i < 16; ++i)
                std::memcpy(&texels[f][i * 4], faces[f], 4);
            init[f] = {texels[f], 16, 0};
        }
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = 4; d.MipLevels = 1; d.ArraySize = 6; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        CHECK_HR(a.dev->CreateTexture2D(&d, init, &a.texCube));
        CHECK_HR(a.dev->CreateShaderResourceView(a.texCube, nullptr, &a.srvCube));

        static const uint8_t slices[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
        uint8_t vol[4 * 4 * 4 * 4];
        for (int z = 0; z < 4; ++z)
            for (int i = 0; i < 16; ++i)
                std::memcpy(&vol[(z * 16 + i) * 4], slices[z], 4);
        D3D11_TEXTURE3D_DESC v{};
        v.Width = v.Height = v.Depth = 4; v.MipLevels = 1; v.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        v.Usage = D3D11_USAGE_IMMUTABLE; v.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA vi{vol, 16, 64};
        CHECK_HR(a.dev->CreateTexture3D(&v, &vi, &a.tex3D));
        CHECK_HR(a.dev->CreateShaderResourceView(a.tex3D, nullptr, &a.srv3D));

        D3D11_TEXTURE2D_DESC u{};
        u.Width = u.Height = 8; u.MipLevels = 1; u.ArraySize = 1; u.Format = DXGI_FORMAT_R8G8B8A8_UNORM; u.SampleDesc.Count = 1;
        u.Usage = D3D11_USAGE_DEFAULT; u.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CHECK_HR(a.dev->CreateTexture2D(&u, nullptr, &a.texUpdate));
        CHECK_HR(a.dev->CreateShaderResourceView(a.texUpdate, nullptr, &a.srvUpdate));
        uint32_t px[64];
        for (auto& p : px) p = 0xffff80ffu; // ABGR: R=255 G=128 B=255
        a.ctx->UpdateSubresource(a.texUpdate, 0, nullptr, px, 8 * 4, 0);
    }
    // DDS through the D3DX11 replacement, the way R4's texture_load does it (skip the top mip, IMMUTABLE)
    {
        const uint8_t col[3] = {0, 200, 100};
        std::vector<uint8_t> dds = makeDDS(64, col);
        D3DX11_IMAGE_INFO info{};
        CHECK_HR(D3DX11GetImageInfoFromMemory(dds.data(), dds.size(), nullptr, &info, nullptr));
        if (info.Width != 64 || info.MipLevels != 7 || info.Format != DXGI_FORMAT_BC1_UNORM)
        {
            SDL_Log("DDS header probe returned %ux%u mips %u fmt %d", info.Width, info.Height, info.MipLevels, int(info.Format));
            return false;
        }
        D3DX11_IMAGE_LOAD_INFO li;
        li.FirstMipLevel = 1;
        li.MipLevels = info.MipLevels - 1;
        li.Width = info.Width / 2;
        li.Height = info.Height / 2;
        li.pSrcInfo = &info;
        li.Usage = D3D11_USAGE_IMMUTABLE;
        li.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Resource* r = nullptr;
        CHECK_HR(D3DX11CreateTextureFromMemory(a.dev, dds.data(), dds.size(), &li, nullptr, &r, nullptr));
        a.texDDS = static_cast<ID3D11Texture2D*>(r);
        D3D11_TEXTURE2D_DESC td;
        a.texDDS->GetDesc(&td);
        if (td.Width != 32 || td.MipLevels != 6)
        {
            SDL_Log("DDS load produced %ux%u with %u mips", td.Width, td.Height, td.MipLevels);
            return false;
        }
        CHECK_HR(a.dev->CreateShaderResourceView(a.texDDS, nullptr, &a.srvDDS));
        // staging path (R4 loads as STAGING and copies to a DEFAULT texture on first use)
        D3DX11_IMAGE_LOAD_INFO ls = li;
        ls.Usage = D3D11_USAGE_STAGING;
        ls.BindFlags = 0;
        ls.CpuAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CHECK_HR(D3DX11CreateTextureFromMemory(a.dev, dds.data(), dds.size(), &ls, nullptr, &r, nullptr));
        ID3D11Texture2D* stage = static_cast<ID3D11Texture2D*>(r);
        stage->GetDesc(&td);
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        CHECK_HR(a.dev->CreateTexture2D(&td, nullptr, &a.texDDSStaged));
        a.ctx->CopyResource(a.texDDSStaged, stage);
        stage->Release();
        CHECK_HR(a.dev->CreateShaderResourceView(a.texDDSStaged, nullptr, &a.srvDDSStaged));
    }

    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
    CHECK_HR(a.dev->CreateQuery(&qd, &a.occHidden));
    CHECK_HR(a.dev->CreateQuery(&qd, &a.occVisible));
    qd.Query = D3D11_QUERY_EVENT;
    CHECK_HR(a.dev->CreateQuery(&qd, &a.event));
    return true;
}

bool createTargets(App& a)
{
    if (!createRT(a, a.albedo, a.W, a.H, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    if (!createRT(a, a.normal, a.W, a.H, DXGI_FORMAT_R16G16B16A16_FLOAT)) return false;
    if (!createRT(a, a.scene, a.W, a.H, DXGI_FORMAT_R16G16B16A16_FLOAT)) return false;
    if (!createRT(a, a.small, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    if (!createRT(a, a.resolved, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM)) return false;
    // main depth like R4's: typeless 24/8 with a D24S8 DSV and a depth-plane SRV
    D3D11_TEXTURE2D_DESC d{};
    d.Width = a.W; d.Height = a.H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R24G8_TYPELESS; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    CHECK_HR(a.dev->CreateTexture2D(&d, nullptr, &a.depthTex));
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    CHECK_HR(a.dev->CreateDepthStencilView(a.depthTex, &dv, &a.dsv));
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    CHECK_HR(a.dev->CreateShaderResourceView(a.depthTex, &sv, &a.depthSRV));
    // 4x MSAA target for the resolve swatch
    D3D11_TEXTURE2D_DESC m{};
    m.Width = m.Height = 64; m.MipLevels = 1; m.ArraySize = 1; m.Format = DXGI_FORMAT_R8G8B8A8_UNORM; m.SampleDesc.Count = 4;
    m.Usage = D3D11_USAGE_DEFAULT; m.BindFlags = D3D11_BIND_RENDER_TARGET;
    CHECK_HR(a.dev->CreateTexture2D(&m, nullptr, &a.msaaTex));
    CHECK_HR(a.dev->CreateRenderTargetView(a.msaaTex, nullptr, &a.msaaRTV));
    return true;
}

// ---- per-frame helpers ------------------------------------------------------------------------------------
template <class T>
void writeCB(App& a, ID3D11Buffer* cb, const T& data)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(a.ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        std::memcpy(m.pData, &data, sizeof(T));
        a.ctx->Unmap(cb, 0);
    }
}

void setViewport(App& a, int w, int h)
{
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    a.ctx->RSSetViewports(1, &vp);
}

// Appends one quad to the streaming vertex buffer (NO_OVERWRITE, DISCARD when it wraps) and draws it.
void drawQuad(App& a, float x, float y, float w, float h, float z, const float color[4], ID3D11ShaderResourceView* srv,
              ID3D11SamplerState* smp, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1)
{
    const UINT need = 6 * sizeof(UIVertex);
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (a.uiPos + need > App::kUiVBSize)
    {
        mode = D3D11_MAP_WRITE_DISCARD;
        a.uiPos = 0;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(a.ctx->Map(a.uiVB, 0, mode, 0, &m)))
        return;
    uint8_t rgba[4];
    for (int i = 0; i < 4; ++i)
        rgba[i] = uint8_t(std::lround(std::fmin(std::fmax(color[i], 0.f), 1.f) * 255.f));
    UIVertex q[4] = {{x, y, z, {}, u0, v0}, {x + w, y, z, {}, u1, v0}, {x, y + h, z, {}, u0, v1}, {x + w, y + h, z, {}, u1, v1}};
    for (auto& v : q) std::memcpy(v.rgba, rgba, 4);
    UIVertex* dst = reinterpret_cast<UIVertex*>(static_cast<uint8_t*>(m.pData) + a.uiPos);
    // two clockwise triangles
    dst[0] = q[0]; dst[1] = q[1]; dst[2] = q[2];
    dst[3] = q[2]; dst[4] = q[1]; dst[5] = q[3];
    a.ctx->Unmap(a.uiVB, 0);
    a.ctx->PSSetShaderResources(0, 1, &srv);
    a.ctx->PSSetSamplers(0, 1, &smp);
    a.ctx->Draw(6, a.uiPos / sizeof(UIVertex));
    a.uiPos += need;
}

void bindUIState(App& a, ID3D11BlendState* bs, ID3D11DepthStencilState* ds)
{
    const float bf[4] = {1, 1, 1, 1};
    UINT stride = sizeof(UIVertex), offset = 0;
    a.ctx->IASetInputLayout(a.uiLayout);
    a.ctx->IASetVertexBuffers(0, 1, &a.uiVB, &stride, &offset);
    a.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a.ctx->VSSetShader(a.uiVS, nullptr, 0);
    a.ctx->PSSetShader(a.uiPS, nullptr, 0);
    ID3D11Buffer* cb = a.cbUI;
    a.ctx->VSSetConstantBuffers(0, 1, &cb);
    a.ctx->RSSetState(a.rsNoCull);
    a.ctx->OMSetBlendState(bs, bf, 0xffffffff);
    a.ctx->OMSetDepthStencilState(ds, 0);
}

void fullscreen(App& a, ID3D11PixelShader* ps)
{
    a.ctx->IASetInputLayout(nullptr);
    a.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a.ctx->VSSetShader(a.quadVS, nullptr, 0);
    a.ctx->PSSetShader(ps, nullptr, 0);
    a.ctx->RSSetState(a.rsNoCull);
    a.ctx->Draw(3, 0);
}

struct Cube { float x, y, z, s, spin; float tint[3]; };
struct ObjVS { Mat4 mvp, model; };
struct ObjPS { float tint[4], uvScale[4]; };
struct LightCB { Mat4 invViewProj; float posRadius[4], color[4], dir[4], params[4]; };
struct CombineCB { float fog[4], misc[4]; };

void drawCube(App& a, const Mat4& viewProj, const Cube& c, float t, ID3D11ShaderResourceView* srv)
{
    Mat4 model = mul(translate(c.x, c.y, c.z), mul(rotateY(t * c.spin), scale(c.s, c.s, c.s)));
    ObjVS vs{mul(viewProj, model), model};
    ObjPS ps{{c.tint[0], c.tint[1], c.tint[2], 1}, {1, 1, 0, 0}};
    writeCB(a, a.cbObjVS, vs);
    writeCB(a, a.cbObjPS, ps);
    ID3D11Buffer* vcb = a.cbObjVS;
    ID3D11Buffer* pcb = a.cbObjPS;
    a.ctx->VSSetConstantBuffers(0, 1, &vcb);
    a.ctx->PSSetConstantBuffers(0, 1, &pcb);
    a.ctx->PSSetShaderResources(0, 1, &srv);
    a.ctx->DrawIndexed(36, 0, 0);
}

bool renderFrame(App& a)
{
    ID3D11DeviceContext1* c = a.ctx;
    const float t = a.frame * (1.0f / 60.0f);
    const float W = float(a.W), H = float(a.H);
    a.uiPos = App::kUiVBSize; // the first streamed quad of every frame discards the buffer

    // CPU throttle like R4's sync point: wait for the event issued two frames ago
    if (a.eventPending)
    {
        MtlBOOL done = FALSE;
        int spins = 0;
        while (c->GetData(a.event, &done, sizeof(done), 0) == S_FALSE && spins++ < 200000) {}
        a.eventPending = false;
    }

    // camera
    const float eye[3] = {sinf(t * 0.35f) * 9.0f, 4.5f, -9.0f + cosf(t * 0.35f) * 1.5f};
    const float target[3] = {0, 1.2f, 2.0f};
    const float zn = 0.5f, zf = 60.0f;
    Mat4 view = lookAt(eye, target);
    Mat4 proj = perspective(1.0f, W / H, zn, zf);
    Mat4 viewProj = mul(proj, view);
    Mat4 invViewProj = inverse(viewProj);

    // ---- Pass 1: G-buffer ------------------------------------------------------------------------------
    {
        ID3D11RenderTargetView* rtvs[8] = {a.albedo.rtv, a.normal.rtv};
        c->OMSetRenderTargets(0, nullptr, nullptr);  // R4's "unbind" idiom
        c->OMSetRenderTargets(8, rtvs, a.dsv);
        const float clear0[4] = {0, 0, 0, 0};
        c->ClearRenderTargetView(a.albedo.rtv, clear0);
        c->ClearRenderTargetView(a.normal.rtv, clear0);
        c->ClearDepthStencilView(a.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        setViewport(a, a.W, a.H);
        const float bf[4] = {1, 1, 1, 1};
        c->OMSetBlendState(a.bsOpaque, bf, 0xffffffff);
        c->OMSetDepthStencilState(a.dsDepth, 0);
        c->RSSetState(a.rsCullBack);
        c->IASetInputLayout(a.sceneLayout);
        UINT stride = sizeof(Vtx), off = 0;
        c->IASetVertexBuffers(0, 1, &a.sceneVB, &stride, &off);
        c->IASetIndexBuffer(a.sceneIB, DXGI_FORMAT_R16_UINT, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(a.gbufVS, nullptr, 0);
        c->PSSetShader(a.gbufPS, nullptr, 0);
        c->PSSetSamplers(0, 1, &a.smpAniso);

        // ground plane (base vertex path)
        {
            ObjVS vs{viewProj, identity()};
            ObjPS ps{{1, 1, 1, 1}, {4, 4, 0, 0}};
            writeCB(a, a.cbObjVS, vs);
            writeCB(a, a.cbObjPS, ps);
            ID3D11Buffer* vcb = a.cbObjVS;
            ID3D11Buffer* pcb = a.cbObjPS;
            c->VSSetConstantBuffers(0, 1, &vcb);
            c->PSSetConstantBuffers(0, 1, &pcb);
            c->PSSetShaderResources(0, 1, &a.srvGround);
            c->DrawIndexed(6, a.planeFirstIndex, INT(a.planeBase));
        }
        // occlusion query on a cube hidden inside the big wall, one on a visible cube
        const Cube cubes[] = {
            {-3.5f, 1.0f, 3.0f, 1.0f, 0.6f, {1.0f, 0.85f, 0.8f}},
            {0.0f, 1.2f, 5.0f, 1.2f, -0.4f, {0.9f, 1.0f, 0.9f}},
            {3.8f, 0.8f, 2.5f, 0.8f, 0.9f, {0.8f, 0.9f, 1.0f}},
            {1.8f, 0.6f, -0.5f, 0.6f, 0.3f, {1.0f, 1.0f, 0.6f}},
            {-2.0f, 0.7f, -0.8f, 0.7f, -0.7f, {1.0f, 0.7f, 0.9f}},
        };
        for (const Cube& cb : cubes)
        {
            if (&cb == &cubes[1])
                c->Begin(a.occVisible);
            drawCube(a, viewProj, cb, t, a.srvBrick);
            if (&cb == &cubes[1])
                c->End(a.occVisible);
        }
        // wall in front of a hidden cube (the wall is drawn first so the hidden cube fails the depth test)
        Cube wall{-1.0f, 1.5f, -3.5f, 1.0f, 0.0f, {0.9f, 0.9f, 0.9f}};
        {
            Mat4 model = mul(translate(wall.x, wall.y, wall.z), scale(3.0f, 1.5f, 0.2f));
            ObjVS vs{mul(viewProj, model), model};
            ObjPS ps{{wall.tint[0], wall.tint[1], wall.tint[2], 1}, {2, 1, 0, 0}};
            writeCB(a, a.cbObjVS, vs);
            writeCB(a, a.cbObjPS, ps);
            ID3D11Buffer* vcb = a.cbObjVS;
            ID3D11Buffer* pcb = a.cbObjPS;
            c->VSSetConstantBuffers(0, 1, &vcb);
            c->PSSetConstantBuffers(0, 1, &pcb);
            c->PSSetShaderResources(0, 1, &a.srvBrick);
            c->DrawIndexed(36, 0, 0);
        }
        Cube hidden{-1.0f, 1.0f, -2.2f, 0.5f, 0.5f, {1, 0, 0}}; // wall at z=-3.5 is nearer to the camera at z=-9
        c->Begin(a.occHidden);
        drawCube(a, viewProj, hidden, t, a.srvBrick);
        c->End(a.occHidden);
    }

    // ---- MSAA triangle -> resolve -> copy (swatch 5) --------------------------------------------------------------
    {
        c->OMSetRenderTargets(1, &a.msaaRTV, nullptr);
        const float clear[4] = {0, 0, 0, 1};
        c->ClearRenderTargetView(a.msaaRTV, clear);
        setViewport(a, 64, 64);
        // A single oversized triangle covering the whole target: every sample of every pixel is covered.
        float uiW = 64, uiH = 64;
        float screen[4] = {2.0f / uiW, -2.0f / uiH, 0, 0};
        writeCB(a, a.cbUI, screen);
        bindUIState(a, a.bsOpaque, a.dsNoDepth);
        const float col[4] = {1.0f, 0.5f, 0.0f, 1.0f};
        drawQuad(a, -8, -8, 80, 80, 0.5f, col, a.srvWhite, a.smpPoint);
        c->ResolveSubresource(a.resolved.tex, 0, a.msaaTex, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        c->CopyResource(a.texCopy, a.resolved.tex);
    }

    // ---- compute fill (swatch 7) ------------------------------------------------------------------------------------
    {
        const float yellow[4] = {1, 1, 0, 1};
        writeCB(a, a.cbFill, yellow);
        ID3D11Buffer* cb = a.cbFill;
        c->CSSetShader(a.fillCS, nullptr, 0);
        c->CSSetConstantBuffers(0, 1, &cb);
        c->CSSetUnorderedAccessViews(0, 1, &a.uavFill, nullptr);
        c->Dispatch(2, 2, 1);
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        c->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    }

    // ---- dynamic texture update (swatch 8) ----------------------------------------------------------------------------
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(c->Map(a.texDynamic, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                {
                    uint8_t* p = static_cast<uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
                    p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255;
                }
            c->Unmap(a.texDynamic, 0);
        }
    }

    // ---- Pass 2: lighting (accumulate into the HDR scene target) ---------------------------------------------------
    {
        c->OMSetRenderTargets(0, nullptr, nullptr);
        c->OMSetRenderTargets(1, &a.scene.rtv, nullptr);
        const float black[4] = {0, 0, 0, 0};
        c->ClearRenderTargetView(a.scene.rtv, black);
        setViewport(a, a.W, a.H);
        ID3D11ShaderResourceView* srvs[3] = {a.albedo.srv, a.normal.srv, a.depthSRV};
        c->PSSetShaderResources(0, 3, srvs);
        c->OMSetDepthStencilState(a.dsNoDepth, 0);
        const float bf[4] = {1, 1, 1, 1};
        ID3D11Buffer* lcb = a.cbLight;
        c->PSSetConstantBuffers(0, 1, &lcb);

        struct L { float pos[3], radius, col[3], dirx[3], kind; } lights[] = {
            {{0, 0, 0}, 0, {1.0f, 0.95f, 0.85f}, {-0.4f, -0.8f, 0.5f}, 0},                           // sun (also paints the sky)
            {{sinf(t) * 4.0f, 1.8f, 2.0f + cosf(t) * 2.0f}, 7.0f, {2.2f, 0.7f, 0.4f}, {0, 0, 0}, 1}, // warm point light
            {{-3.0f, 2.0f, 1.0f + sinf(t * 0.7f) * 3.0f}, 8.0f, {0.3f, 0.7f, 2.4f}, {0, 0, 0}, 1},    // cool point light
        };
        for (int i = 0; i < 3; ++i)
        {
            LightCB cb{};
            cb.invViewProj = invViewProj;
            cb.posRadius[0] = lights[i].pos[0]; cb.posRadius[1] = lights[i].pos[1]; cb.posRadius[2] = lights[i].pos[2];
            cb.posRadius[3] = lights[i].radius;
            cb.color[0] = lights[i].col[0]; cb.color[1] = lights[i].col[1]; cb.color[2] = lights[i].col[2];
            cb.dir[0] = lights[i].dirx[0]; cb.dir[1] = lights[i].dirx[1]; cb.dir[2] = lights[i].dirx[2];
            cb.params[0] = lights[i].kind;
            writeCB(a, a.cbLight, cb); // DISCARD between draws of the same frame
            c->OMSetBlendState(i == 0 ? a.bsOpaque : a.bsAdditive, bf, 0xffffffff);
            fullscreen(a, a.lightPS);
        }
        ID3D11ShaderResourceView* none[3] = {nullptr, nullptr, nullptr};
        c->PSSetShaderResources(0, 3, none);
    }

    // ---- Pass 3: combine into the back buffer, then HUD and swatches ------------------------------------------------
    {
        c->OMSetRenderTargets(0, nullptr, nullptr);
        c->OMSetRenderTargets(1, &a.backRTV, nullptr); // depth is sampled here, so it cannot be bound as DSV
        setViewport(a, a.W, a.H);
        const float bf[4] = {1, 1, 1, 1};
        c->OMSetBlendState(a.bsOpaque, bf, 0xffffffff);
        c->OMSetDepthStencilState(a.dsNoDepth, 0);
        CombineCB cb{{0.45f, 0.5f, 0.6f, 0.02f}, {1.1f, zn, zf, 0}};
        writeCB(a, a.cbCombine, cb);
        ID3D11Buffer* ccb = a.cbCombine;
        c->PSSetConstantBuffers(0, 1, &ccb);
        ID3D11ShaderResourceView* srvs[2] = {a.scene.srv, a.depthSRV};
        c->PSSetShaderResources(0, 2, srvs);
        c->PSSetSamplers(0, 1, &a.smpLinear);
        fullscreen(a, a.combinePS);
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        c->PSSetShaderResources(0, 2, none);

        // HUD + swatches: same back buffer, now with the G-buffer depth attached (a new pass that loads the colour)
        c->OMSetRenderTargets(1, &a.backRTV, a.dsv);
        float screen[4] = {2.0f / W, -2.0f / H, 0, 0};
        writeCB(a, a.cbUI, screen);
        bindUIState(a, a.bsAlpha, a.dsTestOnly);

        // translucent panels, depth-tested against the scene (z = 0.001: in front of everything)
        const float panel[4] = {0.05f, 0.08f, 0.12f, 0.55f};
        drawQuad(a, 12, 12, 330, 74, 0.001f, panel, a.srvWhite, a.smpLinear);
        const float bar[4] = {0.95f, 0.65f, 0.1f, 0.85f};
        const float fill = 0.5f + 0.5f * sinf(t * 1.3f);
        drawQuad(a, 24, 26, 306.0f * fill, 14, 0.0005f, bar, a.srvWhite, a.smpLinear);
        const float pale[4] = {0.8f, 0.9f, 1.0f, 0.9f};
        drawQuad(a, 24, 50, 140, 24, 0.0005f, pale, a.srvChecker, a.smpPoint);
        // glass pane that sits inside the scene (depth z from the world, tested but not written)
        const float glass[4] = {0.3f, 0.8f, 0.7f, 0.35f};
        drawQuad(a, W * 0.5f - 160, H * 0.5f - 90, 320, 160, 0.965f, glass, a.srvWhite, a.smpLinear);

        // swatches
        const float sy = H - 88;
        auto sx = [&](int i) { return 16.0f + i * 80.0f; };
        const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1}, green50[4] = {0, 1, 0, 0.5f}, white[4] = {1, 1, 1, 1};
        const float yel[4] = {1, 1, 0, 1}, mag[4] = {1, 0, 1, 1};
        bindUIState(a, a.bsOpaque, a.dsDepth);
        c->OMSetDepthStencilState(a.dsNoDepth, 0);
        // 0: solid red
        drawQuad(a, sx(0), sy, 64, 64, 0.5f, red, a.srvWhite, a.smpPoint);
        // 1: 50% green over blue
        drawQuad(a, sx(1), sy, 64, 64, 0.5f, blue, a.srvWhite, a.smpPoint);
        c->OMSetBlendState(a.bsAlpha, bf, 0xffffffff);
        drawQuad(a, sx(1), sy, 64, 64, 0.5f, green50, a.srvWhite, a.smpPoint);
        c->OMSetBlendState(a.bsOpaque, bf, 0xffffffff);
        // 2: point-sampled 8x8 checker (blue cell in the top-right quadrant is sampled at the quadrant centre)
        drawQuad(a, sx(2), sy, 64, 64, 0.5f, white, a.srvChecker, a.smpPoint);
        // 3: offscreen 16x16 RT cleared and sampled in a later pass
        drawQuad(a, sx(3), sy, 64, 64, 0.5f, white, a.small.srv, a.smpPoint);
        // 4: MSAA resolved + copied
        drawQuad(a, sx(4), sy, 64, 64, 0.5f, white, a.srvCopy, a.smpPoint);
        // 5: depth test: near yellow first, far magenta later -> yellow
        c->OMSetDepthStencilState(a.dsDepth, 0);
        drawQuad(a, sx(5), sy, 64, 64, 0.2f, yel, a.srvWhite, a.smpPoint);
        drawQuad(a, sx(5), sy, 64, 64, 0.4f, mag, a.srvWhite, a.smpPoint);
        c->OMSetDepthStencilState(a.dsNoDepth, 0);
        // 6: compute-filled UAV
        drawQuad(a, sx(6), sy, 64, 64, 0.5f, white, a.srvUAV, a.smpPoint);
        // 7: staging -> default copy
        drawQuad(a, sx(7), sy, 64, 64, 0.5f, white, a.srvStageDst, a.smpPoint);
        // 8: dynamic texture
        drawQuad(a, sx(8), sy, 64, 64, 0.5f, white, a.srvDynamic, a.smpPoint);
        // 9: BC1
        drawQuad(a, sx(9), sy, 64, 64, 0.5f, white, a.srvBC1, a.smpPoint);
        // 10: DDS loaded through D3DX11CreateTextureFromMemory (first mip skipped), 11: same via a STAGING copy
        drawQuad(a, sx(10), sy, 64, 64, 0.5f, white, a.srvDDS, a.smpLinear);
        drawQuad(a, sx(11), sy, 64, 64, 0.5f, white, a.srvDDSStaged, a.smpLinear);
        // 12/13: cube map sampled along +X and -Z, 14: 3D texture slice 2, 15: UpdateSubresource
        auto probe = [&](int i, ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv, float x, float y, float z) {
            const float dir[4] = {x, y, z, 0};
            writeCB(a, a.cbProbe, dir);
            ID3D11Buffer* pcb = a.cbProbe;
            c->PSSetConstantBuffers(0, 1, &pcb);
            c->PSSetShader(ps, nullptr, 0);
            drawQuad(a, sx(i), sy, 64, 64, 0.5f, white, srv, a.smpPoint);
        };
        probe(12, a.cubePS, a.srvCube, 1, 0, 0);
        probe(13, a.cubePS, a.srvCube, 0, 0, -1);
        probe(14, a.volPS, a.srv3D, 0, 0, 0.625f);
        c->PSSetShader(a.uiPS, nullptr, 0);
        drawQuad(a, sx(15), sy, 64, 64, 0.5f, white, a.srvUpdate, a.smpPoint);
    }

    // sync point for the CPU throttle
    c->End(a.event);
    a.eventPending = true;

    if (a.frame == a.captureFrame)
    {
        RequestCapture(a.swap);
        a.captureRequested = true;
    }
    HRESULT hr = a.swap->Present(1, 0);
    return SUCCEEDED(hr);
}

// The small RT is cleared once at start-up and never drawn into again: the clear must still reach swatch 3.
void clearSmallRT(App& a)
{
    const float c[4] = {0, 0, 1, 1};
    a.ctx->OMSetRenderTargets(1, &a.small.rtv, nullptr);
    a.ctx->ClearRenderTargetView(a.small.rtv, c);
    a.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

// R4's R_constant_table::parse reads shaders through ID3D11ShaderReflection; make sure the container answers it.
bool checkReflection(App&)
{
    ShaderReflection r;
    r.cbuffers = {cbuf("ObjPS", 0, 32, {var("tint", 0, 16, D3D_SVC_VECTOR, 1, 4), var("uvScale", 16, 16, D3D_SVC_VECTOR, 1, 4)})};
    r.resources = {texBind("s_base", 0), smpBind("smp_base", 0)};
    std::vector<uint8_t> bytes = shaderBytes(ShaderStage::Pixel, "gbuffer_ps", r);
    ID3D11ShaderReflection* refl = nullptr;
    if (FAILED(D3DReflect(bytes.data(), bytes.size(), IID_PPV_ARGS(&refl))))
    {
        report("reflection: D3DReflect failed  FAIL");
        return false;
    }
    D3D11_SHADER_DESC sd;
    refl->GetDesc(&sd);
    ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(0);
    D3D11_SHADER_BUFFER_DESC bd{};
    cb->GetDesc(&bd);
    ID3D11ShaderReflectionVariable* v = cb->GetVariableByIndex(1);
    D3D11_SHADER_VARIABLE_DESC vd{};
    v->GetDesc(&vd);
    D3D11_SHADER_TYPE_DESC td{};
    v->GetType()->GetDesc(&td);
    bool tex = false, smp = false;
    for (UINT i = 0; i < sd.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC b;
        refl->GetResourceBindingDesc(i, &b);
        tex |= b.Type == D3D10_SIT_TEXTURE && !strcmp(b.Name, "s_base") && b.BindPoint == 0;
        smp |= b.Type == D3D10_SIT_SAMPLER && !strcmp(b.Name, "smp_base");
    }
    const bool ok = sd.ConstantBuffers == 1 && bd.Size == 32 && bd.Variables == 2 && !strcmp(bd.Name, "ObjPS") && vd.StartOffset == 16 &&
                    !strcmp(vd.Name, "uvScale") && td.Class == D3D10_SVC_VECTOR && td.Type == D3D10_SVT_FLOAT && td.Columns == 4 && tex && smp;
    report("reflection: ID3D11ShaderReflection answers R4's constant-table queries  %s", ok ? "PASS" : "FAIL");
    refl->Release();
    return ok;
}

// R4 switches render targets constantly: bind, clear, draw, "unbind (0,0,0)", bind again. The shim must keep one Metal
// pass for repeated binds of the same set and fold clears into load actions.
bool passEconomyCheck(App& a)
{
    ID3D11DeviceContext1* c = a.ctx;
    ResetFrameStats(c);
    Stats before;
    GetStats(c, &before);
    a.uiPos = App::kUiVBSize;
    float screen[4] = {2.0f / a.W, -2.0f / a.H, 0, 0};
    writeCB(a, a.cbUI, screen);
    bindUIState(a, a.bsOpaque, a.dsNoDepth);
    setViewport(a, a.W, a.H);
    const float col[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    ID3D11RenderTargetView* A = a.albedo.rtv;
    ID3D11RenderTargetView* B = a.scene.rtv;
    for (int i = 0; i < 3; ++i)
    {
        c->OMSetRenderTargets(0, nullptr, nullptr); // unbind idiom
        c->OMSetRenderTargets(1, &A, nullptr);
        if (i == 0)
            c->ClearRenderTargetView(A, col);
        drawQuad(a, 10, 10, 50, 50, 0.5f, col, a.srvWhite, a.smpPoint);
    }
    Stats mid;
    GetStats(c, &mid);
    const uint64_t passesSameSet = mid.renderPasses - before.renderPasses;
    // switch to B (cleared), back to A (cleared again while B was active): each is one pass, clears are free
    c->OMSetRenderTargets(1, &B, nullptr);
    c->ClearRenderTargetView(B, col);
    drawQuad(a, 10, 10, 50, 50, 0.5f, col, a.srvWhite, a.smpPoint);
    c->ClearRenderTargetView(A, col);
    c->OMSetRenderTargets(1, &A, nullptr);
    drawQuad(a, 10, 10, 50, 50, 0.5f, col, a.srvWhite, a.smpPoint);
    Stats after;
    GetStats(c, &after);
    const uint64_t total = after.renderPasses - before.renderPasses;
    const uint64_t folded = after.foldedClears - before.foldedClears;
    const bool ok = passesSameSet == 1 && total == 3 && folded == 3 && after.materializedClears == before.materializedClears;
    report("pass economy: 3 binds of one target set -> %llu pass; +2 target switches with clears -> %llu passes, %llu clears folded, "
           "%llu materialised  %s",
           (unsigned long long)passesSameSet, (unsigned long long)total, (unsigned long long)folded,
           (unsigned long long)(after.materializedClears - before.materializedClears), ok ? "PASS" : "FAIL");
    c->OMSetRenderTargets(0, nullptr, nullptr);
    return ok;
}

bool writePNG(const std::string& path, const CaptureResult& r)
{
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, r.rgba.data(), r.rgba.size(), nullptr);
    CGImageRef img = CGImageCreate(r.width, r.height, 8, 32, r.width * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp,
                                   nullptr, false, kCGRenderingIntentDefault);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dest && img)
    {
        CGImageDestinationAddImage(dest, img, nullptr);
        ok = CGImageDestinationFinalize(dest);
    }
    if (dest) CFRelease(dest);
    if (img) CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    return ok;
}

void verifyCapture(App& a, const CaptureResult& cap)
{
    const Swatch sw[] = {
        {"solid colour", {255, 0, 0}, 3},
        {"alpha blend 50% green over blue", {0, 128, 128}, 3},
        {"point-sampled texture (blue cell)", {40, 40, 200}, 3},
        {"offscreen RT cleared, sampled later", {0, 0, 255}, 3},
        {"4x MSAA resolve + CopyResource", {255, 128, 0}, 3},
        {"depth test (near yellow wins)", {255, 255, 0}, 3},
        {"compute shader writing a UAV", {255, 255, 0}, 3},
        {"staging texture -> CopyResource", {0, 255, 255}, 3},
        {"dynamic texture Map(DISCARD)", {255, 0, 255}, 3},
        {"BC1 texture", {255, 130, 0}, 10},
        {"DDS via D3DX11CreateTextureFromMemory", {0, 203, 99}, 10},
        {"DDS loaded as STAGING then CopyResource", {0, 203, 99}, 10},
        {"cube map, +X face", {200, 50, 50}, 3},
        {"cube map, -Z face", {50, 200, 200}, 3},
        {"3D texture, slice 2", {0, 0, 255}, 3},
        {"UpdateSubresource on a DEFAULT texture", {255, 128, 255}, 3},
    };
    const int sy = a.H - 88;
    int bad = 0;
    for (size_t i = 0; i < sizeof(sw) / sizeof(sw[0]); ++i)
    {
        // Pixel inside the swatch; for the checker pick the centre of the top-right quadrant.
        int px = 16 + int(i) * 80 + 32, py = sy + 32;
        if (i == 2) { px = 16 + 2 * 80 + 16; py = sy + 16; } // top-left quadrant: the blue cell
        const uint8_t* p = &cap.rgba[(size_t(py) * cap.width + px) * 4];
        bool ok = true;
        for (int k = 0; k < 3; ++k)
            if (std::abs(int(p[k]) - int(sw[i].expect[k])) > sw[i].tol)
                ok = false;
        report("swatch %zu %-38s got (%3d,%3d,%3d) want (%3d,%3d,%3d)  %s", i, sw[i].name, p[0], p[1], p[2], sw[i].expect[0],
             sw[i].expect[1], sw[i].expect[2], ok ? "PASS" : "FAIL");
        bad += !ok;
    }
    // Scene sanity: the lit scene must not be blank or uniform.
    double sum = 0, sum2 = 0;
    size_t n = 0;
    for (int y = 100; y < a.H - 120; y += 4)
        for (int x = 0; x < a.W; x += 4)
        {
            const uint8_t* p = &cap.rgba[(size_t(y) * cap.width + x) * 4];
            const double l = 0.3 * p[0] + 0.59 * p[1] + 0.11 * p[2];
            sum += l; sum2 += l * l; ++n;
        }
    const double mean = sum / n, var = sum2 / n - mean * mean;
    const bool sceneOK = mean > 15 && mean < 240 && var > 200;
    report("scene luminance mean %.1f variance %.1f  %s", mean, var, sceneOK ? "PASS" : "FAIL");
    bad += !sceneOK;
    const bool occOK = a.occHiddenSamples == 0 && a.occVisibleSamples > 100;
    report("occlusion: hidden cube %llu samples, visible cube %llu samples  %s", (unsigned long long)a.occHiddenSamples,
         (unsigned long long)a.occVisibleSamples, occOK ? "PASS" : "FAIL");
    bad += !occOK;
    a.failures = bad;
}
} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
    SDL_SetAppMetadata("Metal D3D11 Lab", "0.1", "com.andriishafar.anomaly.metallab");
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    App* a = new App;
    *appstate = a;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) a->maxFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) a->screenshot = argv[++i];
    }
    a->captureFrame = std::max(a->maxFrames / 2, 2);

    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL;
#if TARGET_OS_IOS
    flags |= SDL_WINDOW_FULLSCREEN;
#endif
    a->window = SDL_CreateWindow("Metal D3D11 Lab", 1280, 800, flags);
    if (!a->window)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    a->view = SDL_Metal_CreateView(a->window);
    CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(a->view);
    SDL_GetWindowSizeInPixels(a->window, &a->W, &a->H);
    SetDisplayMode(UINT(a->W), UINT(a->H), 60);

    // Device, swap chain: the same calls CHW makes.
    ID3D11Device* d0 = nullptr;
    ID3D11DeviceContext* c0 = nullptr;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                 D3D11_SDK_VERSION, &d0, &fl, &c0)))
        return SDL_APP_FAILURE;
    d0->QueryInterface(&a->dev);
    c0->QueryInterface(&a->ctx);
    d0->Release();
    c0->Release();
    IDXGIFactory2* factory = nullptr;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    RegisterWindowLayer(a->window, (__bridge void*)layer);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = a->W; sd.Height = a->H; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsd{};
    fsd.Windowed = TRUE;
    if (FAILED(factory->CreateSwapChainForHwnd(a->dev, a->window, &sd, &fsd, nullptr, &a->swap)))
        return SDL_APP_FAILURE;
    factory->Release();
    a->swap->GetBuffer(0, IID_PPV_ARGS(&a->backTex));
    a->dev->CreateRenderTargetView(a->backTex, nullptr, &a->backRTV);

    if (!createPipeline(*a) || !createResources(*a) || !createTargets(*a))
        return SDL_APP_FAILURE;
    clearSmallRT(*a);
    if (!checkReflection(*a))
        a->failures++;
    report("Metal D3D11 Lab: %dx%d, device %s", a->W, a->H, [((__bridge id<MTLDevice>)GetNativeDevice(a->dev)).name UTF8String]);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
    App& a = *static_cast<App*>(appstate);
    ++a.frame;
    if (a.frame == 1 && !passEconomyCheck(a))
        a.failures++;
    if (!renderFrame(a))
        return SDL_APP_FAILURE;

    if (a.captureRequested)
    {
        a.captureRequested = false;
        CaptureResult cap;
        if (WaitForCapture(a.swap, &cap))
        {
            // Occlusion results arrive with the frame's command buffer.
            uint64_t v = 0;
            for (int spin = 0; spin < 1000 && a.ctx->GetData(a.occHidden, &v, 8, 0) == S_FALSE; ++spin) {}
            a.occHiddenSamples = v;
            v = 0;
            for (int spin = 0; spin < 1000 && a.ctx->GetData(a.occVisible, &v, 8, 0) == S_FALSE; ++spin) {}
            a.occVisibleSamples = v;
            verifyCapture(a, cap);
            Stats st;
            GetStats(a.ctx, &st);
            report("stats (cumulative, %d frames): %llu command buffers, %llu render passes (%llu clears folded into load actions, "
                 "%llu materialised), %llu compute, %llu blit, %llu draws, %llu pipelines created / %llu hits",
                 a.frame, (unsigned long long)st.commandBuffers, (unsigned long long)st.renderPasses, (unsigned long long)st.foldedClears,
                 (unsigned long long)st.materializedClears, (unsigned long long)st.computePasses, (unsigned long long)st.blitPasses,
                 (unsigned long long)st.draws, (unsigned long long)st.pipelineCreates, (unsigned long long)st.pipelineHits);
            report("memory: textures %.1f MB, buffers %.1f MB, device allocated %.1f MB", st.textureBytes / 1048576.0,
                 st.bufferBytes / 1048576.0, st.deviceAllocatedBytes / 1048576.0);
            std::string path = a.screenshot.empty() ? std::string("metal_d3d11_test.png") : a.screenshot;
#if TARGET_OS_IOS
            char* pref = SDL_GetPrefPath("andriishafar", "metallab");
            path = std::string(pref ? pref : "") + "metal_d3d11_test.png";
            SDL_free(pref);
#endif
            if (writePNG(path, cap))
                report("screenshot written to %s", path.c_str());
            else
                report("screenshot FAILED to write %s", path.c_str());
            report("RESULT: %s (%d failing checks)", a.failures ? "FAIL" : "PASS", a.failures);
#if TARGET_OS_IOS
            char* pref2 = SDL_GetPrefPath("andriishafar", "metallab");
            FILE* f = fopen((std::string(pref2 ? pref2 : "") + "report.txt").c_str(), "w");
            if (f) { fwrite(g_report.data(), 1, g_report.size(), f); fclose(f); }
            SDL_free(pref2);
#endif
        }
        else
        {
            report("capture failed");
            a.failures = 1;
        }
    }
    if (a.frame >= a.maxFrames)
        return a.failures ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void*, SDL_Event* event)
{
    if (event->type == SDL_EVENT_QUIT || (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE))
        return SDL_APP_SUCCESS;
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    (void)result;
    App* a = static_cast<App*>(appstate);
    if (!a)
        return;
    if (a->ctx)
        a->ctx->Flush();
    // Resources are released with the process; the interesting part is that nothing crashes on shutdown.
    if (a->view)
        SDL_Metal_DestroyView(a->view);
    if (a->window)
        SDL_DestroyWindow(a->window);
    delete a;
    SDL_Quit();
}
