#pragma once

#include "ShaderXlat/ShaderXlat.h"

namespace shaderxlat::detail
{
// SPIR-V descriptor bindings are laid out per D3D register class so a binding number maps straight
// back to a register: set 0, binding = base + register index.
constexpr uint32_t kBindingCbv = 0;
constexpr uint32_t kBindingSrv = 256;
constexpr uint32_t kBindingSampler = 512;
constexpr uint32_t kBindingUav = 768;

struct FrontendInput
{
    const std::string* source = nullptr;
    const std::string* name = nullptr;
    const std::vector<Macro>* macros = nullptr;
    const Options* options = nullptr;
    IncludeProvider* includes = nullptr; // already normalising/tracking
    std::string profile;                 // D3D profile, e.g. "ps_5_0"
};

struct FrontendOutput
{
    std::vector<uint32_t> spirv;
    std::string log;
    bool aliasedSemantics = false; // the duplicate-input-semantic hook was needed (see aliasDuplicateInputSemantics)
    // D3D-style reflection read from the DXBC (names, registers, cbuffer layouts, signatures).
    // msl_index fields and sampler/texture analysis are filled in later by crossCompile().
    Reflection d3d;
};

// HLSL -> DXBC -> SPIR-V (FrontendVkd3d.cpp).
bool frontendCompile(const FrontendInput& in, FrontendOutput& out);

// Parses the RDEF chunk of a DXBC shader into constant buffers and resource bindings.
bool parseRdef(const uint8_t* data, size_t size, Reflection& out, std::string& error);

// FXC-compat source hooks. Rewrites `text` in place and appends the names of the hooks that fired.
void applyFxcCompat(std::string& text, const std::string& path, std::vector<std::string>& applied);

// Preprocessed HLSL only: drops entry-point parameters whose semantic duplicates a (wider) member of a struct parameter and
// defines them from that member. FXC accepts the duplicate, vkd3d-shader stops with E5013. True if it changed the text.
bool aliasDuplicateInputSemantics(std::string& text, const std::string& entry);

// SPIR-V -> MSL plus the SPIRV-Cross derived parts of the reflection. `result.reflection` must
// already hold the frontend's D3D reflection.
bool crossCompile(const std::vector<uint32_t>& spirv, const Options& options, Result& result);

std::string defaultProfile(Stage stage);

inline uint64_t fnv1a64(const void* data, size_t n, uint64_t h = 14695981039346656037ull)
{
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
} // namespace shaderxlat::detail
