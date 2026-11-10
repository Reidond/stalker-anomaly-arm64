// SPIR-V -> WGSL with Tint, plus the WebGPU-side checks Chrome makes on the result. See include/ShaderXlat/Wgsl.h.
#include "ShaderXlat/Wgsl.h"

#include "Internal.h"

#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/allowed_features.h"
#include "src/tint/lang/wgsl/ast/module.h"
#include "src/tint/lang/wgsl/inspector/inspector.h"
#include "src/tint/lang/wgsl/program/program.h"
#include "src/tint/lang/wgsl/reader/reader.h"
#include "src/tint/lang/wgsl/sem/variable.h"
#include "src/tint/lang/wgsl/writer/writer.h"
#include "src/tint/utils/diagnostic/source.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#ifndef ANOMALY_DAWN_REVISION
#define ANOMALY_DAWN_REVISION "unknown"
#endif

namespace shaderxlat::wgsl
{
namespace
{
// SPIR-V enumerants used below (spirv.hpp values).
enum : uint32_t
{
    OpTypeImage = 25,
    OpTypePointer = 32,
    OpVariable = 59,
    OpDecorate = 71,
    DecorationBuiltIn = 11,
    DecorationLocation = 30,
    DecorationBinding = 33,
    DecorationDescriptorSet = 34,
    StorageClassInput = 1,
    StorageClassOutput = 3,
    ImageFormatUnknown = 0,
    OpTypeBool = 20,
    OpTypeInt = 21,
    OpTypeVector = 23,
    OpConstant = 43,
    OpConstantComposite = 44,
    OpFunction = 54,
    OpBitcast = 124,
    OpIsNan = 156,
    OpIsInf = 157,
    OpIEqual = 170,
    OpUGreaterThan = 172,
    OpBitwiseAnd = 199,
    OpName = 5,
    OpExtInst = 12,
    OpEntryPoint = 15,
    OpCapability = 17,
    OpTypeFloat = 22,
    OpLoad = 61,
    OpStore = 62,
    OpFunctionCall = 57,
    OpCompositeConstruct = 80,
    OpFNegate = 127,
    OpFAdd = 129,
    OpFMod = 141,
    OpSelect = 169,
    OpFOrdEqual = 180,
    OpFUnordGreaterThanEqual = 191,
    OpPhi = 245,
    OpLine = 8,
    OpNoLine = 317,
    OpMemberDecorate = 72,
    OpCompositeExtract = 81,
    OpVectorShuffle = 79,
    OpReturnValue = 254,
    StorageClassPrivate = 6,
    CapabilityStorageImageExtendedFormats = 49,
    CapabilityStorageImageReadWithoutFormat = 55,
    CapabilityStorageImageWriteWithoutFormat = 56,
};

// ---- instruction-level SPIR-V rewrites ---------------------------------------------------------------------------
// The module as a list of instructions, so passes can insert and remove instructions and declare new globals.
struct Module
{
    std::vector<uint32_t> header; // 5 words; header[3] is the id bound
    std::vector<std::vector<uint32_t>> ins;

    static uint32_t op(const std::vector<uint32_t>& i) { return i[0] & 0xffff; }
    static std::vector<uint32_t> make(uint32_t opcode, std::initializer_list<uint32_t> operands)
    {
        std::vector<uint32_t> v{(uint32_t(operands.size() + 1) << 16) | opcode};
        v.insert(v.end(), operands);
        return v;
    }
    bool parse(const std::vector<uint32_t>& w)
    {
        header.assign(w.begin(), w.begin() + 5);
        for (size_t i = 5; i < w.size();)
        {
            const uint32_t len = w[i] >> 16;
            if (len == 0 || i + len > w.size())
                return false;
            ins.emplace_back(w.begin() + i, w.begin() + i + len);
            i += len;
        }
        return true;
    }
    std::vector<uint32_t> words() const
    {
        std::vector<uint32_t> w = header;
        for (const auto& i : ins)
            w.insert(w.end(), i.begin(), i.end());
        return w;
    }
    uint32_t newId() { return header[3]++; }
    size_t firstFunction() const
    {
        for (size_t k = 0; k < ins.size(); ++k)
            if (op(ins[k]) == OpFunction)
                return k;
        return ins.size();
    }
    // Inserts a global declaration (type, constant, variable) after every existing one.
    void addGlobal(std::vector<uint32_t> i) { ins.insert(ins.begin() + (std::ptrdiff_t)firstFunction(), std::move(i)); }
    uint32_t find(uint32_t opcode, std::initializer_list<uint32_t> operandsAfterResult)
    {
        for (const auto& i : ins)
            if (op(i) == opcode && i.size() == operandsAfterResult.size() + 2 &&
                std::equal(operandsAfterResult.begin(), operandsAfterResult.end(), i.begin() + 2))
                return i[1];
        return 0;
    }
    uint32_t uintType()
    {
        if (uint32_t t = find(OpTypeInt, {32, 0}))
            return t;
        const uint32_t t = newId();
        addGlobal(make(OpTypeInt, {t, 32, 0}));
        return t;
    }
    uint32_t vectorType(uint32_t component, uint32_t n)
    {
        if (n == 1)
            return component;
        if (uint32_t t = find(OpTypeVector, {component, n}))
            return t;
        const uint32_t t = newId();
        addGlobal(make(OpTypeVector, {t, component, n}));
        return t;
    }
    uint32_t pointerType(uint32_t storageClass, uint32_t pointee)
    {
        if (uint32_t t = find(OpTypePointer, {storageClass, pointee}))
            return t;
        const uint32_t t = newId();
        addGlobal(make(OpTypePointer, {t, storageClass, pointee}));
        return t;
    }
    // Scalar (n == 1) or splatted vector uint constant. Not de-duplicated against existing constants: duplicates are legal.
    uint32_t uintConstant(uint32_t value, uint32_t n)
    {
        const uint32_t u = uintType();
        const uint32_t s = newId();
        addGlobal(make(OpConstant, {u, s, value}));
        if (n == 1)
            return s;
        const uint32_t vt = vectorType(u, n), c = newId();
        std::vector<uint32_t> cc = make(OpConstantComposite, {vt, c});
        for (uint32_t k = 0; k < n; ++k)
            cc.push_back(s);
        cc[0] = (uint32_t(cc.size()) << 16) | OpConstantComposite;
        addGlobal(std::move(cc));
        return c;
    }
};

// Component count of a type id (1 for scalars), from OpTypeVector.
uint32_t componentCount(const Module& m, uint32_t type)
{
    for (const auto& i : m.ins)
        if (Module::op(i) == OpTypeVector && i[1] == type)
            return i[3];
    return 1;
}

// OpIsNan / OpIsInf -> tests on the IEEE bits: isnan(x) = (bits(x) & 0x7fffffff) > 0x7f800000, isinf(x) = ... == 0x7f800000.
// vkd3d emits them for D3D's float -> int conversions (NaN converts to 0), isfinite and the ord/unord comparisons; Tint
// rejects both ("NaNs cannot be represented in WGSL"). The bit test is exact and, unlike x != x, survives fast-math.
size_t polyfillFloatClassification(Module& m)
{
    struct Masks { uint32_t type = 0, absMask = 0, infBits = 0; };
    std::map<uint32_t, Masks> byWidth;
    size_t rewritten = 0;
    for (size_t k = 0; k < m.ins.size(); ++k)
    {
        const uint32_t opc = Module::op(m.ins[k]);
        if ((opc != OpIsNan && opc != OpIsInf) || m.ins[k].size() != 4)
            continue;
        const uint32_t resultType = m.ins[k][1], result = m.ins[k][2], x = m.ins[k][3];
        const uint32_t n = componentCount(m, resultType);
        Masks& mk = byWidth[n];
        if (!mk.type)
        {
            const size_t before = m.ins.size();
            mk.type = m.vectorType(m.uintType(), n);
            mk.absMask = m.uintConstant(0x7fffffffu, n);
            mk.infBits = m.uintConstant(0x7f800000u, n);
            k += m.ins.size() - before; // globals were inserted before this instruction
        }
        const uint32_t bits = m.newId(), magnitude = m.newId();
        std::vector<std::vector<uint32_t>> seq = {
            Module::make(OpBitcast, {mk.type, bits, x}),
            Module::make(OpBitwiseAnd, {mk.type, magnitude, bits, mk.absMask}),
            Module::make(opc == OpIsNan ? OpUGreaterThan : OpIEqual, {resultType, result, magnitude, mk.infBits}),
        };
        m.ins.erase(m.ins.begin() + (std::ptrdiff_t)k);
        m.ins.insert(m.ins.begin() + (std::ptrdiff_t)k, seq.begin(), seq.end());
        k += seq.size() - 1;
        ++rewritten;
    }
    return rewritten;
}

// Operand positions (word indices) that hold ids, for the instructions non-finite constants appear in.
bool idOperand(uint32_t opc, size_t word)
{
    switch (opc)
    {
    case OpStore: return word == 1 || word == 2;
    case OpReturnValue: return word == 1;
    case OpSelect: return word >= 3 && word <= 5;
    case OpCompositeConstruct: case OpFunctionCall: return word >= 3;
    case OpCompositeExtract: return word == 3;
    case OpVectorShuffle: return word == 3 || word == 4;
    case OpExtInst: return word >= 5;
    case OpBitcast: case OpFNegate: return word == 3;
    default:
        // binary float arithmetic and comparisons: result type, result, a, b
        if ((opc >= OpFAdd && opc <= OpFMod) || (opc >= OpFOrdEqual && opc <= OpFUnordGreaterThanEqual))
            return word == 3 || word == 4;
        return false;
    }
}

// Instructions whose operands after the opcode are all literals or debug data, never value uses.
bool debugOrLiteralOnly(uint32_t opc) { return opc == OpLine || opc == OpNoLine || opc == OpName || opc == OpDecorate || opc == OpMemberDecorate; }

// OpConstant %float with an Inf/NaN bit pattern, and composite constants built from them. vkd3d emits 0xffffffff, D3D's
// "true", wherever a comparison result lives in a float register (DXBC registers are typeless), and the shaders' own
// 1.#INF / NaN literals come through the same way. Tint aborts on such constants (TINT_ASSERT(std::isfinite) in
// core/constant/scalar.h), and in WGSL a non-finite const-expression is a shader-creation error anyway. Each scalar
// becomes a load from a private u32 initialised with the same bits, bitcast to f32 at each use; composites are rebuilt
// at each use with OpCompositeConstruct. Runtime values, so nothing folds them. Constants used in a way this pass does
// not know are left alone (Tint then still fails on them). Returns the number of constants replaced.
size_t materializeNonFiniteConstants(Module& m)
{
    std::set<uint32_t> floatTypes;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> bad;                // scalar id -> (float type, bits)
    std::map<uint32_t, std::pair<uint32_t, std::vector<uint32_t>>> badComp; // composite id -> (type, constituents)
    for (const auto& i : m.ins)
    {
        const uint32_t opc = Module::op(i);
        if (opc == OpTypeFloat && i[2] == 32)
            floatTypes.insert(i[1]);
        else if (opc == OpConstant && i.size() == 4 && floatTypes.count(i[1]) && ((i[3] >> 23) & 0xff) == 0xff)
            bad[i[2]] = {i[1], i[3]};
        else if (opc == OpConstantComposite && std::any_of(i.begin() + 3, i.end(), [&](uint32_t c) { return bad.count(c) != 0; }))
            badComp[i[2]] = {i[1], std::vector<uint32_t>(i.begin() + 3, i.end())};
    }
    if (bad.empty())
        return 0;
    auto isBad = [&](uint32_t id) { return bad.count(id) || badComp.count(id); };
    // Only handle constants whose every use is an id operand of an instruction we know, inside a function.
    std::set<uint32_t> unsupported;
    const size_t ff = m.firstFunction();
    for (size_t k = 0; k < m.ins.size(); ++k)
    {
        const auto& i = m.ins[k];
        const uint32_t opc = Module::op(i);
        if (debugOrLiteralOnly(opc) || ((opc == OpConstant || opc == OpConstantComposite) && k < ff && isBad(i[2])))
            continue;
        for (size_t wd = 1; wd < i.size(); ++wd)
            if (isBad(i[wd]) && (k < ff || !idOperand(opc, wd) || opc == OpPhi))
                unsupported.insert(i[wd]);
    }
    // A composite needs all its bad scalars handled, and an unsupported composite keeps its scalars.
    for (const auto& [id, tc] : badComp)
        for (uint32_t c : tc.second)
            if (bad.count(c) && (unsupported.count(c) || unsupported.count(id)))
            {
                unsupported.insert(c);
                unsupported.insert(id);
            }
    std::map<uint32_t, uint32_t> privateVar; // scalar constant id -> variable
    for (const auto& [id, tb] : bad)
    {
        if (unsupported.count(id))
            continue;
        const uint32_t init = m.uintConstant(tb.second, 1);
        const uint32_t ptr = m.pointerType(StorageClassPrivate, m.uintType());
        const uint32_t var = m.newId();
        m.addGlobal(Module::make(OpVariable, {ptr, var, StorageClassPrivate, init}));
        privateVar[id] = var;
    }
    if (privateVar.empty())
        return 0;
    // SPIR-V 1.4+ lists every global an entry point touches in its interface.
    if (m.header[1] >= 0x00010400)
        for (auto& i : m.ins)
            if (Module::op(i) == OpEntryPoint)
            {
                for (const auto& [id, var] : privateVar)
                    i.push_back(var);
                i[0] = (uint32_t(i.size()) << 16) | OpEntryPoint;
            }
    const uint32_t u = m.uintType();
    std::vector<std::vector<uint32_t>> pre;
    auto scalarAt = [&](uint32_t id) -> uint32_t { // emits the load + bitcast for one bad scalar, returns the new id
        const uint32_t loaded = m.newId(), asFloat = m.newId();
        pre.push_back(Module::make(OpLoad, {u, loaded, privateVar[id]}));
        pre.push_back(Module::make(OpBitcast, {bad[id].first, asFloat, loaded}));
        return asFloat;
    };
    for (size_t k = m.firstFunction(); k < m.ins.size(); ++k)
    {
        pre.clear();
        std::vector<uint32_t> i = m.ins[k];
        const uint32_t opc = Module::op(i);
        if (debugOrLiteralOnly(opc))
            continue;
        bool changed = false;
        for (size_t wd = 1; wd < i.size(); ++wd)
        {
            if (!idOperand(opc, wd))
                continue;
            if (privateVar.count(i[wd]))
            {
                i[wd] = scalarAt(i[wd]);
                changed = true;
            }
            else if (auto bc = badComp.find(i[wd]); bc != badComp.end() && !unsupported.count(i[wd]))
            {
                std::vector<uint32_t> cc = {0, bc->second.first, m.newId()};
                for (uint32_t c : bc->second.second)
                    cc.push_back(privateVar.count(c) ? scalarAt(c) : c);
                cc[0] = (uint32_t(cc.size()) << 16) | OpCompositeConstruct;
                pre.push_back(cc);
                i[wd] = cc[2];
                changed = true;
            }
        }
        if (changed)
        {
            m.ins[k] = i;
            m.ins.insert(m.ins.begin() + (std::ptrdiff_t)k, pre.begin(), pre.end());
            k += pre.size();
        }
    }
    // The constants themselves go: Tint asserts on them even unused.
    auto replaced = [&](uint32_t id) { return privateVar.count(id) || (badComp.count(id) && !unsupported.count(id)); };
    m.ins.erase(std::remove_if(m.ins.begin(), m.ins.end(),
                               [&](const std::vector<uint32_t>& i) {
                                   const uint32_t opc = Module::op(i);
                                   return ((opc == OpConstant || opc == OpConstantComposite) && replaced(i[2])) ||
                                          ((opc == OpName || opc == OpDecorate) && replaced(i[1]));
                               }),
                m.ins.end());
    return privateVar.size();
}

// Storage-image capabilities after the formats were filled in: the *WithoutFormat ones (which Tint rejects) go when every
// storage image has a format, and formats outside the core Shader set (r16float, rg16float, ...) need
// StorageImageExtendedFormats, without which SPIR-V validation fails in Tint's reader.
void fixStorageImageCapabilities(Module& m)
{
    static const std::set<uint32_t> core = {1, 2, 3, 4, 5, 21, 22, 23, 24, 30, 31, 32, 33}; // spirv.hpp ImageFormat
    bool unknown = false, extended = false, haveExtended = false;
    for (const auto& i : m.ins)
    {
        if (Module::op(i) == OpTypeImage && i.size() >= 9 && i[7] == 2)
        {
            unknown |= i[8] == ImageFormatUnknown;
            extended |= i[8] != ImageFormatUnknown && !core.count(i[8]);
        }
        haveExtended |= Module::op(i) == OpCapability && i[1] == CapabilityStorageImageExtendedFormats;
    }
    if (!unknown)
        m.ins.erase(std::remove_if(m.ins.begin(), m.ins.end(),
                                   [](const std::vector<uint32_t>& i) {
                                       return Module::op(i) == OpCapability && (i[1] == CapabilityStorageImageReadWithoutFormat ||
                                                                                i[1] == CapabilityStorageImageWriteWithoutFormat);
                                   }),
                    m.ins.end());
    if (extended && !haveExtended)
        m.ins.insert(m.ins.begin(), Module::make(OpCapability, {CapabilityStorageImageExtendedFormats}));
}

uint32_t spirvImageFormat(const std::string& wgsl)
{
    static const std::map<std::string, uint32_t> table = {
        {"rgba32float", 1}, {"rgba16float", 2}, {"r32float", 3},   {"rgba8unorm", 4}, {"rgba8snorm", 5},
        {"rg32float", 6},   {"rg16float", 7},   {"rg11b10ufloat", 8}, {"r16float", 9}, {"rgba16unorm", 10},
        {"rgb10a2unorm", 11}, {"rg16unorm", 12}, {"rg8unorm", 13}, {"r16unorm", 14}, {"r8unorm", 15},
        {"rgba32sint", 21}, {"rgba16sint", 22}, {"rgba8sint", 23}, {"r32sint", 24},  {"rgba32uint", 30},
        {"rgba16uint", 31}, {"rgba8uint", 32},  {"r32uint", 33},   {"rg32uint", 35}, {"rg32sint", 25},
    };
    auto it = table.find(wgsl);
    return it == table.end() ? ImageFormatUnknown : it->second;
}

uint32_t stageGroup(Stage s, const BindingLayout& l)
{
    switch (s)
    {
    case Stage::Vertex: return l.vertex_group;
    case Stage::Pixel: return l.pixel_group;
    default: return l.compute_group;
    }
}

const char* texelFormatName(tint::inspector::ResourceBinding::TexelFormat f)
{
    using F = tint::inspector::ResourceBinding::TexelFormat;
    switch (f)
    {
    case F::kR8Unorm: return "r8unorm";
    case F::kR8Snorm: return "r8snorm";
    case F::kR8Uint: return "r8uint";
    case F::kR8Sint: return "r8sint";
    case F::kRg8Unorm: return "rg8unorm";
    case F::kRg8Snorm: return "rg8snorm";
    case F::kRg8Uint: return "rg8uint";
    case F::kRg8Sint: return "rg8sint";
    case F::kR16Unorm: return "r16unorm";
    case F::kR16Snorm: return "r16snorm";
    case F::kR16Uint: return "r16uint";
    case F::kR16Sint: return "r16sint";
    case F::kR16Float: return "r16float";
    case F::kRg16Unorm: return "rg16unorm";
    case F::kRg16Snorm: return "rg16snorm";
    case F::kRg16Uint: return "rg16uint";
    case F::kRg16Sint: return "rg16sint";
    case F::kRg16Float: return "rg16float";
    case F::kBgra8Unorm: return "bgra8unorm";
    case F::kRgba8Unorm: return "rgba8unorm";
    case F::kRgba8Snorm: return "rgba8snorm";
    case F::kRgba8Uint: return "rgba8uint";
    case F::kRgba8Sint: return "rgba8sint";
    case F::kRgba16Unorm: return "rgba16unorm";
    case F::kRgba16Snorm: return "rgba16snorm";
    case F::kRgba16Uint: return "rgba16uint";
    case F::kRgba16Sint: return "rgba16sint";
    case F::kRgba16Float: return "rgba16float";
    case F::kR32Uint: return "r32uint";
    case F::kR32Sint: return "r32sint";
    case F::kR32Float: return "r32float";
    case F::kRg32Uint: return "rg32uint";
    case F::kRg32Sint: return "rg32sint";
    case F::kRg32Float: return "rg32float";
    case F::kRgba32Uint: return "rgba32uint";
    case F::kRgba32Sint: return "rgba32sint";
    case F::kRgba32Float: return "rgba32float";
    case F::kRgb10A2Uint: return "rgb10a2uint";
    case F::kRgb10A2Unorm: return "rgb10a2unorm";
    case F::kRg11B10Ufloat: return "rg11b10ufloat";
    case F::kNone: return "";
    }
    return "?";
}

std::string stageVarType(const tint::inspector::StageVariable& v)
{
    using C = tint::inspector::ComponentType;
    using K = tint::inspector::CompositionType;
    const char* scalar = v.component_type == C::kF32 ? "f32" : v.component_type == C::kU32 ? "u32"
                       : v.component_type == C::kI32 ? "i32" : v.component_type == C::kF16 ? "f16" : "?";
    switch (v.composition_type)
    {
    case K::kScalar: return scalar;
    case K::kVec2: return std::string("vec2<") + scalar + ">";
    case K::kVec3: return std::string("vec3<") + scalar + ">";
    case K::kVec4: return std::string("vec4<") + scalar + ">";
    default: return std::string("?<") + scalar + ">";
    }
}

const char* interpolationName(tint::inspector::InterpolationType t)
{
    using I = tint::inspector::InterpolationType;
    switch (t)
    {
    case I::kPerspective: return "perspective";
    case I::kLinear: return "linear";
    case I::kFlat: return "flat";
    default: return "";
    }
}

// The extensions a WebGPU device for the web build would enable (features requested at requestDevice) plus every
// fully shipped language feature (an empty `features` set means "shipped only").
tint::wgsl::AllowedFeatures browserFeatures()
{
    tint::wgsl::AllowedFeatures f;
    f.extensions.insert(tint::wgsl::Extension::kClipDistances);
    f.extensions.insert(tint::wgsl::Extension::kF16);
    f.extensions.insert(tint::wgsl::Extension::kDualSourceBlending);
    return f;
}

void fillBindings(tint::inspector::Inspector& insp, const std::string& ep, const Options& o, Result& r)
{
    using RB = tint::inspector::ResourceBinding;
    for (const RB& b : insp.GetResourceBindings(ep))
    {
        Binding out;
        out.group = b.bind_group;
        out.binding = b.binding;
        out.name = b.variable_name;
        switch (b.resource_type)
        {
        case RB::ResourceType::kUniformBuffer: out.kind = "uniform"; break;
        case RB::ResourceType::kStorageBuffer: out.kind = "storage_buffer"; break;
        case RB::ResourceType::kReadOnlyStorageBuffer: out.kind = "storage_buffer_read"; break;
        case RB::ResourceType::kSampler:
            out.kind = b.sampler_type == RB::SamplerType::kComparison ? "sampler_comparison" : "sampler";
            break;
        case RB::ResourceType::kSampledTexture: out.kind = "texture"; break;
        case RB::ResourceType::kMultisampledTexture: out.kind = "multisampled_texture"; break;
        case RB::ResourceType::kWriteOnlyStorageTexture: out.kind = "storage_texture_write"; break;
        case RB::ResourceType::kReadOnlyStorageTexture: out.kind = "storage_texture_read"; break;
        case RB::ResourceType::kReadWriteStorageTexture: out.kind = "storage_texture_read_write"; break;
        case RB::ResourceType::kDepthTexture: out.kind = "depth_texture"; break;
        case RB::ResourceType::kDepthMultisampledTexture: out.kind = "depth_multisampled_texture"; break;
        default: out.kind = "other"; break;
        }
        switch (b.dim)
        {
        case RB::TextureDimension::k1d: out.dim = "1d"; break;
        case RB::TextureDimension::k2d: out.dim = "2d"; break;
        case RB::TextureDimension::k2dArray: out.dim = "2d_array"; break;
        case RB::TextureDimension::k3d: out.dim = "3d"; break;
        case RB::TextureDimension::kCube: out.dim = "cube"; break;
        case RB::TextureDimension::kCubeArray: out.dim = "cube_array"; break;
        default: break;
        }
        if (out.kind.find("texture") != std::string::npos)
            switch (b.sampled_kind)
            {
            case RB::SampledKind::kUInt: out.sampled = "u32"; break;
            case RB::SampledKind::kSInt: out.sampled = "i32"; break;
            case RB::SampledKind::kUnfilterable: out.sampled = "unfilterable_f32"; break;
            default: out.sampled = "f32"; break;
            }
        out.format = texelFormatName(b.image_format);
        if (o.remap_bindings)
        {
            const BindingLayout& l = o.layout;
            if (out.binding >= l.uav_base) { out.d3d_class = 'u'; out.d3d_register = out.binding - l.uav_base; }
            else if (out.binding >= l.sampler_base) { out.d3d_class = 's'; out.d3d_register = out.binding - l.sampler_base; }
            else if (out.binding >= l.texture_base) { out.d3d_class = 't'; out.d3d_register = out.binding - l.texture_base; }
            else { out.d3d_class = 'b'; out.d3d_register = out.binding - l.cbuffer_base; }
        }
        r.bindings.push_back(out);
    }
    for (const auto& p : insp.GetSamplerTextureUses(ep))
        r.sampler_uses.push_back({p.sampler_binding_point.group, p.sampler_binding_point.binding, p.texture_binding_point.group,
                                  p.texture_binding_point.binding});
}

// The entry-point limits Dawn checks in ShaderModule.cpp (ReflectEntryPointUsingTint, ValidateComputeStageWorkgroupSize).
void checkLimits(const tint::inspector::EntryPoint& ep, const Options& o, Result& r)
{
    const Limits& L = o.limits;
    auto err = [&](std::string s) { r.limit_errors.push_back(std::move(s)); };
    std::map<uint32_t, uint32_t> perGroup;
    uint32_t ubos = 0, textures = 0, samplers = 0, storageTex = 0;
    for (const Binding& b : r.bindings)
    {
        if (b.group >= L.maxBindGroups)
            err("bind group " + std::to_string(b.group) + " >= maxBindGroups " + std::to_string(L.maxBindGroups));
        if (b.binding >= L.maxBindingsPerBindGroup)
            err("binding " + std::to_string(b.binding) + " >= maxBindingsPerBindGroup");
        if (b.kind == "uniform") ++ubos;
        else if (b.kind.rfind("sampler", 0) == 0) ++samplers;
        else if (b.kind.rfind("storage_texture", 0) == 0) ++storageTex;
        else if (b.kind.find("texture") != std::string::npos) ++textures;
    }
    if (ubos > L.maxUniformBuffersPerShaderStage) err("uniform buffers " + std::to_string(ubos) + " > maxUniformBuffersPerShaderStage");
    if (textures > L.maxSampledTexturesPerShaderStage) err("sampled textures " + std::to_string(textures) + " > maxSampledTexturesPerShaderStage");
    if (samplers > L.maxSamplersPerShaderStage) err("samplers " + std::to_string(samplers) + " > maxSamplersPerShaderStage");
    if (storageTex > L.maxStorageTexturesPerShaderStage) err("storage textures " + std::to_string(storageTex) + " > maxStorageTexturesPerShaderStage");

    using PS = tint::inspector::PipelineStage;
    if (ep.stage == PS::kVertex)
    {
        for (const auto& v : ep.input_variables)
            if (v.attributes.location && *v.attributes.location >= L.maxVertexAttributes)
                err("vertex attribute location " + std::to_string(*v.attributes.location) + " >= maxVertexAttributes");
        const uint32_t clipSlots = ep.clip_distances_size ? (*ep.clip_distances_size + 3) / 4 : 0;
        for (const auto& v : ep.output_variables)
            if (v.attributes.location && *v.attributes.location >= L.maxInterStageShaderVariables - clipSlots)
                err("vertex output location " + std::to_string(*v.attributes.location) + " >= maxInterStageShaderVariables (" +
                    std::to_string(L.maxInterStageShaderVariables) + (clipSlots ? " - clip_distances slots" : "") + ")");
        if (ep.output_variables.size() + clipSlots > L.maxInterStageShaderVariables)
            err("vertex outputs " + std::to_string(ep.output_variables.size() + clipSlots) + " > maxInterStageShaderVariables");
    }
    else if (ep.stage == PS::kFragment)
    {
        uint32_t total = 0;
        for (const auto& v : ep.input_variables)
        {
            if (!v.attributes.location)
                continue;
            ++total;
            if (*v.attributes.location >= L.maxInterStageShaderVariables)
                err("fragment input location " + std::to_string(*v.attributes.location) + " >= maxInterStageShaderVariables");
        }
        total += ep.front_facing_used + ep.input_sample_mask_used + ep.sample_index_used + ep.primitive_index_used;
        if (total > L.maxInterStageShaderVariables)
            err("fragment inputs " + std::to_string(total) + " > maxInterStageShaderVariables");
        for (const auto& v : ep.output_variables)
            if (v.attributes.location && *v.attributes.location >= L.maxColorAttachments)
                err("color output " + std::to_string(*v.attributes.location) + " >= maxColorAttachments");
    }
    else if (ep.workgroup_size)
    {
        const auto& w = *ep.workgroup_size;
        if (w.x > L.maxComputeWorkgroupSizeX || w.y > L.maxComputeWorkgroupSizeY || w.z > L.maxComputeWorkgroupSizeZ)
            err("workgroup_size(" + std::to_string(w.x) + "," + std::to_string(w.y) + "," + std::to_string(w.z) +
                ") > maxComputeWorkgroupSize");
        if (uint64_t(w.x) * w.y * w.z > L.maxComputeInvocationsPerWorkgroup)
            err("workgroup invocations " + std::to_string(uint64_t(w.x) * w.y * w.z) + " > maxComputeInvocationsPerWorkgroup " +
                std::to_string(L.maxComputeInvocationsPerWorkgroup));
        if (r.workgroup_storage_bytes > L.maxComputeWorkgroupStorageSize)
            err("workgroup storage " + std::to_string(r.workgroup_storage_bytes) + " bytes > maxComputeWorkgroupStorageSize");
    }
}
} // namespace

const char* failureName(Failure f)
{
    switch (f)
    {
    case Failure::None: return "none";
    case Failure::NotBuilt: return "not_built";
    case Failure::SpirvPatch: return "spirv_patch";
    case Failure::SpirvReader: return "spirv_reader";
    case Failure::IrToProgram: return "ir_to_program";
    case Failure::WgslWriter: return "wgsl_writer";
    case Failure::Validation: return "validation";
    case Failure::Lowering: return "lowering";
    }
    return "?";
}

bool available() { return true; }

const char* tintVersion() { return "Tint (Dawn " ANOMALY_DAWN_REVISION ")"; }

bool patchSpirv(std::vector<uint32_t>& w, const Options& o, std::string& error, PatchStats* stats)
{
    if (w.size() < 5 || w[0] != 0x07230203u)
    {
        error = "not a SPIR-V module";
        return false;
    }
    // Pass 1: storage classes of variables, pointer pointees, which ids are builtins, descriptor bindings.
    std::map<uint32_t, uint32_t> varClass, varType, ptrPointee, varBinding;
    std::set<uint32_t> builtins;
    std::map<uint32_t, size_t> imageTypeAt; // OpTypeImage result id -> word index of the instruction
    for (size_t i = 5; i < w.size();)
    {
        const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (len == 0 || i + len > w.size())
        {
            error = "malformed instruction stream";
            return false;
        }
        if (op == OpVariable && len >= 4)
        {
            varType[w[i + 2]] = w[i + 1];
            varClass[w[i + 2]] = w[i + 3];
        }
        else if (op == OpTypePointer && len >= 4)
            ptrPointee[w[i + 1]] = w[i + 3];
        else if (op == OpTypeImage && len >= 9)
            imageTypeAt[w[i + 1]] = i;
        else if (op == OpDecorate && len >= 4 && w[i + 2] == DecorationBuiltIn)
            builtins.insert(w[i + 1]);
        else if (op == OpDecorate && len >= 4 && w[i + 2] == DecorationBinding)
            varBinding[w[i + 1]] = w[i + 3];
        i += len;
    }

    const std::map<uint32_t, uint32_t> locMap(o.locations.begin(), o.locations.end());
    const uint32_t interstageClass = o.stage == Stage::Vertex ? StorageClassOutput : o.stage == Stage::Pixel ? StorageClassInput : ~0u;
    const uint32_t group = stageGroup(o.stage, o.layout);

    // Pass 2: rewrite decorations in place (same word count, so no offsets move).
    for (size_t i = 5; i < w.size();)
    {
        const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpDecorate && len >= 4)
        {
            const uint32_t target = w[i + 1], deco = w[i + 2];
            if (deco == DecorationLocation && !locMap.empty() && !builtins.count(target))
            {
                auto c = varClass.find(target);
                if (c != varClass.end() && c->second == interstageClass)
                {
                    auto m = locMap.find(w[i + 3]);
                    if (m != locMap.end())
                        w[i + 3] = m->second;
                }
            }
            else if (o.remap_bindings && deco == DecorationDescriptorSet)
                w[i + 3] = group;
            else if (o.remap_bindings && deco == DecorationBinding)
            {
                const uint32_t b = w[i + 3];
                const uint32_t reg = b % 256;
                switch (b / 256)
                {
                case detail::kBindingCbv / 256: w[i + 3] = o.layout.cbuffer_base + reg; break;
                case detail::kBindingSrv / 256: w[i + 3] = o.layout.texture_base + reg; break;
                case detail::kBindingSampler / 256: w[i + 3] = o.layout.sampler_base + reg; break;
                default: w[i + 3] = o.layout.uav_base + reg; break;
                }
            }
        }
        i += len;
    }

    // Storage-image formats for UAVs: variable (binding kBindingUav + u) -> pointer -> image type.
    for (const auto& [reg, fmtName] : o.uav_formats)
    {
        const uint32_t fmt = spirvImageFormat(fmtName);
        if (fmt == ImageFormatUnknown)
        {
            error = "unknown texel format " + fmtName;
            return false;
        }
        for (const auto& [var, binding] : varBinding)
        {
            if (binding != detail::kBindingUav + reg)
                continue;
            auto vt = varType.find(var);
            if (vt == varType.end())
                continue;
            auto pt = ptrPointee.find(vt->second);
            if (pt == ptrPointee.end())
                continue;
            auto it = imageTypeAt.find(pt->second);
            if (it != imageTypeAt.end() && w[it->second + 8] == ImageFormatUnknown)
                w[it->second + 8] = fmt;
        }
    }
    Module m;
    if (!m.parse(w))
    {
        error = "malformed instruction stream";
        return false;
    }
    if (o.polyfill_float_classification)
    {
        const size_t classification = polyfillFloatClassification(m);
        const size_t constants = materializeNonFiniteConstants(m);
        if (stats)
        {
            stats->float_classification_rewrites = (uint32_t)classification;
            stats->nonfinite_constants = (uint32_t)constants;
        }
    }
    if (!o.uav_formats.empty())
        fixStorageImageCapabilities(m);
    w = m.words();
    return true;
}

Result fromSpirv(const std::vector<uint32_t>& input, const Options& o)
{
    const auto t0 = std::chrono::steady_clock::now();
    Result r;
    auto done = [&]() -> Result& {
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return r;
    };

    std::vector<uint32_t> spirv = input;
    std::string err;
    PatchStats ps;
    const bool patched = patchSpirv(spirv, o, err, &ps);
    r.float_classification_rewrites = ps.float_classification_rewrites;
    r.nonfinite_constants = ps.nonfinite_constants;
    if (!patched)
    {
        r.failure = Failure::SpirvPatch;
        r.log = err;
        return done();
    }

    auto ir = tint::spirv::reader::ReadIR(spirv);
    if (ir != tint::Success)
    {
        r.failure = Failure::SpirvReader;
        r.log = ir.Failure().reason;
        return done();
    }

    tint::wgsl::writer::Options wo;
    wo.allow_non_uniform_derivatives = o.allow_non_uniform_derivatives;
    wo.disable_unreachable_code_warning = false; // the opt-out is a chromium.* rule; keep the WGSL portable
    wo.allowed_features = tint::wgsl::AllowedFeatures::Everything();
    auto program = tint::wgsl::writer::ProgramFromIR(ir.Get(), wo);
    if (program != tint::Success)
    {
        r.failure = Failure::IrToProgram;
        r.log = program.Failure().reason;
        return done();
    }
    auto printed = tint::wgsl::writer::Generate(program.Get(), wo);
    if (printed != tint::Success)
    {
        r.failure = Failure::WgslWriter;
        r.log = printed.Failure().reason;
        return done();
    }
    r.wgsl = printed->wgsl;

    // What the browser does with the text: parse and resolve (createShaderModule), then lower to IR (pipeline creation).
    tint::wgsl::reader::Options ro;
    ro.allowed_features = browserFeatures();
    tint::Source::File file("shader.wgsl", r.wgsl);
    tint::Program parsed = tint::wgsl::reader::Parse(&file, ro);
    if (!parsed.IsValid())
    {
        r.failure = Failure::Validation;
        r.log = parsed.Diagnostics().Str();
        return done();
    }
    auto lowered = tint::wgsl::reader::ProgramToLoweredIR(parsed);
    if (lowered != tint::Success)
    {
        r.failure = Failure::Lowering;
        r.log = lowered.Failure().reason;
        return done();
    }
    r.ok = true;
    if (parsed.Diagnostics().Count())
        r.log = parsed.Diagnostics().Str(); // warnings

    // Would the shader validate without the derivative-uniformity opt-out?
    static const std::string kDirective = "diagnostic(off, derivative_uniformity);";
    const size_t at = r.wgsl.find(kDirective);
    if (at != std::string::npos)
    {
        std::string strict = r.wgsl;
        strict.erase(at, kDirective.size());
        tint::Source::File strictFile("shader.wgsl", strict);
        tint::Program p2 = tint::wgsl::reader::Parse(&strictFile, ro);
        r.derivative_uniformity_needed = !p2.IsValid();
    }
    for (size_t p = 0; (p = r.wgsl.find("enable ", p)) != std::string::npos;)
    {
        if (p == 0 || r.wgsl[p - 1] == '\n')
        {
            const size_t e = r.wgsl.find(';', p);
            r.enables.push_back(r.wgsl.substr(p + 7, e - p - 7));
        }
        p += 7;
    }
    r.uses_discard = r.wgsl.find("discard;") != std::string::npos;

    // Reflection of the validated program.
    tint::inspector::Inspector insp(parsed);
    const auto eps = insp.GetEntryPoints();
    if (!eps.empty())
    {
        const tint::inspector::EntryPoint& ep = eps.front();
        r.entry_point = ep.name;
        fillBindings(insp, ep.name, o, r);
        for (const auto& v : ep.input_variables)
            if (v.attributes.location)
                r.inputs.push_back({*v.attributes.location, stageVarType(v), interpolationName(v.interpolation_type)});
        for (const auto& v : ep.output_variables)
            if (v.attributes.location)
                r.outputs.push_back({*v.attributes.location, stageVarType(v), interpolationName(v.interpolation_type)});
        r.depth_texture_with_non_comparison_sampler = ep.has_depth_texture_with_non_comparison_sampler;
        r.texture_load_with_depth_texture = ep.has_texture_load_with_depth_texture;
        r.clip_distances = ep.clip_distances_size.value_or(0);
        if (ep.workgroup_size)
        {
            r.workgroup[0] = ep.workgroup_size->x;
            r.workgroup[1] = ep.workgroup_size->y;
            r.workgroup[2] = ep.workgroup_size->z;
        }
        for (const tint::ast::Variable* v : parsed.AST().GlobalVariables())
        {
            const tint::sem::Variable* sv = parsed.Sem().Get(v);
            if (sv && sv->AddressSpace() == tint::core::AddressSpace::kWorkgroup)
                r.workgroup_storage_bytes += (sv->Type()->UnwrapRef()->Size() + 15) & ~15u;
        }
        checkLimits(ep, o, r);
    }
    return done();
}
} // namespace shaderxlat::wgsl
