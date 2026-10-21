// DXGI <-> Metal format tables.
#import "mtl_internal.h"

#include <cstdarg>
#include <cstdio>

namespace mtl_d3d11
{
void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[MetalD3D11] %s\n", buf);
}

namespace
{
struct Row
{
    DXGI_FORMAT dxgi;
    MTLPixelFormat mtl;
    uint8_t bytes;
    uint8_t block;
    uint8_t flags;
};
enum : uint8_t
{
    F_NONE = 0,
    F_COMPRESSED = 1,
    F_DEPTH = 2,
    F_STENCIL = 4,
    F_TYPELESS = 8,
    F_SRGB = 16,
    F_INT = 32
};

#define ROW(d, m, b, k, f) {DXGI_FORMAT_##d, m, b, k, f}
const Row kRows[] = {
    ROW(UNKNOWN, MTLPixelFormatInvalid, 0, 1, F_NONE),
    ROW(R32G32B32A32_TYPELESS, MTLPixelFormatRGBA32Float, 16, 1, F_TYPELESS),
    ROW(R32G32B32A32_FLOAT, MTLPixelFormatRGBA32Float, 16, 1, F_NONE),
    ROW(R32G32B32A32_UINT, MTLPixelFormatRGBA32Uint, 16, 1, F_INT),
    ROW(R32G32B32A32_SINT, MTLPixelFormatRGBA32Sint, 16, 1, F_INT),
    ROW(R32G32B32_TYPELESS, MTLPixelFormatInvalid, 12, 1, F_TYPELESS),
    ROW(R32G32B32_FLOAT, MTLPixelFormatInvalid, 12, 1, F_NONE), // vertex data only
    ROW(R32G32B32_UINT, MTLPixelFormatInvalid, 12, 1, F_INT),
    ROW(R32G32B32_SINT, MTLPixelFormatInvalid, 12, 1, F_INT),
    ROW(R16G16B16A16_TYPELESS, MTLPixelFormatRGBA16Float, 8, 1, F_TYPELESS),
    ROW(R16G16B16A16_FLOAT, MTLPixelFormatRGBA16Float, 8, 1, F_NONE),
    ROW(R16G16B16A16_UNORM, MTLPixelFormatRGBA16Unorm, 8, 1, F_NONE),
    ROW(R16G16B16A16_UINT, MTLPixelFormatRGBA16Uint, 8, 1, F_INT),
    ROW(R16G16B16A16_SNORM, MTLPixelFormatRGBA16Snorm, 8, 1, F_NONE),
    ROW(R16G16B16A16_SINT, MTLPixelFormatRGBA16Sint, 8, 1, F_INT),
    ROW(R32G32_TYPELESS, MTLPixelFormatRG32Float, 8, 1, F_TYPELESS),
    ROW(R32G32_FLOAT, MTLPixelFormatRG32Float, 8, 1, F_NONE),
    ROW(R32G32_UINT, MTLPixelFormatRG32Uint, 8, 1, F_INT),
    ROW(R32G32_SINT, MTLPixelFormatRG32Sint, 8, 1, F_INT),
    ROW(R32G8X24_TYPELESS, MTLPixelFormatDepth32Float_Stencil8, 8, 1, F_TYPELESS | F_DEPTH | F_STENCIL),
    ROW(D32_FLOAT_S8X24_UINT, MTLPixelFormatDepth32Float_Stencil8, 8, 1, F_DEPTH | F_STENCIL),
    ROW(R32_FLOAT_X8X24_TYPELESS, MTLPixelFormatDepth32Float_Stencil8, 8, 1, F_DEPTH | F_STENCIL),
    ROW(X32_TYPELESS_G8X24_UINT, MTLPixelFormatDepth32Float_Stencil8, 8, 1, F_DEPTH | F_STENCIL),
    ROW(R10G10B10A2_TYPELESS, MTLPixelFormatRGB10A2Unorm, 4, 1, F_TYPELESS),
    ROW(R10G10B10A2_UNORM, MTLPixelFormatRGB10A2Unorm, 4, 1, F_NONE),
    ROW(R10G10B10A2_UINT, MTLPixelFormatRGB10A2Uint, 4, 1, F_INT),
    ROW(R11G11B10_FLOAT, MTLPixelFormatRG11B10Float, 4, 1, F_NONE),
    ROW(R8G8B8A8_TYPELESS, MTLPixelFormatRGBA8Unorm, 4, 1, F_TYPELESS),
    ROW(R8G8B8A8_UNORM, MTLPixelFormatRGBA8Unorm, 4, 1, F_NONE),
    ROW(R8G8B8A8_UNORM_SRGB, MTLPixelFormatRGBA8Unorm_sRGB, 4, 1, F_SRGB),
    ROW(R8G8B8A8_UINT, MTLPixelFormatRGBA8Uint, 4, 1, F_INT),
    ROW(R8G8B8A8_SNORM, MTLPixelFormatRGBA8Snorm, 4, 1, F_NONE),
    ROW(R8G8B8A8_SINT, MTLPixelFormatRGBA8Sint, 4, 1, F_INT),
    ROW(R16G16_TYPELESS, MTLPixelFormatRG16Float, 4, 1, F_TYPELESS),
    ROW(R16G16_FLOAT, MTLPixelFormatRG16Float, 4, 1, F_NONE),
    ROW(R16G16_UNORM, MTLPixelFormatRG16Unorm, 4, 1, F_NONE),
    ROW(R16G16_UINT, MTLPixelFormatRG16Uint, 4, 1, F_INT),
    ROW(R16G16_SNORM, MTLPixelFormatRG16Snorm, 4, 1, F_NONE),
    ROW(R16G16_SINT, MTLPixelFormatRG16Sint, 4, 1, F_INT),
    ROW(R32_TYPELESS, MTLPixelFormatR32Float, 4, 1, F_TYPELESS),
    ROW(D32_FLOAT, MTLPixelFormatDepth32Float, 4, 1, F_DEPTH),
    ROW(R32_FLOAT, MTLPixelFormatR32Float, 4, 1, F_NONE),
    ROW(R32_UINT, MTLPixelFormatR32Uint, 4, 1, F_INT),
    ROW(R32_SINT, MTLPixelFormatR32Sint, 4, 1, F_INT),
    // 24-bit depth does not exist on Apple GPUs; D24S8 is stored as 32F + 8 stencil.
    ROW(R24G8_TYPELESS, MTLPixelFormatDepth32Float_Stencil8, 8, 1, F_TYPELESS | F_DEPTH | F_STENCIL),
    ROW(D24_UNORM_S8_UINT, MTLPixelFormatDepth32Float_Stencil8, 8, 1, F_DEPTH | F_STENCIL),
    ROW(R24_UNORM_X8_TYPELESS, MTLPixelFormatDepth32Float_Stencil8, 8, 1, F_DEPTH | F_STENCIL),
    ROW(X24_TYPELESS_G8_UINT, MTLPixelFormatDepth32Float_Stencil8, 8, 1, F_DEPTH | F_STENCIL),
    ROW(R8G8_TYPELESS, MTLPixelFormatRG8Unorm, 2, 1, F_TYPELESS),
    ROW(R8G8_UNORM, MTLPixelFormatRG8Unorm, 2, 1, F_NONE),
    ROW(R8G8_UINT, MTLPixelFormatRG8Uint, 2, 1, F_INT),
    ROW(R8G8_SNORM, MTLPixelFormatRG8Snorm, 2, 1, F_NONE),
    ROW(R8G8_SINT, MTLPixelFormatRG8Sint, 2, 1, F_INT),
    ROW(R16_TYPELESS, MTLPixelFormatR16Float, 2, 1, F_TYPELESS),
    ROW(R16_FLOAT, MTLPixelFormatR16Float, 2, 1, F_NONE),
    ROW(D16_UNORM, MTLPixelFormatDepth16Unorm, 2, 1, F_DEPTH),
    ROW(R16_UNORM, MTLPixelFormatR16Unorm, 2, 1, F_NONE),
    ROW(R16_UINT, MTLPixelFormatR16Uint, 2, 1, F_INT),
    ROW(R16_SNORM, MTLPixelFormatR16Snorm, 2, 1, F_NONE),
    ROW(R16_SINT, MTLPixelFormatR16Sint, 2, 1, F_INT),
    ROW(R8_TYPELESS, MTLPixelFormatR8Unorm, 1, 1, F_TYPELESS),
    ROW(R8_UNORM, MTLPixelFormatR8Unorm, 1, 1, F_NONE),
    ROW(R8_UINT, MTLPixelFormatR8Uint, 1, 1, F_INT),
    ROW(R8_SNORM, MTLPixelFormatR8Snorm, 1, 1, F_NONE),
    ROW(R8_SINT, MTLPixelFormatR8Sint, 1, 1, F_INT),
    ROW(A8_UNORM, MTLPixelFormatA8Unorm, 1, 1, F_NONE),
    ROW(R9G9B9E5_SHAREDEXP, MTLPixelFormatRGB9E5Float, 4, 1, F_NONE),
    ROW(BC1_TYPELESS, MTLPixelFormatBC1_RGBA, 8, 4, F_TYPELESS | F_COMPRESSED),
    ROW(BC1_UNORM, MTLPixelFormatBC1_RGBA, 8, 4, F_COMPRESSED),
    ROW(BC1_UNORM_SRGB, MTLPixelFormatBC1_RGBA_sRGB, 8, 4, F_COMPRESSED | F_SRGB),
    ROW(BC2_TYPELESS, MTLPixelFormatBC2_RGBA, 16, 4, F_TYPELESS | F_COMPRESSED),
    ROW(BC2_UNORM, MTLPixelFormatBC2_RGBA, 16, 4, F_COMPRESSED),
    ROW(BC2_UNORM_SRGB, MTLPixelFormatBC2_RGBA_sRGB, 16, 4, F_COMPRESSED | F_SRGB),
    ROW(BC3_TYPELESS, MTLPixelFormatBC3_RGBA, 16, 4, F_TYPELESS | F_COMPRESSED),
    ROW(BC3_UNORM, MTLPixelFormatBC3_RGBA, 16, 4, F_COMPRESSED),
    ROW(BC3_UNORM_SRGB, MTLPixelFormatBC3_RGBA_sRGB, 16, 4, F_COMPRESSED | F_SRGB),
    ROW(BC4_TYPELESS, MTLPixelFormatBC4_RUnorm, 8, 4, F_TYPELESS | F_COMPRESSED),
    ROW(BC4_UNORM, MTLPixelFormatBC4_RUnorm, 8, 4, F_COMPRESSED),
    ROW(BC4_SNORM, MTLPixelFormatBC4_RSnorm, 8, 4, F_COMPRESSED),
    ROW(BC5_TYPELESS, MTLPixelFormatBC5_RGUnorm, 16, 4, F_TYPELESS | F_COMPRESSED),
    ROW(BC5_UNORM, MTLPixelFormatBC5_RGUnorm, 16, 4, F_COMPRESSED),
    ROW(BC5_SNORM, MTLPixelFormatBC5_RGSnorm, 16, 4, F_COMPRESSED),
    ROW(B5G6R5_UNORM, MTLPixelFormatB5G6R5Unorm, 2, 1, F_NONE),
    ROW(B5G5R5A1_UNORM, MTLPixelFormatBGR5A1Unorm, 2, 1, F_NONE),
    ROW(B8G8R8A8_UNORM, MTLPixelFormatBGRA8Unorm, 4, 1, F_NONE),
    ROW(B8G8R8X8_UNORM, MTLPixelFormatBGRA8Unorm, 4, 1, F_NONE),
    ROW(B8G8R8A8_TYPELESS, MTLPixelFormatBGRA8Unorm, 4, 1, F_TYPELESS),
    ROW(B8G8R8A8_UNORM_SRGB, MTLPixelFormatBGRA8Unorm_sRGB, 4, 1, F_SRGB),
    ROW(B8G8R8X8_TYPELESS, MTLPixelFormatBGRA8Unorm, 4, 1, F_TYPELESS),
    ROW(B8G8R8X8_UNORM_SRGB, MTLPixelFormatBGRA8Unorm_sRGB, 4, 1, F_SRGB),
    ROW(BC6H_TYPELESS, MTLPixelFormatBC6H_RGBUfloat, 16, 4, F_TYPELESS | F_COMPRESSED),
    ROW(BC6H_UF16, MTLPixelFormatBC6H_RGBUfloat, 16, 4, F_COMPRESSED),
    ROW(BC6H_SF16, MTLPixelFormatBC6H_RGBFloat, 16, 4, F_COMPRESSED),
    ROW(BC7_TYPELESS, MTLPixelFormatBC7_RGBAUnorm, 16, 4, F_TYPELESS | F_COMPRESSED),
    ROW(BC7_UNORM, MTLPixelFormatBC7_RGBAUnorm, 16, 4, F_COMPRESSED),
    ROW(BC7_UNORM_SRGB, MTLPixelFormatBC7_RGBAUnorm_sRGB, 16, 4, F_COMPRESSED | F_SRGB),
};
#undef ROW

struct Table
{
    FormatInfo info[128];
    Table()
    {
        for (const Row& r : kRows)
        {
            FormatInfo& fi = info[r.dxgi];
            fi.dxgi = r.dxgi;
            fi.mtl = r.mtl;
            fi.bytesPerBlock = r.bytes;
            fi.blockSize = r.block;
            fi.compressed = r.flags & F_COMPRESSED;
            fi.depth = r.flags & F_DEPTH;
            fi.stencil = r.flags & F_STENCIL;
            fi.typeless = r.flags & F_TYPELESS;
            fi.srgb = r.flags & F_SRGB;
            fi.integer = r.flags & F_INT;
        }
    }
};
} // namespace

const FormatInfo& GetFormatInfo(DXGI_FORMAT f)
{
    static const Table t;
    static const FormatInfo unknown;
    if ((unsigned)f >= 128)
        return unknown;
    return t.info[f];
}

MTLPixelFormat TextureStorageFormat(DXGI_FORMAT f, UINT bindFlags)
{
    const FormatInfo& fi = GetFormatInfo(f);
    const bool ds = (bindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS: return ds ? MTLPixelFormatDepth32Float : MTLPixelFormatR32Float;
    case DXGI_FORMAT_R16_TYPELESS: return ds ? MTLPixelFormatDepth16Unorm : MTLPixelFormatR16Unorm;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS: return MTLPixelFormatDepth32Float_Stencil8;
    default: return fi.mtl;
    }
}

MTLPixelFormat ViewFormat(DXGI_FORMAT f, bool* stencilOnly)
{
    if (stencilOnly)
        *stencilOnly = false;
    switch (f)
    {
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        if (stencilOnly)
            *stencilOnly = true;
        return MTLPixelFormatX32_Stencil8;
    case DXGI_FORMAT_R16_TYPELESS: return MTLPixelFormatR16Float;
    case DXGI_FORMAT_R32_TYPELESS: return MTLPixelFormatR32Float;
    default: return GetFormatInfo(f).mtl;
    }
}

MTLVertexFormat VertexFormatFor(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_FLOAT: return MTLVertexFormatFloat;
    case DXGI_FORMAT_R32G32_FLOAT: return MTLVertexFormatFloat2;
    case DXGI_FORMAT_R32G32B32_FLOAT: return MTLVertexFormatFloat3;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return MTLVertexFormatFloat4;
    case DXGI_FORMAT_R32_UINT: return MTLVertexFormatUInt;
    case DXGI_FORMAT_R32G32_UINT: return MTLVertexFormatUInt2;
    case DXGI_FORMAT_R32G32B32_UINT: return MTLVertexFormatUInt3;
    case DXGI_FORMAT_R32G32B32A32_UINT: return MTLVertexFormatUInt4;
    case DXGI_FORMAT_R32_SINT: return MTLVertexFormatInt;
    case DXGI_FORMAT_R32G32_SINT: return MTLVertexFormatInt2;
    case DXGI_FORMAT_R32G32B32_SINT: return MTLVertexFormatInt3;
    case DXGI_FORMAT_R32G32B32A32_SINT: return MTLVertexFormatInt4;
    case DXGI_FORMAT_R16G16_FLOAT: return MTLVertexFormatHalf2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return MTLVertexFormatHalf4;
    case DXGI_FORMAT_R16G16_UNORM: return MTLVertexFormatUShort2Normalized;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return MTLVertexFormatUShort4Normalized;
    case DXGI_FORMAT_R16G16_SNORM: return MTLVertexFormatShort2Normalized;
    case DXGI_FORMAT_R16G16B16A16_SNORM: return MTLVertexFormatShort4Normalized;
    case DXGI_FORMAT_R16G16_UINT: return MTLVertexFormatUShort2;
    case DXGI_FORMAT_R16G16B16A16_UINT: return MTLVertexFormatUShort4;
    case DXGI_FORMAT_R16G16_SINT: return MTLVertexFormatShort2;
    case DXGI_FORMAT_R16G16B16A16_SINT: return MTLVertexFormatShort4;
    case DXGI_FORMAT_R8G8_UNORM: return MTLVertexFormatUChar2Normalized;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return MTLVertexFormatUChar4Normalized;
    case DXGI_FORMAT_R8G8_SNORM: return MTLVertexFormatChar2Normalized;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return MTLVertexFormatChar4Normalized;
    case DXGI_FORMAT_R8G8_UINT: return MTLVertexFormatUChar2;
    case DXGI_FORMAT_R8G8B8A8_UINT: return MTLVertexFormatUChar4;
    case DXGI_FORMAT_R8G8_SINT: return MTLVertexFormatChar2;
    case DXGI_FORMAT_R8G8B8A8_SINT: return MTLVertexFormatChar4;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return MTLVertexFormatUChar4Normalized_BGRA;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return MTLVertexFormatUInt1010102Normalized;
    default: return MTLVertexFormatInvalid;
    }
}

uint32_t MipCountFor(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t n = 1, m = w > h ? w : h;
    if (d > m)
        m = d;
    while (m > 1)
    {
        m >>= 1;
        ++n;
    }
    return n;
}

size_t RowPitch(const FormatInfo& fi, uint32_t width)
{
    if (fi.compressed)
        return size_t((width + fi.blockSize - 1) / fi.blockSize) * fi.bytesPerBlock;
    return size_t(width) * fi.bytesPerBlock;
}

size_t SliceBytes(const FormatInfo& fi, uint32_t width, uint32_t height)
{
    if (fi.compressed)
        return RowPitch(fi, width) * ((height + fi.blockSize - 1) / fi.blockSize);
    return RowPitch(fi, width) * height;
}

uint64_t HashBytes(const void* p, size_t n, uint64_t seed)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = seed;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}
} // namespace mtl_d3d11
