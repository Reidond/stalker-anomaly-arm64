// MetalFX spatial upscaling (see mtl_upscale.h).
#import <MetalFX/MetalFX.h>

#import "mtl_context.h"

#include <mtl_d3d11/mtl_upscale.h>

namespace mtl_d3d11
{
namespace
{
// One scaler is kept: the renderer asks for one size pair at a time, and a change of render scale makes a new one.
struct ScalerSlot
{
    id<MTLFXSpatialScaler> scaler = nil;
    id<MTLDevice> device = nil;
    NSUInteger inW = 0, inH = 0, outW = 0, outH = 0;
    MTLPixelFormat inFormat = MTLPixelFormatInvalid, outFormat = MTLPixelFormatInvalid;
};
ScalerSlot g_slot;
bool g_loggedFailure = false;

void failOnce(const char* what)
{
    if (!g_loggedFailure)
        Log("! MetalFX: spatial upscaling skipped (%s)", what);
    g_loggedFailure = true;
}

Device* DeviceOf(ID3D11Device* d)
{
    ID3D11Device1* d1 = nullptr;
    if (!d || FAILED(d->QueryInterface(&d1)) || !d1)
        return nullptr;
    Device* dev = static_cast<Device*>(d1);
    d1->Release();
    return dev;
}
} // namespace

bool SpatialUpscalerAvailable(ID3D11Device* pDevice)
{
    Device* dev = DeviceOf(pDevice);
    if (!dev || !dev->mtl)
        return false;
    return [MTLFXSpatialScalerDescriptor supportsDevice:dev->mtl];
}

bool EncodeSpatialUpscale(ID3D11DeviceContext* pContext, ID3D11Texture2D* pSrc, ID3D11Texture2D* pDst)
{
    TextureCore* src = AsTexture(pSrc);
    TextureCore* dst = AsTexture(pDst);
    if (!pContext || !src || !dst || src->samples > 1 || dst->samples > 1 || src->isBackBuffer || dst->isBackBuffer)
    {
        failOnce("not two plain 2D textures");
        return false;
    }
    Device* dev = src->dev;
    Context* ctx = dev->context;
    id<MTLTexture> in = src->tex;
    id<MTLTexture> out = dst->tex;
    if (!in || !out || !dev->mtl)
    {
        failOnce("texture without Metal backing");
        return false;
    }
    if (in.width > out.width || in.height > out.height)
    {
        failOnce("the input is larger than the output");
        return false;
    }
    if (![MTLFXSpatialScalerDescriptor supportsDevice:dev->mtl])
    {
        failOnce("not supported by this GPU");
        return false;
    }

    ScalerSlot& slot = g_slot;
    if (!slot.scaler || slot.device != dev->mtl || slot.inW != in.width || slot.inH != in.height || slot.outW != out.width ||
        slot.outH != out.height || slot.inFormat != in.pixelFormat || slot.outFormat != out.pixelFormat)
    {
        MTLFXSpatialScalerDescriptor* d = [MTLFXSpatialScalerDescriptor new];
        d.inputWidth = in.width;
        d.inputHeight = in.height;
        d.outputWidth = out.width;
        d.outputHeight = out.height;
        d.colorTextureFormat = in.pixelFormat;
        d.outputTextureFormat = out.pixelFormat;
        // R4's output is LDR in gamma space (tone mapping and the display transform are already applied)
        d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
        id<MTLFXSpatialScaler> s = [d newSpatialScalerWithDevice:dev->mtl];
        if (!s)
        {
            failOnce("the scaler could not be created for these formats and sizes");
            return false;
        }
        // The scaler states what usage it needs of the textures it reads and writes; a missing flag would be a Metal validation error
        if ((in.usage & s.colorTextureUsage) != s.colorTextureUsage || (out.usage & s.outputTextureUsage) != s.outputTextureUsage)
        {
            Log("! MetalFX: textures lack the usage the scaler asks for (input has 0x%lx, needs 0x%lx; output has 0x%lx, needs 0x%lx)",
                (unsigned long)in.usage, (unsigned long)s.colorTextureUsage, (unsigned long)out.usage,
                (unsigned long)s.outputTextureUsage);
            g_loggedFailure = true;
            return false;
        }
        slot.scaler = s;
        slot.device = dev->mtl;
        slot.inW = in.width;
        slot.inH = in.height;
        slot.outW = out.width;
        slot.outH = out.height;
        slot.inFormat = in.pixelFormat;
        slot.outFormat = out.pixelFormat;
        Log("* MetalFX spatial upscaler: %lux%lu -> %lux%lu (%s -> %s)", (unsigned long)in.width, (unsigned long)in.height,
            (unsigned long)out.width, (unsigned long)out.height, "perceptual", "private");
    }

    id<MTLCommandBuffer> cb = ctx->externalCommandBuffer(src, dst);
    slot.scaler.colorTexture = in;
    slot.scaler.outputTexture = out;
    slot.scaler.inputContentWidth = in.width;
    slot.scaler.inputContentHeight = in.height;
    [slot.scaler encodeToCommandBuffer:cb];
    return true;
}
} // namespace mtl_d3d11
