// See r4_macros.h. Stand-ins for what CRender::shader_compile reads, with the values the renderer has on a fresh
// install on this port: Metal GPU (feature level 11_0), console defaults of xrRender_console.cpp, no command line
// options. "Default settings" therefore means: no MSAA, no HDR10, plain SSAO (ssao_blur/HBAO/HDAO off), soft water
// and particles, DoF, steep parallax, motion blur, water reflections, 2048 shadow maps with HW PCF, FP16 targets.
#include "xrCore/xrCore.h"

#include <d3d11.h>

#include "r4_macros.h"

#ifndef ECORE_API
#define ECORE_API // xrEngine/stdafx.h: the symbols live in this executable
#endif
#define ENGINE_API
#include "Layers/xrRender/xrRender_console.h"

typedef D3D10_SHADER_MACRO D3D_SHADER_MACRO;

namespace
{
// Layers/xrRenderPC_R4/r4.h: CRender::_options, MSAA_ATEST_*, MMSM_* (only the fields the macro block reads)
enum
{
    MSAA_ATEST_NONE = 0x0,
    MSAA_ATEST_DX10_0_ATOC = 0x1,
    MSAA_ATEST_DX10_1_NATIVE = 0x2,
    MSAA_ATEST_DX10_1_ATOC = 0x3,
};
enum
{
    MMSM_OFF = 0,
    MMSM_ON,
    MMSM_AUTO,
    MMSM_AUTODETECT
};

struct Options
{
    u32 smapsize = 2048;
    u32 HW_smap = 1, HW_smap_PCF = 1, HW_smap_FETCH4 = 0;
    u32 fp16_filter = 1, fp16_blend = 1;
    u32 sjitter = 0, Tshadows = 0, sunfilter = 0, sunstatic = 0, forceskinw = 0, advancedpp = 0;
    u32 ssao_blur_on = 0, ssao_hdao = 0, ssao_hbao = 0, ssao_half_data = 0, hbao_vectorized = 0;
    u32 dx10_msaa = 0, dx10_msaa_opt = 0, dx10_msaa_samples = 1, dx10_msaa_alphatest = 0, dx10_minmax_sm = 0;
    u32 dx10_sm4_1 = 0;
};

// Layers/xrRender/hwcaps.h as CHWCaps::Update() fills it for D3D11 (raster_major = 4, bVTF = TRUE); Apple GPU vendor id
struct StandInCaps
{
    struct
    {
        u32 bVTF = 1;
    } geometry;
    u32 raster_major = 4;
    u32 id_vendor = 0x106b;
};
struct StandInHW
{
    StandInCaps Caps;
    D3D_FEATURE_LEVEL FeatureLevel = D3D_FEATURE_LEVEL_11_0;
} HW;
struct StandInRender
{
    Options o;
} RImplementation;
} // namespace

// Console defaults, Layers/xrRender/xrRender_console.cpp (line numbers of this checkout in the comments)
u32 ps_r_ssao = 3;                 // 55
u32 ps_r_sun_quality = 1;          // 67
u32 ps_r_sun_shafts = 2;           // 28
u32 ps_sunshafts_mode = 2;         // 36
u32 ps_smaa_quality = 3;           // 45
u32 ps_r3_msaa = 0;                // 79
u32 ps_r3_msaa_atest = 0;          // 88
u32 ps_r3_minmax_sm = 3;           // 96
Flags32 ps_r2_ls_flags = {R2FLAG_SUN | R2FLAG_EXP_DONT_TEST_UNSHADOWED | R2FLAG_USE_NVSTENCIL | R2FLAG_EXP_SPLIT_SCENE |
                          R2FLAG_EXP_MT_CALC | R3FLAG_DYN_WET_SURF | R3FLAG_VOLUMETRIC_SMOKE | R2FLAG_DETAIL_BUMP |
                          R2FLAG_DOF | R2FLAG_SOFT_PARTICLES | R2FLAG_SOFT_WATER | R2FLAG_STEEP_PARALLAX |
                          R2FLAG_SUN_FOCUS | R2FLAG_SUN_TSM | R2FLAG_TONEMAP | R2FLAG_VOLUMETRIC_LIGHTS}; // 171
Flags32 ps_r2_ls_flags_ext = {R2FLAGEXT_SSAO_HALF_DATA | R2FLAGEXT_ENABLE_TESSELLATION};                  // 192
Flags32 ps_r2_anomaly_flags = {R2_AN_FLAG_WATER_REFLECTIONS | R2_AN_FLAG_MBLUR | R2_AN_FLAG_FLARES};     // 198
int ps_r4_hdr10_on = 0;                                                                                   // 308
int ps_ssfx_pom_refine = 0;                                                                               // 394
int ps_ssfx_terrain_pom_refine = 0;                                                                       // 400
Fvector4 ps_ssfx_sss_quality = {12.0f, 4.0f, 1.0f, 1.0f};                                                 // 406
int ps_ssfx_il_quality = 32;                                                                              // 411
int ps_ssfx_ao_quality = 4;                                                                               // 415
Fvector3 ps_ssfx_water_quality = {1.0, 2.0, 0.0};                                                         // 420
int ps_ssfx_ssr_quality = 0;                                                                              // 424
Fvector4 ps_ssfx_rain_1 = {2.0f, 0.1f, 0.6f, 2.f};                                                        // 458
Fvector4 ps_ssfx_grass_interactive = {.0f, .0f, 2000.0f, 1.0f};                                          // 466
// xrEngine/xr_ioc_cmd.cpp: renderer_r4 sets r2_advanced_pp, not r2_sun_static
BOOL r2_sun_static = FALSE;
BOOL r2_advanced_pp = TRUE;

// xrEngine/shader_bus.cpp:1203
namespace ShaderBus
{
int version() { return 7; }
} // namespace ShaderBus

namespace
{
// CRender::create(), r4.cpp: the part that turns the settings above into `o`
Options default_options()
{
    Options o;
    o.sunstatic = r2_sun_static;
    o.advancedpp = r2_advanced_pp;
    o.ssao_blur_on = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_BLUR) && (ps_r_ssao != 0);
    u32 ssao_opt_data = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_OPT_DATA) && (ps_r_ssao != 0);
    o.ssao_half_data = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_HALF_DATA) && ssao_opt_data && (ps_r_ssao != 0);
    o.ssao_hdao = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_HDAO) && (ps_r_ssao != 0);
    o.ssao_hbao = !o.ssao_hdao && ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_HBAO) && (ps_r_ssao != 0);
    o.hbao_vectorized = false;
    o.dx10_sm4_1 = ps_r2_ls_flags.test((u32)R3FLAG_USE_DX10_1) && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1);
    const bool hdr10 = !!ps_r4_hdr10_on;
    o.dx10_msaa = ps_r3_msaa && !hdr10;
    o.dx10_msaa_samples = hdr10 ? 1 : (1 << ps_r3_msaa);
    o.dx10_msaa_opt = ps_r2_ls_flags.test(R3FLAG_MSAA_OPT);
    o.dx10_msaa_opt = (o.dx10_msaa_opt && o.dx10_msaa && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1)) ||
                      (o.dx10_msaa && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0));
    o.dx10_minmax_sm = ps_r3_minmax_sm;
    if (o.dx10_minmax_sm == MMSM_AUTODETECT)
        o.dx10_minmax_sm = MMSM_OFF; // only AMD (0x1002) and NVIDIA (0x10DE) boards switch it on
    return o;
}
} // namespace

R4Macros r4_default_macros(const char* name, const char* pFunctionName, const char* pTarget, int m_skinning)
{
    RImplementation.o = default_options();
    const Options& o = RImplementation.o;
    xr_vector<D3D_SHADER_MACRO> m_ShaderOptions; // CRender::addShaderOption: nothing is added by default

#include "Layers/xrRenderPC_R4/r4_shader_macros.inl"

    R4Macros out;
    for (int i = 0; i < def_it && defines[i].Name; ++i) // def_it counts the terminating {0, 0} entry
        out.defines.emplace_back(defines[i].Name, defines[i].Definition ? defines[i].Definition : "");
    out.cache_id = sh_name;
    out.target = pTarget;
    return out;
}
