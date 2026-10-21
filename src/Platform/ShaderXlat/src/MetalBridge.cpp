// ShaderXlatMetal: see ShaderXlat/MetalBridge.h.
#include "ShaderXlat/MetalBridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

namespace shaderxlat_metal
{
namespace
{
using shaderxlat::ScalarType;
using shaderxlat::Stage;
using shaderxlat::TextureDim;

struct State
{
    std::mutex mutex;
    Config config;
    std::unique_ptr<shaderxlat::TranslationCache> cache;
};

State& state()
{
    static State s;
    return s;
}

std::atomic<uint64_t> g_requests{0}, g_cacheHits{0}, g_failures{0}, g_unsupported{0};
std::atomic<uint64_t> g_translateMicros{0};

// Adapts the engine's ID3DInclude (R4's `includer` reads through the virtual file system) to ShaderXlat.
class IncludeAdapter final : public shaderxlat::IncludeProvider
{
public:
    explicit IncludeAdapter(ID3D10Include* include) : include_(include) {}

    bool open(const std::string& name, const std::string&, std::string& resolved, std::string& contents) override
    {
        if (!include_)
            return false;
        // ShaderXlat hands over '/' separators; the engine's file system wants its virtual '\' paths
        std::string path = name;
        std::replace(path.begin(), path.end(), '/', '\\');
        LPCVOID data = nullptr;
        UINT bytes = 0;
        if (FAILED(include_->Open(D3D10_INCLUDE_LOCAL, path.c_str(), nullptr, &data, &bytes)) || !data)
            return false;
        contents.assign(static_cast<const char*>(data), bytes);
        include_->Close(data);
        resolved = name;
        return true;
    }

private:
    ID3D10Include* include_;
};

void logLine(const Config& cfg, const std::string& text)
{
    if (cfg.log)
        cfg.log(text);
}

const char* stageName(Stage s)
{
    switch (s)
    {
    case Stage::Vertex: return "vertex";
    case Stage::Pixel: return "pixel";
    case Stage::Geometry: return "geometry";
    case Stage::Hull: return "hull";
    case Stage::Domain: return "domain";
    case Stage::Compute: return "compute";
    }
    return "?";
}

D3D_SHADER_VARIABLE_TYPE toSvt(ScalarType t)
{
    switch (t)
    {
    case ScalarType::Bool: return D3D_SVT_BOOL;
    case ScalarType::Int: return D3D_SVT_INT;
    case ScalarType::UInt: return D3D_SVT_UINT;
    case ScalarType::Struct: return D3D_SVT_VOID;
    default: return D3D_SVT_FLOAT; // float, half, double and unknown read as float
    }
}

D3D_SHADER_VARIABLE_CLASS toSvc(const shaderxlat::ConstantMember& m)
{
    if (m.type == ScalarType::Struct)
        return D3D_SVC_STRUCT;
    if (m.rows <= 1 && m.columns <= 1)
        return D3D_SVC_SCALAR;
    if (m.rows <= 1)
        return D3D_SVC_VECTOR;
    return m.row_major ? D3D_SVC_MATRIX_ROWS : D3D_SVC_MATRIX_COLUMNS;
}

D3D_SRV_DIMENSION toSrvDimension(TextureDim d)
{
    switch (d)
    {
    case TextureDim::Tex1D: return D3D_SRV_DIMENSION_TEXTURE1D;
    case TextureDim::Tex1DArray: return D3D_SRV_DIMENSION_TEXTURE1DARRAY;
    case TextureDim::Tex2D: return D3D_SRV_DIMENSION_TEXTURE2D;
    case TextureDim::Tex2DArray: return D3D_SRV_DIMENSION_TEXTURE2DARRAY;
    case TextureDim::Tex2DMS: return D3D_SRV_DIMENSION_TEXTURE2DMS;
    case TextureDim::Tex2DMSArray: return D3D_SRV_DIMENSION_TEXTURE2DMSARRAY;
    case TextureDim::Tex3D: return D3D_SRV_DIMENSION_TEXTURE3D;
    case TextureDim::TexCube: return D3D_SRV_DIMENSION_TEXTURECUBE;
    case TextureDim::TexCubeArray: return D3D_SRV_DIMENSION_TEXTURECUBEARRAY;
    case TextureDim::Buffer: return D3D_SRV_DIMENSION_BUFFER;
    }
    return D3D_SRV_DIMENSION_TEXTURE2D;
}

D3D_RESOURCE_RETURN_TYPE toReturnType(ScalarType t)
{
    switch (t)
    {
    case ScalarType::Int: return D3D_RETURN_TYPE_SINT;
    case ScalarType::UInt: return D3D_RETURN_TYPE_UINT;
    default: return D3D_RETURN_TYPE_FLOAT;
    }
}

D3D_REGISTER_COMPONENT_TYPE toComponentType(ScalarType t)
{
    switch (t)
    {
    case ScalarType::Int: return D3D_REGISTER_COMPONENT_SINT32;
    case ScalarType::UInt: return D3D_REGISTER_COMPONENT_UINT32;
    default: return D3D_REGISTER_COMPONENT_FLOAT32;
    }
}

bool isSemantic(const std::string& semantic, const char* upper)
{
    if (semantic.size() != std::strlen(upper))
        return false;
    for (size_t i = 0; i < semantic.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(semantic[i])) != upper[i])
            return false;
    return true;
}
} // namespace

shaderxlat::Stage stageFromTarget(const char* target)
{
    switch (target ? target[0] : 'p')
    {
    case 'v': return Stage::Vertex;
    case 'g': return Stage::Geometry;
    case 'h': return Stage::Hull;
    case 'd': return Stage::Domain;
    case 'c': return Stage::Compute;
    default: return Stage::Pixel;
    }
}

mtl_d3d11::ShaderStage toShimStage(shaderxlat::Stage stage)
{
    switch (stage)
    {
    case Stage::Vertex: return mtl_d3d11::ShaderStage::Vertex;
    case Stage::Geometry: return mtl_d3d11::ShaderStage::Geometry;
    case Stage::Hull: return mtl_d3d11::ShaderStage::Hull;
    case Stage::Domain: return mtl_d3d11::ShaderStage::Domain;
    case Stage::Compute: return mtl_d3d11::ShaderStage::Compute;
    default: return mtl_d3d11::ShaderStage::Pixel;
    }
}

mtl_d3d11::ShaderBlob toShaderBlob(const shaderxlat::Result& result, shaderxlat::Stage stage)
{
    using namespace mtl_d3d11;
    ShaderBlob blob;
    ShaderReflection& out = blob.reflection;
    const shaderxlat::Reflection& in = result.reflection;

    out.stage = toShimStage(stage);
    out.entryPoint = result.entry_point.empty() ? "main0" : result.entry_point;
    std::copy(std::begin(in.threads), std::end(in.threads), std::begin(out.threadgroupSize));
    for (uint32_t& t : out.threadgroupSize)
        t = std::max(t, 1u);

    // R4 binds the cbuffer found at reflection index i to slot b<i> (dx10r_constants.cpp), so the list is dense
    std::vector<const shaderxlat::ConstantBuffer*> bySlot;
    for (const shaderxlat::ConstantBuffer& cb : in.constant_buffers)
    {
        if (cb.slot >= bySlot.size())
            bySlot.resize(cb.slot + 1, nullptr);
        bySlot[cb.slot] = &cb;
    }
    for (uint32_t slot = 0; slot < bySlot.size(); ++slot)
    {
        CBuffer cb;
        const shaderxlat::ConstantBuffer* src = bySlot[slot];
        if (!src)
        {
            cb.name = "$unused_b" + std::to_string(slot);
            cb.d3dSlot = slot;
            cb.metalIndex = slot;
            cb.sizeBytes = 16;
            out.cbuffers.push_back(std::move(cb));
            continue;
        }
        cb.name = src->name;
        cb.d3dSlot = slot;
        cb.metalIndex = src->msl_index;
        cb.sizeBytes = src->size;
        for (const shaderxlat::ConstantMember& m : src->members)
        {
            CBufferVariable v;
            v.name = m.name;
            v.offset = m.offset;
            v.size = m.size;
            v.cls = toSvc(m);
            v.type = toSvt(m.type);
            v.rows = std::max(m.rows, 1u);
            v.columns = std::max(m.columns, 1u);
            v.elements = m.array_elements;
            cb.variables.push_back(std::move(v));
        }
        out.cbuffers.push_back(std::move(cb));
    }

    for (const shaderxlat::TextureBinding& t : in.textures)
    {
        ResourceBinding b;
        b.name = t.name;
        b.type = t.is_uav ? D3D_SIT_UAV_RWTYPED : D3D_SIT_TEXTURE;
        b.d3dSlot = t.slot;
        b.metalIndex = t.msl_index;
        b.dimension = toSrvDimension(t.dim);
        b.returnType = toReturnType(t.sampled_type);
        b.depthTexture = t.depth;
        out.resources.push_back(std::move(b));
    }
    for (const shaderxlat::SamplerBinding& s : in.samplers)
    {
        ResourceBinding b;
        b.name = s.name;
        b.type = D3D_SIT_SAMPLER;
        b.d3dSlot = s.slot;
        b.metalIndex = s.msl_index;
        b.comparisonSampler = s.comparison;
        out.resources.push_back(std::move(b));
    }
    for (const shaderxlat::StorageBuffer& s : in.storage_buffers)
    {
        ResourceBinding b;
        b.name = s.name;
        b.type = s.is_uav ? D3D_SIT_UAV_RWSTRUCTURED : D3D_SIT_STRUCTURED;
        b.d3dSlot = s.slot;
        b.metalIndex = s.msl_index;
        b.dimension = D3D_SRV_DIMENSION_BUFFER;
        out.resources.push_back(std::move(b));
    }

    if (stage == Stage::Vertex)
    {
        for (const shaderxlat::StageVariable& v : in.inputs)
        {
            if (v.system_value || !v.active)
                continue;
            VertexInput vi;
            vi.semantic = v.semantic;
            std::transform(vi.semantic.begin(), vi.semantic.end(), vi.semantic.begin(),
                           [](unsigned char c) { return char(std::toupper(c)); });
            vi.semanticIndex = v.semantic_index;
            vi.attribute = v.location;
            vi.components = v.components;
            vi.componentType = toComponentType(v.type);
            out.inputs.push_back(std::move(vi));
        }
    }
    if (stage == Stage::Pixel)
    {
        for (const shaderxlat::StageVariable& v : in.outputs)
            if (isSemantic(v.semantic, "SV_TARGET"))
                out.colorOutputs = std::max(out.colorOutputs, v.semantic_index + 1);
    }

    blob.kind = PayloadKind::MslSource;
    blob.payload.assign(result.msl.begin(), result.msl.end());
    return blob;
}

HRESULT compile(const mtl_d3d11::CompileRequest& request, std::vector<uint8_t>* container, std::string* errors,
                shaderxlat::Result* detail, bool* cache_hit)
{
    Config cfg;
    shaderxlat::TranslationCache* cache = nullptr;
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        cfg = s.config;
        cache = s.cache.get(); // only replaced by install(), which callers do not race with compiles
    }
    ++g_requests;

    const Stage stage = stageFromTarget(request.target);
    if (stage == Stage::Geometry || stage == Stage::Hull || stage == Stage::Domain)
    {
        const uint64_t n = ++g_unsupported;
        if (errors)
            *errors = std::string(stageName(stage)) + " shaders are not supported on Metal";
        if (n <= 8)
            logLine(cfg, std::string("! Metal: ") + (request.target ? request.target : "?") +
                             " shader skipped (no " + stageName(stage) + " stage on Metal)");
        return E_NOTIMPL;
    }

    shaderxlat::Options options;
    options.stage = stage;
    options.entry = request.entryPoint && *request.entryPoint ? request.entryPoint : "main";
    options.profile = request.target ? request.target : "";
    options.row_major_matrices = (request.flags1 & D3D10_SHADER_PACK_MATRIX_ROW_MAJOR) != 0;
    options.platform = cfg.platform;

    std::vector<shaderxlat::Macro> macros;
    for (const D3D10_SHADER_MACRO* m = request.defines; m && m->Name; ++m)
        macros.push_back({m->Name, m->Definition ? m->Definition : ""});

    const std::string source(static_cast<const char*>(request.source), request.sourceSize);
    const std::string name = request.sourceName && *request.sourceName
                                 ? request.sourceName
                                 : std::string(request.entryPoint ? request.entryPoint : "main") + "." +
                                       (request.target ? request.target : "");
    IncludeAdapter includes(request.include);

    const auto t0 = std::chrono::steady_clock::now();
    bool hit = false;
    shaderxlat::Result result = cache ? cache->translate(source, name, macros, options, &includes, &hit)
                                      : shaderxlat::translate(source, name, macros, options, &includes);
    g_translateMicros += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count());
    if (hit)
        ++g_cacheHits;
    if (cache_hit)
        *cache_hit = hit;

    if (!result.ok)
    {
        ++g_failures;
        if (errors)
            *errors = result.log;
        if (result.category == shaderxlat::Category::UnsupportedStage)
            return E_NOTIMPL;
        std::string log = result.log;
        if (log.size() > 2000)
            log.resize(2000);
        logLine(cfg, "! Metal: HLSL translation failed (" + name + ", " + options.profile + "):\n" + log);
        if (detail)
            *detail = std::move(result);
        return E_FAIL;
    }
    if (cfg.log_warnings && !result.log.empty())
        logLine(cfg, "~ Metal: " + name + ": " + result.log);

    if (container)
        *container = mtl_d3d11::Serialize(toShaderBlob(result, stage));
    if (detail)
        *detail = std::move(result);
    return S_OK;
}

namespace
{
HRESULT hook(const mtl_d3d11::CompileRequest& request, std::vector<uint8_t>* container, std::string* errors, void*)
{
    return compile(request, container, errors);
}
} // namespace

void install(const Config& config)
{
    {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.config = config;
        s.cache = config.cache_dir.empty() ? nullptr : std::make_unique<shaderxlat::TranslationCache>(config.cache_dir);
    }
    mtl_d3d11::SetShaderCompiler(&hook, nullptr);
}

void uninstall()
{
    mtl_d3d11::SetShaderCompiler(nullptr, nullptr);
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.cache.reset();
}

Stats stats()
{
    Stats s;
    s.requests = g_requests;
    s.cache_hits = g_cacheHits;
    s.failures = g_failures;
    s.unsupported_stage = g_unsupported;
    s.translate_ms = double(g_translateMicros) / 1000.0;
    return s;
}

void resetStats()
{
    g_requests = g_cacheHits = g_failures = g_unsupported = g_translateMicros = 0;
}
} // namespace shaderxlat_metal
