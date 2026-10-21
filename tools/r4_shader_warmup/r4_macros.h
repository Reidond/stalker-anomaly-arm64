// r4_macros: the D3D_SHADER_MACRO list CRender::shader_compile builds, evaluated with the renderer's default
// settings and without the engine. The code is not a copy: r4_macros.cpp textually includes
// src/Layers/xrRenderPC_R4/r4_shader_macros.inl, the block r4.cpp itself includes, and only supplies stand-ins for the
// renderer state it reads (see the table in r4_macros.cpp).
#pragma once

#include <string>
#include <utility>
#include <vector>

struct R4Macros
{
    std::vector<std::pair<std::string, std::string>> defines; // Name, Definition
    std::string cache_id;                                     // `sh_name`: the id that names the engine's cache file
    std::string target;                                       // shader model after the feature-level selection
};

// `name`: the shader name as CResourceManager passes it (with the "_N" suffix of skinned vertex shaders),
// `entry`: "main" (or main_vs_2_0 ...), `target`: the profile it asks for ("vs_2_0", "ps_3_0", "cs_5_0"),
// `skinning`: CRender::m_skinning (-1, or 0..4).
R4Macros r4_default_macros(const char* name, const char* entry, const char* target, int skinning);
