# D3D11 / DXGI surface used by the R4 renderer

What the Metal shim in `src/Platform/MetalD3D11` has to provide so the R4 (DX11) renderer can run
unchanged. Everything below is measured on the sources, not guessed; the scanner is committed in
`tools/d3d11_surface_scan/` and regenerates the appendix tables:

```bash
python3 -I tools/d3d11_surface_scan/scan.py "$PWD" > tables.md
```

## 1. Scope and method

- **Roots:** the 225 `.cpp` files in `xrRender_R4.vcxproj` that live in `src/Layers/xrRender`,
  `xrRenderDX10` and `xrRenderPC_R4`, plus every header they reach through `#include` (400 files).
- **Configuration:** `USE_DX11` defined, release flags from `build_config_defines.h`, `NDEBUG`.
  A small preprocessor evaluates `#if/#ifdef/#elif/#else`, so DX9, DX10 and `#if 0` regions are
  not counted. A second pass with `DEBUG` finds debug-only identifiers: only
  `D3DPERF_BeginEvent`/`EndEvent`, which the shim does not need.
- **Counts are call sites in source**, not runtime frequency. Strings and comments are removed.
  The engine's `D3D_*` / `ID3D*` aliases (`xrRenderDX10/DXCommonTypes.h`) are folded into the real
  D3D11 names, and `DXCommonTypes.h` itself is not counted.
- **Not covered:** `xrEngine` (it only sees the renderer through the `IRender` interfaces and
  `HW`), the DX9/DX10/R1–R3 renderers, tools/editor code.

## 2. How R4 reaches D3D11

The D3D11 surface is small and concentrated. R4 does not scatter API calls; it funnels them
through a handful of layers. That shapes the shim design.

| Layer | Files | What it does with D3D11 |
|---|---|---|
| `CHW` (device) | `xrRender/HW.h`, `xrRenderDX10/dx10HW.cpp` | DXGI factory/adapter/output enumeration, `D3D11CreateDevice` (feature level 11_0), flip-model swap chain, colour space, `ResizeBuffers`/`ResizeTarget`/`SetFullscreenState`, back-buffer RTV and a 24/8 depth buffer. Owns `HW.pDevice`, `HW.pContext`, `HW.m_pSwapChain`, `HW.pBaseRT`, `HW.pBaseZB`, `HW.pAnnotation`. |
| `CBackend` (draw state) | `xrRenderDX10/dx10R_Backend_Runtime.h`, `xrRender/R_Backend*.{h,cpp}` | Every `*SetShader`, `IASet*`, `*SetConstantBuffers`, `OMSetRenderTargets`, `Draw`, `DrawIndexed`, `Dispatch`, `RSSetScissorRects` goes through here, each **once**. State is cached on the CPU and flushed right before a draw. |
| State manager + caches | `xrRenderDX10/StateManager/*` | `RSSetState`, `OMSetDepthStencilState`, `OMSetBlendState`, all `*SetSamplers`, all `*SetShaderResources`. Rasterizer/depth/blend/sampler objects are created through hash-keyed caches (`dx10StateCache`, `dx10SamplerStateCache`), so the engine already de-duplicates state objects. |
| Resource manager | `dx10ResourceManager_*.cpp`, `dx10SH_Texture.cpp`, `dx10SH_RT.cpp`, `dx10Texture.cpp`, `dx10ConstantBuffer.cpp`, `dx10BufferUtils.cpp` | Textures (DDS via D3DX11), render targets, constant buffers, input layouts, shader creation and reflection. |
| Dynamic geometry | `xrRender/R_DStreams.cpp`, `R_Backend.cpp` | One big dynamic VB and IB (`Map` with `DISCARD`/`NO_OVERWRITE`), a static quad index buffer. |
| Render phases | `xrRenderPC_R4/r4_rendertarget_*.cpp`, `xrRender/rendertarget_phase_*.cpp` | Direct `ClearRenderTargetView` (47), `CopyResource` (36), `ClearDepthStencilView`, `ResolveSubresource`, `RSSetViewports`, plus the compute phases. |

Facts that matter for a Metal backend:

1. **One immediate context, no deferred contexts, no command lists, no predication, no stream
   output, no indirect draws, no hardware instancing.** `DrawInstanced`, `DrawIndexedInstanced`,
   `DrawAuto`, `UpdateSubresource`, `GenerateMips`, `ClearUnorderedAccessView*` and
   `CheckMultisampleQualityLevels` have no call sites at all.
2. **Draws are two functions** (`CBackend::Render` ×2) **and one dispatch** (`Compute`). Right
   before the draw they apply SRVs, RT/ZB, the input layout, rasterizer/depth/blend state and
   flush dirty constant buffers. All state is therefore known at one point, which is where the
   shim builds pipeline state.
3. **Render-target binding is lazy and padded.** `set_RT`/`set_ZB` record the target and call
   `OMSetRenderTargets(0,0,0)` once (to unbind targets that may be sampled next). The real bind is
   `OMSetRenderTargets(8, pRT, pZB)` at draw time, always 8 slots with nulls. The same target set
   is bound again and again; the shim must not turn each of those into a new Metal pass.
4. **Clears happen right after the bind.** 47 `ClearRenderTargetView` + 6 `ClearDepthStencilView`
   directly follow a target switch, so they can usually be folded into the pass load action.
5. **Vertex input is one stream.** `IASetVertexBuffers(0, 1, ...)`, 16-bit indices at offset 0
   (`IASetIndexBuffer(ib, R16_UINT, 0)`), topology from `{point, line list/strip, triangle
   list/strip}` (triangle fans are dropped by the engine; patch lists only with tessellation).
6. **Constant buffers:** one `DYNAMIC` buffer per reflected cbuffer (`dx10ConstantBuffer`),
   refilled by `Map(WRITE_DISCARD)` + `memcpy` + `Unmap` on every flush when dirty. Sizes come from
   reflection (multiples of 16 bytes, up to 64 KB). Slots are 0..13 per stage (`MaxCBuffers = 14`).
7. **Shaders come from runtime `D3DCompile`** (SM 4.0/4.1/5.0 profiles). The bytecode is used
   three ways: `CreateXShader(bytecode)`, `D3DReflect(bytecode)` (constant-table parsing) and
   `D3DGetInputSignatureBlob(bytecode)` (input layout creation). The engine also keeps a disk
   cache of that bytecode. Any replacement for the bytecode must answer all three.
8. **Input layouts** are created per (vertex declaration × VS signature) and cached in the
   declaration (`decl->vs_to_layout`). The layout is created *before* the shader is known to be
   bound, so semantics must be resolved against the VS at pipeline-build time.
9. **SRV/sampler/UAV slots are fixed per stage** using the D3D register number
   (`CTexture::rstPixel/rstVertex/...` offsets are added by the engine, not the API).
10. **D3D9 enums leak into the D3D11 build.** The shared blender code speaks `D3DRS_*`, `D3DBLEND_*`,
    `D3DCMP_*`, `D3DPT_*`, `D3DFMT_*`, `D3DDECL*`; the DX10 wrapper converts them to D3D11 descs
    (`dx10StateUtils.cpp`, `dx10TextureUtils.cpp`, `dx10BufferUtils.cpp`). See §7.

## 3. Interface and method coverage

### 3.1 Interfaces (all in the appendix, Interfaces table)

Used by R4: `ID3D11Device(1)`, `ID3D11DeviceContext(1)`, `ID3D11Buffer`, `Texture1D/2D/3D`,
`Resource`, `ShaderResourceView`, `RenderTargetView`, `DepthStencilView`,
`UnorderedAccessView`, `SamplerState`, `BlendState`, `DepthStencilState`, `RasterizerState`,
`InputLayout`, `VertexShader`, `PixelShader`, `GeometryShader`, `HullShader`, `DomainShader`,
`ComputeShader`, `Query`, `ID3DUserDefinedAnnotation`, `ID3D11ShaderReflection` (+ constant
buffer / variable / type), `ID3D10Blob`, `ID3D10Include`, and from DXGI `IDXGIFactory2/5`,
`IDXGIAdapter(1)`, `IDXGIOutput`, `IDXGISwapChain1/3`.

Not used: `ID3D11CommandList`, `ID3D11Counter`, `ID3D11Predicate`, `ID3D11ClassLinkage` /
`ClassInstance`, `ID3D11DeviceChild::SetPrivateData`, `ID3D11Debug`/`InfoQueue`, `IDXGIDevice`,
`IDXGISurface`, `IDXGIKeyedMutex`.

`QueryInterface` is used to upcast (`ID3D11Device` → `ID3D11Device1`, context → `Context1` and
`ID3DUserDefinedAnnotation`, swap chain → `IDXGISwapChain3`, factory → `IDXGIFactory5`). The code
uses MSVC conveniences (`__uuidof`, `IID_PPV_ARGS`, the templated `QueryInterface(T**)`); the shim
header has to supply them.

### 3.2 Device methods (53 call sites, 20 methods)

Hot: `CreateTexture2D` 14, `CreateShaderResourceView` 7, `CreateBuffer` 5, `CreateRenderTargetView` 4,
`CreateTexture3D` 4, `CreateQuery` 3. One call each: the three state-object creators, `CreateSamplerState`
(2), `CreateInputLayout`, the six shader creators, `CreateDepthStencilView` (2),
`CreateUnorderedAccessView`, `CreateTexture1D`. Never called: `CheckFormatSupport` (commented out),
`CheckMultisampleQualityLevels`, `GetImmediateContext`, `CreateDeferredContext`.

### 3.3 Context methods (173 call sites, 50 methods)

| Group | Methods (call sites) | Notes for Metal |
|---|---|---|
| Clears | `ClearRenderTargetView` 47, `ClearDepthStencilView` 6 | Fold into the next pass's load action; fall back to a clear draw only mid-pass. |
| Copies | `CopyResource` 36, `CopySubresourceRegion` 1, `ResolveSubresource` 2 | 36 whole-texture RT→RT copies per frame at most (post-process "grab" copies). Blit encoder; each one breaks the current render pass. |
| Mapping | `Map` 8 / `Unmap` 7 | Dynamic VB/IB streams, constant buffers (`DISCARD`), dynamic video textures (`DISCARD` on a 2D texture), screenshot read-back (`D3D_MAP_READ` on a staging texture). |
| Targets | `OMSetRenderTargets` 6 (3 files), `OMGetRenderTargets` 1 | 8 slots + DSV; `OMSetRenderTargets(0,0,0)` is the "unbind" idiom. |
| Fixed function state | `RSSetState`, `OMSetDepthStencilState` (with stencil ref), `OMSetBlendState` (with blend factor + sample mask), `RSSetViewports` 9, `RSSetScissorRects` 2 | One viewport, one scissor rect. `RSSetScissorRects(0, 0)` disables scissoring (the state object carries `ScissorEnable`). |
| Input assembler | `IASetVertexBuffers`, `IASetIndexBuffer`, `IASetInputLayout`, `IASetPrimitiveTopology` | 1 each; single stream. |
| Per-stage binding | `VS/PS/GS/HS/DS/CS` × `SetShader`, `SetConstantBuffers`, `SetSamplers`, `SetShaderResources` | The same code paths for all six stages; `CS` additionally `CSSetUnorderedAccessViews` (3). |
| Draw / dispatch | `DrawIndexed` 1, `Draw` 1, `Dispatch` 2 | No instancing, no indirect. |
| Queries | `Begin` 1, `End` 3, `GetData` 1 | `QueryHelper.h`; occlusion and event queries only. |
| Misc | `ClearState` 1 | Called on device reset. |

### 3.4 DXGI

`SetFullscreenState` 5, `ResizeBuffers` 4, `GetBuffer` 3 (back buffer as `ID3D11Texture2D`), `SetColorSpace1`
3, `Present` 2 (interval + `DXGI_PRESENT_ALLOW_TEARING` / `DXGI_PRESENT_TEST`), `ResizeTarget`,
`CheckColorSpaceSupport`, `GetContainingOutput`, factory `EnumAdapters1`/`CreateSwapChainForHwnd`/
`CheckFeatureSupport`, adapter `EnumOutputs`, output `GetDisplayModeList` 4,
`Get/SetGammaControl(+Capabilities)`. This is all in `CHW` and `xr_effgamma.cpp`; on Apple it is
replaced by SDL3 window/display code, so the shim only needs the swap chain object
(`GetBuffer`, `Present`, `ResizeBuffers`, colour space).

### 3.5 Reflection and compiler

`ID3D11ShaderReflection`: `GetDesc`, `GetConstantBufferByIndex`, `GetResourceBindingDesc`; constant
buffer: `GetDesc`, `GetVariableByIndex`; variable: `GetDesc`, `GetType`; type: `GetDesc`. The fields
read are `D3D11_SHADER_DESC.{ConstantBuffers, BoundResources}`, `SHADER_BUFFER_DESC.{Name, Type, Size,
Variables}`, `SHADER_VARIABLE_DESC.{Name, StartOffset, Size}`, `SHADER_TYPE_DESC.{Class, Type, Rows,
Columns}` (the whole type desc is also CRC'd and `memcmp`'d, so it must be stable plain data),
`SHADER_INPUT_BIND_DESC.{Name, Type, BindPoint, BindCount}`. Resource binding types seen:
`SIT_TEXTURE`, `SIT_SAMPLER`, `SIT_UAV_RWTYPED`; type classes `SCALAR`, `VECTOR`, `MATRIX_ROWS` (others are
fatal). Matrices must be row-major packed (`D3D10_SHADER_PACK_MATRIX_ROW_MAJOR` is passed to the
compiler).

Free functions: `D3DCompile`, `D3DReflect`, `D3DGetInputSignatureBlob`, `D3DDisassemble` (only with the
`disasm` debug option), `D3D11CreateDevice`, `CreateDXGIFactory2`.

### 3.6 D3DX11

Only texture plumbing, and it is on the hot path of every texture load:

- `D3DX11GetImageInfoFromMemory` (header of a DDS) and `D3DX11CreateTextureFromMemory` with
  `D3DX11_IMAGE_LOAD_INFO` (`FirstMipLevel`, `MipLevels`, `Width`/`Height` for LOD reduction, `Usage` =
  `IMMUTABLE` or `STAGING`, `BindFlags`, `CpuAccessFlags`) for **every DDS texture, cube map and
  volume**. The shim needs its own DDS loader that honours exactly these fields.
- `D3DX11SaveTextureToMemory`/`ToFile`, `D3DX11LoadTextureFromTexture` (screenshots, `r__screenshot.cpp`,
  DDS/PNG/JPG/BMP) and `D3DX11FilterTexture` (one call, `r4_rendertarget.cpp`: builds the mip chain of the jitter noise texture after `CreateTexture2D` with `MipLevels = 0`, i.e. "full chain").
  These are low priority.

## 4. Formats

DXGI formats referenced (44 in the appendix). What matters:

- **Colour/render targets:** `R8G8B8A8_UNORM` (most), `R16G16B16A16_FLOAT`, `R16G16_FLOAT`/`R16_FLOAT`,
  `R32_FLOAT`, `R32G32B32A32_FLOAT`, `R10G10B10A2_UNORM`, `R11G11B10_FLOAT`, `R8_UNORM`, `R16G16(B16A16)_UNORM`,
  `A8_UNORM`, `R8G8B8A8_SNORM`. Integer/typeless variants appear only in the format conversion table.
- **Depth/stencil:** `R24G8_TYPELESS` with `D24_UNORM_S8_UINT` DSV and `R24_UNORM_X8_TYPELESS`
  (depth) / `X24_TYPELESS_G8_UINT` (stencil) SRVs (main depth, MSAA depth), `R32_TYPELESS` with
  `D32_FLOAT` DSV and `R32_FLOAT` SRV (shadow maps). **Metal on Apple Silicon has no 24-bit depth**,
  so `D24S8` becomes `Depth32Float_Stencil8` (8 bytes per pixel).
- **Compressed:** `BC1_UNORM`, `BC5_UNORM`, `BC7_UNORM` named in the conversion table; DDS files add the rest
  of BC1–BC7. All are natively sampled on M-series.
- **Swap chain:** `R8G8B8A8_UNORM`, or `R10G10B10A2_UNORM` for HDR10 (`ps_r4_hdr10_on`).
- **Vertex formats** (from `D3DDECLTYPE_*` in `dx10BufferUtils.cpp`): `R32_FLOAT` … `R32G32B32A32_FLOAT`,
  `R8G8B8A8_UNORM` (colour), `R8G8B8A8_UINT` (`UBYTE4`), `R16G16(B16A16)_SINT/SNORM/UNORM`, `R16G16(B16A16)_FLOAT`.
  `POSITION`, `BLENDWEIGHT`, `BLENDINDICES`, `NORMAL`, `PSIZE`, `TEXCOORD`, `TANGENT`, `BINORMAL`, `POSITIONT`, `COLOR` are the only semantic names.

## 5. Resource and state descriptors

| Descriptor | Fields actually read or written by R4 |
|---|---|
| `D3D11_TEXTURE2D_DESC` | Width, Height, MipLevels, ArraySize, Format, SampleDesc.{Count, Quality}, Usage (`DEFAULT`, `DYNAMIC`, `IMMUTABLE`, `STAGING`), BindFlags (`SHADER_RESOURCE`, `RENDER_TARGET`, `DEPTH_STENCIL`, `UNORDERED_ACCESS`), CPUAccessFlags, MiscFlags (`TEXTURECUBE` only). `SampleDesc.Quality = D3D11_STANDARD_MULTISAMPLE_PATTERN` for MSAA RTs with the opt path. |
| `D3D11_TEXTURE3D_DESC` | 3D fluid grid, the material LUT (`t_material_surf`) |
| `D3D11_TEXTURE1D_DESC` | Fluid noise lookup only |
| `D3D11_BUFFER_DESC` | ByteWidth, Usage, BindFlags (`VERTEX/INDEX/CONSTANT_BUFFER`), CPUAccessFlags. No `StructureByteStride`, no `MiscFlags`, no raw/structured buffers. |
| `D3D11_SUBRESOURCE_DATA` | `pSysMem`, `SysMemPitch`, `SysMemSlicePitch` for initial data |
| `D3D11_MAPPED_SUBRESOURCE` | `pData`, `RowPitch`, `DepthPitch` |
| `D3D11_SHADER_RESOURCE_VIEW_DESC` | Format, ViewDimension (`TEXTURE2D`, `TEXTURE2DMS`, `TEXTURE3D`, `TEXTURECUBE`), `MipLevels`/`MostDetailedMip`. Mostly `nullptr` (default view); explicit for depth-as-colour and fluid. |
| `D3D11_RENDER_TARGET_VIEW_DESC` | Mostly `nullptr`; `TEXTURE3D` with `FirstWSlice`/`WSize` (fluid) |
| `D3D11_DEPTH_STENCIL_VIEW_DESC` | Format, `TEXTURE2D`/`TEXTURE2DMS`, `MipSlice` |
| `D3D11_UNORDERED_ACCESS_VIEW_DESC` | `TEXTURE2D` only (HDAO/SSAO output) |
| `D3D11_SAMPLER_DESC` | Filter (point, linear, anisotropic, comparison variants), AddressU/V/W (wrap, clamp, mirror, mirror-once, **border**), MipLODBias, MaxAnisotropy, ComparisonFunc, BorderColor, MinLOD, MaxLOD |
| `D3D11_RASTERIZER_DESC` | FillMode (solid, wireframe), CullMode, FrontCounterClockwise, DepthBias, DepthBiasClamp, SlopeScaledDepthBias, DepthClipEnable, ScissorEnable, MultisampleEnable, AntialiasedLineEnable |
| `D3D11_DEPTH_STENCIL_DESC` | DepthEnable, DepthWriteMask, DepthFunc, StencilEnable, Read/WriteMask, Front/BackFace {FailOp, DepthFailOp, PassOp, Func}; stencil ref passed to `OMSetDepthStencilState` |
| `D3D11_BLEND_DESC` | AlphaToCoverageEnable, IndependentBlendEnable, RenderTarget[8] {BlendEnable, SrcBlend, DestBlend, BlendOp, SrcBlendAlpha, DestBlendAlpha, BlendOpAlpha, RenderTargetWriteMask}; blend factor and sample mask via `OMSetBlendState`. `SRC1_*` factors exist in the conversion table (dual-source blending). |
| `D3D11_INPUT_ELEMENT_DESC` | SemanticName/Index, Format, InputSlot (0), AlignedByteOffset, per-vertex class only |
| `D3D11_VIEWPORT` | TopLeftX/Y, Width, Height, MinDepth, MaxDepth |
| `D3D11_QUERY_DESC` | `EVENT` and `OCCLUSION` |
| `DXGI_SWAP_CHAIN_DESC1` etc. | Width, Height, Format, BufferCount (2), `FLIP_DISCARD`, `ALPHA_MODE_IGNORE`, flags (`ALLOW_MODE_SWITCH`, `ALLOW_TEARING`) |

## 6. Features by importance for the Metal shim

"Default" means Anomaly's out-of-the-box settings (`r3_msaa` off, HDR10 off, tessellation off).

| Feature | D3D11 surface | In default config? | Shim priority |
|---|---|---|---|
| Deferred G-buffer, light accumulation, combine, post-process chain | RT switching, clears, copies, MRT (up to 8 RTs), blend/depth/stencil states, all sampler kinds, R16F/R32F/R11G11B10 formats | yes | **P0** |
| Shadow maps (sun cascades, spot, point), HW comparison sampling | `R32_TYPELESS` / D24 depth textures as DSV + SRV, comparison samplers, depth bias, viewport changes | yes | **P0** |
| Dynamic VB/IB streaming | `DYNAMIC` buffers, `Map` `DISCARD` / `NO_OVERWRITE` | yes | **P0** |
| Constant buffers | per-stage cbuffers refilled by `Map(DISCARD)` | yes | **P0** |
| DDS textures (BC1–7, uncompressed, cube, volume, mip skipping) | `D3DX11CreateTextureFromMemory` replacement, `IMMUTABLE`/`STAGING`, `CopyResource` staging→default | yes | **P0** |
| Event query (sync point) and occlusion queries | `CreateQuery`/`Begin`/`End`/`GetData` | yes | **P0** |
| Back buffer, `Present`, `ResizeBuffers` | swap chain object over `CAMetalLayer` | yes | **P0** |
| Dynamic video/GIF textures | `DYNAMIC` 2D texture `Map(DISCARD)` | intro videos | P1 |
| Compute: HDAO (`ssao_hdao`, `ssao_hdao_msaa`) is the only user | `Dispatch`, `CS*Set*`, UAV on an RT texture (`RWTexture2D`), `CSCompiler`/`ComputeShader` | HDAO SSAO mode only | P1 |
| Screenshots | staging read-back, `D3DX11SaveTextureToMemory` | on demand | P1 (own encoder) |
| MSAA (`r3_msaa` 2/4/8) | `SampleDesc.Count>1`, `Texture2DMS`, `ResolveSubresource`, MS DSV, alpha-to-coverage, sample mask, standard pattern quality | **off** | P2 |
| HDR10 output | `R10G10B10A2_UNORM` swap chain, `SetColorSpace1` | **off** | P2 |
| Tessellation | `Hull`/`DomainShader`, 3-control-point patch topology | **off** (`r4_enable_tessellation`, restart) | P2, not planned |
| Geometry shaders | `CreateGeometryShader`; **only** the 3D fluid blenders (`fluid_array`) use a GS | fluid is initialised unconditionally (`FluidManager.Initialize(70,70,70)`) | P2, not planned; skip fluid on Apple |
| 3D render targets | `RTV` `TEXTURE3D`, `FirstWSlice` | fluid only | P2 |
| Dual-source blending (`SRC1_*`) | conversion table only; no blender uses it today | no | P2 (Metal supports it) |
| Gamma ramp (`SetGammaControl`) | `IDXGIOutput` | menu gamma | replace with shader gamma |
| PerfHUD adapter, `DXGI_PRESENT_ALLOW_TEARING`, `ALLOW_MODE_SWITCH`, `D3D11_CREATE_DEVICE_DEBUG` | `CHW` | no | ignore |
| PIX events (`PIX_EVENT`, 71 uses) | `ID3DUserDefinedAnnotation::BeginEvent/EndEvent` | debug builds | map to Metal debug groups |

## 7. Non-D3D11 symbols the renderer depends on

These are not D3D11 but the R4 sources need them, so any port has to provide them somewhere else:

- **D3D9 enums and structs** (233 distinct identifiers, ~1,780 uses): `D3DPT_*`, `D3DCMP_*`,
  `D3DSTENCILOP_*`, `D3DBLEND_*`, `D3DFMT_*` (148 uses), `D3DRS_*`, `D3DFVF_*`, `D3DCULL_*`,
  `D3DDECLTYPE_*`, `D3DDECLUSAGE_*`, `D3DTEXF_*`, `D3DTADDRESS_*`, `D3DTSS_*`, `D3DSAMP_*`, `D3DPOOL_*`,
  `D3DVERTEXELEMENT9`, `D3DFORMAT`, `D3DPRIMITIVETYPE`, `D3DCOLOR_*`, `D3DLOCK_*`, `D3DUSAGE_*`. They are
  the engine's own vocabulary (blender recorder, vertex declarations, `SGeometry`, state wrapper).
- **D3DX9 helpers** (~245 uses): `D3DXVECTOR2/3/4`, `D3DXMATRIX`, `D3DXPLANE`, `D3DXMatrix*`, `D3DXVec3*`,
  `D3DXPlane*`, `D3DXGetDeclVertexSize`, `D3DXGetDeclLength`, `D3DXDeclaratorFromFVF`, `D3DXGetFVFVertexSize`,
  `D3DXFloat16To32Array`. Math can map to the engine's own `Fmatrix`/`Fvector`; the declaration helpers need a small port.
- **Windows basics used in D3D-facing code:** `HRESULT`, `S_OK`, `FAILED`/`SUCCEEDED`, `E_FAIL`,
  `E_NOTIMPL`, `UINT`, `BOOL`, `HWND`, `HMONITOR`, `RECT`, `GUID`/`REFIID`, `ZeroMemory`, `__uuidof`,
  `IID_PPV_ARGS`, `__stdcall`.

The Metal shim covers the D3D11, DXGI, D3DCompiler and D3DX11 parts. The D3D9 vocabulary is outside
this library; it needs a small shared `d3d9types` header owned by whoever ports `xrRender`.

## 8. Disabled or optional code found in the DX11 build

- `#ifdef DEBUG` only: `D3DPERF_*`, device debug layer (`--dxgi-dbg`), `Validate()`.
- `MASTER_GOLD` off: NVIDIA PerfHUD adapter search in `CHW::CreateD3D`.
- `r2_test_hw.cpp` (hardware probe) creates its own temporary device and swap chain with
  `D3D11CreateDeviceAndSwapChain`; replace with a stub on Apple.
- `D3D10_*` names that survive in the DX11 build are reflection constants (`SVT/SVC/SIT`), bind
  flags passed through the DX10-era screenshot path, and blob/include types; the shim must define
  them (they are identical in value to the D3D11 ones).

---

## Appendix: generated tables

Produced by `tools/d3d11_surface_scan/scan.py`. "Uses" are source mentions in live code; "Mostly in"
lists the files with the most hits (enum-family rows omit it). Identifiers appear under their real
D3D11/D3D10/DXGI names, with the engine's `D3D_*`/`ID3D*` aliases folded in.

### Interfaces (41 identifiers, 394 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `ID3D11Buffer` | 79 | 21 | r4_rendertarget.h, R_Backend.h, Shader.h +18 |
| `ID3D11Texture2D` | 40 | 13 | dx10SH_Texture.cpp, r4_rendertarget.cpp, r__screenshot.cpp +10 |
| `ID3D11ShaderResourceView` | 33 | 9 | dx10ShaderResourceStateCache.h, dx10ShaderResourceStateCache.cpp, R_Backend_Runtime.cpp +6 |
| `ID3D11Resource` | 26 | 12 | SH_Texture.h, dx10SH_Texture.cpp, r4_rendertarget.cpp +9 |
| `ID3D11RenderTargetView` | 20 | 11 | R_Backend.h, r4_rendertarget.h, dx103DFluidData.h +8 |
| `ID3D10Blob` | 17 | 8 | r__screenshot.cpp, SH_Atomic.h, dx10R_Backend_Runtime.h +5 |
| `ID3D11DepthStencilView` | 17 | 9 | r4_rendertarget.h, r4_rendertarget.cpp, R_Backend.h +6 |
| `ID3D11Texture3D` | 14 | 4 | dx103DFluidManager.cpp, dx10SH_Texture.cpp, dx103DFluidData.h +1 |
| `ID3D11SamplerState` | 13 | 5 | dx10SamplerStateCache.cpp, dx10SamplerStateCache.h, ComputeShader.h +2 |
| `ID3D11Query` | 10 | 3 | QueryHelper.h, r__occlusion.h, r4.h |
| `ID3D11ComputeShader` | 9 | 7 | R_Backend.h, ComputeShader.h, SH_Atomic.h +4 |
| `ID3D11BlendState` | 8 | 5 | dx10StateCache.cpp, dx10StateManager.h, dx10State.h +2 |
| `ID3D11DepthStencilState` | 8 | 5 | dx10StateCache.cpp, dx10StateManager.h, dx10State.h +2 |
| `ID3D11RasterizerState` | 8 | 5 | dx10StateCache.cpp, dx10StateManager.h, dx10State.h +2 |
| `ID3D11ShaderReflection` | 8 | 3 | r4.cpp, dx10r_constants.cpp, r_constants.h |
| `ID3D11InputLayout` | 7 | 5 | dx10R_Backend_Runtime.h, SH_Atomic.h, R_Backend.h +2 |
| `ID3D11ShaderReflectionConstantBuffer` | 7 | 6 | dx10r_constants.cpp, dx10ConstantBuffer.h, r_constants.h +3 |
| `IDXGIOutput` | 7 | 4 | dx10HW.cpp, HW.h, dxRenderDeviceRender.cpp +1 |
| `ID3D11DeviceContext` | 6 | 5 | r2_test_hw.cpp, r4_rendertarget.h, dx10HW.cpp +2 |
| `ID3D11DomainShader` | 6 | 5 | R_Backend.h, SH_Atomic.h, dx10R_Backend_Runtime.h +2 |
| `ID3D11HullShader` | 6 | 5 | R_Backend.h, SH_Atomic.h, dx10R_Backend_Runtime.h +2 |
| `ID3D11UnorderedAccessView` | 6 | 5 | ComputeShader.h, SH_RT.h, ComputeShader.cpp +2 |
| `ID3D11GeometryShader` | 5 | 4 | R_Backend.h, SH_Atomic.h, dx10R_Backend_Runtime.h +1 |
| `ID3D11PixelShader` | 5 | 4 | R_Backend.h, SH_Atomic.h, dx10R_Backend_Runtime.h +1 |
| `ID3D11VertexShader` | 4 | 3 | R_Backend.h, SH_Atomic.h, dx10R_Backend_Runtime.h |
| `ID3D11Device` | 3 | 2 | r2_test_hw.cpp, dx10HW.cpp |
| `ID3D11Texture1D` | 3 | 2 | dx10SH_Texture.cpp, dx103DFluidRenderer.cpp |
| `ID3D11ShaderReflectionType` | 2 | 2 | dx10ConstantBuffer.cpp, dx10r_constants.cpp |
| `ID3D11ShaderReflectionVariable` | 2 | 2 | dx10ConstantBuffer.cpp, dx10r_constants.cpp |
| `ID3DUserDefinedAnnotation` | 2 | 2 | HW.h, dx10HW.cpp |
| `IDXGIAdapter1` | 2 | 2 | HW.h, dx10HW.cpp |
| `IDXGISwapChain` | 2 | 1 | r2_test_hw.cpp |
| `ID3D10Include` | 1 | 1 | r4.cpp |
| `ID3D10Texture2D` | 1 | 1 | r4_rendertarget_phase_combine.cpp |
| `ID3D11Device1` | 1 | 1 | HW.h |
| `ID3D11DeviceContext1` | 1 | 1 | HW.h |
| `IDXGIAdapter` | 1 | 1 | r2_test_hw.cpp |
| `IDXGIFactory2` | 1 | 1 | HW.h |
| `IDXGIFactory5` | 1 | 1 | dx10HW.cpp |
| `IDXGISwapChain1` | 1 | 1 | HW.h |
| `IDXGISwapChain3` | 1 | 1 | dx10HW.cpp |

### Descriptor and data structs (34 identifiers, 197 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_SAMPLER_DESC` | 21 | 9 | dx10StateUtils.h, dx10StateUtils.cpp, tss_def.cpp +6 |
| `D3D11_TEXTURE2D_DESC` | 20 | 9 | r4_rendertarget.cpp, dx10SH_Texture.cpp, r__screenshot.cpp +6 |
| `D3D11_RASTERIZER_DESC` | 18 | 8 | dx10StateUtils.h, dx10StateUtils.cpp, dx10StateCache.cpp +5 |
| `D3D11_BLEND_DESC` | 17 | 7 | dx10StateUtils.h, dx10StateUtils.cpp, dx10StateCache.cpp +4 |
| `D3D11_DEPTH_STENCIL_DESC` | 17 | 7 | dx10StateUtils.h, dx10StateUtils.cpp, dx10StateCache.cpp +4 |
| `D3D11_BUFFER_DESC` | 11 | 5 | stats_manager.cpp, dx10BufferUtils.cpp, ModelPool.cpp +2 |
| `D3D11_TEXTURE3D_DESC` | 9 | 6 | dx103DFluidData.cpp, dx103DFluidManager.cpp, dx10SH_Texture.cpp +3 |
| `D3D11_VIEWPORT` | 9 | 7 | r4.cpp, dx103DFluidManager.cpp, dxImGuiRender.cpp +4 |
| `D3D11_MAPPED_SUBRESOURCE` | 7 | 4 | dx10SH_Texture.cpp, R_DStreams.cpp, r__screenshot.cpp +1 |
| `D3D11_SUBRESOURCE_DATA` | 7 | 5 | dx103DFluidRenderer.cpp, r4_rendertarget.cpp, dx10BufferUtils.cpp +2 |
| `DXGI_MODE_DESC` | 5 | 1 | dx10HW.cpp |
| `D3D11_DEPTH_STENCIL_VIEW_DESC` | 4 | 2 | r4_rendertarget.cpp, dx10SH_RT.cpp |
| `D3D11_INPUT_ELEMENT_DESC` | 4 | 3 | dx10BufferUtils.cpp, SH_Atomic.h, dx10BufferUtils.h |
| `DXGI_SWAP_CHAIN_DESC1` | 4 | 2 | dx10HW.cpp, HW.h |
| `D3D10_SHADER_MACRO` | 3 | 2 | r4.cpp, r4.h |
| `D3D11_QUERY_DESC` | 3 | 2 | r4.cpp, QueryHelper.h |
| `D3D11_SHADER_RESOURCE_VIEW_DESC` | 3 | 3 | dx103DFluidData.cpp, dx103DFluidManager.cpp, dx10SH_Texture.cpp |
| `DXGI_GAMMA_CONTROL` | 3 | 2 | xr_effgamma.cpp, xr_effgamma.h |
| `DXGI_GAMMA_CONTROL_CAPABILITIES` | 3 | 2 | xr_effgamma.cpp, xr_effgamma.h |
| `DXGI_RATIONAL` | 3 | 2 | dx10HW.cpp, HW.h |
| `DXGI_SWAP_CHAIN_FULLSCREEN_DESC` | 3 | 2 | dx10HW.cpp, HW.h |
| `D3D11_RENDER_TARGET_VIEW_DESC` | 2 | 2 | dx103DFluidData.cpp, dx103DFluidManager.cpp |
| `D3D11_SHADER_BUFFER_DESC` | 2 | 2 | dx10ConstantBuffer.cpp, dx10r_constants.cpp |
| `D3D11_SHADER_TYPE_DESC` | 2 | 2 | dx10ConstantBuffer.h, dx10r_constants.cpp |
| `D3D11_SHADER_VARIABLE_DESC` | 2 | 2 | dx10ConstantBuffer.cpp, dx10r_constants.cpp |
| `D3D11_TEXTURE1D_DESC` | 2 | 2 | dx103DFluidRenderer.cpp, dx10SH_Texture.cpp |
| `D3D11_UNORDERED_ACCESS_VIEW_DESC` | 2 | 1 | dx10SH_RT.cpp |
| `DXGI_ADAPTER_DESC` | 2 | 1 | dx10HW.cpp |
| `DXGI_OUTPUT_DESC` | 2 | 1 | dx10HW.cpp |
| `DXGI_RGB` | 2 | 1 | xr_effgamma.cpp |
| `DXGI_SWAP_CHAIN_DESC` | 2 | 1 | r2_test_hw.cpp |
| `D3D10_INCLUDE_TYPE` | 1 | 1 | r4.cpp |
| `D3D11_SHADER_DESC` | 1 | 1 | dx10r_constants.cpp |
| `D3D11_SHADER_INPUT_BIND_DESC` | 1 | 1 | dx10r_constants.cpp |

### Enum family `BLEND` (18 identifiers, 50 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_BLEND_ONE` | 6 | 1 |  |
| `D3D11_BLEND_ZERO` | 5 | 1 |  |
| `D3D11_BLEND_DEST_ALPHA` | 3 | 1 |  |
| `D3D11_BLEND_DEST_COLOR` | 3 | 1 |  |
| `D3D11_BLEND_INV_DEST_ALPHA` | 3 | 1 |  |
| `D3D11_BLEND_INV_DEST_COLOR` | 3 | 1 |  |
| `D3D11_BLEND_INV_SRC1_COLOR` | 3 | 1 |  |
| `D3D11_BLEND_INV_SRC_ALPHA` | 3 | 1 |  |
| `D3D11_BLEND_INV_SRC_COLOR` | 3 | 1 |  |
| `D3D11_BLEND_SRC1_COLOR` | 3 | 1 |  |
| `D3D11_BLEND_SRC_ALPHA` | 3 | 1 |  |
| `D3D11_BLEND_SRC_COLOR` | 3 | 1 |  |
| `D3D11_BLEND` | 2 | 2 |  |
| `D3D11_BLEND_INV_SRC1_ALPHA` | 2 | 1 |  |
| `D3D11_BLEND_SRC1_ALPHA` | 2 | 1 |  |
| `D3D11_BLEND_BLEND_FACTOR` | 1 | 1 |  |
| `D3D11_BLEND_INV_BLEND_FACTOR` | 1 | 1 |  |
| `D3D11_BLEND_SRC_ALPHA_SAT` | 1 | 1 |  |

### Enum family `BIND` (9 identifiers, 33 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_BIND_SHADER_RESOURCE` | 15 | 7 |  |
| `D3D10_BIND_SHADER_RESOURCE` | 4 | 3 |  |
| `D3D11_BIND_DEPTH_STENCIL` | 3 | 2 |  |
| `D3D11_BIND_INDEX_BUFFER` | 3 | 3 |  |
| `D3D10_BIND_RENDER_TARGET` | 2 | 2 |  |
| `D3D11_BIND_RENDER_TARGET` | 2 | 1 |  |
| `D3D11_BIND_VERTEX_BUFFER` | 2 | 2 |  |
| `D3D11_BIND_CONSTANT_BUFFER` | 1 | 1 |  |
| `D3D11_BIND_UNORDERED_ACCESS` | 1 | 1 |  |

### Enum family `USAGE` (5 identifiers, 32 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_USAGE_DEFAULT` | 16 | 10 |  |
| `D3D11_USAGE_DYNAMIC` | 5 | 3 |  |
| `D3D11_USAGE_IMMUTABLE` | 4 | 3 |  |
| `D3D11_USAGE_STAGING` | 4 | 3 |  |
| `D3D11_USAGE` | 3 | 2 |  |

### Enum family `FEATURE_LEVEL` (4 identifiers, 30 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D_FEATURE_LEVEL_11_0` | 14 | 5 |  |
| `D3D_FEATURE_LEVEL_10_1` | 7 | 2 |  |
| `D3D_FEATURE_LEVEL` | 6 | 3 |  |
| `D3D_FEATURE_LEVEL_10_0` | 3 | 1 |  |

### Enum family `COMMONSHADER` (1 identifiers, 28 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT` | 28 | 5 |  |

### Enum family `STENCIL_OP` (9 identifiers, 24 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_STENCIL_OP_KEEP` | 14 | 1 |  |
| `D3D11_STENCIL_OP` | 3 | 3 |  |
| `D3D11_STENCIL_OP_DECR` | 1 | 1 |  |
| `D3D11_STENCIL_OP_DECR_SAT` | 1 | 1 |  |
| `D3D11_STENCIL_OP_INCR` | 1 | 1 |  |
| `D3D11_STENCIL_OP_INCR_SAT` | 1 | 1 |  |
| `D3D11_STENCIL_OP_INVERT` | 1 | 1 |  |
| `D3D11_STENCIL_OP_REPLACE` | 1 | 1 |  |
| `D3D11_STENCIL_OP_ZERO` | 1 | 1 |  |

### Enum family `COMPARISON` (9 identifiers, 23 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_COMPARISON_ALWAYS` | 6 | 1 |  |
| `D3D11_COMPARISON_FUNC` | 5 | 4 |  |
| `D3D11_COMPARISON_LESS` | 3 | 1 |  |
| `D3D11_COMPARISON_LESS_EQUAL` | 3 | 3 |  |
| `D3D11_COMPARISON_NEVER` | 2 | 1 |  |
| `D3D11_COMPARISON_EQUAL` | 1 | 1 |  |
| `D3D11_COMPARISON_GREATER` | 1 | 1 |  |
| `D3D11_COMPARISON_GREATER_EQUAL` | 1 | 1 |  |
| `D3D11_COMPARISON_NOT_EQUAL` | 1 | 1 |  |

### Enum family `TEXTURE_ADDRESS` (6 identifiers, 22 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_TEXTURE_ADDRESS_CLAMP` | 9 | 2 |  |
| `D3D11_TEXTURE_ADDRESS_WRAP` | 5 | 2 |  |
| `D3D11_TEXTURE_ADDRESS_BORDER` | 4 | 1 |  |
| `D3D11_TEXTURE_ADDRESS_MODE` | 2 | 2 |  |
| `D3D11_TEXTURE_ADDRESS_MIRROR` | 1 | 1 |  |
| `D3D11_TEXTURE_ADDRESS_MIRROR_ONCE` | 1 | 1 |  |

### Enum family `FILTER` (7 identifiers, 21 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_FILTER` | 11 | 1 |  |
| `D3D11_FILTER_ANISOTROPIC` | 2 | 2 |  |
| `D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT` | 2 | 1 |  |
| `D3D11_FILTER_MIN_MAG_MIP_LINEAR` | 2 | 2 |  |
| `D3D11_FILTER_MIN_MAG_MIP_POINT` | 2 | 1 |  |
| `D3D11_FILTER_COMPARISON_ANISOTROPIC` | 1 | 1 |  |
| `D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT` | 1 | 1 |  |

### Enum family `PRIMITIVE_TOPOLOGY` (8 identifiers, 19 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_PRIMITIVE_TOPOLOGY` | 8 | 2 |  |
| `D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED` | 4 | 2 |  |
| `D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST` | 2 | 1 |  |
| `D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST` | 1 | 1 |  |
| `D3D11_PRIMITIVE_TOPOLOGY_LINELIST` | 1 | 1 |  |
| `D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP` | 1 | 1 |  |
| `D3D11_PRIMITIVE_TOPOLOGY_POINTLIST` | 1 | 1 |  |
| `D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP` | 1 | 1 |  |

### Enum family `RESOURCE_DIMENSION` (4 identifiers, 15 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_RESOURCE_DIMENSION` | 6 | 1 |  |
| `D3D11_RESOURCE_DIMENSION_TEXTURE2D` | 6 | 1 |  |
| `D3D11_RESOURCE_DIMENSION_TEXTURE3D` | 2 | 1 |  |
| `D3D11_RESOURCE_DIMENSION_TEXTURE1D` | 1 | 1 |  |

### Enum family `BLEND_OP` (6 identifiers, 12 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_BLEND_OP_ADD` | 6 | 1 |  |
| `D3D11_BLEND_OP` | 2 | 2 |  |
| `D3D11_BLEND_OP_MAX` | 1 | 1 |  |
| `D3D11_BLEND_OP_MIN` | 1 | 1 |  |
| `D3D11_BLEND_OP_REV_SUBTRACT` | 1 | 1 |  |
| `D3D11_BLEND_OP_SUBTRACT` | 1 | 1 |  |

### Enum family `MAP` (4 identifiers, 10 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_MAP_WRITE_DISCARD` | 5 | 3 |  |
| `D3D11_MAP_READ` | 2 | 1 |  |
| `D3D11_MAP_WRITE_NO_OVERWRITE` | 2 | 1 |  |
| `D3D11_MAP` | 1 | 1 |  |

### Enum family `CLEAR` (2 identifiers, 10 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_CLEAR_DEPTH` | 6 | 4 |  |
| `D3D11_CLEAR_STENCIL` | 4 | 2 |  |

### Enum family `FILL` (3 identifiers, 9 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_FILL_SOLID` | 4 | 2 |  |
| `D3D11_FILL_MODE` | 3 | 3 |  |
| `D3D11_FILL_WIREFRAME` | 2 | 2 |  |

### Enum family `CPU_ACCESS` (2 identifiers, 8 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_CPU_ACCESS_WRITE` | 7 | 4 |  |
| `D3D11_CPU_ACCESS_READ` | 1 | 1 |  |

### Enum family `CULL` (4 identifiers, 8 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_CULL_MODE` | 3 | 3 |  |
| `D3D11_CULL_BACK` | 2 | 1 |  |
| `D3D11_CULL_NONE` | 2 | 1 |  |
| `D3D11_CULL_FRONT` | 1 | 1 |  |

### Enum family `DRIVER_TYPE` (3 identifiers, 7 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_DRIVER_TYPE_HARDWARE` | 3 | 2 |  |
| `D3D11_DRIVER_TYPE_REFERENCE` | 2 | 1 |  |
| `D3D_DRIVER_TYPE` | 2 | 2 |  |

### Enum family `(types and other)` (2 identifiers, 6 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D_OK` | 5 | 2 |  |
| `D3D11_CBUFFER_TYPE` | 1 | 1 |  |

### Enum family `SVC` (6 identifiers, 6 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D10_SVC_MATRIX_COLUMNS` | 1 | 1 |  |
| `D3D10_SVC_MATRIX_ROWS` | 1 | 1 |  |
| `D3D10_SVC_OBJECT` | 1 | 1 |  |
| `D3D10_SVC_SCALAR` | 1 | 1 |  |
| `D3D10_SVC_STRUCT` | 1 | 1 |  |
| `D3D10_SVC_VECTOR` | 1 | 1 |  |

### Enum family `SRV_DIMENSION` (4 identifiers, 5 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_SRV_DIMENSION_TEXTURE3D` | 2 | 2 |  |
| `D3D11_SRV_DIMENSION_TEXTURE2D` | 1 | 1 |  |
| `D3D11_SRV_DIMENSION_TEXTURE2DMS` | 1 | 1 |  |
| `D3D11_SRV_DIMENSION_TEXTURECUBE` | 1 | 1 |  |

### Enum family `SHADER_PACK` (1 identifiers, 5 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D10_SHADER_PACK_MATRIX_ROW_MAJOR` | 5 | 3 |  |

### Enum family `DSV_DIMENSION` (2 identifiers, 5 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_DSV_DIMENSION_TEXTURE2D` | 4 | 2 |  |
| `D3D11_DSV_DIMENSION_TEXTURE2DMS` | 1 | 1 |  |

### Enum family `SVT` (4 identifiers, 4 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D10_SVT_BOOL` | 1 | 1 |  |
| `D3D10_SVT_FLOAT` | 1 | 1 |  |
| `D3D10_SVT_INT` | 1 | 1 |  |
| `D3D10_SVT_UINT` | 1 | 1 |  |

### Enum family `DEPTH_WRITE_MASK` (2 identifiers, 4 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_DEPTH_WRITE_MASK_ALL` | 3 | 2 |  |
| `D3D11_DEPTH_WRITE_MASK_ZERO` | 1 | 1 |  |

### Enum family `COLOR_WRITE_ENABLE` (1 identifiers, 4 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_COLOR_WRITE_ENABLE_ALL` | 4 | 3 |  |

### Enum family `SIT` (3 identifiers, 3 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D10_SIT_SAMPLER` | 1 | 1 |  |
| `D3D10_SIT_TEXTURE` | 1 | 1 |  |
| `D3D11_SIT_UAV_RWTYPED` | 1 | 1 |  |

### Enum family `QUERY` (2 identifiers, 3 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_QUERY_EVENT` | 2 | 1 |  |
| `D3D11_QUERY_OCCLUSION` | 1 | 1 |  |

### Enum family `RTV_DIMENSION` (1 identifiers, 2 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_RTV_DIMENSION_TEXTURE3D` | 2 | 2 |  |

### Enum family `CREATE_DEVICE` (2 identifiers, 2 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_CREATE_DEVICE_BGRA_SUPPORT` | 1 | 1 |  |
| `D3D11_CREATE_DEVICE_DEBUG` | 1 | 1 |  |

### Enum family `SDK_VERSION` (1 identifiers, 2 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_SDK_VERSION` | 2 | 2 |  |

### Enum family `REQ` (1 identifiers, 2 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION` | 2 | 1 |  |

### Enum family `RESOURCE_MISC` (1 identifiers, 2 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_RESOURCE_MISC_TEXTURECUBE` | 2 | 2 |  |

### Enum family `INPUT` (1 identifiers, 1 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_INPUT_PER_VERTEX_DATA` | 1 | 1 |  |

### Enum family `STANDARD_MULTISAMPLE` (1 identifiers, 1 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_STANDARD_MULTISAMPLE_PATTERN` | 1 | 1 |  |

### Enum family `UAV_DIMENSION` (1 identifiers, 1 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3D11_UAV_DIMENSION_TEXTURE2D` | 1 | 1 |  |

### DXGI formats (44 identifiers, 108 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `DXGI_FORMAT` | 16 | 12 |  |
| `DXGI_FORMAT_R8G8B8A8_UNORM` | 15 | 8 |  |
| `DXGI_FORMAT_R16G16B16A16_FLOAT` | 7 | 5 |  |
| `DXGI_FORMAT_R16_FLOAT` | 5 | 3 |  |
| `DXGI_FORMAT_UNKNOWN` | 5 | 4 |  |
| `DXGI_FORMAT_R24G8_TYPELESS` | 4 | 3 |  |
| `DXGI_FORMAT_BC1_UNORM` | 3 | 1 |  |
| `DXGI_FORMAT_R32G32B32A32_FLOAT` | 3 | 3 |  |
| `DXGI_FORMAT_R32_FLOAT` | 3 | 3 |  |
| `DXGI_FORMAT_R32_TYPELESS` | 3 | 3 |  |
| `DXGI_FORMAT_R8G8B8A8_SNORM` | 3 | 2 |  |
| `DXGI_FORMAT_R8_UNORM` | 3 | 3 |  |
| `DXGI_FORMAT_D24_UNORM_S8_UINT` | 2 | 2 |  |
| `DXGI_FORMAT_R10G10B10A2_UNORM` | 2 | 2 |  |
| `DXGI_FORMAT_R16G16B16A16_UNORM` | 2 | 2 |  |
| `DXGI_FORMAT_R16G16_FLOAT` | 2 | 2 |  |
| `DXGI_FORMAT_R16G16_UNORM` | 2 | 2 |  |
| `DXGI_FORMAT_R24_UNORM_X8_TYPELESS` | 2 | 1 |  |
| `DXGI_FORMAT_A8_UNORM` | 1 | 1 |  |
| `DXGI_FORMAT_BC5_UNORM` | 1 | 1 |  |
| `DXGI_FORMAT_BC7_UNORM` | 1 | 1 |  |
| `DXGI_FORMAT_D32_FLOAT` | 1 | 1 |  |
| `DXGI_FORMAT_R10G10B10A2_TYPELESS` | 1 | 1 |  |
| `DXGI_FORMAT_R11G11B10_FLOAT` | 1 | 1 |  |
| `DXGI_FORMAT_R16G16B16A16_SINT` | 1 | 1 |  |
| `DXGI_FORMAT_R16G16B16A16_SNORM` | 1 | 1 |  |
| `DXGI_FORMAT_R16G16B16A16_TYPELESS` | 1 | 1 |  |
| `DXGI_FORMAT_R16G16_SINT` | 1 | 1 |  |
| `DXGI_FORMAT_R16G16_SNORM` | 1 | 1 |  |
| `DXGI_FORMAT_R16_SINT` | 1 | 1 |  |
| `DXGI_FORMAT_R16_UINT` | 1 | 1 |  |
| `DXGI_FORMAT_R32G32B32A32_SINT` | 1 | 1 |  |
| `DXGI_FORMAT_R32G32B32A32_TYPELESS` | 1 | 1 |  |
| `DXGI_FORMAT_R32G32B32_FLOAT` | 1 | 1 |  |
| `DXGI_FORMAT_R32G32B32_SINT` | 1 | 1 |  |
| `DXGI_FORMAT_R32G32B32_TYPELESS` | 1 | 1 |  |
| `DXGI_FORMAT_R32G32_FLOAT` | 1 | 1 |  |
| `DXGI_FORMAT_R8G8B8A8_UINT` | 1 | 1 |  |
| `DXGI_FORMAT_R8G8_SNORM` | 1 | 1 |  |
| `DXGI_FORMAT_R8G8_TYPELESS` | 1 | 1 |  |
| `DXGI_FORMAT_R8G8_UNORM` | 1 | 1 |  |
| `DXGI_FORMAT_R8_TYPELESS` | 1 | 1 |  |
| `DXGI_FORMAT_X24_TYPELESS_G8_UINT` | 1 | 1 |  |
| `DXGI_FORMAT_X32_TYPELESS_G8X24_UINT` | 1 | 1 |  |

### Other DXGI constants (14 identifiers, 27 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH` | 5 | 1 | dx10HW.cpp |
| `DXGI_ERROR_NOT_FOUND` | 4 | 1 | dx10HW.cpp |
| `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` | 4 | 1 | dx10HW.cpp |
| `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` | 2 | 1 | dx10HW.cpp |
| `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709` | 2 | 1 | dx10HW.cpp |
| `DXGI_USAGE_RENDER_TARGET_OUTPUT` | 2 | 2 | dx10HW.cpp, r2_test_hw.cpp |
| `DXGI_ALPHA_MODE_IGNORE` | 1 | 1 | dx10HW.cpp |
| `DXGI_ERROR_DEVICE_REMOVED` | 1 | 1 | dxRenderDeviceRender.cpp |
| `DXGI_ERROR_DEVICE_RESET` | 1 | 1 | dxRenderDeviceRender.cpp |
| `DXGI_FEATURE_PRESENT_ALLOW_TEARING` | 1 | 1 | dx10HW.cpp |
| `DXGI_PRESENT_ALLOW_TEARING` | 1 | 1 | dxRenderDeviceRender.cpp |
| `DXGI_PRESENT_TEST` | 1 | 1 | dxRenderDeviceRender.cpp |
| `DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT` | 1 | 1 | dx10HW.cpp |
| `DXGI_SWAP_EFFECT_FLIP_DISCARD` | 1 | 1 | dx10HW.cpp |

### Free functions and D3DX11/D3DX10 helpers (19 identifiers, 32 uses)

| Identifier | Uses | Files | Mostly in |
|---|--:|--:|---|
| `D3DReflect` | 4 | 1 | r4.cpp |
| `D3DX11SaveTextureToMemory` | 4 | 1 | r__screenshot.cpp |
| `D3DX11_IFF_DDS` | 4 | 2 | r__screenshot.cpp, dx10Texture.cpp |
| `D3DX11LoadTextureFromTexture` | 3 | 1 | r__screenshot.cpp |
| `D3DX11CreateTextureFromMemory` | 2 | 1 | dx10Texture.cpp |
| `D3DX11_IMAGE_LOAD_INFO` | 2 | 1 | dx10Texture.cpp |
| `CreateDXGIFactory2` | 1 | 1 | dx10HW.cpp |
| `D3D11CreateDevice` | 1 | 1 | dx10HW.cpp |
| `D3DCompile` | 1 | 1 | r4.cpp |
| `D3DDisassemble` | 1 | 1 | r4.cpp |
| `D3DGetInputSignatureBlob` | 1 | 1 | r4.cpp |
| `D3DX10_FILTER_POINT` | 1 | 1 | r4_rendertarget.cpp |
| `D3DX11FilterTexture` | 1 | 1 | r4_rendertarget.cpp |
| `D3DX11GetImageInfoFromMemory` | 1 | 1 | dx10Texture.cpp |
| `D3DX11SaveTextureToFile` | 1 | 1 | dx10Texture.cpp |
| `D3DX11_IFF_BMP` | 1 | 1 | r__screenshot.cpp |
| `D3DX11_IFF_JPG` | 1 | 1 | r__screenshot.cpp |
| `D3DX11_IFF_PNG` | 1 | 1 | r__screenshot.cpp |
| `D3DX11_IMAGE_INFO` | 1 | 1 | dx10Texture.cpp |

## Generated call tables

Receiver-filtered: a name only counts when called on a D3D-looking receiver (`HW.pContext`, `HW.pDevice`, `m_pSwapChain`, ...).

### ID3D11DeviceContext (50 methods, 173 call sites)

| Method | Calls | Files | Mostly in |
|---|--:|--:|---|
| `ClearRenderTargetView` | 47 | 18 | r4_rendertarget_phase_combine.cpp, dx103DFluidManager.cpp, r4_R_render.cpp +15 |
| `CopyResource` | 36 | 13 | r4_rendertarget_phase_combine.cpp, rendertarget_phase_blur.cpp, rendertarget_phase_nightvision.cpp +10 |
| `RSSetViewports` | 9 | 7 | r4.cpp, dx103DFluidManager.cpp, dxImGuiRender.cpp +4 |
| `Map` | 8 | 4 | R_DStreams.cpp, dx10SH_Texture.cpp, r__screenshot.cpp +1 |
| `Unmap` | 7 | 4 | dx10SH_Texture.cpp, R_DStreams.cpp, r__screenshot.cpp +1 |
| `ClearDepthStencilView` | 6 | 4 | r4_rendertarget_phase_scene.cpp, dxRenderDeviceRender.cpp, r4_rendertarget_phase_smap_D.cpp +1 |
| `OMSetRenderTargets` | 6 | 3 | dx10R_Backend_Runtime.h, r4_rendertarget_phase_hdao.cpp, r4_rendertarget_phase_accumulator.cpp |
| `CSSetShaderResources` | 3 | 3 | dx10ShaderResourceStateCache.cpp, ComputeShader.cpp, r4_rendertarget_phase_hdao.cpp |
| `CSSetUnorderedAccessViews` | 3 | 2 | r4_rendertarget_phase_hdao.cpp, ComputeShader.cpp |
| `End` | 3 | 2 | r4.cpp, QueryHelper.h |
| `CSSetConstantBuffers` | 2 | 2 | dx10R_Backend_Runtime.h, ComputeShader.cpp |
| `CSSetSamplers` | 2 | 2 | dx10SamplerStateCache.cpp, ComputeShader.cpp |
| `Dispatch` | 2 | 2 | dx10R_Backend_Runtime.h, ComputeShader.cpp |
| `RSSetScissorRects` | 2 | 1 | dx10R_Backend_Runtime.h |
| `ResolveSubresource` | 2 | 1 | r4_rendertarget_phase_combine.cpp |
| `Begin` | 1 | 1 | QueryHelper.h |
| `CSSetShader` | 1 | 1 | dx10R_Backend_Runtime.h |
| `ClearState` | 1 | 1 | R_Backend_Runtime.cpp |
| `CopySubresourceRegion` | 1 | 1 | r4_rendertarget.cpp |
| `DSSetConstantBuffers` | 1 | 1 | dx10R_Backend_Runtime.h |
| `DSSetSamplers` | 1 | 1 | dx10SamplerStateCache.cpp |
| `DSSetShader` | 1 | 1 | dx10R_Backend_Runtime.h |
| `DSSetShaderResources` | 1 | 1 | dx10ShaderResourceStateCache.cpp |
| `Draw` | 1 | 1 | dx10R_Backend_Runtime.h |
| `DrawIndexed` | 1 | 1 | dx10R_Backend_Runtime.h |
| `GSSetConstantBuffers` | 1 | 1 | dx10R_Backend_Runtime.h |
| `GSSetSamplers` | 1 | 1 | dx10SamplerStateCache.cpp |
| `GSSetShader` | 1 | 1 | dx10R_Backend_Runtime.h |
| `GSSetShaderResources` | 1 | 1 | dx10ShaderResourceStateCache.cpp |
| `GetData` | 1 | 1 | QueryHelper.h |
| `HSSetConstantBuffers` | 1 | 1 | dx10R_Backend_Runtime.h |
| `HSSetSamplers` | 1 | 1 | dx10SamplerStateCache.cpp |
| `HSSetShader` | 1 | 1 | dx10R_Backend_Runtime.h |
| `HSSetShaderResources` | 1 | 1 | dx10ShaderResourceStateCache.cpp |
| `IASetIndexBuffer` | 1 | 1 | dx10R_Backend_Runtime.h |
| `IASetInputLayout` | 1 | 1 | dx10R_Backend_Runtime.h |
| `IASetPrimitiveTopology` | 1 | 1 | dx10R_Backend_Runtime.h |
| `IASetVertexBuffers` | 1 | 1 | dx10R_Backend_Runtime.h |
| `OMGetRenderTargets` | 1 | 1 | r4_rendertarget_phase_hdao.cpp |
| `OMSetBlendState` | 1 | 1 | dx10StateManager.cpp |
| `OMSetDepthStencilState` | 1 | 1 | dx10StateManager.cpp |
| `PSSetConstantBuffers` | 1 | 1 | dx10R_Backend_Runtime.h |
| `PSSetSamplers` | 1 | 1 | dx10SamplerStateCache.cpp |
| `PSSetShader` | 1 | 1 | dx10R_Backend_Runtime.h |
| `PSSetShaderResources` | 1 | 1 | dx10ShaderResourceStateCache.cpp |
| `RSSetState` | 1 | 1 | dx10StateManager.cpp |
| `VSSetConstantBuffers` | 1 | 1 | dx10R_Backend_Runtime.h |
| `VSSetSamplers` | 1 | 1 | dx10SamplerStateCache.cpp |
| `VSSetShader` | 1 | 1 | dx10R_Backend_Runtime.h |
| `VSSetShaderResources` | 1 | 1 | dx10ShaderResourceStateCache.cpp |

### ID3D11Device (20 methods, 53 call sites)

| Method | Calls | Files | Mostly in |
|---|--:|--:|---|
| `CreateTexture2D` | 14 | 7 | r4_rendertarget.cpp, dx10SH_Texture.cpp, r__screenshot.cpp +4 |
| `CreateShaderResourceView` | 7 | 2 | dx10SH_Texture.cpp, gifPlayer.cpp |
| `CreateBuffer` | 5 | 3 | dx10BufferUtils.cpp, R_DStreams.cpp, R_Backend.cpp |
| `CreateRenderTargetView` | 4 | 4 | dx103DFluidData.cpp, dx103DFluidManager.cpp, dx10SH_RT.cpp +1 |
| `CreateTexture3D` | 4 | 4 | dx103DFluidData.cpp, dx103DFluidManager.cpp, dx10SH_Texture.cpp +1 |
| `CreateQuery` | 3 | 2 | r4.cpp, QueryHelper.h |
| `CreateDepthStencilView` | 2 | 2 | dx10HW.cpp, dx10SH_RT.cpp |
| `CreateSamplerState` | 2 | 2 | dx10SamplerStateCache.cpp, CSCompiler.cpp |
| `CreateBlendState` | 1 | 1 | dx10StateCache.cpp |
| `CreateComputeShader` | 1 | 1 | ShaderResourceTraits.h |
| `CreateDepthStencilState` | 1 | 1 | dx10StateCache.cpp |
| `CreateDomainShader` | 1 | 1 | ShaderResourceTraits.h |
| `CreateGeometryShader` | 1 | 1 | r4.cpp |
| `CreateHullShader` | 1 | 1 | ShaderResourceTraits.h |
| `CreateInputLayout` | 1 | 1 | dx10R_Backend_Runtime.h |
| `CreatePixelShader` | 1 | 1 | r4.cpp |
| `CreateRasterizerState` | 1 | 1 | dx10StateCache.cpp |
| `CreateTexture1D` | 1 | 1 | dx103DFluidRenderer.cpp |
| `CreateUnorderedAccessView` | 1 | 1 | dx10SH_RT.cpp |
| `CreateVertexShader` | 1 | 1 | r4.cpp |

### IDXGISwapChain1/3 (8 methods, 20 call sites)

| Method | Calls | Files | Mostly in |
|---|--:|--:|---|
| `SetFullscreenState` | 5 | 2 | dx10HW.cpp, dxRenderDeviceRender.cpp |
| `ResizeBuffers` | 4 | 1 | dx10HW.cpp |
| `GetBuffer` | 3 | 3 | dx10HW.cpp, r4_rendertarget_phase_combine.cpp, r4_R_render.cpp |
| `SetColorSpace1` | 3 | 1 | dx10HW.cpp |
| `Present` | 2 | 1 | dxRenderDeviceRender.cpp |
| `CheckColorSpaceSupport` | 1 | 1 | dx10HW.cpp |
| `GetContainingOutput` | 1 | 1 | xr_effgamma.cpp |
| `ResizeTarget` | 1 | 1 | dx10HW.cpp |

### IDXGIFactory/Adapter/Output (7 methods, 14 call sites)

| Method | Calls | Files | Mostly in |
|---|--:|--:|---|
| `GetDisplayModeList` | 4 | 1 | dx10HW.cpp |
| `EnumAdapters1` | 3 | 1 | dx10HW.cpp |
| `EnumOutputs` | 3 | 1 | dx10HW.cpp |
| `CheckFeatureSupport` | 1 | 1 | dx10HW.cpp |
| `CreateSwapChainForHwnd` | 1 | 1 | dx10HW.cpp |
| `GetGammaControlCapabilities` | 1 | 1 | xr_effgamma.cpp |
| `SetGammaControl` | 1 | 1 | xr_effgamma.cpp |

### ID3D11ShaderReflection* (3 methods, 4 call sites)

| Method | Calls | Files | Mostly in |
|---|--:|--:|---|
| `GetVariableByIndex` | 2 | 2 | dx10ConstantBuffer.cpp, dx10r_constants.cpp |
| `GetConstantBufferByIndex` | 1 | 1 | dx10r_constants.cpp |
| `GetResourceBindingDesc` | 1 | 1 | dx10r_constants.cpp |

### Blob / annotation / resource helpers (6 methods, 42 call sites)

| Method | Calls | Files | Mostly in |
|---|--:|--:|---|
| `GetBufferPointer` | 13 | 4 | r__screenshot.cpp, r4.cpp, dx10ResourceManager_Resources.cpp +1 |
| `GetBufferSize` | 13 | 4 | r__screenshot.cpp, r4.cpp, dx10ResourceManager_Resources.cpp +1 |
| `GetType` | 8 | 3 | dx10SH_Texture.cpp, dx10ConstantBuffer.cpp, dx10r_constants.cpp |
| `GetResource` | 6 | 3 | r4_rendertarget.cpp, r__screenshot.cpp, r4_R_render.cpp |
| `BeginEvent` | 1 | 1 | dx10EventWrapper.cpp |
| `EndEvent` | 1 | 1 | dx10EventWrapper.cpp |

<!-- 400 files scanned, 225 .cpp roots, debug-only identifiers: ['D3DPERF_BeginEvent', 'D3DPERF_EndEvent'] -->
