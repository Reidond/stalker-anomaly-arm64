// DXGI subset used by the R4 renderer: formats, swap chain, factory/adapter/output stubs.
// Numeric values match the Windows headers.
#pragma once

#include "mtl_wintypes.h"

#ifndef __dxgi_h__
#define __dxgi_h__
#endif

#define DXGI_ERROR_INVALID_CALL ((HRESULT)0x887A0001)
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002)
#define DXGI_ERROR_MORE_DATA ((HRESULT)0x887A0003)
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004)
#define DXGI_ERROR_DEVICE_REMOVED ((HRESULT)0x887A0005)
#define DXGI_ERROR_DEVICE_HUNG ((HRESULT)0x887A0006)
#define DXGI_ERROR_DEVICE_RESET ((HRESULT)0x887A0007)
#define DXGI_ERROR_WAS_STILL_DRAWING ((HRESULT)0x887A000A)
#define DXGI_STATUS_OCCLUDED ((HRESULT)0x087A0001)

enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R32G32B32A32_UINT = 3,
    DXGI_FORMAT_R32G32B32A32_SINT = 4,
    DXGI_FORMAT_R32G32B32_TYPELESS = 5,
    DXGI_FORMAT_R32G32B32_FLOAT = 6,
    DXGI_FORMAT_R32G32B32_UINT = 7,
    DXGI_FORMAT_R32G32B32_SINT = 8,
    DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R16G16B16A16_UNORM = 11,
    DXGI_FORMAT_R16G16B16A16_UINT = 12,
    DXGI_FORMAT_R16G16B16A16_SNORM = 13,
    DXGI_FORMAT_R16G16B16A16_SINT = 14,
    DXGI_FORMAT_R32G32_TYPELESS = 15,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_R32G32_UINT = 17,
    DXGI_FORMAT_R32G32_SINT = 18,
    DXGI_FORMAT_R32G8X24_TYPELESS = 19,
    DXGI_FORMAT_D32_FLOAT_S8X24_UINT = 20,
    DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS = 21,
    DXGI_FORMAT_X32_TYPELESS_G8X24_UINT = 22,
    DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24,
    DXGI_FORMAT_R10G10B10A2_UINT = 25,
    DXGI_FORMAT_R11G11B10_FLOAT = 26,
    DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
    DXGI_FORMAT_R8G8B8A8_UINT = 30,
    DXGI_FORMAT_R8G8B8A8_SNORM = 31,
    DXGI_FORMAT_R8G8B8A8_SINT = 32,
    DXGI_FORMAT_R16G16_TYPELESS = 33,
    DXGI_FORMAT_R16G16_FLOAT = 34,
    DXGI_FORMAT_R16G16_UNORM = 35,
    DXGI_FORMAT_R16G16_UINT = 36,
    DXGI_FORMAT_R16G16_SNORM = 37,
    DXGI_FORMAT_R16G16_SINT = 38,
    DXGI_FORMAT_R32_TYPELESS = 39,
    DXGI_FORMAT_D32_FLOAT = 40,
    DXGI_FORMAT_R32_FLOAT = 41,
    DXGI_FORMAT_R32_UINT = 42,
    DXGI_FORMAT_R32_SINT = 43,
    DXGI_FORMAT_R24G8_TYPELESS = 44,
    DXGI_FORMAT_D24_UNORM_S8_UINT = 45,
    DXGI_FORMAT_R24_UNORM_X8_TYPELESS = 46,
    DXGI_FORMAT_X24_TYPELESS_G8_UINT = 47,
    DXGI_FORMAT_R8G8_TYPELESS = 48,
    DXGI_FORMAT_R8G8_UNORM = 49,
    DXGI_FORMAT_R8G8_UINT = 50,
    DXGI_FORMAT_R8G8_SNORM = 51,
    DXGI_FORMAT_R8G8_SINT = 52,
    DXGI_FORMAT_R16_TYPELESS = 53,
    DXGI_FORMAT_R16_FLOAT = 54,
    DXGI_FORMAT_D16_UNORM = 55,
    DXGI_FORMAT_R16_UNORM = 56,
    DXGI_FORMAT_R16_UINT = 57,
    DXGI_FORMAT_R16_SNORM = 58,
    DXGI_FORMAT_R16_SINT = 59,
    DXGI_FORMAT_R8_TYPELESS = 60,
    DXGI_FORMAT_R8_UNORM = 61,
    DXGI_FORMAT_R8_UINT = 62,
    DXGI_FORMAT_R8_SNORM = 63,
    DXGI_FORMAT_R8_SINT = 64,
    DXGI_FORMAT_A8_UNORM = 65,
    DXGI_FORMAT_R1_UNORM = 66,
    DXGI_FORMAT_R9G9B9E5_SHAREDEXP = 67,
    DXGI_FORMAT_R8G8_B8G8_UNORM = 68,
    DXGI_FORMAT_G8R8_G8B8_UNORM = 69,
    DXGI_FORMAT_BC1_TYPELESS = 70,
    DXGI_FORMAT_BC1_UNORM = 71,
    DXGI_FORMAT_BC1_UNORM_SRGB = 72,
    DXGI_FORMAT_BC2_TYPELESS = 73,
    DXGI_FORMAT_BC2_UNORM = 74,
    DXGI_FORMAT_BC2_UNORM_SRGB = 75,
    DXGI_FORMAT_BC3_TYPELESS = 76,
    DXGI_FORMAT_BC3_UNORM = 77,
    DXGI_FORMAT_BC3_UNORM_SRGB = 78,
    DXGI_FORMAT_BC4_TYPELESS = 79,
    DXGI_FORMAT_BC4_UNORM = 80,
    DXGI_FORMAT_BC4_SNORM = 81,
    DXGI_FORMAT_BC5_TYPELESS = 82,
    DXGI_FORMAT_BC5_UNORM = 83,
    DXGI_FORMAT_BC5_SNORM = 84,
    DXGI_FORMAT_B5G6R5_UNORM = 85,
    DXGI_FORMAT_B5G5R5A1_UNORM = 86,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
    DXGI_FORMAT_B8G8R8X8_UNORM = 88,
    DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM = 89,
    DXGI_FORMAT_B8G8R8A8_TYPELESS = 90,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
    DXGI_FORMAT_B8G8R8X8_TYPELESS = 92,
    DXGI_FORMAT_B8G8R8X8_UNORM_SRGB = 93,
    DXGI_FORMAT_BC6H_TYPELESS = 94,
    DXGI_FORMAT_BC6H_UF16 = 95,
    DXGI_FORMAT_BC6H_SF16 = 96,
    DXGI_FORMAT_BC7_TYPELESS = 97,
    DXGI_FORMAT_BC7_UNORM = 98,
    DXGI_FORMAT_BC7_UNORM_SRGB = 99,
    DXGI_FORMAT_B4G4R4A4_UNORM = 115,
    DXGI_FORMAT_FORCE_UINT = 0xffffffff
};

enum DXGI_COLOR_SPACE_TYPE
{
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 = 0,
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 = 1,
    DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709 = 2,
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 = 12,
    DXGI_COLOR_SPACE_CUSTOM = 0xFFFFFFFF
};

enum DXGI_MODE_SCANLINE_ORDER
{
    DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED = 0,
    DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE = 1,
    DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST = 2,
    DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST = 3
};

enum DXGI_MODE_SCALING
{
    DXGI_MODE_SCALING_UNSPECIFIED = 0,
    DXGI_MODE_SCALING_CENTERED = 1,
    DXGI_MODE_SCALING_STRETCHED = 2
};

enum DXGI_MODE_ROTATION
{
    DXGI_MODE_ROTATION_UNSPECIFIED = 0,
    DXGI_MODE_ROTATION_IDENTITY = 1,
    DXGI_MODE_ROTATION_ROTATE90 = 2,
    DXGI_MODE_ROTATION_ROTATE180 = 3,
    DXGI_MODE_ROTATION_ROTATE270 = 4
};

enum DXGI_SWAP_EFFECT
{
    DXGI_SWAP_EFFECT_DISCARD = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4
};

enum DXGI_SCALING
{
    DXGI_SCALING_STRETCH = 0,
    DXGI_SCALING_NONE = 1,
    DXGI_SCALING_ASPECT_RATIO_STRETCH = 2
};

enum DXGI_ALPHA_MODE
{
    DXGI_ALPHA_MODE_UNSPECIFIED = 0,
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_STRAIGHT = 2,
    DXGI_ALPHA_MODE_IGNORE = 3
};

enum DXGI_SWAP_CHAIN_FLAG
{
    DXGI_SWAP_CHAIN_FLAG_NONPREROTATED = 1,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2,
    DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE = 4,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048
};

enum DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG
{
    DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT = 0x1,
    DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_OVERLAY_PRESENT = 0x2
};

enum DXGI_FEATURE
{
    DXGI_FEATURE_PRESENT_ALLOW_TEARING = 0
};

#define DXGI_USAGE_SHADER_INPUT (1u << 4)
#define DXGI_USAGE_RENDER_TARGET_OUTPUT (1u << 5)
#define DXGI_USAGE_BACK_BUFFER (1u << 6)
#define DXGI_PRESENT_TEST 0x00000001u
#define DXGI_PRESENT_DO_NOT_SEQUENCE 0x00000002u
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200u

struct DXGI_RATIONAL
{
    UINT Numerator;
    UINT Denominator;
};

struct DXGI_SAMPLE_DESC
{
    UINT Count;
    UINT Quality;
};

struct DXGI_MODE_DESC
{
    UINT Width;
    UINT Height;
    DXGI_RATIONAL RefreshRate;
    DXGI_FORMAT Format;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
};

struct DXGI_SWAP_CHAIN_DESC1
{
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    MtlBOOL Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT BufferUsage;
    UINT BufferCount;
    DXGI_SCALING Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    DXGI_ALPHA_MODE AlphaMode;
    UINT Flags;
};

struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC
{
    DXGI_RATIONAL RefreshRate;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
    MtlBOOL Windowed;
};

struct DXGI_SWAP_CHAIN_DESC
{
    DXGI_MODE_DESC BufferDesc;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT BufferUsage;
    UINT BufferCount;
    HWND OutputWindow;
    MtlBOOL Windowed;
    DXGI_SWAP_EFFECT SwapEffect;
    UINT Flags;
};

struct DXGI_ADAPTER_DESC
{
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
};
typedef DXGI_ADAPTER_DESC DXGI_ADAPTER_DESC1;

struct DXGI_OUTPUT_DESC
{
    WCHAR DeviceName[32];
    RECT DesktopCoordinates;
    MtlBOOL AttachedToDesktop;
    DXGI_MODE_ROTATION Rotation;
    HMONITOR Monitor;
};

struct DXGI_RGB
{
    float Red;
    float Green;
    float Blue;
};

struct DXGI_GAMMA_CONTROL
{
    DXGI_RGB Scale;
    DXGI_RGB Offset;
    DXGI_RGB GammaCurve[1025];
};

struct DXGI_GAMMA_CONTROL_CAPABILITIES
{
    MtlBOOL ScaleAndOffsetSupported;
    float MaxConvertedValue;
    float MinConvertedValue;
    UINT NumGammaControlPoints;
    float ControlPointPositions[1025];
};

struct IDXGIOutput;
struct IDXGIAdapter1;

struct IDXGIObject : IUnknown
{
    virtual HRESULT GetParent(MtlREFIID riid, void** ppParent) = 0;
};

struct IDXGIDeviceSubObject : IDXGIObject
{
    virtual HRESULT GetDevice(MtlREFIID riid, void** ppDevice) = 0;
};

struct IDXGIOutput : IDXGIObject
{
    virtual HRESULT GetDesc(DXGI_OUTPUT_DESC* pDesc) = 0;
    virtual HRESULT GetDisplayModeList(DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes, DXGI_MODE_DESC* pDesc) = 0;
    virtual HRESULT FindClosestMatchingMode(const DXGI_MODE_DESC* pModeToMatch, DXGI_MODE_DESC* pClosestMatch,
                                            IUnknown* pConcernedDevice) = 0;
    virtual HRESULT GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* pGammaCaps) = 0;
    virtual HRESULT SetGammaControl(const DXGI_GAMMA_CONTROL* pArray) = 0;
    virtual HRESULT GetGammaControl(DXGI_GAMMA_CONTROL* pArray) = 0;
};

struct IDXGIAdapter : IDXGIObject
{
    virtual HRESULT EnumOutputs(UINT Output, IDXGIOutput** ppOutput) = 0;
    virtual HRESULT GetDesc(DXGI_ADAPTER_DESC* pDesc) = 0;
};

struct IDXGIAdapter1 : IDXGIAdapter
{
    virtual HRESULT GetDesc1(DXGI_ADAPTER_DESC1* pDesc) = 0;
};

struct IDXGISwapChain : IDXGIDeviceSubObject
{
    virtual HRESULT Present(UINT SyncInterval, UINT Flags) = 0;
    virtual HRESULT GetBuffer(UINT Buffer, MtlREFIID riid, void** ppSurface) = 0;
    virtual HRESULT SetFullscreenState(MtlBOOL Fullscreen, IDXGIOutput* pTarget) = 0;
    virtual HRESULT GetFullscreenState(MtlBOOL* pFullscreen, IDXGIOutput** ppTarget) = 0;
    virtual HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) = 0;
    virtual HRESULT ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                  UINT SwapChainFlags) = 0;
    virtual HRESULT ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) = 0;
    virtual HRESULT GetContainingOutput(IDXGIOutput** ppOutput) = 0;
};

struct IDXGISwapChain1 : IDXGISwapChain
{
    virtual HRESULT GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) = 0;
};

struct IDXGISwapChain3 : IDXGISwapChain1
{
    virtual UINT GetCurrentBackBufferIndex() = 0;
    virtual HRESULT CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport) = 0;
    virtual HRESULT SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) = 0;
};

struct IDXGIFactory : IDXGIObject
{
    virtual HRESULT EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) = 0;
};

struct IDXGIFactory1 : IDXGIFactory
{
    virtual HRESULT EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) = 0;
};

struct IDXGIFactory2 : IDXGIFactory1
{
    // hWnd is interpreted by the shim: a CAMetalLayer* or a handle registered with MtlRegisterWindowLayer.
    virtual HRESULT CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                           IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) = 0;
};

struct IDXGIFactory5 : IDXGIFactory2
{
    virtual HRESULT CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData,
                                        UINT FeatureSupportDataSize) = 0;
};
typedef IDXGIFactory5 IDXGIFactory3;
typedef IDXGIFactory5 IDXGIFactory4;

MTL_DECLARE_IID(IDXGIObject, 0xaec22fb8, 0x76f3, 0x4639, 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e)
MTL_DECLARE_IID(IDXGIDeviceSubObject, 0x3d3e0379, 0xf9de, 0x4d58, 0xbb, 0x6c, 0x18, 0xd6, 0x29, 0x92, 0xf1, 0xa6)
MTL_DECLARE_IID(IDXGISwapChain, 0x310d36a0, 0xd2e7, 0x4c0a, 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a)
MTL_DECLARE_IID(IDXGISwapChain1, 0x790a45f7, 0x0d42, 0x4876, 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa)
MTL_DECLARE_IID(IDXGISwapChain3, 0x94d99bdb, 0xf1f8, 0x4ab0, 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1)
MTL_DECLARE_IID(IDXGIFactory, 0x7b7166ec, 0x21c7, 0x44ae, 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69)
MTL_DECLARE_IID(IDXGIFactory1, 0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87)
MTL_DECLARE_IID(IDXGIFactory2, 0x50c83a1c, 0xe072, 0x4c48, 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0)
MTL_DECLARE_IID(IDXGIFactory5, 0x7632e1f5, 0xee65, 0x4dca, 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d)
MTL_DECLARE_IID(IDXGIAdapter, 0x2411e7e1, 0x12ac, 0x4ccf, 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0)
MTL_DECLARE_IID(IDXGIAdapter1, 0x29038f61, 0x3839, 0x4626, 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05)
MTL_DECLARE_IID(IDXGIOutput, 0xae02eedb, 0xc735, 0x4690, 0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13, 0xaa)

// CreateDXGIFactory2 returns a minimal factory: one adapter (the Metal device), one output.
extern "C++" HRESULT CreateDXGIFactory2(UINT Flags, MtlREFIID riid, void** ppFactory);
