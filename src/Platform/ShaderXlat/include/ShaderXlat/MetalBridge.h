// ShaderXlatMetal: connects ShaderXlat (HLSL to MSL) to the D3DCompile hook of the Metal D3D11 shim.
//
// R4 calls D3DCompile(source, defines, include, "main", "ps_5_0", flags, ...) and expects "bytecode" it can hand to
// CreatePixelShader, D3DReflect and D3DGetInputSignatureBlob. install() registers a hook with
// mtl_d3d11::SetShaderCompiler() that translates the HLSL with the R4 macro list, turns the D3D-style reflection into
// the shim's container (mtl_shader.h) and returns that. The engine stores the returned bytes in its own shader cache,
// and the TranslationCache below it keeps the MSL and reflection, so a changed container format never forces a
// re-translation.
//
// Geometry, hull and domain shaders fail with E_NOTIMPL and a one-line log message: Metal has no such stages.
#pragma once

#include <TargetConditionals.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ShaderXlat/ShaderXlat.h"
#include <mtl_d3d11/mtl_shader.h>

namespace shaderxlat_metal
{
struct Config
{
    // Directory of the on-disk TranslationCache (created on first use); empty means no disk cache.
    std::string cache_dir;
#if TARGET_OS_IPHONE
    shaderxlat::Platform platform = shaderxlat::Platform::IOS;
#else
    shaderxlat::Platform platform = shaderxlat::Platform::MacOS;
#endif
    // Receives failures ("translation failed: ...") and, with log_warnings, the compiler messages of successes.
    std::function<void(const std::string&)> log;
    bool log_warnings = false;
};

struct Stats
{
    uint64_t requests = 0;
    uint64_t cache_hits = 0;
    uint64_t failures = 0;
    uint64_t unsupported_stage = 0;
    double translate_ms = 0; // wall time spent inside translate(), cache hits included
};

// Registers the hook with the shim. Calling it again replaces the configuration (and drops the cache handle).
void install(const Config& config);
// Removes the hook again.
void uninstall();
Stats stats();
void resetStats();

// The pieces the hook is made of, exposed for tools and tests.
shaderxlat::Stage stageFromTarget(const char* target); // "vs_5_0" -> Vertex; unknown prefixes -> Pixel
mtl_d3d11::ShaderStage toShimStage(shaderxlat::Stage stage);
// Reflection as the shim wants it: cbuffer i has d3dSlot == i (holes are filled with placeholders), vertex inputs
// are the ones that survive in the MSL, SV_Target outputs are counted.
mtl_d3d11::ShaderBlob toShaderBlob(const shaderxlat::Result& result, shaderxlat::Stage stage);
// Translates and serializes one request without touching the global hook state. Returns S_OK, E_NOTIMPL (stage
// Metal does not have) or E_FAIL; `errors` gets the compiler log on failure.
HRESULT compile(const mtl_d3d11::CompileRequest& request, std::vector<uint8_t>* container, std::string* errors,
                shaderxlat::Result* detail = nullptr, bool* cache_hit = nullptr);
} // namespace shaderxlat_metal
