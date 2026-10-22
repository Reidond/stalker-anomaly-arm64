// Smoke test for ShaderXlat: translation, reflection, includes, cache, retargeting and the failure
// paths, on small inline shaders (no game data needed).
#include "ShaderXlat/ShaderXlat.h"

#include <cstdio>
#include <filesystem>
#include <map>

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

struct MapIncludes final : IncludeProvider
{
    std::map<std::string, std::string> files;
    int opens = 0;
    bool open(const std::string& name, const std::string&, std::string& resolved, std::string& contents) override
    {
        ++opens;
        auto it = files.find(name);
        if (it == files.end())
            return false;
        resolved = name;
        contents = it->second;
        return true;
    }
};

static const char* kPixel = R"(
#include "common.h"
Texture2D s_base;
sampler smp_base;
Texture2D<float> s_smap : register(ps, t3);
SamplerComparisonState smp_smap;
cbuffer dynamic_light { float4 Ldynamic_color; float3 Ldynamic_pos; float range; row_major float4x4 m_shadow; float4 arr[3]; }
struct v2p { float4 hpos : SV_Position; float2 tc : TEXCOORD0; float4 c : COLOR0; };
float4 main(v2p I) : SV_Target
{
    float sh = s_smap.SampleCmpLevelZero(smp_smap, I.tc, 0.5);
    return s_base.Sample(smp_base, I.tc) * Ldynamic_color * sh * I.c * SCALE + arr[2];
}
)";

int main()
{
    MapIncludes inc;
    inc.files["common.h"] = "#define SCALE 2.0\n";

    Options o;
    o.stage = Stage::Pixel;
    const std::vector<Macro> macros;

    // ---- translate + reflection
    Result r = translate(kPixel, "selftest.ps", macros, o, &inc);
    if (!r.ok)
        fprintf(stderr, "log: %s\n", r.log.c_str());
    CHECK(r.ok);
    CHECK(r.msl.find("fragment") != std::string::npos);
    CHECK(r.entry_point == "main0");
    CHECK(r.applied_hooks.size() == 1 && r.applied_hooks[0] == "register-stage-qualifier");
    CHECK(r.dependencies.size() == 1 && r.dependencies[0].name == "common.h");
    CHECK(r.reflection.constant_buffers.size() == 1);
    if (!r.reflection.constant_buffers.empty())
    {
        const ConstantBuffer& cb = r.reflection.constant_buffers[0];
        CHECK(cb.name == "dynamic_light" && cb.slot == 0 && cb.msl_index == 0);
        CHECK(cb.members.size() == 5);
        if (cb.members.size() == 5)
        {
            CHECK(cb.members[0].name == "Ldynamic_color" && cb.members[0].offset == 0 && cb.members[0].columns == 4);
            // float3 packs into the register after the float4; `range` fills its 4th component.
            CHECK(cb.members[1].name == "Ldynamic_pos" && cb.members[1].offset == 16);
            CHECK(cb.members[2].name == "range" && cb.members[2].offset == 28);
            CHECK(cb.members[3].name == "m_shadow" && cb.members[3].offset == 32 && cb.members[3].row_major && cb.members[3].size == 64);
            CHECK(cb.members[4].name == "arr" && cb.members[4].offset == 96 && cb.members[4].array_elements == 3 && cb.members[4].array_stride == 16);
        }
        CHECK(cb.size == 144);
    }
    CHECK(r.reflection.textures.size() == 2);
    bool sawSmap = false;
    for (const TextureBinding& t : r.reflection.textures)
        if (t.name == "s_smap")
        {
            sawSmap = true;
            CHECK(t.slot == 3 && t.msl_index == 3 && t.depth);
        }
    CHECK(sawSmap);
    CHECK(r.reflection.samplers.size() == 2);
    for (const SamplerBinding& s : r.reflection.samplers)
        CHECK((s.name == "smp_smap") == s.comparison);
    CHECK(r.reflection.sampler_texture_pairs.size() == 2);
    CHECK(r.reflection.inputs.size() >= 2);

    // ---- vertex shader: semantics, interstage locations that match the pixel stage
    Options ov;
    ov.stage = Stage::Vertex;
    Result v = translate("struct v2p { float4 hpos : SV_Position; float2 tc : TEXCOORD0; float4 c : COLOR0; };"
                         "v2p main(float4 pos : POSITION, float2 tc : TEXCOORD0, float4 c : COLOR0) { v2p O; O.hpos = pos; O.tc = tc; O.c = c; return O; }",
                         "selftest.vs", macros, ov, nullptr);
    CHECK(v.ok);
    CHECK(checkStageInterface(v.reflection, r.reflection).empty());
    int attrs = 0;
    for (const StageVariable& in : v.reflection.inputs)
        if (!in.system_value)
            ++attrs;
    CHECK(attrs == 3);

    // ---- retarget to iOS keeps the reflection and changes the MSL platform bits only
    Options keep = o;
    keep.keep_spirv = true;
    Result k = translate(kPixel, "selftest.ps", macros, keep, &inc);
    CHECK(k.ok && !k.spirv.empty());
    Options ios = o;
    ios.platform = Platform::IOS;
    Result ri = retarget(k, ios);
    CHECK(ri.ok && ri.msl.find("fragment") != std::string::npos);
    CHECK(ri.reflection.constant_buffers.size() == k.reflection.constant_buffers.size());

    // ---- FXC lets an entry parameter reuse a semantic of a struct member of a different width (models_lfo_light_dot_weapons.ps
    // with a static sun: p_flat.tcdh is float4, `float2 tc0 : TEXCOORD0` repeats it); vkd3d-shader reports E5013 without the hook
    Result alias = translate("struct p_flat { float4 tcdh : TEXCOORD0; float3 N : TEXCOORD2; };\n"
                             "float4 main(p_flat I, float4 pos2d : SV_Position, float2 tc0 : TEXCOORD0) : SV_Target\n"
                             "{ return float4(tc0, I.N.x, pos2d.x); }",
                             "alias.ps", macros, o, nullptr);
    if (!alias.ok)
        fprintf(stderr, "log: %s\n", alias.log.c_str());
    CHECK(alias.ok);
    bool sawAlias = false;
    for (const std::string& h : alias.applied_hooks)
        sawAlias |= h == "duplicate-input-semantic-alias";
    CHECK(sawAlias);

    // ---- failure paths
    Result bad = translate("float4 main() : SV_Target { return undefined_identifier; }", "bad.ps", macros, o, nullptr);
    CHECK(!bad.ok && bad.category == Category::FrontendError && !bad.log.empty());
    Options gs = o;
    gs.stage = Stage::Geometry;
    CHECK(translate("x", "x.gs", macros, gs, nullptr).category == Category::UnsupportedStage);
    // vkd3d-shader dereferences NULL on this input; the crash guard must contain it.
    Result crash = translate("float4 main() : SV_Target { float x = 0; for (int i = 0; i < NOPE; ++i) x += NOPE; return x; }", "crash.ps",
                             macros, o, nullptr);
    CHECK(!crash.ok);

    // ---- cache
    const std::string dir = (std::filesystem::temp_directory_path() / "shaderxlat_selftest_cache").string();
    TranslationCache cache(dir);
    cache.clear();
    bool hit = true;
    Result c1 = cache.translate(kPixel, "selftest.ps", macros, o, &inc, &hit);
    CHECK(c1.ok && !hit);
    Result c2 = cache.translate(kPixel, "selftest.ps", macros, o, &inc, &hit);
    CHECK(c2.ok && hit && c2.msl == c1.msl && c2.reflection.constant_buffers.size() == 1);
    CHECK(c2.reflection.constant_buffers[0].members[3].row_major);
    inc.files["common.h"] = "#define SCALE 3.0\n"; // an addon replaced the header
    Result c3 = cache.translate(kPixel, "selftest.ps", macros, o, &inc, &hit);
    CHECK(c3.ok && !hit);
    Result c4 = cache.translate(kPixel, "selftest.ps", {{"UNUSED", "1"}}, o, &inc, &hit);
    CHECK(c4.ok && !hit);
    cache.clear();

    printf("%s: %s\n", version(), g_failures ? "FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
