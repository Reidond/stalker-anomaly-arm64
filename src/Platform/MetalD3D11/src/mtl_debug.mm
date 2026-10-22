// Debug aids: look at the contents of any texture the renderer owns (colour, float, depth, stencil) as an 8-bit image.
#import "mtl_context.h"

#include <algorithm>
#include <cmath>

namespace mtl_d3d11
{
namespace
{
float halfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float v;
    if (exp == 0)
        v = std::ldexp(float(man), -24);
    else if (exp == 31)
        v = man ? NAN : INFINITY;
    else
        v = std::ldexp(float(man | 1024), int(exp) - 25);
    return sign ? -v : v;
}

// bytes per pixel and a reader returning up to four channels as floats (missing channels are 0, alpha 1)
struct PixelReader
{
    size_t bpp = 0;
    int channels = 0;
    std::string name;
    void (*read)(const uint8_t*, float*) = nullptr;
};

void readBGRA8(const uint8_t* p, float* o) { o[0] = p[2] / 255.f; o[1] = p[1] / 255.f; o[2] = p[0] / 255.f; o[3] = p[3] / 255.f; }
void readRGBA8(const uint8_t* p, float* o) { for (int i = 0; i < 4; ++i) o[i] = p[i] / 255.f; }
void readR8(const uint8_t* p, float* o) { o[0] = p[0] / 255.f; o[1] = o[2] = 0; o[3] = 1; }
void readRG8(const uint8_t* p, float* o) { o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; o[2] = 0; o[3] = 1; }
void readRGBA16F(const uint8_t* p, float* o) { const uint16_t* h = reinterpret_cast<const uint16_t*>(p); for (int i = 0; i < 4; ++i) o[i] = halfToFloat(h[i]); }
void readRG16F(const uint8_t* p, float* o) { const uint16_t* h = reinterpret_cast<const uint16_t*>(p); o[0] = halfToFloat(h[0]); o[1] = halfToFloat(h[1]); o[2] = 0; o[3] = 1; }
void readR16F(const uint8_t* p, float* o) { o[0] = halfToFloat(*reinterpret_cast<const uint16_t*>(p)); o[1] = o[2] = 0; o[3] = 1; }
void readR32F(const uint8_t* p, float* o) { o[0] = *reinterpret_cast<const float*>(p); o[1] = o[2] = 0; o[3] = 1; }
void readRG32F(const uint8_t* p, float* o) { const float* f = reinterpret_cast<const float*>(p); o[0] = f[0]; o[1] = f[1]; o[2] = 0; o[3] = 1; }
void readRGBA32F(const uint8_t* p, float* o) { const float* f = reinterpret_cast<const float*>(p); for (int i = 0; i < 4; ++i) o[i] = f[i]; }
void readRGB10A2(const uint8_t* p, float* o)
{
    const uint32_t v = *reinterpret_cast<const uint32_t*>(p);
    o[0] = (v & 1023) / 1023.f; o[1] = ((v >> 10) & 1023) / 1023.f; o[2] = ((v >> 20) & 1023) / 1023.f; o[3] = (v >> 30) / 3.f;
}
void readU8(const uint8_t* p, float* o) { o[0] = float(p[0]); o[1] = o[2] = 0; o[3] = 1; }

bool readerFor(MTLPixelFormat f, PixelReader* r)
{
    switch (f)
    {
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB: *r = {4, 4, "BGRA8", readBGRA8}; return true;
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB: *r = {4, 4, "RGBA8", readRGBA8}; return true;
    case MTLPixelFormatR8Unorm: *r = {1, 1, "R8", readR8}; return true;
    case MTLPixelFormatRG8Unorm: *r = {2, 2, "RG8", readRG8}; return true;
    case MTLPixelFormatRGBA16Float: *r = {8, 4, "RGBA16F", readRGBA16F}; return true;
    case MTLPixelFormatRG16Float: *r = {4, 2, "RG16F", readRG16F}; return true;
    case MTLPixelFormatR16Float: *r = {2, 1, "R16F", readR16F}; return true;
    case MTLPixelFormatR32Float:
    case MTLPixelFormatDepth32Float: *r = {4, 1, "R32F", readR32F}; return true;
    case MTLPixelFormatRG32Float: *r = {8, 2, "RG32F", readRG32F}; return true;
    case MTLPixelFormatRGBA32Float: *r = {16, 4, "RGBA32F", readRGBA32F}; return true;
    case MTLPixelFormatRGB10A2Unorm: *r = {4, 4, "RGB10A2", readRGB10A2}; return true;
    default: return false;
    }
}

// ---- BC1 / BC3 (DXT1 / DXT5) decoding, to look at compressed textures
void decodeBC1Colors(const uint8_t* b, uint8_t out[4][4])
{
    const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
    auto expand = [](uint16_t c, uint8_t* o) {
        o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
        o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
        o[2] = uint8_t((c & 31) * 255 / 31);
        o[3] = 255;
    };
    expand(c0, out[0]);
    expand(c1, out[1]);
    for (int i = 0; i < 3; ++i)
    {
        if (c0 > c1)
        {
            out[2][i] = uint8_t((2 * out[0][i] + out[1][i]) / 3);
            out[3][i] = uint8_t((out[0][i] + 2 * out[1][i]) / 3);
        }
        else
        {
            out[2][i] = uint8_t((out[0][i] + out[1][i]) / 2);
            out[3][i] = 0;
        }
    }
    out[2][3] = 255;
    out[3][3] = c0 > c1 ? 255 : 0;
}

// One 4x4 block to 16 RGBA pixels (row-major, 4 bytes each).
void decodeBCBlock(const uint8_t* block, bool bc3, uint8_t* px)
{
    uint8_t pal[4][4];
    decodeBC1Colors(block + (bc3 ? 8 : 0), pal);
    const uint32_t idx = uint32_t(block[(bc3 ? 8 : 0) + 4]) | (uint32_t(block[(bc3 ? 8 : 0) + 5]) << 8) |
                         (uint32_t(block[(bc3 ? 8 : 0) + 6]) << 16) | (uint32_t(block[(bc3 ? 8 : 0) + 7]) << 24);
    uint8_t alpha[8];
    uint64_t abits = 0;
    if (bc3)
    {
        alpha[0] = block[0];
        alpha[1] = block[1];
        if (alpha[0] > alpha[1])
            for (int i = 1; i < 7; ++i) alpha[1 + i] = uint8_t(((7 - i) * alpha[0] + i * alpha[1]) / 7);
        else
        {
            for (int i = 1; i < 5; ++i) alpha[1 + i] = uint8_t(((5 - i) * alpha[0] + i * alpha[1]) / 5);
            alpha[6] = 0;
            alpha[7] = 255;
        }
        for (int i = 0; i < 6; ++i) abits |= uint64_t(block[2 + i]) << (8 * i);
    }
    for (int i = 0; i < 16; ++i)
    {
        const uint8_t* c = pal[(idx >> (2 * i)) & 3];
        px[i * 4 + 0] = c[0];
        px[i * 4 + 1] = c[1];
        px[i * 4 + 2] = c[2];
        px[i * 4 + 3] = bc3 ? alpha[(abits >> (3 * i)) & 7] : c[3];
    }
}
} // namespace

bool DebugReadTexture(ID3D11DeviceContext* pContext, ID3D11Resource* pResource, const DebugReadOptions& opt, DebugImage* out)
{
    if (!pContext || !pResource || !out)
        return false;
    TextureCore* t = AsTexture(pResource);
    if (!t || t->dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D || t->samples > 1)
        return false;
    pContext->Flush(); // everything recorded so far has to be on the queue before the read-back
    Device* dev = t->dev;
    id<MTLTexture> tex = t->isBackBuffer ? nil : t->tex;
    if (!tex)
        return false;
    const MTLPixelFormat fmt = tex.pixelFormat;
    const bool bc1 = fmt == MTLPixelFormatBC1_RGBA || fmt == MTLPixelFormatBC1_RGBA_sRGB;
    const bool bc3 = fmt == MTLPixelFormatBC3_RGBA || fmt == MTLPixelFormatBC3_RGBA_sRGB;
    const bool hasDepth = fmt == MTLPixelFormatDepth32Float || fmt == MTLPixelFormatDepth32Float_Stencil8;
    const bool stencilOnly = fmt == MTLPixelFormatX32_Stencil8;
    const bool stencilWanted = (fmt == MTLPixelFormatDepth32Float_Stencil8 && opt.stencil) || stencilOnly;

    // the rectangle to read (BC textures: whole 4x4 blocks)
    NSUInteger cx = std::min<NSUInteger>(opt.cropX, tex.width - 1), cy = std::min<NSUInteger>(opt.cropY, tex.height - 1);
    NSUInteger cw = opt.cropW ? opt.cropW : tex.width, ch = opt.cropH ? opt.cropH : tex.height;
    if (bc1 || bc3)
    {
        cx &= ~NSUInteger(3);
        cy &= ~NSUInteger(3);
        cw = (cw + 3) & ~NSUInteger(3);
        ch = (ch + 3) & ~NSUInteger(3);
    }
    cw = std::min<NSUInteger>(cw, tex.width - cx);
    ch = std::min<NSUInteger>(ch, tex.height - cy);

    PixelReader reader;
    size_t bytesPerRow, bytesTotal;
    if (bc1 || bc3)
    {
        const size_t blockBytes = bc3 ? 16 : 8;
        bytesPerRow = (cw / 4) * blockBytes;
        bytesTotal = bytesPerRow * (ch / 4);
    }
    else
    {
        if (stencilWanted)
            reader = {1, 1, "S8", readU8};
        else if (hasDepth)
            reader = {4, 1, "D32F", readR32F};
        else if (!readerFor(fmt, &reader))
            return false;
        bytesPerRow = cw * reader.bpp;
        bytesTotal = bytesPerRow * ch;
    }

    id<MTLBuffer> buf = [dev->mtl newBufferWithLength:bytesTotal options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [dev->queue commandBuffer];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    MTLBlitOption options = MTLBlitOptionNone;
    if (fmt == MTLPixelFormatDepth32Float_Stencil8)
        options = stencilWanted ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil;
    [be copyFromTexture:tex
             sourceSlice:0
             sourceLevel:0
            sourceOrigin:MTLOriginMake(cx, cy, 0)
              sourceSize:MTLSizeMake(cw, ch, 1)
                toBuffer:buf
       destinationOffset:0
  destinationBytesPerRow:bytesPerRow
destinationBytesPerImage:bytesTotal
                 options:options];
    [be endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    const uint8_t* src = static_cast<const uint8_t*>(buf.contents);
    out->width = uint32_t(cw);
    out->height = uint32_t(ch);
    out->rgba.assign(cw * ch * 4, 255);

    if (bc1 || bc3)
    {
        // decode into RGBA8; the "channel" mode shows one of R, G, B, A as grey, colour shows RGB
        out->format = bc3 ? "BC3" : "BC1";
        const size_t blockBytes = bc3 ? 16 : 8;
        double sum = 0;
        float minv = 255.f, maxv = 0.f;
        for (size_t by = 0; by < ch / 4; ++by)
            for (size_t bx = 0; bx < cw / 4; ++bx)
            {
                uint8_t px[64];
                decodeBCBlock(src + by * bytesPerRow + bx * blockBytes, bc3, px);
                for (int i = 0; i < 16; ++i)
                {
                    uint8_t* o = &out->rgba[((by * 4 + i / 4) * cw + bx * 4 + i % 4) * 4];
                    if (opt.mode == DebugReadOptions::Channel)
                    {
                        const uint8_t v = px[i * 4 + std::min(opt.channel, 3)];
                        o[0] = o[1] = o[2] = v;
                        sum += v;
                        minv = std::min<float>(minv, v);
                        maxv = std::max<float>(maxv, v);
                    }
                    else
                    {
                        o[0] = px[i * 4];
                        o[1] = px[i * 4 + 1];
                        o[2] = px[i * 4 + 2];
                        sum += px[i * 4 + 3];
                        minv = std::min<float>(minv, px[i * 4 + 3]);
                        maxv = std::max<float>(maxv, px[i * 4 + 3]);
                    }
                    o[3] = 255;
                }
            }
        // statistics are over alpha in colour mode (what makes a sprite visible), over the shown channel otherwise
        out->minValue = minv / 255.f;
        out->maxValue = maxv / 255.f;
        out->mean = float(sum / double(cw * ch)) / 255.f;
        return true;
    }

    out->format = reader.name;
    float lo = opt.lo, hi = opt.hi;
    float minv = INFINITY, maxv = -INFINITY;
    double sum = 0;
    const size_t n = cw * ch;
    // statistics first (over the channels shown), for the auto range
    for (size_t i = 0; i < n; ++i)
    {
        float v[4];
        reader.read(src + i * reader.bpp, v);
        const int c = std::min(opt.channel, reader.channels - 1);
        const float x = opt.mode == DebugReadOptions::Colour || opt.mode == DebugReadOptions::Signed
                            ? std::min({v[0], v[std::min(1, reader.channels - 1)], v[std::min(2, reader.channels - 1)]})
                            : v[c];
        if (std::isfinite(x))
        {
            minv = std::min(minv, x);
            maxv = std::max(maxv, x);
            sum += x;
        }
    }
    out->minValue = minv;
    out->maxValue = maxv;
    out->mean = float(sum / double(n));
    if (opt.mode == DebugReadOptions::Channel && lo == hi)
    {
        lo = minv;
        hi = maxv > minv ? maxv : minv + 1.f;
    }
    for (size_t i = 0; i < n; ++i)
    {
        float v[4];
        reader.read(src + i * reader.bpp, v);
        uint8_t* o = &out->rgba[i * 4];
        auto to8 = [](float x) { return uint8_t(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f); };
        switch (opt.mode)
        {
        case DebugReadOptions::Colour:
            o[0] = to8(v[0]); o[1] = to8(reader.channels > 1 ? v[1] : v[0]); o[2] = to8(reader.channels > 2 ? v[2] : v[0]);
            break;
        case DebugReadOptions::Signed:
            o[0] = to8(v[0] * 0.5f + 0.5f); o[1] = to8(v[1] * 0.5f + 0.5f); o[2] = to8(v[2] * 0.5f + 0.5f);
            break;
        default:
        {
            const float c = v[std::min(opt.channel, reader.channels - 1)];
            const uint8_t gv = to8((c - lo) / (hi - lo));
            o[0] = o[1] = o[2] = gv;
        }
        }
        o[3] = 255;
    }
    return true;
}
} // namespace mtl_d3d11
