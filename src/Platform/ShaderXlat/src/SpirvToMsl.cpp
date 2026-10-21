// SPIR-V -> MSL (SPIRV-Cross), applying the binding policy and finishing the reflection.
//
// Binding policy (Options): constant buffer b# -> [[buffer(cbuffer_base + #)]], SRV texture t# -> [[texture(#)]],
// UAV texture u# -> [[texture(uav_texture_base + #)]], sampler s# -> [[sampler(#)]]. The SPIR-V descriptor
// bindings were assigned by the frontend as class base + register (Internal.h).
#include "Internal.h"

#include "spirv_glsl.hpp"
#include "spirv_msl.hpp"

#include <map>

namespace shaderxlat::detail
{
namespace
{
using namespace spirv_cross;

// Stable location for an interstage varying, identical in the producing and consuming stage so the
// MSL `[[user(locnN)]]` names line up the way D3D's semantic matching does.
uint32_t interstageLocation(const std::string& sem, uint32_t index)
{
    struct Row { const char* name; uint32_t base; uint32_t count; };
    static const Row rows[] = {{"TEXCOORD", 0, 16}, {"COLOR", 16, 4}, {"NORMAL", 20, 2}, {"TANGENT", 22, 2},
                               {"BINORMAL", 24, 2}, {"POSITION", 26, 2}, {"FOG", 28, 1}, {"PSIZE", 29, 1},
                               {"BLENDWEIGHT", 30, 1}, {"BLENDINDICES", 31, 1}};
    for (const Row& r : rows)
        if (sem == r.name && index < r.count)
            return r.base + index;
    uint32_t h = 2166136261u;
    for (char c : sem)
        h = (h ^ (uint8_t)c) * 16777619u;
    return 32 + (h + index) % 32;
}

bool spirvUsesDiscard(const std::vector<uint32_t>& w)
{
    for (size_t i = 5; i < w.size();)
    {
        const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (len == 0)
            break;
        if (op == spv::OpKill || op == spv::OpTerminateInvocation || op == spv::OpDemoteToHelperInvocation)
            return true;
        i += len;
    }
    return false;
}

spv::ExecutionModel modelOf(Stage s)
{
    switch (s)
    {
    case Stage::Vertex: return spv::ExecutionModelVertex;
    case Stage::Pixel: return spv::ExecutionModelFragment;
    case Stage::Compute: return spv::ExecutionModelGLCompute;
    case Stage::Geometry: return spv::ExecutionModelGeometry;
    case Stage::Hull: return spv::ExecutionModelTessellationControl;
    case Stage::Domain: return spv::ExecutionModelTessellationEvaluation;
    }
    return spv::ExecutionModelFragment;
}
} // namespace

bool crossCompile(const std::vector<uint32_t>& spirv, const Options& opt, Result& result)
{
    try
    {
        CompilerMSL msl(spirv);
        const spv::ExecutionModel model = modelOf(opt.stage);
        ShaderResources res = msl.get_shader_resources();
        Reflection& refl = result.reflection; // pre-filled from the DXBC by the frontend

        auto findByBinding = [&](const SmallVector<Resource>& list, uint32_t binding) -> const Resource* {
            for (const Resource& r : list)
                if (msl.get_decoration(r.id, spv::DecorationBinding) == binding)
                    return &r;
            return nullptr;
        };
        auto addBinding = [&](SPIRType::BaseType bt, uint32_t binding, uint32_t buf, uint32_t tex, uint32_t smp) {
            MSLResourceBinding b;
            b.stage = model;
            b.basetype = bt;
            b.desc_set = 0;
            b.binding = binding;
            b.count = 1;
            b.msl_buffer = buf;
            b.msl_texture = tex;
            b.msl_sampler = smp;
            msl.add_msl_resource_binding(b);
        };

        // ---- constant buffers
        for (ConstantBuffer& cb : refl.constant_buffers)
        {
            cb.msl_index = opt.cbuffer_base + cb.slot;
            cb.size = (cb.size + 15) & ~15u;
            addBinding(SPIRType::Struct, kBindingCbv + cb.slot, cb.msl_index, 0, 0);
        }

        // ---- textures (SRV: t#, UAV: u#)
        for (TextureBinding& t : refl.textures)
        {
            const uint32_t binding = (t.is_uav ? kBindingUav : kBindingSrv) + t.slot;
            const Resource* r = findByBinding(t.is_uav ? res.storage_images : res.separate_images, binding);
            t.msl_index = t.is_uav ? opt.uav_texture_base + t.slot : t.slot;
            if (!r)
                continue; // eliminated as dead code
            const SPIRType& ty = msl.get_type(r->type_id);
            t.depth = ty.image.depth;
            if (t.is_uav)
            {
                t.uav_read_only = msl.has_decoration(r->id, spv::DecorationNonWritable);
                t.uav_write_only = msl.has_decoration(r->id, spv::DecorationNonReadable);
            }
            addBinding(ty.basetype, binding, 0, t.msl_index, 0);
        }

        // ---- structured / raw buffers
        for (StorageBuffer& sb : refl.storage_buffers)
        {
            const uint32_t binding = (sb.is_uav ? kBindingUav : kBindingSrv) + sb.slot;
            sb.msl_index = opt.storage_buffer_base + (sb.is_uav ? 8u : 0u) + sb.slot;
            if (findByBinding(res.storage_buffers, binding))
                addBinding(SPIRType::Struct, binding, sb.msl_index, 0, 0);
        }

        // ---- samplers
        for (SamplerBinding& s : refl.samplers)
        {
            s.msl_index = s.slot;
            if (findByBinding(res.separate_samplers, kBindingSampler + s.slot))
                addBinding(SPIRType::Sampler, kBindingSampler + s.slot, 0, 0, s.msl_index);
        }

        // ---- stage inputs / outputs: SPIR-V Location == D3D signature register
        auto remap = [&](const SmallVector<Resource>& vars, std::vector<StageVariable>& sig, bool interstage) {
            for (const Resource& r : vars)
            {
                const uint32_t loc = msl.get_decoration(r.id, spv::DecorationLocation);
                for (StageVariable& v : sig)
                {
                    if (v.system_value || v.reg != loc)
                        continue;
                    if (interstage)
                    {
                        v.location = interstageLocation(v.semantic, v.semantic_index);
                        msl.set_decoration(r.id, spv::DecorationLocation, v.location);
                    }
                    else
                        v.location = loc;
                    v.name = msl.get_name(r.id);
                    v.active = true;
                    break;
                }
            }
        };
        remap(res.stage_inputs, refl.inputs, opt.stage == Stage::Pixel);
        remap(res.stage_outputs, refl.outputs, opt.stage == Stage::Vertex);
        for (const BuiltInResource& b : res.builtin_outputs)
            if (b.builtin == spv::BuiltInFragDepth)
                refl.writes_depth = true;
        for (const BuiltInResource& b : res.builtin_inputs)
            if (b.builtin == spv::BuiltInSampleId)
                refl.uses_sample_index = true;
        refl.uses_discard = spirvUsesDiscard(spirv);
        if (opt.stage == Stage::Compute)
            for (uint32_t i = 0; i < 3; ++i)
                refl.threads[i] = msl.get_execution_mode_argument(spv::ExecutionModeLocalSize, i);

        // ---- sampler/texture pairing (ids are shared between compilers parsing the same SPIR-V)
        struct PairIds { uint32_t sampler, image; const SamplerBinding* smp; const TextureBinding* tex; };
        std::vector<PairIds> pairIds;
        {
            CompilerGLSL glsl(spirv);
            // Texture.Load() has no sampler; give SPIRV-Cross a dummy so the analysis can proceed.
            const uint32_t dummySampler = glsl.build_dummy_sampler_for_combined_images();
            glsl.build_combined_image_samplers();
            std::map<std::string, bool> seen;
            for (const CombinedImageSampler& cis : glsl.get_combined_image_samplers())
            {
                if (dummySampler && cis.sampler_id == dummySampler)
                    continue;
                const uint32_t sb = glsl.get_decoration(cis.sampler_id, spv::DecorationBinding);
                const uint32_t ib = glsl.get_decoration(cis.image_id, spv::DecorationBinding);
                const SamplerBinding* smp = nullptr;
                const TextureBinding* tex = nullptr;
                for (const SamplerBinding& sm : refl.samplers)
                    if (kBindingSampler + sm.slot == sb)
                        smp = &sm;
                for (const TextureBinding& t : refl.textures)
                    if ((t.is_uav ? kBindingUav : kBindingSrv) + t.slot == ib)
                        tex = &t;
                if (!smp || !tex)
                    continue;
                if (seen.emplace(smp->name + "|" + tex->name, true).second)
                    refl.sampler_texture_pairs.push_back({smp->name, tex->name});
                pairIds.push_back({cis.sampler_id, cis.image_id, smp, tex});
            }
        }

        // ---- MSL
        CompilerMSL::Options mo;
        mo.platform = opt.platform == Platform::IOS ? CompilerMSL::Options::iOS : CompilerMSL::Options::macOS;
        mo.msl_version = CompilerMSL::Options::make_msl_version(opt.msl_version / 10000, (opt.msl_version / 100) % 100);
        mo.enable_decoration_binding = false;
        mo.argument_buffers = false;
        mo.texture_buffer_native = true;
        mo.pad_fragment_output_components = true;
        mo.check_discarded_frag_stores = true; // HLSL `discard` ends the invocation; Metal's does not
        msl.set_msl_options(mo);

        result.msl = msl.compile();

        // Comparison sampling is only known after compile() has analysed image/sampler usage: a
        // sampler used by SampleCmp* needs a compare function in its MTLSamplerState, and the texture
        // must be a depth texture.
        for (const PairIds& p : pairIds)
        {
            if (msl.variable_is_depth_or_compare(p.sampler))
                for (SamplerBinding& sm : refl.samplers)
                    if (sm.slot == p.smp->slot)
                        sm.comparison = true;
            if (msl.variable_is_depth_or_compare(p.image))
                for (TextureBinding& t : refl.textures)
                    if (t.slot == p.tex->slot && t.is_uav == p.tex->is_uav)
                        t.depth = true;
        }
        const auto eps = msl.get_entry_points_and_stages();
        if (!eps.empty())
            result.entry_point = msl.get_cleansed_entry_point_name(eps[0].name, eps[0].execution_model);
        return true;
    }
    catch (const CompilerError& e)
    {
        result.log += std::string("SPIRV-Cross: ") + e.what() + "\n";
        result.category = Category::CrossError;
        return false;
    }
    catch (const std::exception& e)
    {
        result.log += std::string("internal: ") + e.what() + "\n";
        result.category = Category::InternalError;
        return false;
    }
}
} // namespace shaderxlat::detail
