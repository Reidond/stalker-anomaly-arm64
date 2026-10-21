// ShaderXlat: runtime HLSL (SM4/5, as written for the X-Ray D3D11 renderer) to Metal Shading
// Language translator, with D3D11-style reflection.
//
//   HLSL --[vkd3d-shader]--> DXBC (SM5, RDEF + signatures) --[vkd3d-shader]--> SPIR-V
//        --[SPIRV-Cross]--> MSL
//
// Reflection mirrors ID3D11ShaderReflection: it is read from the DXBC the HLSL compiler emits, so
// names, register numbers and constant-buffer offsets follow FXC's D3D11 packing rules.
//
// The library is self-contained: no external processes, no JIT, no threads of its own. It is safe to
// call translate() concurrently from several threads.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shaderxlat
{
enum class Stage : uint8_t { Vertex, Pixel, Geometry, Hull, Domain, Compute };

enum class Platform : uint8_t { MacOS, IOS };

// A `#define NAME VALUE` passed on the command line, like D3D_SHADER_MACRO.
struct Macro
{
    std::string name;
    std::string value;
};

// Resolves `#include` directives. The engine searches "<shader dir>/<name>" first and then "<name>";
// implement whatever search policy fits (loose files, archive, overlay roots).
class IncludeProvider
{
public:
    virtual ~IncludeProvider() = default;
    // `name` is the path as written in the directive with backslashes already turned into '/'.
    // `from` is the resolved path of the including file ("" for the root source). On success fill
    // `resolved_path` (a stable identity for the file, used for caching) and `contents`.
    virtual bool open(const std::string& name, const std::string& from, std::string& resolved_path,
                      std::string& contents) = 0;
};

struct Options
{
    Stage stage = Stage::Pixel;
    std::string entry = "main";
    // D3D target profile used to pick the shader model for the frontend (e.g. "ps_5_0"). Optional:
    // derived from `stage` when empty.
    std::string profile;
    // D3D10_SHADER_PACK_MATRIX_ROW_MAJOR, which the engine passes for every shader.
    bool row_major_matrices = true;
    Platform platform = Platform::MacOS;
    // MSL language version as major*10000 + minor*100 (30000 = Metal 3.0, the default).
    uint32_t msl_version = 30000;
    // Apply the minimal FXC-compat source hooks (see FxcCompat.cpp). Never edits the game's files;
    // only the in-memory text handed to the frontend.
    bool fxc_compat = true;
    // MSL buffer argument table layout: constant buffers use [[buffer(cbuffer_base + slot)]],
    // structured/raw/UAV buffers use [[buffer(storage_buffer_base + n)]]. Vertex streams are bound
    // by the pipeline's vertex descriptor and should live at >= vertex_stream_base.
    uint32_t cbuffer_base = 0;
    uint32_t storage_buffer_base = 14;
    uint32_t vertex_stream_base = 20;
    // MSL texture table: SRV t# -> [[texture(#)]]; UAV textures u# -> [[texture(uav_texture_base + #)]].
    // Samplers: s# -> [[sampler(#)]].
    uint32_t uav_texture_base = 64;
    // Keep the intermediate SPIR-V in Result::spirv (it is always produced, this just keeps it).
    bool keep_spirv = false;
    bool debug_names = true; // keep identifiers readable in the MSL
};

enum class ScalarType : uint8_t { Unknown, Bool, Int, UInt, Half, Float, Double, Struct };

struct ConstantMember
{
    std::string name;
    uint32_t offset = 0; // absolute byte offset inside the constant buffer, D3D packing rules
    uint32_t size = 0;   // bytes the variable occupies (arrays: up to the end of the last element)
    bool used = true;    // D3D_SVF_USED: the shader reads it (unused ones still occupy space)
    ScalarType type = ScalarType::Float;
    uint32_t rows = 1;
    uint32_t columns = 1;
    uint32_t array_elements = 0; // 0 = not an array
    uint32_t array_stride = 0;   // bytes between elements (multiple of 16)
    bool row_major = false;      // D3D_SVC_MATRIX_ROWS: rows are contiguous 16-byte registers
    std::string type_name;       // HLSL struct type name, "" for numeric types
    std::vector<ConstantMember> members; // for ScalarType::Struct
};

struct ConstantBuffer
{
    std::string name;         // "$Globals" for variables declared outside any cbuffer
    uint32_t slot = 0;        // D3D register b#
    uint32_t msl_index = 0;   // [[buffer(n)]]
    uint32_t size = 0;        // bytes, multiple of 16
    std::vector<ConstantMember> members;
};

enum class TextureDim : uint8_t { Tex1D, Tex1DArray, Tex2D, Tex2DArray, Tex2DMS, Tex2DMSArray, Tex3D, TexCube, TexCubeArray, Buffer };

struct TextureBinding
{
    std::string name;
    uint32_t slot = 0;      // D3D register t# (SRV) or u# (UAV)
    uint32_t msl_index = 0; // [[texture(n)]]
    TextureDim dim = TextureDim::Tex2D;
    ScalarType sampled_type = ScalarType::Float;
    bool depth = false;     // declared/used as a depth texture (needed for SampleCmp)
    bool multisampled = false;
    bool is_uav = false;    // RWTexture*: [[texture(n)]] with access::read_write
    bool uav_write_only = false;
    bool uav_read_only = false;
};

struct SamplerBinding
{
    std::string name;
    uint32_t slot = 0;      // D3D register s#
    uint32_t msl_index = 0; // [[sampler(n)]]
    bool comparison = false; // used with SampleCmp*: create the MTLSamplerState with a compare function
};

// StructuredBuffer / ByteAddressBuffer / RWStructuredBuffer ... (none are used by the stock shaders).
struct StorageBuffer
{
    std::string name;
    uint32_t slot = 0;      // D3D register t# or u#
    uint32_t msl_index = 0; // [[buffer(n)]]
    bool is_uav = false;
};

// A sampler/texture pair that is used together by at least one sample instruction.
struct SamplerTexturePair
{
    std::string sampler;
    std::string texture;
};

struct StageVariable
{
    std::string semantic;         // upper-case HLSL semantic without index, e.g. "TEXCOORD"
    uint32_t semantic_index = 0;
    std::string name;             // HLSL variable name if known
    uint32_t location = 0;        // MSL [[attribute(n)]] (vertex inputs) / [[user(locn)]] (interstage) / [[color(n)]]
    uint32_t reg = 0;             // D3D signature register
    uint32_t mask = 0;            // D3D component mask (x=1, y=2, z=4, w=8)
    ScalarType type = ScalarType::Float;
    uint32_t components = 4;
    bool system_value = false;    // SV_* mapped to an MSL built-in
    bool active = false;          // survives dead-code elimination: appears in the MSL interface (non-SV only)
};

struct Reflection
{
    std::vector<ConstantBuffer> constant_buffers;
    std::vector<TextureBinding> textures;
    std::vector<SamplerBinding> samplers;
    std::vector<StorageBuffer> storage_buffers;
    std::vector<SamplerTexturePair> sampler_texture_pairs;
    std::vector<StageVariable> inputs;
    std::vector<StageVariable> outputs;
    uint32_t threads[3] = {0, 0, 0}; // compute: thread group size
    bool writes_depth = false;
    bool uses_discard = false;
    bool uses_sample_index = false;
};

struct Timings
{
    double preprocess_ms = 0; // FXC-compat hooks + include resolution done outside the frontend
    double frontend_ms = 0;   // HLSL -> SPIR-V
    double cross_ms = 0;      // SPIR-V -> MSL + reflection
    double total_ms = 0;
};

enum class Category : uint8_t
{
    None,
    // Not translatable on Metal (geometry/tessellation stages). Result::ok is false and the reason is in `log`.
    UnsupportedStage,
    FrontendError,  // HLSL did not compile
    CrossError,     // SPIRV-Cross failed
    InternalError,
};

struct IncludeDependency
{
    std::string name;     // path as written in the directive, '/' separators
    std::string resolved; // IncludeProvider::open() resolved_path
    uint64_t hash = 0;    // FNV-1a 64 of the file contents
};

struct Result
{
    bool ok = false;
    Category category = Category::None;
    std::string log;                 // compiler messages, one per line
    std::string msl;                 // MSL source; the entry point is always named `entry_point`
    std::string entry_point;         // MSL function name (HLSL `main` becomes "main0" in MSL; see Result)
    Reflection reflection;
    std::vector<uint32_t> spirv;     // only when Options::keep_spirv
    // Every #include the compile read (name as written, resolved path, hash of the contents as the
    // frontend saw it). TranslationCache re-checks these to invalidate stale entries.
    std::vector<IncludeDependency> dependencies;
    std::vector<std::string> applied_hooks; // names of FXC-compat hooks that rewrote the source
    Timings timings;
};

// Translates one shader. `includes` may be null when the source has no #include.
Result translate(const std::string& source, const std::string& source_name, const std::vector<Macro>& macros,
                 const Options& options, IncludeProvider* includes);

// Re-runs the SPIR-V -> MSL half of translate() for another platform/MSL version without repeating
// the HLSL compile. `translated` must come from translate() with Options::keep_spirv set.
Result retarget(const Result& translated, const Options& options);

// ---- on-disk translation cache -------------------------------------------------------------------
//
// Translating one shader takes about 10 ms (up to several seconds for the largest HBAO combine
// shader), so the renderer keeps results on disk. An entry is keyed by the HLSL source, macros,
// options and library version; it also records every #include it read and is discarded when any of
// them changed, which keeps addons that replace shader files working without a manual flush.
class TranslationCache
{
public:
    // `directory` is created on first use. Entries are written atomically (temp file + rename), so
    // several processes or threads may share a directory.
    explicit TranslationCache(std::string directory);

    // Like translate(), but consults the cache first and stores successful results. Failed
    // translations are not cached. `cache_hit` (optional) reports which path was taken.
    Result translate(const std::string& source, const std::string& source_name, const std::vector<Macro>& macros,
                     const Options& options, IncludeProvider* includes, bool* cache_hit = nullptr);

    // Deletes every entry in the directory.
    void clear();

private:
    std::string directory_;
};

// ---- helpers for the pipeline layer --------------------------------------------------------------

// D3D allows a pixel shader to read a varying the vertex shader never wrote (it reads zero); Metal
// refuses to build such a pipeline, and also when the types differ. Returns one line per problem
// found between a vertex shader's outputs and a pixel shader's active inputs; empty when they link.
std::vector<std::string> checkStageInterface(const Reflection& vertex, const Reflection& pixel);

// Human-readable library version (includes the frontend in use).
const char* version();
// Name of the HLSL -> SPIR-V frontend this library was built with ("dxc" or "vkd3d-shader").
const char* frontend_name();

} // namespace shaderxlat
