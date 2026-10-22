// Spatial upscaling with MetalFX (MTLFXSpatialScaler), for the renderer's render scale: the 3D scene is rendered at a fraction of the
// output size and this brings it to the output size before the UI is drawn at the output's own resolution.
//
//     if (mtl_d3d11::SpatialUpscalerAvailable(device))
//         ok = mtl_d3d11::EncodeSpatialUpscale(context, sceneColor, upscaled);   // sceneColor: render size, upscaled: output size
//     // not available, or it returned false: draw sceneColor into the back buffer with a filtered quad instead
#pragma once

#include "d3d11_metal.h"

namespace mtl_d3d11
{
// MetalFX spatial scaling is supported by this device (the framework is there and the GPU family is).
bool SpatialUpscalerAvailable(ID3D11Device* pDevice);

// Records an upscale of `pSrc` into `pDst` in the immediate context's command buffer (the open render pass ends, the next draw starts a
// new one). Both are single-sample 2D textures of this device, `pSrc` at the render size and `pDst` at the output size (at least as large),
// 8-bit or 16-bit float colour in gamma space (LDR; the scaler runs in its perceptual mode). `pSrc` needs a shader-resource view or render
// target bind, `pDst` a render-target bind (and an unordered-access bind, which gives the texture the shader-write usage MetalFX asks
// for). The scaler is created on first use and again when a size or format changes. Returns false, after logging why once, when
// nothing was recorded.
bool EncodeSpatialUpscale(ID3D11DeviceContext* pContext, ID3D11Texture2D* pSrc, ID3D11Texture2D* pDst);
} // namespace mtl_d3d11
