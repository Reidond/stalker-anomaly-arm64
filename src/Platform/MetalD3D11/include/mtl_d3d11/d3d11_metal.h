// D3D11 subset used by the R4 renderer, declared for the Metal implementation.
//
// Coverage follows docs/ios-port/D3D11-SURFACE.md: everything R4 calls, plus a few neighbours that are
// cheap on Metal (instanced draws, UpdateSubresource, GenerateMips, UAV clears). Numeric enum values match
// the Windows headers. Tessellation and geometry shader objects exist as types so engine code compiles, but
// creating them fails with E_NOTIMPL.
#pragma once

#include "dxgi_metal.h"
#include "mtl_wintypes.h"

#ifndef __d3d11_h__
#define __d3d11_h__
#endif

#define D3D11_SDK_VERSION 7
#define D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT 16
#define D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT 128
#define D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT 14
#define D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT 32
#define D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT 8
#define D3D11_PS_CS_UAV_REGISTER_COUNT 8
#define D3D11_REQ_TEXTURE1D_U_DIMENSION 16384
#define D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION 16384
#define D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION 2048
#define D3D11_STANDARD_MULTISAMPLE_PATTERN 0xffffffff
#define D3D11_APPEND_ALIGNED_ELEMENT 0xffffffff
#define D3D11_DEFAULT_STENCIL_REFERENCE 0
#define D3D11_REQ_MIP_LEVELS 15
#define D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION 2048
#define D3D11_FLOAT32_MAX 3.402823466e+38f
#ifndef D3D_OK
#define D3D_OK S_OK // d3d9.h's name; R4's DX11 build still tests it
#endif

typedef RECT D3D11_RECT;

// ---------------------------------------------------------------------------------------------------------
// Feature levels, driver types, creation flags
// ---------------------------------------------------------------------------------------------------------
enum D3D_FEATURE_LEVEL
{
    D3D_FEATURE_LEVEL_9_1 = 0x9100,
    D3D_FEATURE_LEVEL_9_2 = 0x9200,
    D3D_FEATURE_LEVEL_9_3 = 0x9300,
    D3D_FEATURE_LEVEL_10_0 = 0xa000,
    D3D_FEATURE_LEVEL_10_1 = 0xa100,
    D3D_FEATURE_LEVEL_11_0 = 0xb000,
    D3D_FEATURE_LEVEL_11_1 = 0xb100,
    D3D_FEATURE_LEVEL_12_0 = 0xc000,
    D3D_FEATURE_LEVEL_12_1 = 0xc100
};

enum D3D_DRIVER_TYPE
{
    D3D_DRIVER_TYPE_UNKNOWN = 0,
    D3D_DRIVER_TYPE_HARDWARE = 1,
    D3D_DRIVER_TYPE_REFERENCE = 2,
    D3D_DRIVER_TYPE_NULL = 3,
    D3D_DRIVER_TYPE_SOFTWARE = 4,
    D3D_DRIVER_TYPE_WARP = 5
};
typedef D3D_DRIVER_TYPE D3D11_DRIVER_TYPE;
#define D3D11_DRIVER_TYPE_HARDWARE D3D_DRIVER_TYPE_HARDWARE
#define D3D11_DRIVER_TYPE_REFERENCE D3D_DRIVER_TYPE_REFERENCE

enum D3D11_CREATE_DEVICE_FLAG
{
    D3D11_CREATE_DEVICE_SINGLETHREADED = 0x1,
    D3D11_CREATE_DEVICE_DEBUG = 0x2,
    D3D11_CREATE_DEVICE_SWITCH_TO_REF = 0x4,
    D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS = 0x8,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20
};

// ---------------------------------------------------------------------------------------------------------
// Resource enums
// ---------------------------------------------------------------------------------------------------------
enum D3D11_USAGE
{
    D3D11_USAGE_DEFAULT = 0,
    D3D11_USAGE_IMMUTABLE = 1,
    D3D11_USAGE_DYNAMIC = 2,
    D3D11_USAGE_STAGING = 3
};

enum D3D11_BIND_FLAG
{
    D3D11_BIND_VERTEX_BUFFER = 0x1,
    D3D11_BIND_INDEX_BUFFER = 0x2,
    D3D11_BIND_CONSTANT_BUFFER = 0x4,
    D3D11_BIND_SHADER_RESOURCE = 0x8,
    D3D11_BIND_STREAM_OUTPUT = 0x10,
    D3D11_BIND_RENDER_TARGET = 0x20,
    D3D11_BIND_DEPTH_STENCIL = 0x40,
    D3D11_BIND_UNORDERED_ACCESS = 0x80
};

#define D3D10_BIND_SHADER_RESOURCE D3D11_BIND_SHADER_RESOURCE // names left over in the screenshot/combine paths
#define D3D10_BIND_RENDER_TARGET D3D11_BIND_RENDER_TARGET

enum D3D11_CPU_ACCESS_FLAG
{
    D3D11_CPU_ACCESS_WRITE = 0x10000,
    D3D11_CPU_ACCESS_READ = 0x20000
};

enum D3D11_RESOURCE_MISC_FLAG
{
    D3D11_RESOURCE_MISC_GENERATE_MIPS = 0x1,
    D3D11_RESOURCE_MISC_SHARED = 0x2,
    D3D11_RESOURCE_MISC_TEXTURECUBE = 0x4,
    D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS = 0x10,
    D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS = 0x20,
    D3D11_RESOURCE_MISC_BUFFER_STRUCTURED = 0x40,
    D3D11_RESOURCE_MISC_RESOURCE_CLAMP = 0x80,
    D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX = 0x100,
    D3D11_RESOURCE_MISC_GDI_COMPATIBLE = 0x200
};

enum D3D11_MAP
{
    D3D11_MAP_READ = 1,
    D3D11_MAP_WRITE = 2,
    D3D11_MAP_READ_WRITE = 3,
    D3D11_MAP_WRITE_DISCARD = 4,
    D3D11_MAP_WRITE_NO_OVERWRITE = 5
};
#define D3D11_MAP_FLAG_DO_NOT_WAIT 0x100000u

enum D3D11_CLEAR_FLAG
{
    D3D11_CLEAR_DEPTH = 0x1,
    D3D11_CLEAR_STENCIL = 0x2
};

enum D3D11_RESOURCE_DIMENSION
{
    D3D11_RESOURCE_DIMENSION_UNKNOWN = 0,
    D3D11_RESOURCE_DIMENSION_BUFFER = 1,
    D3D11_RESOURCE_DIMENSION_TEXTURE1D = 2,
    D3D11_RESOURCE_DIMENSION_TEXTURE2D = 3,
    D3D11_RESOURCE_DIMENSION_TEXTURE3D = 4
};

enum D3D11_FORMAT_SUPPORT
{
    D3D11_FORMAT_SUPPORT_BUFFER = 0x1,
    D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER = 0x2,
    D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER = 0x4,
    D3D11_FORMAT_SUPPORT_TEXTURE1D = 0x10,
    D3D11_FORMAT_SUPPORT_TEXTURE2D = 0x20,
    D3D11_FORMAT_SUPPORT_TEXTURE3D = 0x40,
    D3D11_FORMAT_SUPPORT_TEXTURECUBE = 0x80,
    D3D11_FORMAT_SUPPORT_SHADER_LOAD = 0x100,
    D3D11_FORMAT_SUPPORT_SHADER_SAMPLE = 0x200,
    D3D11_FORMAT_SUPPORT_MIP = 0x1000,
    D3D11_FORMAT_SUPPORT_MIP_AUTOGEN = 0x2000,
    D3D11_FORMAT_SUPPORT_RENDER_TARGET = 0x4000,
    D3D11_FORMAT_SUPPORT_BLENDABLE = 0x8000,
    D3D11_FORMAT_SUPPORT_DEPTH_STENCIL = 0x10000,
    D3D11_FORMAT_SUPPORT_CPU_LOCKABLE = 0x20000,
    D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE = 0x40000,
    D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET = 0x200000,
    D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD = 0x400000,
    D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW = 0x2000000
};

// ---------------------------------------------------------------------------------------------------------
// Pipeline state enums
// ---------------------------------------------------------------------------------------------------------
enum D3D11_COMPARISON_FUNC
{
    D3D11_COMPARISON_NEVER = 1,
    D3D11_COMPARISON_LESS = 2,
    D3D11_COMPARISON_EQUAL = 3,
    D3D11_COMPARISON_LESS_EQUAL = 4,
    D3D11_COMPARISON_GREATER = 5,
    D3D11_COMPARISON_NOT_EQUAL = 6,
    D3D11_COMPARISON_GREATER_EQUAL = 7,
    D3D11_COMPARISON_ALWAYS = 8
};

enum D3D11_DEPTH_WRITE_MASK
{
    D3D11_DEPTH_WRITE_MASK_ZERO = 0,
    D3D11_DEPTH_WRITE_MASK_ALL = 1
};

enum D3D11_STENCIL_OP
{
    D3D11_STENCIL_OP_KEEP = 1,
    D3D11_STENCIL_OP_ZERO = 2,
    D3D11_STENCIL_OP_REPLACE = 3,
    D3D11_STENCIL_OP_INCR_SAT = 4,
    D3D11_STENCIL_OP_DECR_SAT = 5,
    D3D11_STENCIL_OP_INVERT = 6,
    D3D11_STENCIL_OP_INCR = 7,
    D3D11_STENCIL_OP_DECR = 8
};

enum D3D11_BLEND
{
    D3D11_BLEND_ZERO = 1,
    D3D11_BLEND_ONE = 2,
    D3D11_BLEND_SRC_COLOR = 3,
    D3D11_BLEND_INV_SRC_COLOR = 4,
    D3D11_BLEND_SRC_ALPHA = 5,
    D3D11_BLEND_INV_SRC_ALPHA = 6,
    D3D11_BLEND_DEST_ALPHA = 7,
    D3D11_BLEND_INV_DEST_ALPHA = 8,
    D3D11_BLEND_DEST_COLOR = 9,
    D3D11_BLEND_INV_DEST_COLOR = 10,
    D3D11_BLEND_SRC_ALPHA_SAT = 11,
    D3D11_BLEND_BLEND_FACTOR = 14,
    D3D11_BLEND_INV_BLEND_FACTOR = 15,
    D3D11_BLEND_SRC1_COLOR = 16,
    D3D11_BLEND_INV_SRC1_COLOR = 17,
    D3D11_BLEND_SRC1_ALPHA = 18,
    D3D11_BLEND_INV_SRC1_ALPHA = 19
};

enum D3D11_BLEND_OP
{
    D3D11_BLEND_OP_ADD = 1,
    D3D11_BLEND_OP_SUBTRACT = 2,
    D3D11_BLEND_OP_REV_SUBTRACT = 3,
    D3D11_BLEND_OP_MIN = 4,
    D3D11_BLEND_OP_MAX = 5
};

enum D3D11_COLOR_WRITE_ENABLE
{
    D3D11_COLOR_WRITE_ENABLE_RED = 1,
    D3D11_COLOR_WRITE_ENABLE_GREEN = 2,
    D3D11_COLOR_WRITE_ENABLE_BLUE = 4,
    D3D11_COLOR_WRITE_ENABLE_ALPHA = 8,
    D3D11_COLOR_WRITE_ENABLE_ALL = 15
};

enum D3D11_FILL_MODE
{
    D3D11_FILL_WIREFRAME = 2,
    D3D11_FILL_SOLID = 3
};

enum D3D11_CULL_MODE
{
    D3D11_CULL_NONE = 1,
    D3D11_CULL_FRONT = 2,
    D3D11_CULL_BACK = 3
};

enum D3D11_FILTER
{
    D3D11_FILTER_MIN_MAG_MIP_POINT = 0,
    D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR = 0x1,
    D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x4,
    D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR = 0x5,
    D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT = 0x10,
    D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x11,
    D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT = 0x14,
    D3D11_FILTER_MIN_MAG_MIP_LINEAR = 0x15,
    D3D11_FILTER_ANISOTROPIC = 0x55,
    D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT = 0x80,
    D3D11_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR = 0x81,
    D3D11_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x84,
    D3D11_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR = 0x85,
    D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT = 0x90,
    D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x91,
    D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT = 0x94,
    D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR = 0x95,
    D3D11_FILTER_COMPARISON_ANISOTROPIC = 0xd5,
    D3D11_FILTER_MINIMUM_MIN_MAG_MIP_POINT = 0x100,
    D3D11_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR = 0x115,
    D3D11_FILTER_MINIMUM_ANISOTROPIC = 0x155,
    D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_POINT = 0x180,
    D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR = 0x195,
    D3D11_FILTER_MAXIMUM_ANISOTROPIC = 0x1d5
};

enum D3D11_TEXTURE_ADDRESS_MODE
{
    D3D11_TEXTURE_ADDRESS_WRAP = 1,
    D3D11_TEXTURE_ADDRESS_MIRROR = 2,
    D3D11_TEXTURE_ADDRESS_CLAMP = 3,
    D3D11_TEXTURE_ADDRESS_BORDER = 4,
    D3D11_TEXTURE_ADDRESS_MIRROR_ONCE = 5
};

enum D3D_PRIMITIVE_TOPOLOGY
{
    D3D_PRIMITIVE_TOPOLOGY_UNDEFINED = 0,
    D3D_PRIMITIVE_TOPOLOGY_POINTLIST = 1,
    D3D_PRIMITIVE_TOPOLOGY_LINELIST = 2,
    D3D_PRIMITIVE_TOPOLOGY_LINESTRIP = 3,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5,
    D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ = 10,
    D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ = 11,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ = 12,
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ = 13,
    D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST = 33,
    D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST = 35,
    D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST = 36
};
typedef D3D_PRIMITIVE_TOPOLOGY D3D11_PRIMITIVE_TOPOLOGY;
#define D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED D3D_PRIMITIVE_TOPOLOGY_UNDEFINED
#define D3D11_PRIMITIVE_TOPOLOGY_POINTLIST D3D_PRIMITIVE_TOPOLOGY_POINTLIST
#define D3D11_PRIMITIVE_TOPOLOGY_LINELIST D3D_PRIMITIVE_TOPOLOGY_LINELIST
#define D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP D3D_PRIMITIVE_TOPOLOGY_LINESTRIP
#define D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
#define D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
#define D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST

enum D3D11_INPUT_CLASSIFICATION
{
    D3D11_INPUT_PER_VERTEX_DATA = 0,
    D3D11_INPUT_PER_INSTANCE_DATA = 1
};

enum D3D_SRV_DIMENSION
{
    D3D_SRV_DIMENSION_UNKNOWN = 0,
    D3D_SRV_DIMENSION_BUFFER = 1,
    D3D_SRV_DIMENSION_TEXTURE1D = 2,
    D3D_SRV_DIMENSION_TEXTURE1DARRAY = 3,
    D3D_SRV_DIMENSION_TEXTURE2D = 4,
    D3D_SRV_DIMENSION_TEXTURE2DARRAY = 5,
    D3D_SRV_DIMENSION_TEXTURE2DMS = 6,
    D3D_SRV_DIMENSION_TEXTURE2DMSARRAY = 7,
    D3D_SRV_DIMENSION_TEXTURE3D = 8,
    D3D_SRV_DIMENSION_TEXTURECUBE = 9,
    D3D_SRV_DIMENSION_TEXTURECUBEARRAY = 10,
    D3D_SRV_DIMENSION_BUFFEREX = 11
};
// like d3d11.h of the Windows SDK: the D3D11 names alias the d3dcommon.h ones
typedef D3D_SRV_DIMENSION D3D11_SRV_DIMENSION;
#define D3D11_SRV_DIMENSION_UNKNOWN D3D_SRV_DIMENSION_UNKNOWN
#define D3D11_SRV_DIMENSION_BUFFER D3D_SRV_DIMENSION_BUFFER
#define D3D11_SRV_DIMENSION_TEXTURE1D D3D_SRV_DIMENSION_TEXTURE1D
#define D3D11_SRV_DIMENSION_TEXTURE1DARRAY D3D_SRV_DIMENSION_TEXTURE1DARRAY
#define D3D11_SRV_DIMENSION_TEXTURE2D D3D_SRV_DIMENSION_TEXTURE2D
#define D3D11_SRV_DIMENSION_TEXTURE2DARRAY D3D_SRV_DIMENSION_TEXTURE2DARRAY
#define D3D11_SRV_DIMENSION_TEXTURE2DMS D3D_SRV_DIMENSION_TEXTURE2DMS
#define D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY D3D_SRV_DIMENSION_TEXTURE2DMSARRAY
#define D3D11_SRV_DIMENSION_TEXTURE3D D3D_SRV_DIMENSION_TEXTURE3D
#define D3D11_SRV_DIMENSION_TEXTURECUBE D3D_SRV_DIMENSION_TEXTURECUBE
#define D3D11_SRV_DIMENSION_TEXTURECUBEARRAY D3D_SRV_DIMENSION_TEXTURECUBEARRAY
#define D3D11_SRV_DIMENSION_BUFFEREX D3D_SRV_DIMENSION_BUFFEREX

enum D3D11_RTV_DIMENSION
{
    D3D11_RTV_DIMENSION_UNKNOWN = 0,
    D3D11_RTV_DIMENSION_BUFFER = 1,
    D3D11_RTV_DIMENSION_TEXTURE1D = 2,
    D3D11_RTV_DIMENSION_TEXTURE1DARRAY = 3,
    D3D11_RTV_DIMENSION_TEXTURE2D = 4,
    D3D11_RTV_DIMENSION_TEXTURE2DARRAY = 5,
    D3D11_RTV_DIMENSION_TEXTURE2DMS = 6,
    D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY = 7,
    D3D11_RTV_DIMENSION_TEXTURE3D = 8
};

enum D3D11_DSV_DIMENSION
{
    D3D11_DSV_DIMENSION_UNKNOWN = 0,
    D3D11_DSV_DIMENSION_TEXTURE1D = 1,
    D3D11_DSV_DIMENSION_TEXTURE1DARRAY = 2,
    D3D11_DSV_DIMENSION_TEXTURE2D = 3,
    D3D11_DSV_DIMENSION_TEXTURE2DARRAY = 4,
    D3D11_DSV_DIMENSION_TEXTURE2DMS = 5,
    D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY = 6
};

enum D3D11_UAV_DIMENSION
{
    D3D11_UAV_DIMENSION_UNKNOWN = 0,
    D3D11_UAV_DIMENSION_BUFFER = 1,
    D3D11_UAV_DIMENSION_TEXTURE1D = 2,
    D3D11_UAV_DIMENSION_TEXTURE1DARRAY = 3,
    D3D11_UAV_DIMENSION_TEXTURE2D = 4,
    D3D11_UAV_DIMENSION_TEXTURE2DARRAY = 5,
    D3D11_UAV_DIMENSION_TEXTURE3D = 8
};

enum D3D11_DSV_FLAG
{
    D3D11_DSV_READ_ONLY_DEPTH = 0x1,
    D3D11_DSV_READ_ONLY_STENCIL = 0x2
};

enum D3D11_QUERY
{
    D3D11_QUERY_EVENT = 0,
    D3D11_QUERY_OCCLUSION = 1,
    D3D11_QUERY_TIMESTAMP = 2,
    D3D11_QUERY_TIMESTAMP_DISJOINT = 3,
    D3D11_QUERY_PIPELINE_STATISTICS = 4,
    D3D11_QUERY_OCCLUSION_PREDICATE = 5
};
#define D3D11_ASYNC_GETDATA_DONOTFLUSH 0x1u

// ---------------------------------------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------------------------------------
struct D3D11_BOX
{
    UINT left;
    UINT top;
    UINT front;
    UINT right;
    UINT bottom;
    UINT back;
};

struct D3D11_VIEWPORT
{
    FLOAT TopLeftX;
    FLOAT TopLeftY;
    FLOAT Width;
    FLOAT Height;
    FLOAT MinDepth;
    FLOAT MaxDepth;
};

struct D3D11_SUBRESOURCE_DATA
{
    const void* pSysMem;
    UINT SysMemPitch;
    UINT SysMemSlicePitch;
};

struct D3D11_MAPPED_SUBRESOURCE
{
    void* pData;
    UINT RowPitch;
    UINT DepthPitch;
};

struct D3D11_BUFFER_DESC
{
    UINT ByteWidth;
    D3D11_USAGE Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
    UINT StructureByteStride;
};

struct D3D11_TEXTURE1D_DESC
{
    UINT Width;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_FORMAT Format;
    D3D11_USAGE Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
};

struct D3D11_TEXTURE2D_DESC
{
    UINT Width;
    UINT Height;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    D3D11_USAGE Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
};

struct D3D11_TEXTURE3D_DESC
{
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT MipLevels;
    DXGI_FORMAT Format;
    D3D11_USAGE Usage;
    UINT BindFlags;
    UINT CPUAccessFlags;
    UINT MiscFlags;
};

struct D3D11_BUFFER_SRV
{
    union { UINT FirstElement; UINT ElementOffset; };
    union { UINT NumElements; UINT ElementWidth; };
};
struct D3D11_TEX1D_SRV { UINT MostDetailedMip; UINT MipLevels; };
struct D3D11_TEX1D_ARRAY_SRV { UINT MostDetailedMip; UINT MipLevels; UINT FirstArraySlice; UINT ArraySize; };
struct D3D11_TEX2D_SRV { UINT MostDetailedMip; UINT MipLevels; };
struct D3D11_TEX2D_ARRAY_SRV { UINT MostDetailedMip; UINT MipLevels; UINT FirstArraySlice; UINT ArraySize; };
struct D3D11_TEX3D_SRV { UINT MostDetailedMip; UINT MipLevels; };
struct D3D11_TEXCUBE_SRV { UINT MostDetailedMip; UINT MipLevels; };
struct D3D11_TEXCUBE_ARRAY_SRV { UINT MostDetailedMip; UINT MipLevels; UINT First2DArrayFace; UINT NumCubes; };
struct D3D11_TEX2DMS_SRV { UINT UnusedField_NothingToDefine; };
struct D3D11_TEX2DMS_ARRAY_SRV { UINT FirstArraySlice; UINT ArraySize; };

struct D3D11_SHADER_RESOURCE_VIEW_DESC
{
    DXGI_FORMAT Format;
    D3D11_SRV_DIMENSION ViewDimension;
    union
    {
        D3D11_BUFFER_SRV Buffer;
        D3D11_TEX1D_SRV Texture1D;
        D3D11_TEX1D_ARRAY_SRV Texture1DArray;
        D3D11_TEX2D_SRV Texture2D;
        D3D11_TEX2D_ARRAY_SRV Texture2DArray;
        D3D11_TEX2DMS_SRV Texture2DMS;
        D3D11_TEX2DMS_ARRAY_SRV Texture2DMSArray;
        D3D11_TEX3D_SRV Texture3D;
        D3D11_TEXCUBE_SRV TextureCube;
        D3D11_TEXCUBE_ARRAY_SRV TextureCubeArray;
    };
};

struct D3D11_BUFFER_RTV { UINT FirstElement; UINT NumElements; };
struct D3D11_TEX1D_RTV { UINT MipSlice; };
struct D3D11_TEX1D_ARRAY_RTV { UINT MipSlice; UINT FirstArraySlice; UINT ArraySize; };
struct D3D11_TEX2D_RTV { UINT MipSlice; };
struct D3D11_TEX2DMS_RTV { UINT UnusedField_NothingToDefine; };
struct D3D11_TEX2D_ARRAY_RTV { UINT MipSlice; UINT FirstArraySlice; UINT ArraySize; };
struct D3D11_TEX2DMS_ARRAY_RTV { UINT FirstArraySlice; UINT ArraySize; };
struct D3D11_TEX3D_RTV { UINT MipSlice; UINT FirstWSlice; UINT WSize; };

struct D3D11_RENDER_TARGET_VIEW_DESC
{
    DXGI_FORMAT Format;
    D3D11_RTV_DIMENSION ViewDimension;
    union
    {
        D3D11_BUFFER_RTV Buffer;
        D3D11_TEX1D_RTV Texture1D;
        D3D11_TEX1D_ARRAY_RTV Texture1DArray;
        D3D11_TEX2D_RTV Texture2D;
        D3D11_TEX2D_ARRAY_RTV Texture2DArray;
        D3D11_TEX2DMS_RTV Texture2DMS;
        D3D11_TEX2DMS_ARRAY_RTV Texture2DMSArray;
        D3D11_TEX3D_RTV Texture3D;
    };
};

struct D3D11_TEX1D_DSV { UINT MipSlice; };
struct D3D11_TEX1D_ARRAY_DSV { UINT MipSlice; UINT FirstArraySlice; UINT ArraySize; };
struct D3D11_TEX2D_DSV { UINT MipSlice; };
struct D3D11_TEX2D_ARRAY_DSV { UINT MipSlice; UINT FirstArraySlice; UINT ArraySize; };
struct D3D11_TEX2DMS_DSV { UINT UnusedField_NothingToDefine; };
struct D3D11_TEX2DMS_ARRAY_DSV { UINT FirstArraySlice; UINT ArraySize; };

struct D3D11_DEPTH_STENCIL_VIEW_DESC
{
    DXGI_FORMAT Format;
    D3D11_DSV_DIMENSION ViewDimension;
    UINT Flags;
    union
    {
        D3D11_TEX1D_DSV Texture1D;
        D3D11_TEX1D_ARRAY_DSV Texture1DArray;
        D3D11_TEX2D_DSV Texture2D;
        D3D11_TEX2D_ARRAY_DSV Texture2DArray;
        D3D11_TEX2DMS_DSV Texture2DMS;
        D3D11_TEX2DMS_ARRAY_DSV Texture2DMSArray;
    };
};

struct D3D11_BUFFER_UAV { UINT FirstElement; UINT NumElements; UINT Flags; };
struct D3D11_TEX1D_UAV { UINT MipSlice; };
struct D3D11_TEX1D_ARRAY_UAV { UINT MipSlice; UINT FirstArraySlice; UINT ArraySize; };
struct D3D11_TEX2D_UAV { UINT MipSlice; };
struct D3D11_TEX2D_ARRAY_UAV { UINT MipSlice; UINT FirstArraySlice; UINT ArraySize; };
struct D3D11_TEX3D_UAV { UINT MipSlice; UINT FirstWSlice; UINT WSize; };

struct D3D11_UNORDERED_ACCESS_VIEW_DESC
{
    DXGI_FORMAT Format;
    D3D11_UAV_DIMENSION ViewDimension;
    union
    {
        D3D11_BUFFER_UAV Buffer;
        D3D11_TEX1D_UAV Texture1D;
        D3D11_TEX1D_ARRAY_UAV Texture1DArray;
        D3D11_TEX2D_UAV Texture2D;
        D3D11_TEX2D_ARRAY_UAV Texture2DArray;
        D3D11_TEX3D_UAV Texture3D;
    };
};

struct D3D11_SAMPLER_DESC
{
    D3D11_FILTER Filter;
    D3D11_TEXTURE_ADDRESS_MODE AddressU;
    D3D11_TEXTURE_ADDRESS_MODE AddressV;
    D3D11_TEXTURE_ADDRESS_MODE AddressW;
    FLOAT MipLODBias;
    UINT MaxAnisotropy;
    D3D11_COMPARISON_FUNC ComparisonFunc;
    FLOAT BorderColor[4];
    FLOAT MinLOD;
    FLOAT MaxLOD;
};

struct D3D11_DEPTH_STENCILOP_DESC
{
    D3D11_STENCIL_OP StencilFailOp;
    D3D11_STENCIL_OP StencilDepthFailOp;
    D3D11_STENCIL_OP StencilPassOp;
    D3D11_COMPARISON_FUNC StencilFunc;
};

struct D3D11_DEPTH_STENCIL_DESC
{
    MtlBOOL DepthEnable;
    D3D11_DEPTH_WRITE_MASK DepthWriteMask;
    D3D11_COMPARISON_FUNC DepthFunc;
    MtlBOOL StencilEnable;
    UINT8 StencilReadMask;
    UINT8 StencilWriteMask;
    D3D11_DEPTH_STENCILOP_DESC FrontFace;
    D3D11_DEPTH_STENCILOP_DESC BackFace;
};

struct D3D11_RENDER_TARGET_BLEND_DESC
{
    MtlBOOL BlendEnable;
    D3D11_BLEND SrcBlend;
    D3D11_BLEND DestBlend;
    D3D11_BLEND_OP BlendOp;
    D3D11_BLEND SrcBlendAlpha;
    D3D11_BLEND DestBlendAlpha;
    D3D11_BLEND_OP BlendOpAlpha;
    UINT8 RenderTargetWriteMask;
};

struct D3D11_BLEND_DESC
{
    MtlBOOL AlphaToCoverageEnable;
    MtlBOOL IndependentBlendEnable;
    D3D11_RENDER_TARGET_BLEND_DESC RenderTarget[8];
};

struct D3D11_RASTERIZER_DESC
{
    D3D11_FILL_MODE FillMode;
    D3D11_CULL_MODE CullMode;
    MtlBOOL FrontCounterClockwise;
    INT DepthBias;
    FLOAT DepthBiasClamp;
    FLOAT SlopeScaledDepthBias;
    MtlBOOL DepthClipEnable;
    MtlBOOL ScissorEnable;
    MtlBOOL MultisampleEnable;
    MtlBOOL AntialiasedLineEnable;
};

struct D3D11_INPUT_ELEMENT_DESC
{
    LPCSTR SemanticName;
    UINT SemanticIndex;
    DXGI_FORMAT Format;
    UINT InputSlot;
    UINT AlignedByteOffset;
    D3D11_INPUT_CLASSIFICATION InputSlotClass;
    UINT InstanceDataStepRate;
};

struct D3D11_QUERY_DESC
{
    D3D11_QUERY Query;
    UINT MiscFlags;
};

// ---------------------------------------------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------------------------------------------
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ClassInstance;
struct ID3D11ClassLinkage;

struct ID3D11DeviceChild : IUnknown
{
    virtual void GetDevice(ID3D11Device** ppDevice) = 0;
};

struct ID3D11Resource : ID3D11DeviceChild
{
    virtual void GetType(D3D11_RESOURCE_DIMENSION* pResourceDimension) = 0;
    virtual void SetEvictionPriority(UINT EvictionPriority) = 0;
    virtual UINT GetEvictionPriority() = 0;
};

struct ID3D11Buffer : ID3D11Resource
{
    virtual void GetDesc(D3D11_BUFFER_DESC* pDesc) = 0;
};

struct ID3D11Texture1D : ID3D11Resource
{
    virtual void GetDesc(D3D11_TEXTURE1D_DESC* pDesc) = 0;
};

struct ID3D11Texture2D : ID3D11Resource
{
    virtual void GetDesc(D3D11_TEXTURE2D_DESC* pDesc) = 0;
};

struct ID3D11Texture3D : ID3D11Resource
{
    virtual void GetDesc(D3D11_TEXTURE3D_DESC* pDesc) = 0;
};

struct ID3D11View : ID3D11DeviceChild
{
    virtual void GetResource(ID3D11Resource** ppResource) = 0;
};

struct ID3D11ShaderResourceView : ID3D11View
{
    virtual void GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc) = 0;
};

struct ID3D11RenderTargetView : ID3D11View
{
    virtual void GetDesc(D3D11_RENDER_TARGET_VIEW_DESC* pDesc) = 0;
};

struct ID3D11DepthStencilView : ID3D11View
{
    virtual void GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc) = 0;
};

struct ID3D11UnorderedAccessView : ID3D11View
{
    virtual void GetDesc(D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc) = 0;
};

struct ID3D11SamplerState : ID3D11DeviceChild
{
    virtual void GetDesc(D3D11_SAMPLER_DESC* pDesc) = 0;
};

struct ID3D11DepthStencilState : ID3D11DeviceChild
{
    virtual void GetDesc(D3D11_DEPTH_STENCIL_DESC* pDesc) = 0;
};

struct ID3D11BlendState : ID3D11DeviceChild
{
    virtual void GetDesc(D3D11_BLEND_DESC* pDesc) = 0;
};

struct ID3D11RasterizerState : ID3D11DeviceChild
{
    virtual void GetDesc(D3D11_RASTERIZER_DESC* pDesc) = 0;
};

struct ID3D11InputLayout : ID3D11DeviceChild
{
};

struct ID3D11VertexShader : ID3D11DeviceChild
{
};
struct ID3D11PixelShader : ID3D11DeviceChild
{
};
struct ID3D11GeometryShader : ID3D11DeviceChild
{
};
struct ID3D11HullShader : ID3D11DeviceChild
{
};
struct ID3D11DomainShader : ID3D11DeviceChild
{
};
struct ID3D11ComputeShader : ID3D11DeviceChild
{
};

struct ID3D11Asynchronous : ID3D11DeviceChild
{
    virtual UINT GetDataSize() = 0;
};

struct ID3D11Query : ID3D11Asynchronous
{
    virtual void GetDesc(D3D11_QUERY_DESC* pDesc) = 0;
};

struct ID3DUserDefinedAnnotation : IUnknown
{
    virtual INT BeginEvent(LPCWSTR Name) = 0;
    virtual INT EndEvent() = 0;
    virtual void SetMarker(LPCWSTR Name) = 0;
    virtual MtlBOOL GetStatus() = 0;
};

struct ID3D11DeviceContext : ID3D11DeviceChild
{
    // Input assembler
    virtual void IASetInputLayout(ID3D11InputLayout* pInputLayout) = 0;
    virtual void IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVertexBuffers,
                                    const UINT* pStrides, const UINT* pOffsets) = 0;
    virtual void IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset) = 0;
    virtual void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) = 0;

    // Shader stages
    virtual void VSSetShader(ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances,
                             UINT NumClassInstances) = 0;
    virtual void VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) = 0;
    virtual void VSSetShaderResources(UINT StartSlot, UINT NumViews,
                                      ID3D11ShaderResourceView* const* ppShaderResourceViews) = 0;
    virtual void VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) = 0;

    virtual void PSSetShader(ID3D11PixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances,
                             UINT NumClassInstances) = 0;
    virtual void PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) = 0;
    virtual void PSSetShaderResources(UINT StartSlot, UINT NumViews,
                                      ID3D11ShaderResourceView* const* ppShaderResourceViews) = 0;
    virtual void PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) = 0;

    virtual void GSSetShader(ID3D11GeometryShader* pShader, ID3D11ClassInstance* const* ppClassInstances,
                             UINT NumClassInstances) = 0;
    virtual void GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) = 0;
    virtual void GSSetShaderResources(UINT StartSlot, UINT NumViews,
                                      ID3D11ShaderResourceView* const* ppShaderResourceViews) = 0;
    virtual void GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) = 0;

    virtual void HSSetShader(ID3D11HullShader* pHullShader, ID3D11ClassInstance* const* ppClassInstances,
                             UINT NumClassInstances) = 0;
    virtual void HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) = 0;
    virtual void HSSetShaderResources(UINT StartSlot, UINT NumViews,
                                      ID3D11ShaderResourceView* const* ppShaderResourceViews) = 0;
    virtual void HSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) = 0;

    virtual void DSSetShader(ID3D11DomainShader* pDomainShader, ID3D11ClassInstance* const* ppClassInstances,
                             UINT NumClassInstances) = 0;
    virtual void DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) = 0;
    virtual void DSSetShaderResources(UINT StartSlot, UINT NumViews,
                                      ID3D11ShaderResourceView* const* ppShaderResourceViews) = 0;
    virtual void DSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) = 0;

    virtual void CSSetShader(ID3D11ComputeShader* pComputeShader, ID3D11ClassInstance* const* ppClassInstances,
                             UINT NumClassInstances) = 0;
    virtual void CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppConstantBuffers) = 0;
    virtual void CSSetShaderResources(UINT StartSlot, UINT NumViews,
                                      ID3D11ShaderResourceView* const* ppShaderResourceViews) = 0;
    virtual void CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers) = 0;
    virtual void CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs,
                                           ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
                                           const UINT* pUAVInitialCounts) = 0;

    // Rasterizer, output merger
    virtual void RSSetState(ID3D11RasterizerState* pRasterizerState) = 0;
    virtual void RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) = 0;
    virtual void RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) = 0;
    virtual void OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView* const* ppRenderTargetViews,
                                    ID3D11DepthStencilView* pDepthStencilView) = 0;
    virtual void OMGetRenderTargets(UINT NumViews, ID3D11RenderTargetView** ppRenderTargetViews,
                                    ID3D11DepthStencilView** ppDepthStencilView) = 0;
    virtual void OMSetBlendState(ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) = 0;
    virtual void OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState, UINT StencilRef) = 0;

    // Draw / dispatch
    virtual void Draw(UINT VertexCount, UINT StartVertexLocation) = 0;
    virtual void DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) = 0;
    virtual void DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation,
                               UINT StartInstanceLocation) = 0;
    virtual void DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
                                      INT BaseVertexLocation, UINT StartInstanceLocation) = 0;
    virtual void Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) = 0;

    // Resources
    virtual HRESULT Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
                        D3D11_MAPPED_SUBRESOURCE* pMappedResource) = 0;
    virtual void Unmap(ID3D11Resource* pResource, UINT Subresource) = 0;
    virtual void UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox,
                                   const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) = 0;
    virtual void CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY,
                                       UINT DstZ, ID3D11Resource* pSrcResource, UINT SrcSubresource,
                                       const D3D11_BOX* pSrcBox) = 0;
    virtual void CopyResource(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource) = 0;
    virtual void ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource, ID3D11Resource* pSrcResource,
                                    UINT SrcSubresource, DXGI_FORMAT Format) = 0;
    virtual void GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) = 0;

    virtual void ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]) = 0;
    virtual void ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth,
                                       UINT8 Stencil) = 0;
    virtual void ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* pUnorderedAccessView,
                                              const UINT Values[4]) = 0;
    virtual void ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView* pUnorderedAccessView,
                                               const FLOAT Values[4]) = 0;

    // Queries
    virtual void Begin(ID3D11Asynchronous* pAsync) = 0;
    virtual void End(ID3D11Asynchronous* pAsync) = 0;
    virtual HRESULT GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize, UINT GetDataFlags) = 0;

    virtual void ClearState() = 0;
    virtual void Flush() = 0;
};

struct ID3D11DeviceContext1 : ID3D11DeviceContext
{
};

struct ID3D11Device : IUnknown
{
    virtual HRESULT CreateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData,
                                 ID3D11Buffer** ppBuffer) = 0;
    virtual HRESULT CreateTexture1D(const D3D11_TEXTURE1D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData,
                                    ID3D11Texture1D** ppTexture1D) = 0;
    virtual HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData,
                                    ID3D11Texture2D** ppTexture2D) = 0;
    virtual HRESULT CreateTexture3D(const D3D11_TEXTURE3D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData,
                                    ID3D11Texture3D** ppTexture3D) = 0;
    virtual HRESULT CreateShaderResourceView(ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc,
                                             ID3D11ShaderResourceView** ppSRView) = 0;
    virtual HRESULT CreateUnorderedAccessView(ID3D11Resource* pResource,
                                              const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc,
                                              ID3D11UnorderedAccessView** ppUAView) = 0;
    virtual HRESULT CreateRenderTargetView(ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc,
                                           ID3D11RenderTargetView** ppRTView) = 0;
    virtual HRESULT CreateDepthStencilView(ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                           ID3D11DepthStencilView** ppDepthStencilView) = 0;
    virtual HRESULT CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements,
                                      const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength,
                                      ID3D11InputLayout** ppInputLayout) = 0;
    virtual HRESULT CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                       ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader) = 0;
    virtual HRESULT CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                         ID3D11ClassLinkage* pClassLinkage,
                                         ID3D11GeometryShader** ppGeometryShader) = 0;
    virtual HRESULT CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                      ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader) = 0;
    virtual HRESULT CreateHullShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                     ID3D11ClassLinkage* pClassLinkage, ID3D11HullShader** ppHullShader) = 0;
    virtual HRESULT CreateDomainShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                       ID3D11ClassLinkage* pClassLinkage, ID3D11DomainShader** ppDomainShader) = 0;
    virtual HRESULT CreateComputeShader(const void* pShaderBytecode, SIZE_T BytecodeLength,
                                        ID3D11ClassLinkage* pClassLinkage, ID3D11ComputeShader** ppComputeShader) = 0;
    virtual HRESULT CreateBlendState(const D3D11_BLEND_DESC* pBlendStateDesc, ID3D11BlendState** ppBlendState) = 0;
    virtual HRESULT CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc,
                                            ID3D11DepthStencilState** ppDepthStencilState) = 0;
    virtual HRESULT CreateRasterizerState(const D3D11_RASTERIZER_DESC* pRasterizerDesc,
                                          ID3D11RasterizerState** ppRasterizerState) = 0;
    virtual HRESULT CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc,
                                       ID3D11SamplerState** ppSamplerState) = 0;
    virtual HRESULT CreateQuery(const D3D11_QUERY_DESC* pQueryDesc, ID3D11Query** ppQuery) = 0;
    virtual HRESULT CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) = 0;
    virtual HRESULT CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount, UINT* pNumQualityLevels) = 0;
    virtual UINT GetCreationFlags() = 0;
    virtual HRESULT GetDeviceRemovedReason() = 0;
    virtual void GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) = 0;
    virtual D3D_FEATURE_LEVEL GetFeatureLevel() = 0;
};

struct ID3D11Device1 : ID3D11Device
{
    virtual void GetImmediateContext1(ID3D11DeviceContext1** ppImmediateContext) = 0;
};

MTL_DECLARE_IID(ID3D11DeviceChild, 0x1841e5c8, 0x16b0, 0x489b, 0xbc, 0xc8, 0x44, 0xcf, 0xb0, 0xd5, 0xde, 0xae)
MTL_DECLARE_IID(ID3D11Resource, 0xdc8e63f3, 0xd12b, 0x4952, 0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d)
MTL_DECLARE_IID(ID3D11Buffer, 0x48570b85, 0xd1ee, 0x4fcd, 0xa2, 0x50, 0xeb, 0x35, 0x07, 0x22, 0xb0, 0x37)
MTL_DECLARE_IID(ID3D11Texture1D, 0xf8fb5c27, 0xc6b3, 0x4f75, 0xa4, 0xc8, 0x43, 0x9a, 0xf2, 0xef, 0x56, 0x4c)
MTL_DECLARE_IID(ID3D11Texture2D, 0x6f15aaf2, 0xd208, 0x4e89, 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c)
MTL_DECLARE_IID(ID3D11Texture3D, 0x037e866e, 0xf56d, 0x4357, 0xa8, 0xaf, 0x9d, 0xab, 0xbe, 0x6e, 0x25, 0x0e)
MTL_DECLARE_IID(ID3D11Device, 0xdb6f6ddb, 0xac77, 0x4e88, 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40)
MTL_DECLARE_IID(ID3D11Device1, 0xa04bfb29, 0x08ef, 0x43d6, 0xa4, 0x9c, 0xa9, 0xbd, 0xbd, 0xcb, 0xe6, 0x86)
MTL_DECLARE_IID(ID3D11DeviceContext, 0xc0bfa96c, 0xe089, 0x44fb, 0x8e, 0xaf, 0x26, 0xf8, 0x79, 0x61, 0x90, 0xda)
MTL_DECLARE_IID(ID3D11DeviceContext1, 0xbb2c6faa, 0xb5fb, 0x4082, 0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1)
MTL_DECLARE_IID(ID3DUserDefinedAnnotation, 0xb2daad8b, 0x03d4, 0x4dbf, 0x95, 0xeb, 0x32, 0xab, 0x4b, 0x63, 0xd0, 0xab)

// ---------------------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------------------
// Creates the Metal device and immediate context. Adapter, software module and flags are ignored apart from
// D3D11_CREATE_DEVICE_DEBUG (turns on extra shim validation). The feature level reported is 11_0.
HRESULT D3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags,
                          const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
                          ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel,
                          ID3D11DeviceContext** ppImmediateContext);
