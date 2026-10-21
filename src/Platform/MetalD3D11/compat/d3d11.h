// Compatibility header: maps the Windows SDK include to the Metal shim (see docs/ios-port/METAL-SHIM.md).
#pragma once
#include <mtl_d3d11/d3d11shader_metal.h>

// R4 still asks the swap chain for its back buffer with the D3D10 interface id in one spot.
typedef ID3D11Texture2D ID3D10Texture2D;
