// ShaderXlat WGSL back end: the SPIR-V that translate() produces (Options::keep_spirv) -> WGSL for WebGPU, with Tint
// (Dawn's shader compiler) as the SPIR-V reader, WGSL writer and validator.
//
//   HLSL --[vkd3d-shader]--> DXBC --[vkd3d-shader]--> SPIR-V --[patch]--> SPIR-V --[Tint]--> WGSL --[Tint]--> valid?
//
// Built only with -DANOMALY_SHADERXLAT_WGSL=ON (CMake target ShaderXlatWgsl, needs external/dawn); the default build
// has no Tint. Results are for ahead-of-time translation on the Mac: the browser never runs vkd3d.
//
// The patch step rewrites SPIR-V decorations:
//   * descriptor bindings: D3D registers are per stage, WebGPU bindings per pipeline layout, so every stage gets its
//     own bind group (BindingLayout) and keeps the register number inside it;
//   * interstage locations: D3D links vertex outputs to pixel inputs by semantic, WebGPU by @location, so the
//     caller passes a register -> location map built from the semantics (see interstageLocation() in shader_batch);
//   * storage-image texel formats: D3D UAVs carry no format in the shader, WGSL needs one.
// and rewrites one vkd3d pattern Tint rejects: OpIsNan/OpIsInf become integer tests on the float's bits.
//
// Tint aborts the process on an internal compiler error, so batch callers should run fromSpirv() in a child process
// (tools/shader_batch does).
#pragma once

#include "ShaderXlat/ShaderXlat.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace shaderxlat::wgsl
{
// WebGPU core default limits (the values every adapter must support; createShaderModule/createPipeline checks them
// against the device's limits). Raise them only to what a device was created with.
struct Limits
{
    uint32_t maxBindGroups = 4;
    uint32_t maxBindingsPerBindGroup = 1000;
    uint32_t maxInterStageShaderVariables = 16;
    uint32_t maxVertexAttributes = 16;
    uint32_t maxColorAttachments = 8;
    uint32_t maxUniformBuffersPerShaderStage = 12;
    uint32_t maxSampledTexturesPerShaderStage = 16;
    uint32_t maxSamplersPerShaderStage = 16;
    uint32_t maxStorageTexturesPerShaderStage = 4;
    uint32_t maxComputeInvocationsPerWorkgroup = 256;
    uint32_t maxComputeWorkgroupSizeX = 256;
    uint32_t maxComputeWorkgroupSizeY = 256;
    uint32_t maxComputeWorkgroupSizeZ = 64;
    uint32_t maxComputeWorkgroupStorageSize = 16384;
};

// @group(g) @binding(base + register): one group per stage, register classes at fixed bases inside it. The bases
// leave room for D3D11's per-stage maxima the engine can reach (b0..b13, t0..t31, s0..s15, u0..u7).
struct BindingLayout
{
    uint32_t vertex_group = 0;
    uint32_t pixel_group = 1;
    uint32_t compute_group = 0;
    uint32_t cbuffer_base = 0;  // b#  -> 0..15
    uint32_t texture_base = 16; // t#  -> 16..47
    uint32_t sampler_base = 48; // s#  -> 48..63
    uint32_t uav_base = 64;     // u#  -> 64..
};

struct Options
{
    Stage stage = Stage::Pixel;
    // Emit `diagnostic(off, derivative_uniformity);`. HLSL allows derivatives (implicit-LOD sampling, ddx/ddy) in
    // non-uniform control flow; WGSL's uniformity analysis rejects them unless this is off. Valid WGSL either way.
    bool allow_non_uniform_derivatives = true;
    bool remap_bindings = true;
    BindingLayout layout;
    // Interstage variables (vertex outputs, pixel inputs): SPIR-V Location (= D3D signature register, as vkd3d emits
    // it) -> WGSL @location. Registers not listed keep their number.
    std::vector<std::pair<uint32_t, uint32_t>> locations;
    // Rewrite OpIsNan/OpIsInf (which Tint rejects) as tests on the float's bits. vkd3d emits them for every D3D
    // float -> int conversion (NaN converts to 0), isfinite() and the ord/unord comparisons.
    bool polyfill_float_classification = true;
    // UAV register u# -> WGSL texel format ("r32float", "r16float", "rgba16float", ...) for storage images that the
    // SPIR-V declares with an Unknown format.
    std::vector<std::pair<uint32_t, std::string>> uav_formats;
    Limits limits;
};

enum class Failure : uint8_t
{
    None,
    NotBuilt,     // ShaderXlat built without ANOMALY_SHADERXLAT_WGSL
    SpirvPatch,   // the SPIR-V could not be patched (malformed, or an unknown UAV format was needed)
    SpirvReader,  // Tint's SPIR-V reader rejected the module
    IrToProgram,  // Tint could not raise its IR to a WGSL program
    WgslWriter,   // Tint could not print the program
    Validation,   // the printed WGSL does not parse/resolve (what createShaderModule does in the browser)
    Lowering,     // the WGSL resolves but does not lower to Tint IR (what Dawn does before backend codegen)
};

const char* failureName(Failure f);

// One resource as the validated WGSL declares it.
struct Binding
{
    uint32_t group = 0;
    uint32_t binding = 0;
    std::string name;      // WGSL variable name
    std::string kind;      // uniform, sampler, sampler_comparison, texture, depth_texture, multisampled_texture,
                           // storage_texture_{read,write,read_write}, storage_buffer, ...
    std::string dim;       // 1d, 2d, 2d_array, 3d, cube, cube_array, "" for non-textures
    std::string sampled;   // f32, i32, u32, unfilterable_f32 (textures)
    std::string format;    // storage textures
    char d3d_class = '?';  // 'b', 't', 's', 'u' (from the binding number, when remap_bindings)
    uint32_t d3d_register = 0;
};

struct SamplerUse
{
    uint32_t sampler_group = 0, sampler_binding = 0;
    uint32_t texture_group = 0, texture_binding = 0;
};

struct StageVar
{
    uint32_t location = 0;
    std::string type; // f32, vec2<f32>, vec4<u32>, ...
    std::string interpolation;
};

struct Result
{
    bool ok = false;           // valid WGSL that also lowers to IR (the per-shader part of what Chrome checks)
    Failure failure = Failure::None;
    std::string log;           // Tint diagnostics, one per line
    std::string wgsl;
    std::string entry_point;   // WGSL function name

    std::vector<std::string> enables;         // `enable X;` directives in the output
    bool derivative_uniformity_needed = false; // the WGSL does not validate without diagnostic(off, derivative_uniformity)
    std::vector<Binding> bindings;
    std::vector<SamplerUse> sampler_uses;     // sampler/texture pairs used by textureSample* / textureGather*
    std::vector<StageVar> inputs, outputs;    // @location inputs and outputs of the entry point
    bool depth_texture_with_non_comparison_sampler = false;
    bool texture_load_with_depth_texture = false;
    bool uses_discard = false;
    uint32_t clip_distances = 0;              // size of the clip_distances array, 0 when unused
    uint32_t workgroup[3] = {0, 0, 0};
    uint32_t workgroup_storage_bytes = 0;
    // Entry-point checks Dawn makes against the device limits (createShaderModule / pipeline creation), using
    // Options::limits. Not part of `ok`: they depend on the device the page requests.
    std::vector<std::string> limit_errors;
    // SPIR-V rewrites applied before Tint (patchSpirv)
    uint32_t float_classification_rewrites = 0; // OpIsNan/OpIsInf turned into bit tests
    uint32_t nonfinite_constants = 0;           // Inf/NaN float constants turned into run-time values
    double ms = 0;
};

// What patchSpirv() changed beyond decorations.
struct PatchStats
{
    uint32_t float_classification_rewrites = 0;
    uint32_t nonfinite_constants = 0;
};

// True when this build has the WGSL back end.
bool available();
// "Tint <dawn revision>".
const char* tintVersion();

// Patches the decorations described above. Returns false (with `error`) on malformed SPIR-V.
bool patchSpirv(std::vector<uint32_t>& spirv, const Options& options, std::string& error, PatchStats* stats = nullptr);

// SPIR-V (from translate() with keep_spirv) -> validated WGSL.
Result fromSpirv(const std::vector<uint32_t>& spirv, const Options& options);
} // namespace shaderxlat::wgsl
