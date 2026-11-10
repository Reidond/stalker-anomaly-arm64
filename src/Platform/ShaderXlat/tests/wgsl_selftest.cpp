// Smoke test for the WGSL back end (ANOMALY_SHADERXLAT_WGSL): bindings, interstage locations, comparison sampling and
// the SPIR-V rewrites for vkd3d patterns Tint rejects, on small inline shaders (no game data needed).
#include "ShaderXlat/ShaderXlat.h"
#include "ShaderXlat/Wgsl.h"

#include <cstdio>

using namespace shaderxlat;

static int g_failures = 0;
#define CHECK(cond)                                                                      \
    do                                                                                   \
    {                                                                                    \
        if (!(cond))                                                                     \
        {                                                                                \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

static Result hlsl(const char* src, Stage stage)
{
    Options o;
    o.stage = stage;
    o.keep_spirv = true;
    Result r = translate(src, "test", {}, o, nullptr);
    if (!r.ok)
        fprintf(stderr, "translate failed: %s\n", r.log.c_str());
    return r;
}

static const wgsl::Binding* binding(const wgsl::Result& r, const char* kind)
{
    for (const auto& b : r.bindings)
        if (b.kind == kind)
            return &b;
    return nullptr;
}

int main()
{
    CHECK(wgsl::available());

    // Comparison sampling: depth texture + comparison sampler, pixel bindings in group 1 at base + register.
    {
        Result r = hlsl(R"(
Texture2D s_base : register(t0);
sampler smp_base : register(s0);
Texture2D<float> s_smap : register(t3);
SamplerComparisonState smp_smap : register(s2);
cbuffer dynamic_light : register(b1) { float4 Ldynamic_color; }
struct v2p { float4 hpos : SV_Position; float2 tc : TEXCOORD0; };
float4 main(v2p I) : SV_Target
{
    return s_base.Sample(smp_base, I.tc) * Ldynamic_color * s_smap.SampleCmpLevelZero(smp_smap, I.tc, 0.5);
}
)", Stage::Pixel);
        CHECK(r.ok);
        wgsl::Options o;
        o.stage = Stage::Pixel;
        const wgsl::Result w = wgsl::fromSpirv(r.spirv, o);
        CHECK(w.ok);
        const wgsl::Binding* depth = binding(w, "depth_texture");
        const wgsl::Binding* cmp = binding(w, "sampler_comparison");
        CHECK(depth && depth->group == 1 && depth->binding == 16 + 3 && depth->d3d_class == 't' && depth->d3d_register == 3);
        CHECK(cmp && cmp->group == 1 && cmp->binding == 48 + 2);
        const wgsl::Binding* ubo = binding(w, "uniform");
        CHECK(ubo && ubo->binding == 1 && ubo->d3d_class == 'b');
        CHECK(binding(w, "texture") && binding(w, "sampler"));
        CHECK(w.limit_errors.empty());
    }

    // float -> int conversion: vkd3d emits OpIsNan (D3D converts NaN to 0), which Tint rejects unless rewritten.
    {
        Result r = hlsl(R"(
struct v2p { float4 hpos : SV_Position; float2 tc : TEXCOORD0; };
float4 main(v2p I) : SV_Target { int i = (int)(I.tc.x * 7.0); uint u = (uint)I.tc.y; return float4(i, u, 0, 1); }
)", Stage::Pixel);
        CHECK(r.ok);
        wgsl::Options o;
        o.stage = Stage::Pixel;
        o.polyfill_float_classification = false;
        const wgsl::Result raw = wgsl::fromSpirv(r.spirv, o);
        CHECK(!raw.ok && raw.failure == wgsl::Failure::SpirvReader && raw.log.find("IsNan") != std::string::npos);
        o.polyfill_float_classification = true;
        const wgsl::Result fixed = wgsl::fromSpirv(r.spirv, o);
        CHECK(fixed.ok);
        CHECK(fixed.wgsl.find("bitcast<") != std::string::npos);
    }

    // A non-finite literal: Tint asserts on the constant, so it must come from a private u32 at run time.
    {
        Result r = hlsl(R"(
struct v2p { float4 hpos : SV_Position; float2 tc : TEXCOORD0; };
float4 main(v2p I) : SV_Target { return I.tc.x > 0.5 ? asfloat(0x7f800000u) : asfloat(0xffffffffu); }
)", Stage::Pixel);
        CHECK(r.ok);
        wgsl::Options o;
        o.stage = Stage::Pixel;
        const wgsl::Result w = wgsl::fromSpirv(r.spirv, o);
        CHECK(w.ok);
        CHECK(w.wgsl.find("var<private>") != std::string::npos);
    }

    // Interstage locations follow the caller's register -> location map; vertex bindings stay in group 0.
    {
        Result r = hlsl(R"(
cbuffer m : register(b0) { float4x4 m_WVP; }
struct a2v { float4 p : POSITION; float2 tc : TEXCOORD0; };
struct v2p { float4 hpos : SV_Position; float2 tc : TEXCOORD3; float4 c : COLOR0; };
v2p main(a2v I) { v2p O; O.hpos = mul(m_WVP, I.p); O.tc = I.tc; O.c = I.p; return O; }
)", Stage::Vertex);
        CHECK(r.ok);
        wgsl::Options o;
        o.stage = Stage::Vertex;
        for (const auto& v : r.reflection.outputs)
            if (!v.system_value)
                o.locations.push_back({v.reg, v.semantic == "TEXCOORD" ? 3u : 10u});
        const wgsl::Result w = wgsl::fromSpirv(r.spirv, o);
        CHECK(w.ok);
        bool tc = false, color = false;
        for (const auto& v : w.outputs)
        {
            tc |= v.location == 3 && v.type == "vec2<f32>";
            color |= v.location == 10 && v.type == "vec4<f32>";
        }
        CHECK(tc && color);
        CHECK(w.bindings.size() == 1 && w.bindings[0].group == 0 && w.bindings[0].binding == 0);
    }

    // Typed UAV without a format in the shader (the HDAO case): needs the format from the caller.
    {
        Result r = hlsl(R"(
RWTexture2D<float> u_out : register(u0);
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { u_out[id.xy] = 0.5; }
)", Stage::Compute);
        CHECK(r.ok);
        wgsl::Options o;
        o.stage = Stage::Compute;
        const wgsl::Result noFormat = wgsl::fromSpirv(r.spirv, o);
        CHECK(!noFormat.ok);
        o.uav_formats.push_back({0, "r16float"});
        const wgsl::Result w = wgsl::fromSpirv(r.spirv, o);
        CHECK(w.ok);
        const wgsl::Binding* st = binding(w, "storage_texture_write");
        CHECK(st && st->format == "r16float" && st->group == 0 && st->binding == 64);
        CHECK(w.workgroup[0] == 8 && w.workgroup[1] == 8 && w.limit_errors.empty());
    }

    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("wgsl selftest passed (%s)\n", wgsl::tintVersion());
    return g_failures ? 1 : 0;
}
