// Headless checks of shim parts the SDL test app does not draw: image encoding, filtered texture copies and the
// pixel-input patching. No window needed; exits 0 when everything passes.
#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include <mtl_d3d11/d3d11_metal.h>
#include <mtl_d3d11/d3dx11_metal.h>
#include <mtl_d3d11/mtl_d3d11.h>
#include <mtl_d3d11/mtl_shader.h>

namespace
{
int g_failed = 0;
void check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    g_failed += ok ? 0 : 1;
}

struct Rgba
{
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;
};

bool decode(const void* data, size_t size, Rgba* out)
{
    CFDataRef d = CFDataCreate(nullptr, static_cast<const UInt8*>(data), CFIndex(size));
    CGImageSourceRef src = CGImageSourceCreateWithData(d, nullptr);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
    bool ok = false;
    if (img)
    {
        out->w = uint32_t(CGImageGetWidth(img));
        out->h = uint32_t(CGImageGetHeight(img));
        out->px.assign(size_t(out->w) * out->h * 4, 0);
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(out->px.data(), out->w, out->h, 8, out->w * 4, cs,
                                                 kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGContextDrawImage(ctx, CGRectMake(0, 0, out->w, out->h), img);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        CGImageRelease(img);
        ok = true;
    }
    if (src) CFRelease(src);
    CFRelease(d);
    return ok;
}

ID3D11Texture2D* makeTexture(ID3D11Device* dev, uint32_t w, uint32_t h, const std::vector<uint8_t>& px)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), w * 4, 0};
    ID3D11Texture2D* t = nullptr;
    dev->CreateTexture2D(&td, px.empty() ? nullptr : &sd, &t);
    return t;
}

// pixel shader container around hand-written MSL
std::vector<uint8_t> pixelContainer(const std::string& msl)
{
    mtl_d3d11::ShaderBlob blob;
    blob.reflection.stage = mtl_d3d11::ShaderStage::Pixel;
    blob.reflection.entryPoint = "main0";
    blob.reflection.colorOutputs = 1;
    blob.payload.assign(msl.begin(), msl.end());
    return mtl_d3d11::Serialize(blob);
}
} // namespace

int main()
{
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx)))
    {
        printf("no Metal device\n");
        return 2;
    }

    printf("[1] image encoding (D3DX11SaveTextureToMemory)\n");
    {
        const uint32_t W = 32, H = 16;
        std::vector<uint8_t> px(W * H * 4);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                uint8_t* p = &px[(y * W + x) * 4];
                p[0] = uint8_t(x * 8); p[1] = uint8_t(y * 16); p[2] = 77; p[3] = 255;
            }
        ID3D11Texture2D* tex = makeTexture(dev, W, H, px);
        check(tex != nullptr, "texture with initial data");
        ID3D10Blob* png = nullptr;
        ID3D10Blob* jpg = nullptr;
        check(SUCCEEDED(D3DX11SaveTextureToMemory(ctx, tex, D3DX11_IFF_PNG, &png, 0)) && png, "PNG encodes");
        Rgba img;
        bool same = png && decode(png->GetBufferPointer(), png->GetBufferSize(), &img) && img.w == W && img.h == H;
        for (size_t i = 0; same && i < px.size(); ++i)
            same = img.px[i] == px[i];
        check(same, "PNG decodes to the same pixels");
        check(SUCCEEDED(D3DX11SaveTextureToMemory(ctx, tex, D3DX11_IFF_JPG, &jpg, 0)) && jpg, "JPG encodes");
        double diff = 0;
        Rgba j;
        bool decoded = jpg && decode(jpg->GetBufferPointer(), jpg->GetBufferSize(), &j) && j.w == W && j.h == H;
        for (size_t i = 0; decoded && i < px.size(); i += 4)
            for (int c = 0; c < 3; ++c)
                diff += std::abs(int(j.px[i + c]) - int(px[i + c]));
        check(decoded && diff / (W * H * 3) < 8.0, "JPG decodes close to the source");
        if (png) png->Release();
        if (jpg) jpg->Release();
        tex->Release();
    }

    printf("[2] filtered copy (D3DX11LoadTextureFromTexture)\n");
    {
        const uint32_t S = 64, D = 8;
        std::vector<uint8_t> checker(S * S * 4);
        for (uint32_t y = 0; y < S; ++y)
            for (uint32_t x = 0; x < S; ++x)
            {
                const uint8_t v = ((x + y) & 1) ? 255 : 0;
                uint8_t* p = &checker[(y * S + x) * 4];
                p[0] = p[1] = p[2] = v; p[3] = 255;
            }
        ID3D11Texture2D* src = makeTexture(dev, S, S, checker);
        ID3D11Texture2D* dst = makeTexture(dev, D, D, std::vector<uint8_t>(D * D * 4, 0));
        check(SUCCEEDED(D3DX11LoadTextureFromTexture(ctx, src, nullptr, dst)), "64x64 -> 8x8 succeeds");
        ID3D10Blob* out = nullptr;
        D3DX11SaveTextureToMemory(ctx, dst, D3DX11_IFF_PNG, &out, 0);
        Rgba img;
        bool ok = out && decode(out->GetBufferPointer(), out->GetBufferSize(), &img) && img.w == D;
        for (size_t i = 0; ok && i < img.px.size(); i += 4)
            ok = std::abs(int(img.px[i]) - 127) <= 2; // area average of a 1-pixel checkerboard
        check(ok, "every texel is the area average (127/128)");
        if (out) out->Release();
        src->Release();
        dst->Release();
    }

    printf("[3] pixel inputs the vertex shader does not write\n");
    {
        const std::string msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct main0_out\n{\n    float4 o0 [[color(0)]];\n};\n\n"
            "struct main0_in\n{\n    float2 v0 [[user(locn0)]];\n    float4 v1 [[user(locn1)]];\n    float4 v7 [[user(locn7)]];\n};\n\n"
            "fragment main0_out main0(main0_in in [[stage_in]], float4 gl_FragCoord [[position]])\n{\n"
            "    main0_out out = {};\n    out.o0 = float4(in.v0, 0.0, 1.0) + in.v1 + in.v7 * in.v7.x;\n    return out;\n}\n";
        const auto vars = mtl_d3d11::ParseInterpolants(msl, "main0_in");
        check(vars.size() == 3 && vars[2].location == 7 && vars[2].name == "v7" && vars[2].type == "float4",
              "ParseInterpolants reads type, name and location");
        ID3D11PixelShader* ps = nullptr;
        std::vector<uint8_t> c = pixelContainer(msl);
        check(SUCCEEDED(dev->CreatePixelShader(c.data(), c.size(), nullptr, &ps)), "original MSL compiles");
        if (ps) ps->Release();

        const std::string patched = mtl_d3d11::PatchFragmentInputs(msl, {vars[2]});
        check(patched.find("locn7") == std::string::npos && patched.find("v7_unwritten") != std::string::npos &&
                  patched.find("in.v7") == std::string::npos && patched.find("in.v0") != std::string::npos,
              "one input becomes a zero local");
        c = pixelContainer(patched);
        check(SUCCEEDED(dev->CreatePixelShader(c.data(), c.size(), nullptr, &ps)), "patched MSL compiles");
        if (ps) ps->Release();

        const std::string none = mtl_d3d11::PatchFragmentInputs(msl, vars);
        check(none.find("[[stage_in]]") == std::string::npos && none.find("v0_unwritten") != std::string::npos,
              "no inputs left: the stage_in parameter goes away");
        c = pixelContainer(none);
        check(SUCCEEDED(dev->CreatePixelShader(c.data(), c.size(), nullptr, &ps)), "patched MSL without stage_in compiles");
        if (ps) ps->Release();
    }

    ctx->Release();
    dev->Release();
    printf("\n%s\n", g_failed ? "FAILED" : "all checks passed");
    return g_failed ? 1 : 0;
}
