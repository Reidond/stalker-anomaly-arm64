// D3DCompiler / shader reflection subset. In the Metal build there is no DXBC: "bytecode" is the container
// described in mtl_shader.h (MSL or metallib plus reflection). D3DReflect and D3DGetInputSignatureBlob read
// that container, so R4's constant-table parsing and input layout creation keep working unchanged.
#pragma once

#include "d3d11_metal.h"

#ifndef __d3d11shader_h__
#define __d3d11shader_h__
#endif
#ifndef __d3dcompiler_h__
#define __d3dcompiler_h__
#endif

// ---------------------------------------------------------------------------------------------------------
// Blob / include
// ---------------------------------------------------------------------------------------------------------
struct ID3D10Blob : IUnknown
{
    virtual void* GetBufferPointer() = 0;
    virtual SIZE_T GetBufferSize() = 0;
};
MTL_DECLARE_IID(ID3D10Blob, 0x8ba5fb08, 0x5195, 0x40e2, 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02)
typedef ID3D10Blob* LPD3D10BLOB;
typedef ID3D10Blob* LPD3DBLOB;

enum D3D_INCLUDE_TYPE
{
    D3D_INCLUDE_LOCAL = 0,
    D3D_INCLUDE_SYSTEM = 1
};
typedef D3D_INCLUDE_TYPE D3D10_INCLUDE_TYPE;
#define D3D10_INCLUDE_LOCAL D3D_INCLUDE_LOCAL
#define D3D10_INCLUDE_SYSTEM D3D_INCLUDE_SYSTEM

struct ID3D10Include
{
    virtual HRESULT Open(D3D10_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData,
                         UINT* pBytes) = 0;
    virtual HRESULT Close(LPCVOID pData) = 0;
};

struct D3D10_SHADER_MACRO
{
    LPCSTR Name;
    LPCSTR Definition;
};

// Compile flags (subset)
#define D3D10_SHADER_DEBUG (1 << 0)
#define D3D10_SHADER_SKIP_VALIDATION (1 << 1)
#define D3D10_SHADER_SKIP_OPTIMIZATION (1 << 2)
#define D3D10_SHADER_PACK_MATRIX_ROW_MAJOR (1 << 3)
#define D3D10_SHADER_PACK_MATRIX_COLUMN_MAJOR (1 << 4)
#define D3D10_SHADER_AVOID_FLOW_CONTROL (1 << 9)
#define D3D10_SHADER_ENABLE_STRICTNESS (1 << 11)
#define D3D10_SHADER_OPTIMIZATION_LEVEL3 (1 << 15)
#define D3DCOMPILE_DEBUG D3D10_SHADER_DEBUG
#define D3DCOMPILE_PACK_MATRIX_ROW_MAJOR D3D10_SHADER_PACK_MATRIX_ROW_MAJOR
#define D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR D3D10_SHADER_PACK_MATRIX_COLUMN_MAJOR

// ---------------------------------------------------------------------------------------------------------
// Reflection enums and descriptors (values match d3dcommon.h / d3d11shader.h)
// ---------------------------------------------------------------------------------------------------------
enum D3D_SHADER_VARIABLE_CLASS
{
    D3D_SVC_SCALAR = 0,
    D3D_SVC_VECTOR = 1,
    D3D_SVC_MATRIX_ROWS = 2,
    D3D_SVC_MATRIX_COLUMNS = 3,
    D3D_SVC_OBJECT = 4,
    D3D_SVC_STRUCT = 5,
    D3D_SVC_INTERFACE_CLASS = 6,
    D3D_SVC_INTERFACE_POINTER = 7
};

enum D3D_SHADER_VARIABLE_TYPE
{
    D3D_SVT_VOID = 0,
    D3D_SVT_BOOL = 1,
    D3D_SVT_INT = 2,
    D3D_SVT_FLOAT = 3,
    D3D_SVT_STRING = 4,
    D3D_SVT_TEXTURE = 5,
    D3D_SVT_TEXTURE1D = 6,
    D3D_SVT_TEXTURE2D = 7,
    D3D_SVT_TEXTURE3D = 8,
    D3D_SVT_TEXTURECUBE = 9,
    D3D_SVT_SAMPLER = 10,
    D3D_SVT_UINT = 19,
    D3D_SVT_UINT8 = 20
};

enum D3D_SHADER_INPUT_TYPE
{
    D3D_SIT_CBUFFER = 0,
    D3D_SIT_TBUFFER = 1,
    D3D_SIT_TEXTURE = 2,
    D3D_SIT_SAMPLER = 3,
    D3D_SIT_UAV_RWTYPED = 4,
    D3D_SIT_STRUCTURED = 5,
    D3D_SIT_UAV_RWSTRUCTURED = 6,
    D3D_SIT_BYTEADDRESS = 7,
    D3D_SIT_UAV_RWBYTEADDRESS = 8,
    D3D_SIT_UAV_APPEND_STRUCTURED = 9,
    D3D_SIT_UAV_CONSUME_STRUCTURED = 10,
    D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER = 11
};

enum D3D_CBUFFER_TYPE
{
    D3D_CT_CBUFFER = 0,
    D3D_CT_TBUFFER = 1,
    D3D_CT_INTERFACE_POINTERS = 2,
    D3D_CT_RESOURCE_BIND_INFO = 3
};

enum D3D_RESOURCE_RETURN_TYPE
{
    D3D_RETURN_TYPE_UNORM = 1,
    D3D_RETURN_TYPE_SNORM = 2,
    D3D_RETURN_TYPE_SINT = 3,
    D3D_RETURN_TYPE_UINT = 4,
    D3D_RETURN_TYPE_FLOAT = 5,
    D3D_RETURN_TYPE_MIXED = 6
};

enum D3D_REGISTER_COMPONENT_TYPE
{
    D3D_REGISTER_COMPONENT_UNKNOWN = 0,
    D3D_REGISTER_COMPONENT_UINT32 = 1,
    D3D_REGISTER_COMPONENT_SINT32 = 2,
    D3D_REGISTER_COMPONENT_FLOAT32 = 3
};

#define D3D10_SVC_SCALAR D3D_SVC_SCALAR
#define D3D10_SVC_VECTOR D3D_SVC_VECTOR
#define D3D10_SVC_MATRIX_ROWS D3D_SVC_MATRIX_ROWS
#define D3D10_SVC_MATRIX_COLUMNS D3D_SVC_MATRIX_COLUMNS
#define D3D10_SVC_OBJECT D3D_SVC_OBJECT
#define D3D10_SVC_STRUCT D3D_SVC_STRUCT
#define D3D10_SVT_BOOL D3D_SVT_BOOL
#define D3D10_SVT_INT D3D_SVT_INT
#define D3D10_SVT_FLOAT D3D_SVT_FLOAT
#define D3D10_SVT_UINT D3D_SVT_UINT
#define D3D10_SIT_CBUFFER D3D_SIT_CBUFFER
#define D3D10_SIT_TEXTURE D3D_SIT_TEXTURE
#define D3D10_SIT_SAMPLER D3D_SIT_SAMPLER
#define D3D11_SIT_UAV_RWTYPED D3D_SIT_UAV_RWTYPED
typedef D3D_CBUFFER_TYPE D3D11_CBUFFER_TYPE;

struct D3D11_SHADER_DESC
{
    UINT Version;
    LPCSTR Creator;
    UINT Flags;
    UINT ConstantBuffers;
    UINT BoundResources;
    UINT InputParameters;
    UINT OutputParameters;
    UINT InstructionCount;
    UINT TempRegisterCount;
    UINT TempArrayCount;
    UINT DefCount;
    UINT DclCount;
    UINT TextureNormalInstructions;
    UINT TextureLoadInstructions;
    UINT TextureCompInstructions;
    UINT TextureBiasInstructions;
    UINT TextureGradientInstructions;
    UINT FloatInstructionCount;
    UINT IntInstructionCount;
    UINT UintInstructionCount;
    UINT StaticFlowControlCount;
    UINT DynamicFlowControlCount;
    UINT MacroInstructionCount;
    UINT ArrayInstructionCount;
    UINT CutInstructionCount;
    UINT EmitInstructionCount;
    UINT GSOutputTopology;
    UINT GSMaxOutputVertexCount;
    UINT InputPrimitive;
    UINT PatchConstantParameters;
    UINT cGSInstanceCount;
    UINT cControlPoints;
    UINT HSOutputPrimitive;
    UINT HSPartitioning;
    UINT TessellatorDomain;
    UINT cBarrierInstructions;
    UINT cInterlockedInstructions;
    UINT cTextureStoreInstructions;
};

struct D3D11_SHADER_BUFFER_DESC
{
    LPCSTR Name;
    D3D_CBUFFER_TYPE Type;
    UINT Variables;
    UINT Size;
    UINT uFlags;
};

struct D3D11_SHADER_VARIABLE_DESC
{
    LPCSTR Name;
    UINT StartOffset;
    UINT Size;
    UINT uFlags;
    LPVOID DefaultValue;
    UINT StartTexture;
    UINT TextureSize;
    UINT StartSampler;
    UINT SamplerSize;
};

struct D3D11_SHADER_TYPE_DESC
{
    D3D_SHADER_VARIABLE_CLASS Class;
    D3D_SHADER_VARIABLE_TYPE Type;
    UINT Rows;
    UINT Columns;
    UINT Elements;
    UINT Members;
    UINT Offset;
    LPCSTR Name;
};

struct D3D11_SHADER_INPUT_BIND_DESC
{
    LPCSTR Name;
    D3D_SHADER_INPUT_TYPE Type;
    UINT BindPoint;
    UINT BindCount;
    UINT uFlags;
    D3D_RESOURCE_RETURN_TYPE ReturnType;
    D3D_SRV_DIMENSION Dimension;
    UINT NumSamples;
};

struct D3D11_SIGNATURE_PARAMETER_DESC
{
    LPCSTR SemanticName;
    UINT SemanticIndex;
    UINT Register;
    UINT SystemValueType;
    D3D_REGISTER_COMPONENT_TYPE ComponentType;
    UINT8 Mask;
    UINT8 ReadWriteMask;
    UINT Stream;
    UINT MinPrecision;
};

struct ID3D11ShaderReflectionType
{
    virtual HRESULT GetDesc(D3D11_SHADER_TYPE_DESC* pDesc) = 0;
    virtual ID3D11ShaderReflectionType* GetMemberTypeByIndex(UINT Index) = 0;
    virtual LPCSTR GetMemberTypeName(UINT Index) = 0;
};

struct ID3D11ShaderReflectionVariable
{
    virtual HRESULT GetDesc(D3D11_SHADER_VARIABLE_DESC* pDesc) = 0;
    virtual ID3D11ShaderReflectionType* GetType() = 0;
};

struct ID3D11ShaderReflectionConstantBuffer
{
    virtual HRESULT GetDesc(D3D11_SHADER_BUFFER_DESC* pDesc) = 0;
    virtual ID3D11ShaderReflectionVariable* GetVariableByIndex(UINT Index) = 0;
    virtual ID3D11ShaderReflectionVariable* GetVariableByName(LPCSTR Name) = 0;
};

struct ID3D11ShaderReflection : IUnknown
{
    virtual HRESULT GetDesc(D3D11_SHADER_DESC* pDesc) = 0;
    virtual ID3D11ShaderReflectionConstantBuffer* GetConstantBufferByIndex(UINT Index) = 0;
    virtual ID3D11ShaderReflectionConstantBuffer* GetConstantBufferByName(LPCSTR Name) = 0;
    virtual HRESULT GetResourceBindingDesc(UINT ResourceIndex, D3D11_SHADER_INPUT_BIND_DESC* pDesc) = 0;
    virtual HRESULT GetInputParameterDesc(UINT ParameterIndex, D3D11_SIGNATURE_PARAMETER_DESC* pDesc) = 0;
    virtual HRESULT GetOutputParameterDesc(UINT ParameterIndex, D3D11_SIGNATURE_PARAMETER_DESC* pDesc) = 0;
    virtual HRESULT GetThreadGroupSize(UINT* pSizeX, UINT* pSizeY, UINT* pSizeZ) = 0;
};
MTL_DECLARE_IID(ID3D11ShaderReflection, 0x8d536ca1, 0x0cca, 0x4956, 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84)
#define IID_ID3D11ShaderReflection mtl_uuidof<ID3D11ShaderReflection>()

// ---------------------------------------------------------------------------------------------------------
// D3DCompiler entry points
// ---------------------------------------------------------------------------------------------------------
// D3DCompile hands the HLSL to the compile hook installed with MtlSetShaderCompiler (see mtl_shader.h); the
// hook returns the container. Without a hook it fails with E_NOTIMPL.
HRESULT D3DCompile(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName, const D3D10_SHADER_MACRO* pDefines,
                   ID3D10Include* pInclude, LPCSTR pEntrypoint, LPCSTR pTarget, UINT Flags1, UINT Flags2,
                   ID3D10Blob** ppCode, ID3D10Blob** ppErrorMsgs);
// Parses the container header; ppReflector gets an ID3D11ShaderReflection.
HRESULT D3DReflect(LPCVOID pSrcData, SIZE_T SrcDataSize, MtlREFIID pInterface, void** ppReflector);
// Returns a blob holding only the vertex-input part of the container.
HRESULT D3DGetInputSignatureBlob(LPCVOID pSrcData, SIZE_T SrcDataSize, ID3D10Blob** ppSignatureBlob);
// Writes the MSL source (or a note for precompiled libraries) into a blob.
HRESULT D3DDisassemble(LPCVOID pSrcData, SIZE_T SrcDataSize, UINT Flags, LPCSTR szComments, ID3D10Blob** ppDisassembly);
