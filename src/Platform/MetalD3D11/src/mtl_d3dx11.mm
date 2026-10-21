// D3DX11 texture helpers: DDS parsing and creation, mip generation, read-back saving.
#import "mtl_context.h"

#include <algorithm>
#include <cstdio>

namespace mtl_d3d11
{
namespace
{
constexpr uint32_t kDDSMagic = 0x20534444; // 'DDS '

struct DDSPixelFormat
{
    uint32_t size, flags, fourCC, bitCount, rMask, gMask, bMask, aMask;
};
struct DDSHeader
{
    uint32_t size, flags, height, width, pitchOrLinearSize, depth, mipCount, reserved1[11];
    DDSPixelFormat pf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct DDSHeaderDX10
{
    uint32_t dxgiFormat, dimension, miscFlag, arraySize, miscFlags2;
};
enum : uint32_t
{
    DDPF_ALPHAPIXELS = 0x1,
    DDPF_ALPHA = 0x2,
    DDPF_FOURCC = 0x4,
    DDPF_RGB = 0x40,
    DDPF_LUMINANCE = 0x20000,
    DDSD_DEPTH = 0x800000,
    DDSCAPS2_CUBEMAP = 0x200,
    DDSCAPS2_VOLUME = 0x200000
};
constexpr uint32_t fourCC(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

struct ParsedDDS
{
    D3DX11_IMAGE_INFO info{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool expand24 = false; // 24-bit RGB source widened to BGRA8
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
    bool cube = false;
};

bool matchMasks(const DDSPixelFormat& pf, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return pf.rMask == r && pf.gMask == g && pf.bMask == b && pf.aMask == a;
}

DXGI_FORMAT formatFromLegacy(const DDSPixelFormat& pf, bool* expand24)
{
    *expand24 = false;
    if (pf.flags & DDPF_FOURCC)
    {
        switch (pf.fourCC)
        {
        case fourCC('D', 'X', 'T', '1'): return DXGI_FORMAT_BC1_UNORM;
        case fourCC('D', 'X', 'T', '2'):
        case fourCC('D', 'X', 'T', '3'): return DXGI_FORMAT_BC2_UNORM;
        case fourCC('D', 'X', 'T', '4'):
        case fourCC('D', 'X', 'T', '5'): return DXGI_FORMAT_BC3_UNORM;
        case fourCC('A', 'T', 'I', '1'):
        case fourCC('B', 'C', '4', 'U'): return DXGI_FORMAT_BC4_UNORM;
        case fourCC('B', 'C', '4', 'S'): return DXGI_FORMAT_BC4_SNORM;
        case fourCC('A', 'T', 'I', '2'):
        case fourCC('B', 'C', '5', 'U'): return DXGI_FORMAT_BC5_UNORM;
        case fourCC('B', 'C', '5', 'S'): return DXGI_FORMAT_BC5_SNORM;
        case 36: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case 110: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case 111: return DXGI_FORMAT_R16_FLOAT;
        case 112: return DXGI_FORMAT_R16G16_FLOAT;
        case 113: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case 114: return DXGI_FORMAT_R32_FLOAT;
        case 115: return DXGI_FORMAT_R32G32_FLOAT;
        case 116: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }
    if (pf.flags & DDPF_RGB)
    {
        switch (pf.bitCount)
        {
        case 32:
            if (matchMasks(pf, 0xff, 0xff00, 0xff0000, 0xff000000)) return DXGI_FORMAT_R8G8B8A8_UNORM;
            if (matchMasks(pf, 0xff0000, 0xff00, 0xff, 0xff000000)) return DXGI_FORMAT_B8G8R8A8_UNORM;
            if (matchMasks(pf, 0xff0000, 0xff00, 0xff, 0)) return DXGI_FORMAT_B8G8R8X8_UNORM;
            if (matchMasks(pf, 0xffff, 0xffff0000, 0, 0)) return DXGI_FORMAT_R16G16_UNORM;
            if (matchMasks(pf, 0x3ff, 0xffc00, 0x3ff00000, 0xc0000000)) return DXGI_FORMAT_R10G10B10A2_UNORM;
            break;
        case 24:
            if (matchMasks(pf, 0xff0000, 0xff00, 0xff, 0))
            {
                *expand24 = true;
                return DXGI_FORMAT_B8G8R8A8_UNORM;
            }
            break;
        case 16:
            if (matchMasks(pf, 0xf800, 0x7e0, 0x1f, 0)) return DXGI_FORMAT_B5G6R5_UNORM;
            if (matchMasks(pf, 0x7c00, 0x3e0, 0x1f, 0x8000)) return DXGI_FORMAT_B5G5R5A1_UNORM;
            break;
        }
    }
    else if (pf.flags & DDPF_LUMINANCE)
    {
        if (pf.bitCount == 8) return DXGI_FORMAT_R8_UNORM;
        if (pf.bitCount == 16 && matchMasks(pf, 0xffff, 0, 0, 0)) return DXGI_FORMAT_R16_UNORM;
        if (pf.bitCount == 16 && matchMasks(pf, 0xff, 0, 0, 0xff00)) return DXGI_FORMAT_R8G8_UNORM;
    }
    else if (pf.flags & DDPF_ALPHA)
    {
        if (pf.bitCount == 8) return DXGI_FORMAT_A8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

bool parseDDS(const void* src, size_t size, ParsedDDS* out)
{
    const uint8_t* p = static_cast<const uint8_t*>(src);
    if (size < 4 + sizeof(DDSHeader))
        return false;
    uint32_t magic;
    std::memcpy(&magic, p, 4);
    if (magic != kDDSMagic)
        return false;
    DDSHeader h;
    std::memcpy(&h, p + 4, sizeof(h));
    if (h.size != sizeof(DDSHeader) || h.pf.size != sizeof(DDSPixelFormat))
        return false;
    size_t offset = 4 + sizeof(DDSHeader);
    DDSHeaderDX10 dx10{};
    bool hasDX10 = (h.pf.flags & DDPF_FOURCC) && h.pf.fourCC == fourCC('D', 'X', '1', '0');
    ParsedDDS r;
    r.info.Width = h.width;
    r.info.Height = h.height;
    r.info.Depth = (h.flags & DDSD_DEPTH) || (h.caps2 & DDSCAPS2_VOLUME) ? std::max<uint32_t>(h.depth, 1) : 1;
    r.info.MipLevels = h.mipCount ? h.mipCount : 1;
    r.info.ArraySize = 1;
    r.info.ImageFileFormat = D3DX11_IFF_DDS;
    r.info.ResourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    if (hasDX10)
    {
        if (size < offset + sizeof(dx10))
            return false;
        std::memcpy(&dx10, p + offset, sizeof(dx10));
        offset += sizeof(dx10);
        r.format = DXGI_FORMAT(dx10.dxgiFormat);
        r.info.ArraySize = std::max<uint32_t>(dx10.arraySize, 1);
        if (dx10.dimension == 4)
            r.info.ResourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
        else if (dx10.dimension == 2)
            r.info.ResourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
        if (dx10.miscFlag & D3D11_RESOURCE_MISC_TEXTURECUBE)
        {
            r.cube = true;
            r.info.ArraySize *= 6;
        }
    }
    else
    {
        r.format = formatFromLegacy(h.pf, &r.expand24);
        if (r.info.Depth > 1)
            r.info.ResourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
        if (h.caps2 & DDSCAPS2_CUBEMAP)
        {
            r.cube = true;
            r.info.ArraySize = 6;
        }
    }
    if (r.format == DXGI_FORMAT_UNKNOWN || GetFormatInfo(r.format).dxgi == DXGI_FORMAT_UNKNOWN)
        return false;
    r.info.Format = r.format;
    r.info.MiscFlags = r.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    r.data = p + offset;
    r.dataSize = size - offset;
    *out = r;
    return true;
}

// Bytes of one source mip level (all depth slices) in the file.
size_t srcMipBytes(const ParsedDDS& d, uint32_t w, uint32_t h, uint32_t depth, size_t* rowPitch, size_t* slicePitch)
{
    const FormatInfo& fi = GetFormatInfo(d.format);
    size_t rp;
    if (d.expand24)
        rp = size_t(w) * 3;
    else
        rp = RowPitch(fi, w);
    const size_t rows = fi.compressed ? (h + fi.blockSize - 1) / fi.blockSize : h;
    *rowPitch = rp;
    *slicePitch = rp * rows;
    return *slicePitch * depth;
}
} // namespace
} // namespace mtl_d3d11

using namespace mtl_d3d11;

HRESULT D3DX11GetImageInfoFromMemory(LPCVOID pSrcData, SIZE_T size, ID3DX11ThreadPump*, D3DX11_IMAGE_INFO* pInfo, HRESULT* pHResult)
{
    ParsedDDS d;
    HRESULT hr = (pSrcData && pInfo && parseDDS(pSrcData, size, &d)) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        *pInfo = d.info;
    if (pHResult)
        *pHResult = hr;
    return hr;
}

HRESULT D3DX11CreateTextureFromMemory(ID3D11Device* pDevice, LPCVOID pSrcData, SIZE_T size, D3DX11_IMAGE_LOAD_INFO* pLoad,
                                      ID3DX11ThreadPump*, ID3D11Resource** ppTexture, HRESULT* pHResult)
{
    auto finish = [&](HRESULT hr) {
        if (pHResult)
            *pHResult = hr;
        return hr;
    };
    ParsedDDS d;
    if (!pDevice || !ppTexture || !pSrcData || !parseDDS(pSrcData, size, &d))
        return finish(E_FAIL);
    D3DX11_IMAGE_LOAD_INFO li;
    if (pLoad)
        li = *pLoad;
    auto def = [](UINT v, UINT fallback) { return v == D3DX11_DEFAULT ? fallback : v; };

    const UINT first = def(li.FirstMipLevel, 0);
    if (first >= d.info.MipLevels)
        return finish(E_INVALIDARG);
    const UINT srcW = std::max<UINT>(d.info.Width >> first, 1);
    const UINT srcH = std::max<UINT>(d.info.Height >> first, 1);
    const UINT srcD = std::max<UINT>(d.info.Depth >> first, 1);
    const UINT w = std::min(def(li.Width, srcW), srcW);
    const UINT h = std::min(def(li.Height, srcH), srcH);
    UINT mips = def(li.MipLevels, d.info.MipLevels - first);
    mips = std::max<UINT>(1, std::min(mips, d.info.MipLevels - first));
    const D3D11_USAGE usage = li.Usage == (D3D11_USAGE)D3DX11_DEFAULT ? D3D11_USAGE_DEFAULT : li.Usage;
    const UINT bind = def(li.BindFlags, usage == D3D11_USAGE_STAGING ? 0 : D3D11_BIND_SHADER_RESOURCE);
    const UINT cpu = def(li.CpuAccessFlags, 0);
    const UINT misc = def(li.MiscFlags, d.info.MiscFlags);
    const DXGI_FORMAT fmt = d.format;
    const bool volume = d.info.ResourceDimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    const UINT slices = volume ? 1 : d.info.ArraySize;

    // Walk the file: [slice][mip] with 3D mips holding their depth slices.
    struct MipSrc { const uint8_t* ptr; size_t row, slice; };
    std::vector<std::vector<MipSrc>> src(slices);
    size_t cursor = 0;
    for (UINT s = 0; s < slices; ++s)
        for (UINT m = 0; m < d.info.MipLevels; ++m)
        {
            const UINT mw = std::max<UINT>(d.info.Width >> m, 1), mh = std::max<UINT>(d.info.Height >> m, 1),
                       md = volume ? std::max<UINT>(d.info.Depth >> m, 1) : 1;
            size_t rp, sp;
            const size_t bytes = srcMipBytes(d, mw, mh, md, &rp, &sp);
            if (cursor + bytes > d.dataSize)
                return finish(E_FAIL);
            src[s].push_back({d.data + cursor, rp, sp});
            cursor += bytes;
        }

    // 24-bit sources are widened to BGRA8 here (kept alive until creation finishes).
    std::vector<std::vector<uint8_t>> widened;
    std::vector<D3D11_SUBRESOURCE_DATA> init;
    for (UINT s = 0; s < slices; ++s)
        for (UINT m = 0; m < mips; ++m)
        {
            MipSrc ms = src[s][first + m];
            const UINT mw = std::max<UINT>(w >> m, 1), mh = std::max<UINT>(h >> m, 1);
            if (d.expand24)
            {
                widened.emplace_back(size_t(mw) * mh * 4);
                uint8_t* o = widened.back().data();
                for (UINT y = 0; y < mh; ++y)
                    for (UINT x = 0; x < mw; ++x)
                    {
                        const uint8_t* px = ms.ptr + y * ms.row + x * 3;
                        uint8_t* op = o + (size_t(y) * mw + x) * 4;
                        op[0] = px[0]; op[1] = px[1]; op[2] = px[2]; op[3] = 255;
                    }
                init.push_back({o, mw * 4, mw * mh * 4});
            }
            else
                init.push_back({ms.ptr, UINT(ms.row), UINT(ms.slice)});
        }

    HRESULT hr;
    if (volume)
    {
        D3D11_TEXTURE3D_DESC td{w, h, std::min(def(li.Depth, srcD), srcD), mips, fmt, usage, bind, cpu, misc};
        ID3D11Texture3D* t = nullptr;
        hr = pDevice->CreateTexture3D(&td, init.data(), &t);
        *ppTexture = t;
    }
    else
    {
        D3D11_TEXTURE2D_DESC td{w, h, mips, slices, fmt, {1, 0}, usage, bind, cpu, misc};
        ID3D11Texture2D* t = nullptr;
        hr = pDevice->CreateTexture2D(&td, init.data(), &t);
        *ppTexture = t;
    }
    return finish(hr);
}

HRESULT D3DX11FilterTexture(ID3D11DeviceContext* pContext, ID3D11Resource* pTexture, UINT, UINT)
{
    if (!pContext || !pTexture)
        return E_INVALIDARG;
    ID3D11Device* dev = nullptr;
    pContext->GetDevice(&dev);
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = dev->CreateShaderResourceView(pTexture, nullptr, &srv);
    dev->Release();
    if (FAILED(hr))
        return hr;
    pContext->GenerateMips(srv);
    srv->Release();
    return S_OK;
}

HRESULT D3DX11LoadTextureFromTexture(ID3D11DeviceContext* pContext, ID3D11Resource* pSrc, D3DX11_IMAGE_LOAD_INFO*, ID3D11Resource* pDst)
{
    if (!pContext || !pSrc || !pDst)
        return E_INVALIDARG;
    TextureCore* s = AsTexture(pSrc);
    TextureCore* d = AsTexture(pDst);
    if (!s || !d || s->width != d->width || s->height != d->height)
        return E_NOTIMPL; // no filtered resize
    pContext->CopySubresourceRegion(pDst, 0, 0, 0, 0, pSrc, 0, nullptr);
    return S_OK;
}

namespace
{
class MemBlob final : public ComObject<ID3D10Blob>
{
public:
    explicit MemBlob(std::vector<uint8_t> d) : data(std::move(d)) {}
    void* GetBufferPointer() override { return data.data(); }
    SIZE_T GetBufferSize() override { return data.size(); }
    std::vector<uint8_t> data;
};
} // namespace

HRESULT D3DX11SaveTextureToMemory(ID3D11DeviceContext* pContext, ID3D11Resource* pSrc, D3DX11_IMAGE_FILE_FORMAT fmt,
                                  ID3D10Blob** ppBlob, UINT)
{
    if (!pContext || !pSrc || !ppBlob)
        return E_INVALIDARG;
    if (fmt != D3DX11_IFF_DDS && fmt != D3DX11_IFF_BMP)
        return E_NOTIMPL;
    TextureCore* t = AsTexture(pSrc);
    if (!t || t->dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return E_INVALIDARG;
    ID3D11Device* dev = nullptr;
    pContext->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC td{t->width, t->height, 1, 1, t->dxgi, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D* staging = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &staging);
    dev->Release();
    if (FAILED(hr))
        return hr;
    pContext->CopySubresourceRegion(staging, 0, 0, 0, 0, pSrc, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    hr = pContext->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr))
    {
        staging->Release();
        return hr;
    }
    const FormatInfo& fi = GetFormatInfo(t->dxgi);
    std::vector<uint8_t> out;
    if (fmt == D3DX11_IFF_DDS)
    {
        DDSHeader h{};
        h.size = sizeof(DDSHeader);
        h.flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8;
        h.height = t->height;
        h.width = t->width;
        h.pitchOrLinearSize = UINT(RowPitch(fi, t->width));
        h.mipCount = 1;
        h.pf.size = sizeof(DDSPixelFormat);
        h.pf.flags = DDPF_FOURCC;
        h.pf.fourCC = fourCC('D', 'X', '1', '0');
        h.caps = 0x1000;
        DDSHeaderDX10 ext{uint32_t(t->dxgi), 3, 0, 1, 0};
        out.resize(4 + sizeof(h) + sizeof(ext));
        std::memcpy(out.data(), &kDDSMagic, 4);
        std::memcpy(out.data() + 4, &h, sizeof(h));
        std::memcpy(out.data() + 4 + sizeof(h), &ext, sizeof(ext));
        const size_t rp = RowPitch(fi, t->width);
        const size_t rows = fi.compressed ? (t->height + 3) / 4 : t->height;
        for (size_t r = 0; r < rows; ++r)
            out.insert(out.end(), static_cast<uint8_t*>(m.pData) + r * m.RowPitch, static_cast<uint8_t*>(m.pData) + r * m.RowPitch + rp);
    }
    else
    {
        const bool rgba = t->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM || t->dxgi == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra = t->dxgi == DXGI_FORMAT_B8G8R8A8_UNORM || t->dxgi == DXGI_FORMAT_B8G8R8X8_UNORM;
        if (!rgba && !bgra)
        {
            pContext->Unmap(staging, 0);
            staging->Release();
            return E_NOTIMPL;
        }
        const uint32_t w = t->width, h = t->height, rowBytes = (w * 3 + 3) & ~3u, dataBytes = rowBytes * h;
        out.assign(54 + dataBytes, 0);
        uint8_t* b = out.data();
        b[0] = 'B'; b[1] = 'M';
        const uint32_t fileSize = 54 + dataBytes, offBits = 54, hdr = 40;
        std::memcpy(b + 2, &fileSize, 4);
        std::memcpy(b + 10, &offBits, 4);
        std::memcpy(b + 14, &hdr, 4);
        std::memcpy(b + 18, &w, 4);
        const int32_t negH = -int32_t(h); // top-down
        std::memcpy(b + 22, &negH, 4);
        const uint16_t planes = 1, bits = 24;
        std::memcpy(b + 26, &planes, 2);
        std::memcpy(b + 28, &bits, 2);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* px = static_cast<uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
                uint8_t* o = b + 54 + y * rowBytes + x * 3;
                o[0] = bgra ? px[0] : px[2];
                o[1] = px[1];
                o[2] = bgra ? px[2] : px[0];
            }
    }
    pContext->Unmap(staging, 0);
    staging->Release();
    *ppBlob = new MemBlob(std::move(out));
    return S_OK;
}

HRESULT D3DX11SaveTextureToFile(ID3D11DeviceContext* pContext, ID3D11Resource* pSrc, D3DX11_IMAGE_FILE_FORMAT fmt, LPCSTR pFile)
{
    ID3D10Blob* blob = nullptr;
    HRESULT hr = D3DX11SaveTextureToMemory(pContext, pSrc, fmt, &blob, 0);
    if (FAILED(hr))
        return hr;
    FILE* f = fopen(pFile, "wb");
    if (!f)
    {
        blob->Release();
        return E_FAIL;
    }
    fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f);
    fclose(f);
    blob->Release();
    return S_OK;
}
