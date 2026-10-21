// D3DX11 texture helpers used by R4, reimplemented on the shim: a DDS loader that honours
// D3DX11_IMAGE_LOAD_INFO, header probing, mip generation and a read-back saver.
//
// Only DDS input is supported (that is all Anomaly ships). Output formats: DDS and BMP (screenshots can
// layer a PNG/JPG encoder on top; see METAL-SHIM.md).
#pragma once

#include "d3d11_metal.h"

#ifndef __d3dx11_h__
#define __d3dx11_h__
#endif

#define D3DX11_DEFAULT ((UINT)-1)
#define D3DX11_FROM_FILE ((UINT)-3)
#define DXGI_FORMAT_FROM_FILE ((DXGI_FORMAT)-3)

enum D3DX11_IMAGE_FILE_FORMAT
{
    D3DX11_IFF_BMP = 0,
    D3DX11_IFF_JPG = 1,
    D3DX11_IFF_PNG = 3,
    D3DX11_IFF_DDS = 4,
    D3DX11_IFF_TIFF = 10,
    D3DX11_IFF_GIF = 11,
    D3DX11_IFF_WMP = 12,
    D3DX11_IFF_FORCE_DWORD = 0x7fffffff
};

#define D3DX10_FILTER_NONE (1 << 0)
#define D3DX10_FILTER_POINT (2 << 0)
#define D3DX10_FILTER_LINEAR (3 << 0)
#define D3DX10_FILTER_TRIANGLE (4 << 0)
#define D3DX10_FILTER_BOX (5 << 0)
#define D3DX11_FILTER_POINT D3DX10_FILTER_POINT
#define D3DX11_FILTER_LINEAR D3DX10_FILTER_LINEAR

struct D3DX11_IMAGE_INFO
{
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT ArraySize;
    UINT MipLevels;
    UINT MiscFlags;
    DXGI_FORMAT Format;
    D3D11_RESOURCE_DIMENSION ResourceDimension;
    D3DX11_IMAGE_FILE_FORMAT ImageFileFormat;
};

struct D3DX11_IMAGE_LOAD_INFO
{
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT FirstMipLevel;
    UINT MipLevels;
    D3D11_USAGE Usage;
    UINT BindFlags;
    UINT CpuAccessFlags;
    UINT MiscFlags;
    DXGI_FORMAT Format;
    UINT Filter;
    UINT MipFilter;
    D3DX11_IMAGE_INFO* pSrcInfo;

    D3DX11_IMAGE_LOAD_INFO()
        : Width(D3DX11_DEFAULT), Height(D3DX11_DEFAULT), Depth(D3DX11_DEFAULT), FirstMipLevel(D3DX11_DEFAULT),
          MipLevels(D3DX11_DEFAULT), Usage((D3D11_USAGE)D3DX11_DEFAULT), BindFlags(D3DX11_DEFAULT),
          CpuAccessFlags(D3DX11_DEFAULT), MiscFlags(D3DX11_DEFAULT), Format((DXGI_FORMAT)D3DX11_DEFAULT),
          Filter(D3DX11_DEFAULT), MipFilter(D3DX11_DEFAULT), pSrcInfo(nullptr)
    {
    }
};

struct ID3DX11ThreadPump;

HRESULT D3DX11GetImageInfoFromMemory(LPCVOID pSrcData, SIZE_T SrcDataSize, ID3DX11ThreadPump* pPump,
                                     D3DX11_IMAGE_INFO* pSrcInfo, HRESULT* pHResult);

// Creates a 2D, cube or 3D texture from a DDS image. FirstMipLevel skips that many top mips of the source;
// Width/Height/MipLevels (when not D3DX11_DEFAULT) describe the resulting texture. Usage IMMUTABLE/DEFAULT
// uploads immediately; STAGING keeps the data CPU-side for a later CopyResource.
HRESULT D3DX11CreateTextureFromMemory(ID3D11Device* pDevice, LPCVOID pSrcData, SIZE_T SrcDataSize,
                                      D3DX11_IMAGE_LOAD_INFO* pLoadInfo, ID3DX11ThreadPump* pPump,
                                      ID3D11Resource** ppTexture, HRESULT* pHResult);

// Regenerates the mip chain of a 2D texture (box filter; the filter argument is accepted and ignored).
HRESULT D3DX11FilterTexture(ID3D11DeviceContext* pContext, ID3D11Resource* pTexture, UINT SrcLevel, UINT MipFilter);

// Copies a texture to another with the same layout (used by screenshots to downsize/convert).
HRESULT D3DX11LoadTextureFromTexture(ID3D11DeviceContext* pContext, ID3D11Resource* pSrcTexture,
                                     D3DX11_IMAGE_LOAD_INFO* pLoadInfo, ID3D11Resource* pDstTexture);

struct ID3D10Blob;
// Reads a 2D texture back and encodes it. Supported: D3DX11_IFF_DDS, D3DX11_IFF_BMP. Others return E_NOTIMPL.
HRESULT D3DX11SaveTextureToMemory(ID3D11DeviceContext* pContext, ID3D11Resource* pSrcTexture,
                                  D3DX11_IMAGE_FILE_FORMAT DestFormat, ID3D10Blob** ppDestBuf, UINT Flags);
HRESULT D3DX11SaveTextureToFile(ID3D11DeviceContext* pContext, ID3D11Resource* pSrcTexture,
                                D3DX11_IMAGE_FILE_FORMAT DestFormat, LPCSTR pDestFile);
