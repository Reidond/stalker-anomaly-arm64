// Permutation profiles for the batch compile. They mirror CRender::shader_compile in
// src/Layers/xrRenderPC_R4/r4.cpp for the D3D11 renderer (SM_5, HW shadow maps with PCF, FP16
// render targets, MSAA off unless the profile says otherwise).
#pragma once

#include "ShaderXlat/ShaderXlat.h"

#include <string>
#include <vector>

namespace batch
{
struct Profile
{
    std::string name;
    std::vector<shaderxlat::Macro> macros;
};

// `skin` is CRender::m_skinning: -1 for SKIN_NONE (everything but skinned model vertex shaders), 0..4.
std::vector<shaderxlat::Macro> withSkin(std::vector<shaderxlat::Macro> m, int skin);

std::vector<Profile> allProfiles();
} // namespace batch
