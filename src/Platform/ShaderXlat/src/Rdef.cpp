// Reader for the RDEF chunk of SM4/SM5 DXBC (the data behind ID3D11ShaderReflection).
//
//   header: u32 cbuffer_count, cbuffer_offset, resource_count, resource_offset,
//           u32 target (minor | major << 8 | program type << 16), flags, creator_offset
//   SM5+  : u32 'RD11' + 7 u32 (header size and the sizes of the descs below)
//   binding desc : name, input type, return type, dimension, num samples, bind point, bind count, flags
//   cbuffer desc : name, variable count, variable offset, size, flags, type
//   variable desc: name, start offset, size, flags, type offset, default value, + 4 u32 on SM5
//   type desc    : (class | type << 16), (rows | columns << 16), (elements | members << 16), member offset,
//                  + 5 u32 on SM5 (the last is the struct name)
//   member desc  : name, type offset, byte offset
#include "Internal.h"

#include <algorithm>
#include <cstring>

namespace shaderxlat::detail
{
namespace
{
struct Reader
{
    const uint8_t* d;
    size_t n;
    bool bad = false;

    uint32_t u32(size_t off)
    {
        if (off + 4 > n)
        {
            bad = true;
            return 0;
        }
        uint32_t v;
        std::memcpy(&v, d + off, 4);
        return v;
    }

    std::string str(size_t off)
    {
        if (off >= n)
        {
            bad = true;
            return {};
        }
        size_t e = off;
        while (e < n && d[e])
            ++e;
        return std::string((const char*)d + off, e - off);
    }
};

enum { SVC_SCALAR = 0, SVC_VECTOR = 1, SVC_MATRIX_ROWS = 2, SVC_MATRIX_COLUMNS = 3, SVC_STRUCT = 5 };
enum { SVT_BOOL = 1, SVT_INT = 2, SVT_FLOAT = 3, SVT_UINT = 19, SVT_UINT8 = 20, SVT_DOUBLE = 39 };
enum { SVF_USED = 2 };

enum
{
    SIT_CBUFFER = 0, SIT_TBUFFER, SIT_TEXTURE, SIT_SAMPLER, SIT_UAV_RWTYPED, SIT_STRUCTURED, SIT_UAV_RWSTRUCTURED,
    SIT_BYTEADDRESS, SIT_UAV_RWBYTEADDRESS, SIT_UAV_APPEND_STRUCTURED, SIT_UAV_CONSUME_STRUCTURED,
    SIT_UAV_RWSTRUCTURED_WITH_COUNTER
};

ScalarType scalarFromSvt(uint32_t t)
{
    switch (t)
    {
    case SVT_BOOL: return ScalarType::Bool;
    case SVT_INT: return ScalarType::Int;
    case SVT_FLOAT: return ScalarType::Float;
    case SVT_UINT:
    case SVT_UINT8: return ScalarType::UInt;
    case SVT_DOUBLE: return ScalarType::Double;
    default: return ScalarType::Float;
    }
}

TextureDim dimFromSrv(uint32_t d)
{
    switch (d)
    {
    case 1: return TextureDim::Buffer;
    case 2: return TextureDim::Tex1D;
    case 3: return TextureDim::Tex1DArray;
    case 5: return TextureDim::Tex2DArray;
    case 6: return TextureDim::Tex2DMS;
    case 7: return TextureDim::Tex2DMSArray;
    case 8: return TextureDim::Tex3D;
    case 9: return TextureDim::TexCube;
    case 10: return TextureDim::TexCubeArray;
    case 11: return TextureDim::Buffer;
    default: return TextureDim::Tex2D;
    }
}

ScalarType returnType(uint32_t r)
{
    switch (r)
    {
    case 3: return ScalarType::Int;
    case 4: return ScalarType::UInt;
    case 7: return ScalarType::Double;
    default: return ScalarType::Float;
    }
}

struct TypeInfo
{
    uint32_t cls, type, rows, cols, elements, memberCount, memberOffset;
};

// Size of one array element of a numeric type, D3D packing: rows are 16-byte registers.
uint32_t numericSize(const TypeInfo& t)
{
    switch (t.cls)
    {
    case SVC_SCALAR: return 4;
    case SVC_VECTOR: return t.cols * 4;
    case SVC_MATRIX_ROWS: return (t.rows - 1) * 16 + t.cols * 4;
    case SVC_MATRIX_COLUMNS: return (t.cols - 1) * 16 + t.rows * 4;
    default: return 0;
    }
}

struct Parser
{
    Reader r;
    bool sm5 = false;
    uint32_t typeDescSize = 16, memberDescSize = 12, varDescSize = 24;

    TypeInfo type(size_t off)
    {
        TypeInfo t;
        const uint32_t d0 = r.u32(off), d1 = r.u32(off + 4), d2 = r.u32(off + 8);
        t.cls = d0 & 0xffff;
        t.type = d0 >> 16;
        t.rows = d1 & 0xffff;
        t.cols = d1 >> 16;
        t.elements = d2 & 0xffff;
        t.memberCount = d2 >> 16;
        t.memberOffset = r.u32(off + 12);
        return t;
    }

    // Fills `m` (name/offset/used already set by the caller) from the type at `off`. Returns the byte
    // extent of one array element.
    uint32_t fillType(ConstantMember& m, size_t off, uint32_t baseOffset, int depth)
    {
        const TypeInfo t = type(off);
        m.type = t.cls == SVC_STRUCT ? ScalarType::Struct : scalarFromSvt(t.type);
        m.rows = t.rows ? t.rows : 1;
        m.columns = t.cols ? t.cols : 1;
        m.row_major = t.cls == SVC_MATRIX_ROWS;
        m.array_elements = t.elements;
        if (sm5)
            m.type_name = r.str(r.u32(off + 32));
        uint32_t one = 0;
        if (t.cls == SVC_STRUCT && depth < 8)
        {
            uint32_t end = 0;
            for (uint32_t k = 0; k < t.memberCount; ++k)
            {
                const size_t mo = t.memberOffset + (size_t)k * memberDescSize;
                ConstantMember sub;
                sub.name = r.str(r.u32(mo));
                const uint32_t rel = r.u32(mo + 8);
                sub.offset = baseOffset + rel;
                const uint32_t ext = fillType(sub, r.u32(mo + 4), sub.offset, depth + 1);
                const uint32_t total = sub.array_elements ? sub.array_stride * (sub.array_elements - 1) + ext : ext;
                sub.size = total;
                end = std::max(end, rel + total);
                m.members.push_back(std::move(sub));
            }
            one = end;
        }
        else
            one = numericSize(t);
        if (t.elements)
        {
            m.array_stride = (one + 15) & ~15u;
            return one;
        }
        return one;
    }
};
} // namespace

bool parseRdef(const uint8_t* data, size_t size, Reflection& out, std::string& error)
{
    Parser p;
    p.r = Reader{data, size};
    Reader& r = p.r;
    const uint32_t cbCount = r.u32(0), cbOff = r.u32(4), resCount = r.u32(8), resOff = r.u32(12), target = r.u32(16);
    const uint32_t major = (target >> 8) & 0xff;
    uint32_t cbDescSize = 24, bindDescSize = 32;
    p.sm5 = major >= 5;
    if (p.sm5)
    {
        if (r.u32(28) != 0x31314452 && r.u32(28) != 0x52443131) // 'RD11' / reversed for SM5.1
        {
            error = "RDEF: missing RD11 header";
            return false;
        }
        cbDescSize = r.u32(36);
        bindDescSize = r.u32(40);
        p.varDescSize = r.u32(44);
        p.typeDescSize = r.u32(48);
        p.memberDescSize = r.u32(52);
    }
    else
        p.varDescSize = 24;
    if (r.bad || cbCount > 4096 || resCount > 4096 || bindDescSize < 32 || cbDescSize < 24)
    {
        error = "RDEF: corrupt header";
        return false;
    }

    // Bound resources.
    for (uint32_t i = 0; i < resCount; ++i)
    {
        const size_t b = resOff + (size_t)i * bindDescSize;
        const std::string name = r.str(r.u32(b));
        const uint32_t sit = r.u32(b + 4), ret = r.u32(b + 8), dim = r.u32(b + 12), slot = r.u32(b + 20);
        switch (sit)
        {
        case SIT_CBUFFER:
        case SIT_TBUFFER: break; // filled from the cbuffer table below
        case SIT_TEXTURE:
        case SIT_UAV_RWTYPED:
        {
            TextureBinding t;
            t.name = name;
            t.slot = slot;
            t.dim = dimFromSrv(dim);
            t.sampled_type = returnType(ret);
            t.is_uav = sit == SIT_UAV_RWTYPED;
            t.multisampled = t.dim == TextureDim::Tex2DMS || t.dim == TextureDim::Tex2DMSArray;
            out.textures.push_back(t);
            break;
        }
        case SIT_SAMPLER:
        {
            SamplerBinding s;
            s.name = name;
            s.slot = slot;
            out.samplers.push_back(s);
            break;
        }
        default:
        {
            StorageBuffer sb;
            sb.name = name;
            sb.slot = slot;
            sb.is_uav = sit != SIT_STRUCTURED && sit != SIT_BYTEADDRESS;
            out.storage_buffers.push_back(sb);
        }
        }
    }

    // Constant buffers: the slot comes from the matching bound-resource entry.
    for (uint32_t i = 0; i < cbCount; ++i)
    {
        const size_t b = cbOff + (size_t)i * cbDescSize;
        ConstantBuffer cb;
        cb.name = r.str(r.u32(b));
        const uint32_t varCount = r.u32(b + 4), varOff = r.u32(b + 8);
        cb.size = r.u32(b + 12);
        const uint32_t cbType = r.u32(b + 20);
        if (cbType != 0 /* D3D_CT_CBUFFER */)
            continue;
        bool found = false;
        for (uint32_t k = 0; k < resCount && !found; ++k)
        {
            const size_t rb = resOff + (size_t)k * bindDescSize;
            if (r.u32(rb + 4) <= SIT_TBUFFER && r.str(r.u32(rb)) == cb.name)
            {
                cb.slot = r.u32(rb + 20);
                found = true;
            }
        }
        if (!found || varCount > 65536)
            continue;
        for (uint32_t j = 0; j < varCount; ++j)
        {
            const size_t vb = varOff + (size_t)j * p.varDescSize;
            ConstantMember m;
            m.name = r.str(r.u32(vb));
            m.offset = r.u32(vb + 4);
            m.size = r.u32(vb + 8);
            m.used = (r.u32(vb + 12) & SVF_USED) != 0;
            const uint32_t ext = p.fillType(m, r.u32(vb + 16), m.offset, 0);
            if (m.array_elements && m.size == 0)
                m.size = m.array_stride * (m.array_elements - 1) + ext;
            cb.members.push_back(std::move(m));
        }
        out.constant_buffers.push_back(std::move(cb));
    }
    if (r.bad)
    {
        error = "RDEF: out-of-range offset";
        return false;
    }
    return true;
}
} // namespace shaderxlat::detail
