#include "profiles.h"

namespace batch
{
using shaderxlat::Macro;

namespace
{
// Defines r4.cpp adds unconditionally on D3D11.
std::vector<Macro> baseDefines()
{
    return {{"FP16_FILTER", "1"}, {"FP16_BLEND", "1"}, {"USE_HWSMAP", "1"}, {"USE_HWSMAP_PCF", "1"},
            {"USE_BRANCHING", "1"}, {"USE_VTF", "1"}, {"SM_5", "1"}, {"SSFX_MODEXE", "1"}, {"SHADER_BUS", "7"}};
}

void add(std::vector<Macro>& v, const char* n, const char* val = "1") { v.push_back({n, val}); }

// SSFX_* macros are always defined, clamped to the console ranges.
void ssfx(std::vector<Macro>& v, int ssr, int water, int waterParallax, int il, int ao, int pom, int terrainPom, int sssDir,
          int sssOmni)
{
    add(v, "SSFX_SSR_QUALITY", std::to_string(ssr).c_str());
    add(v, "SSFX_WATER_QUALITY", std::to_string(water).c_str());
    add(v, "SSFX_WATER_PARALLAX", std::to_string(waterParallax).c_str());
    add(v, "SSFX_IL_QUALITY", std::to_string(il).c_str());
    add(v, "SSFX_AO_QUALITY", std::to_string(ao).c_str());
    add(v, "SSFX_POM_REFINE", std::to_string(pom).c_str());
    add(v, "SSFX_TERRA_POM_REFINE", std::to_string(terrainPom).c_str());
    add(v, "SSFX_SSS_DIR_QUALITY", std::to_string(sssDir).c_str());
    add(v, "SSFX_SSS_OMNI_QUALITY", std::to_string(sssOmni).c_str());
}
} // namespace

std::vector<Macro> withSkin(std::vector<Macro> m, int skin)
{
    if (skin < 0)
        add(m, "SKIN_NONE");
    else
        add(m, ("SKIN_" + std::to_string(skin)).c_str());
    return m;
}

std::vector<Profile> allProfiles()
{
    std::vector<Profile> out;

    {   // Everything advanced off: the "low" preset.
        Profile p{"low", baseDefines()};
        add(p.macros, "SMAP_size", "1024");
        ssfx(p.macros, 0, 0, 0, 16, 2, 0, 0, 1, 1);
        out.push_back(std::move(p));
    }
    {   // Plain SSAO (mode "default"), soft effects, SMAA medium.
        Profile p{"medium", baseDefines()};
        add(p.macros, "SMAP_size", "2048");
        add(p.macros, "USE_SOFT_WATER");
        add(p.macros, "USE_SOFT_PARTICLES");
        add(p.macros, "USE_DOF");
        add(p.macros, "SUN_SHAFTS_QUALITY", "2");
        add(p.macros, "SSAO_QUALITY", "2");
        add(p.macros, "SUN_QUALITY", "1");
        add(p.macros, "ALLOW_STEEPPARALLAX");
        add(p.macros, "USE_REFLECTIONS");
        add(p.macros, "SMAA_QUALITY", "2");
        add(p.macros, "USE_MBLUR");
        ssfx(p.macros, 2, 1, 1, 24, 3, 1, 1, 8, 4);
        out.push_back(std::move(p));
    }
    {   // HBAO with blur and vectorised code, high sun quality.
        Profile p{"high_hbao", baseDefines()};
        add(p.macros, "SMAP_size", "2048");
        add(p.macros, "USE_SOFT_WATER");
        add(p.macros, "USE_SOFT_PARTICLES");
        add(p.macros, "USE_DOF");
        add(p.macros, "SUN_SHAFTS_QUALITY", "3");
        add(p.macros, "SSAO_QUALITY", "3");
        add(p.macros, "SUN_QUALITY", "3");
        add(p.macros, "ALLOW_STEEPPARALLAX");
        add(p.macros, "USE_REFLECTIONS");
        add(p.macros, "SMAA_QUALITY", "3");
        add(p.macros, "USE_MBLUR");
        add(p.macros, "USE_SSAO_BLUR");
        add(p.macros, "SSAO_OPT_DATA", "1");
        add(p.macros, "VECTORIZED_CODE");
        add(p.macros, "USE_HBAO");
        add(p.macros, "SSFX_RAIN_QUALITY", "2");
        add(p.macros, "SSFX_INT_GRASS", "5");
        ssfx(p.macros, 3, 2, 2, 32, 4, 1, 1, 12, 6);
        out.push_back(std::move(p));
    }
    {   // HDAO (the default ssao mode), everything at maximum, min/max shadow maps.
        Profile p{"ultra_hdao", baseDefines()};
        add(p.macros, "SMAP_size", "4096");
        add(p.macros, "USE_SOFT_WATER");
        add(p.macros, "USE_SOFT_PARTICLES");
        add(p.macros, "USE_DOF");
        add(p.macros, "SUN_SHAFTS_QUALITY", "3");
        add(p.macros, "SSAO_QUALITY", "3");
        add(p.macros, "SUN_QUALITY", "4");
        add(p.macros, "ALLOW_STEEPPARALLAX");
        add(p.macros, "USE_REFLECTIONS");
        add(p.macros, "SMAA_QUALITY", "4");
        add(p.macros, "USE_MBLUR");
        add(p.macros, "USE_SSAO_BLUR");
        add(p.macros, "HDAO");
        add(p.macros, "USE_MINMAX_SM");
        add(p.macros, "SSFX_RAIN_QUALITY", "2");
        add(p.macros, "SSFX_INT_GRASS", "15");
        ssfx(p.macros, 5, 4, 3, 64, 8, 1, 1, 24, 12);
        out.push_back(std::move(p));
    }
    {   // SSAO "ultra" (quality 4) with HDAO: the only configuration that uses the HDAO compute shader.
        // Not part of the default sweep: ssao.ps has no branch for quality 4, see docs/ios-port/SHADERS.md.
        Profile p{"ssao4_hdao", baseDefines()};
        add(p.macros, "SMAP_size", "2048");
        add(p.macros, "SUN_QUALITY", "1");
        add(p.macros, "SSAO_QUALITY", "4");
        add(p.macros, "USE_SSAO_BLUR");
        add(p.macros, "HDAO");
        ssfx(p.macros, 2, 1, 1, 24, 3, 1, 1, 8, 4);
        out.push_back(std::move(p));
    }
    {   // MSAA on (4 samples, optimisation off): not part of the default sweep, run with --profile msaa4.
        Profile p{"msaa4", baseDefines()};
        add(p.macros, "SMAP_size", "2048");
        add(p.macros, "SUN_QUALITY", "1");
        add(p.macros, "SSAO_QUALITY", "2");
        add(p.macros, "USE_MSAA");
        add(p.macros, "MSAA_SAMPLES", "4");
        ssfx(p.macros, 2, 1, 1, 24, 3, 1, 1, 8, 4);
        out.push_back(std::move(p));
    }
    return out;
}
} // namespace batch
