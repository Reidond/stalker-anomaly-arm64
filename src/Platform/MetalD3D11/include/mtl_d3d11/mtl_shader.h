// Shader interface of the D3D11-on-Metal shim.
//
// Producer: the HLSL -> SPIR-V -> MSL translator (a separate session). Consumer: this shim.
// R4 hands "shader bytecode" to ID3D11Device::Create*Shader(), D3DReflect() and
// D3DGetInputSignatureBlob(). Here that bytecode is a *container* produced by Serialize() below: the
// translator's MSL (or a precompiled .metallib) plus a reflection struct. Because the engine caches the
// bytes it gets back from D3DCompile on disk, the container is the on-disk cache format too.
//
// Binding conventions the translator must follow
// ----------------------------------------------
// The reflection carries an explicit Metal index for every binding, so the shim never guesses. Those
// indices must respect the following layout, which the shim relies on for the parts it owns:
//
//   [[buffer(0 .. 13)]]   constant buffers, b0..b13 -> buffer(b#)            (kMaxCBuffers = 14)
//   [[buffer(14 .. 15)]]  buffer SRVs / UAVs (R4 has none today)               (kFirstResourceBuffer)
//   [[buffer(16 .. 30)]]  vertex streams, input slot n -> buffer(16 + n)       (kVertexBufferBase)
//   [[texture(n)]]        SRV t# -> texture(t#); UAV u# -> texture(kUavTextureBase + u#)
//   [[sampler(n)]]        s# -> sampler(s#)
//
// Vertex inputs use [[attribute(n)]] with the index given in VertexInput::attribute; the shim builds the
// MTLVertexDescriptor by matching (semantic, semanticIndex) against the bound input layout, so any
// attribute numbering works. Fragment outputs: SV_Target n -> [[color(n)]]; SV_Depth -> [[depth(any)]].
// Compute: the entry point carries numthreads; it is repeated in threadgroupSize for the dispatch.
//
// Engine assumptions the reflection must honour
// ---------------------------------------------
//  * cbuffers[i].d3dSlot == i. R4 binds the cbuffer found at reflection index i to slot b<i>
//    (dx10r_constants.cpp). If the HLSL leaves holes, insert empty placeholder cbuffers.
//  * Names are the original HLSL names: textures/samplers are looked up by name ("s_base", "smp_nofilter").
//  * Matrices are row-major packed (D3D10_SHADER_PACK_MATRIX_ROW_MAJOR); `offset`/`size` follow HLSL cbuffer
//    packing, i.e. what D3D11 reflection would report, because R4 writes into the buffer by byte offset.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "d3d11shader_metal.h"

namespace mtl_d3d11
{
constexpr uint32_t kMaxCBuffers = 14;
constexpr uint32_t kFirstResourceBuffer = 14;
constexpr uint32_t kVertexBufferBase = 16;
constexpr uint32_t kMaxVertexStreams = 15;
constexpr uint32_t kUavTextureBase = 64;

enum class ShaderStage : uint8_t
{
    Vertex = 0,
    Pixel = 1,
    Geometry = 2,
    Hull = 3,
    Domain = 4,
    Compute = 5
};
constexpr int kShaderStageCount = 6;

// One member of a cbuffer: what R4 reads through ID3D11ShaderReflectionVariable/Type.
struct CBufferVariable
{
    std::string name;
    uint32_t offset = 0; // bytes from the start of the cbuffer
    uint32_t size = 0;   // bytes occupied (D3D semantics: arrays include inter-element padding)
    D3D_SHADER_VARIABLE_CLASS cls = D3D_SVC_SCALAR;
    D3D_SHADER_VARIABLE_TYPE type = D3D_SVT_FLOAT;
    uint32_t rows = 1;
    uint32_t columns = 1;
    uint32_t elements = 0; // 0 for non-arrays
};

struct CBuffer
{
    std::string name;
    uint32_t d3dSlot = 0;
    uint32_t metalIndex = 0; // [[buffer(N)]]
    uint32_t sizeBytes = 0;  // multiple of 16
    std::vector<CBufferVariable> variables;
};

// A texture, sampler, UAV or buffer binding other than a cbuffer.
struct ResourceBinding
{
    std::string name;
    D3D_SHADER_INPUT_TYPE type = D3D_SIT_TEXTURE; // SIT_TEXTURE, SIT_SAMPLER, SIT_UAV_RWTYPED, SIT_STRUCTURED, ...
    uint32_t d3dSlot = 0;                         // t#, s# or u#
    uint32_t count = 1;
    uint32_t metalIndex = 0; // [[texture(N)]], [[sampler(N)]] or [[buffer(N)]]
    D3D_SRV_DIMENSION dimension = D3D_SRV_DIMENSION_TEXTURE2D;
    D3D_RESOURCE_RETURN_TYPE returnType = D3D_RETURN_TYPE_FLOAT;
    uint32_t numSamples = 0;  // for TEXTURE2DMS
    bool depthTexture = false; // MSL depth2d<> / depthcube<>; the bound view must be a depth format
    bool comparisonSampler = false;
};

struct VertexInput
{
    std::string semantic; // upper-case HLSL semantic name, e.g. "TEXCOORD"
    uint32_t semanticIndex = 0;
    uint32_t attribute = 0; // [[attribute(N)]]
    uint32_t components = 4;
    D3D_REGISTER_COMPONENT_TYPE componentType = D3D_REGISTER_COMPONENT_FLOAT32;
};

struct ShaderReflection
{
    ShaderStage stage = ShaderStage::Pixel;
    std::string entryPoint = "main0"; // MSL function name
    std::vector<CBuffer> cbuffers;
    std::vector<ResourceBinding> resources;
    std::vector<VertexInput> inputs; // vertex stage only
    uint32_t threadgroupSize[3] = {1, 1, 1}; // compute only
    uint32_t colorOutputs = 0;               // pixel stage: number of SV_Target outputs
};

enum class PayloadKind : uint8_t
{
    MslSource = 0, // UTF-8 Metal Shading Language, compiled by the shim on first use
    MetalLib = 1   // precompiled .metallib bytes; the shim loads them with newLibraryWithData
};

struct ShaderBlob
{
    ShaderReflection reflection;
    PayloadKind kind = PayloadKind::MslSource;
    std::vector<uint8_t> payload;
};

// Container layout: 'XMSL', u32 version, u8 stage, u8 payloadKind, u16 reserved, then length-prefixed
// reflection and payload sections. Serialize/Deserialize are exact inverses; Deserialize validates bounds
// and returns false on any inconsistency. ParseReflection skips the payload (cheap; used by D3DReflect).
std::vector<uint8_t> Serialize(const ShaderBlob& blob);
bool Deserialize(const void* data, size_t size, ShaderBlob* out);
bool ParseReflection(const void* data, size_t size, ShaderReflection* out);
bool IsShaderContainer(const void* data, size_t size);

// A stage-to-stage variable of a translated shader: a member of the MSL struct main0_out (vertex) or main0_in (pixel)
// declared with [[user(locnN)]].
struct Interpolant
{
    std::string type; // "float4"
    std::string name; // member name in the MSL
    uint32_t location = 0;
};
std::vector<Interpolant> ParseInterpolants(const std::string& msl, const char* structName);
// D3D reads zero for a pixel input the vertex shader never writes; Metal refuses such a pipeline. Returns the pixel
// shader's source without the given inputs, which become zero-initialised locals.
std::string PatchFragmentInputs(const std::string& msl, const std::vector<Interpolant>& remove);

// Hook for D3DCompile(): called with the HLSL R4 wants compiled; must return a container.
struct CompileRequest
{
    const void* source;
    size_t sourceSize;
    const char* sourceName;
    const D3D10_SHADER_MACRO* defines;
    ID3D10Include* include;
    const char* entryPoint;
    const char* target; // "vs_5_0", "ps_5_0", "cs_5_0", ...
    uint32_t flags1;
    uint32_t flags2;
};
typedef HRESULT (*CompileFn)(const CompileRequest& request, std::vector<uint8_t>* container, std::string* errors,
                             void* user);
void SetShaderCompiler(CompileFn fn, void* user);
} // namespace mtl_d3d11
