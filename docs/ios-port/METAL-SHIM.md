# D3D11 on Metal: the shim under R4

R4 (Anomaly's DX11 renderer) keeps its code. Underneath it, `src/Platform/MetalD3D11` provides the D3D11/DXGI
subset it uses, implemented on Metal. The subset comes from the survey in [D3D11-SURFACE.md](D3D11-SURFACE.md).
Branch `claude/metal-d3d11-9647f6`; nothing here depends on engine code.

## What is in the tree

| Path | Purpose |
|---|---|
| `src/Platform/MetalD3D11/include/mtl_d3d11/` | Public headers: `d3d11_metal.h`, `dxgi_metal.h`, `d3d11shader_metal.h` (reflection + D3DCompiler), `d3dx11_metal.h` (DDS), `mtl_shader.h` (shader interface), `mtl_d3d11.h` (device/swap-chain creation, stats, capture), `mtl_wintypes.h` (HRESULT, GUID, `__uuidof`, ...). Enum values match the Windows headers. |
| `src/Platform/MetalD3D11/compat/` | One-line forwarding headers named like the Windows SDK (`d3d11.h`, `d3d11_1.h`, `dxgi1_4.h`, `D3DCompiler.h`, `D3Dx11core.h`, ...). Put this directory on the include path instead of the Windows SDK. |
| `src/Platform/MetalD3D11/src/` | Objective-C++ implementation (ARC), about 6,000 lines. `mtl_context.mm` is the immediate context, `mtl_device.mm` creation and views, `mtl_swapchain.mm`/`mtl_dxgi.mm` presentation, `mtl_shader.mm` the container and reflection facade, `mtl_d3dx11.mm` the DDS loader, `mtl_formats.mm` the format tables. |
| `tools/metal_d3d11_test` | SDL3 test app that drives the shim through the D3D11 API. |
| `tools/d3d11_surface_scan` | Survey scanner. |

CMake targets: `metal_d3d11` (static library) and `metal_d3d11_compat` (interface target: include path + link).

```bash
cmake --preset macos-arm64
cmake --build --preset macos-arm64 --target metal_d3d11_test
MTL_DEBUG_LAYER=1 build/macos-arm64/tools/metal_d3d11_test/metal_d3d11_test.app/Contents/MacOS/metal_d3d11_test \
  --frames 120 --screenshot /tmp/shot.png
```

The app exits 0 when every check passes. For iPad see the end of this file.

## Design

### Object model

- Every interface is a plain C++ class with virtual `AddRef`/`Release`/`QueryInterface` (no real COM). Names are the
  global Windows names (`ID3D11Device`, `IDXGISwapChain1`, `D3D11_BLEND_DESC`, ...), so R4 sources compile unchanged.
- `__uuidof(T)`, `IID_PPV_ARGS(&p)` and the templated `QueryInterface(&p)` are provided. Interfaces get a
  process-unique GUID unless the engine asks for a specific one (reflection, device, annotation, swap chain).
- The immediate context shares the device's reference count, so releasing the device before the context is safe.
- Public structs use `MtlBOOL` (`int32_t`) so layouts match Windows in C++ and Objective-C++ (where `BOOL` is a bool).
  Define `MTL_D3D11_NO_WINTYPES` if the engine's platform layer already provides `HRESULT`, `UINT`, `RECT`, ...

### Memory and resources

The target is Apple GPUs with unified memory (the shim refuses to start otherwise).

| D3D11 resource | Metal storage |
|---|---|
| `DEFAULT` buffer, `IMMUTABLE` buffer | One shared `MTLBuffer`. `UpdateSubresource` goes through the upload ring and a blit. |
| `DYNAMIC` buffer (write-combined) | Shared. `Map(WRITE_DISCARD)` renames the backing: buffers up to 256 KB sub-allocate from a per-frame **constant ring** (256-byte aligned); larger ones rotate through a per-buffer pool of up to 8 backings, each tagged with the command buffer that last used it, and wait only when all are in flight. `NO_OVERWRITE` returns the live pointer. |
| Constant buffers | The same path. R4 refills one dynamic buffer per cbuffer on every flush; a draw binds the current `(MTLBuffer, offset)`, so changing only the offset is a `setBufferOffset`. |
| `IMMUTABLE`/`DEFAULT` texture with initial data, no GPU writes | Shared texture filled with `replaceRegion` (no staging copy, no second allocation). |
| RT, DS, UAV, MSAA, `DEFAULT` without data | Private. Initial data (rare) goes through the upload ring. |
| `DYNAMIC` texture | Three shared textures; `Map(WRITE_DISCARD)` hands out a CPU shadow and `Unmap` writes it into the next free texture. |
| `STAGING` texture | One shared `MTLBuffer` holding every subresource (rows padded to 16 bytes). `CopyResource` in either direction is a buffer<->texture blit, and `Map(READ)` waits for the copy's command buffer. |

Formats: all 44 DXGI formats R4 names plus BC1-BC7, typeless and sRGB variants. Typeless textures get
`MTLTextureUsagePixelFormatView`. **Apple GPUs have no 24-bit depth**, so `D24_UNORM_S8_UINT`/`R24G8_TYPELESS` map to
`Depth32Float_Stencil8` (8 bytes per pixel) and `R32_TYPELESS` with `DEPTH_STENCIL` to `Depth32Float`. An SRV of the depth plane
(`R24_UNORM_X8_TYPELESS`, `R32_FLOAT`) binds the depth texture itself, which Metal lets a shader read through `texture2d<float>`
(verified with Metal API validation); the stencil plane (`X24_TYPELESS_G8_UINT`) is an `X32_Stencil8` view.
`CreateTexture2D` with `MipLevels = 0` builds the full chain, and `D3DX11FilterTexture` regenerates mips with a blit.

`GetStats()` reports live texture and buffer bytes and `MTLDevice.currentAllocatedSize` for the M2 memory work.

### Immediate context: state, pipelines, draws

- All `*Set*` calls only record state (strong references, like D3D). Nothing reaches Metal until a draw.
- A draw builds a **pipeline key** from VS, PS, input layout, the vertex strides, the blend state, the colour/depth/stencil
  formats, the sample count and the topology class. `MTLRenderPipelineState` objects are cached; depth-stencil and
  sampler states are created once with their D3D state object; rasterizer state, viewport, scissor, depth bias, blend
  colour and stencil reference are encoder state, applied when dirty.
- **Input layouts bind late.** `CreateInputLayout` only stores the elements. At pipeline-build time each vertex input in the
  VS reflection is matched by (semantic, index) against the bound layout to fill the `MTLVertexDescriptor`
  (stream *n* is `buffer(16 + n)`, strides come from `IASetVertexBuffers`). Inputs the layout lacks read zeros.
- Per-stage resource binding is driven by the shader reflection: for each cbuffer/texture/sampler/UAV the shader declares,
  the bound D3D object for its D3D slot is bound at the Metal index the reflection gives. Empty slots get placeholder
  textures/samplers/buffers (D3D reads zeros; Metal would otherwise validate-fail). An encoder-side cache skips redundant calls.
- D3D hazard rules are emulated: binding a resource as an SRV unbinds it as a render target and vice versa.
- `DrawIndexed` uses `baseVertex`; instancing and 32-bit indices work although R4 uses neither.
- Compute: `Dispatch` ends the render pass, opens a compute encoder, binds cbuffers/SRVs/samplers and typed texture UAVs and
  dispatches with the reflected `numthreads`.

### Render passes (R4 switches targets constantly)

- `OMSetRenderTargets` records the wanted attachment set only. The Metal render encoder is created by the first draw that
  needs it and reused while draws ask for the same set, so R4's "unbind with `(0,0,0)`, set, draw" idiom costs nothing.
- `ClearRenderTargetView`/`ClearDepthStencilView` never encode a clear. They become **load actions**: the clear is parked per
  attachment and consumed by the next pass that attaches it. R4 clears right after binding, so these are free.
- Clearing an attachment of the *open* pass ends that pass (its contents must be stored first) and parks the clear.
- A parked clear that nothing attached still has to take effect before anything reads the texture (SRV bind, copy, resolve,
  UAV, `Present`). Those paths first materialise it as an empty clear-only pass.
- `CopyResource`, `CopySubresourceRegion`, `UpdateSubresource`, `GenerateMips` and read-backs run in blit encoders;
  `ResolveSubresource` is a load+`StoreAndMultisampleResolve` pass. Each ends the open render pass; counters tell how often.
- Stores are always `Store`: no attachment is discarded or memoryless yet.

Measured in the test app, per frame: G-buffer (MRT + depth), MSAA target, resolve, lighting (HDR accumulation, 3 additive
draws), combine, HUD with depth = **6 render passes** for 29 draws, with the 5 per-frame clears folded and 1 per run
materialised. The self-check `pass economy` binds one target set three times (1 pass) and does two target switches with
clears (3 passes in total, 3 clears folded, 0 materialised).

### Queries

- `EVENT`: `End` stamps the current command buffer. `GetData` without `D3D11_ASYNC_GETDATA_DONOTFLUSH` commits that buffer, so
  R4's spin loop on the sync point cannot deadlock; it returns `S_FALSE` until Metal reports completion.
- `OCCLUSION`/`OCCLUSION_PREDICATE`: each pass that draws while a query is active gets a slot in a per-frame visibility
  buffer (`MTLVisibilityResultModeCounting`); the completion handler sums the slots into the query.

### Swap chain

`CreateSwapChain` (or `IDXGIFactory2::CreateSwapChainForHwnd` with a window registered via `RegisterWindowLayer`) wraps a
`CAMetalLayer` taken from `SDL_Metal_GetLayer`. `GetBuffer(0)` returns one stable `ID3D11Texture2D`; its drawable is acquired
when the first pass attaches it and presented by `Present` (which also advances the rings and throttles to
`framesInFlight`). `R8G8B8A8_UNORM` becomes a `BGRA8Unorm` layer (CAMetalLayer has no RGBA8; channel order is a storage
detail, read-back swizzles). `RequestCapture`/`WaitForCapture` read the presented frame for screenshots.
`IDXGIFactory`/`Adapter1`/`Output` report one Apple GPU and one display (`SetDisplayMode` sets what `GetDisplayModeList` returns).

### Shader interface

Defined in `mtl_shader.h`; the HLSL->MSL translator produces it. R4's "bytecode" is a container (MSL source or `.metallib`
plus reflection). `D3DCompile` calls a hook (`SetShaderCompiler`) that returns the container; `D3DReflect` and
`D3DGetInputSignatureBlob` read it, and `ID3D11ShaderReflection` answers exactly the queries `R_constant_table::parse`
and `dx10ConstantBuffer` make (checked by the test app). Binding conventions: cbuffer `b#` -> `[[buffer(b#)]]`, vertex streams
`[[buffer(16+n)]]`, SRV `t#` -> `[[texture(t#)]]`, UAV `u#` -> `[[texture(64+u#)]]`, sampler `s#` -> `[[sampler(s#)]]`.
Engine assumptions (cbuffer index equals slot, original HLSL names, HLSL packing, row-major matrices) are listed in the header.

## Coverage against the survey

| Survey item | Status |
|---|---|
| Device, immediate context, `D3D11CreateDevice`, `ID3D11Device1`/`Context1`, `ID3DUserDefinedAnnotation` | Done. Feature level 11_0 is reported. Annotations map to Metal debug groups. |
| Buffers: VB/IB/CB, `DISCARD`/`NO_OVERWRITE`, `UpdateSubresource`, staging | Done (ring + rename pools). |
| Textures 1D/2D/3D/cube/arrays, mips, BC1-7, RT, DS, MSAA, UAV, dynamic, staging | Done. 2D, cube, 3D, MSAA, dynamic, staging and BC1 are exercised by the test app; 1D and array textures are implemented but untested. |
| SRV/RTV/DSV/UAV, samplers | Done, including depth/stencil plane views and typeless reinterpretation. |
| Blend/depth/rasterizer states, input layouts | Done. `SRC1_*` dual-source blend factors map to Metal's `Source1*`. |
| `Draw`, `DrawIndexed`, instanced draws, viewport, scissor, clears, copies, resolve, `Flush`, `ClearState` | Done. |
| Queries (`EVENT`, `OCCLUSION`) | Done. Timestamp/pipeline-statistics queries fail with `E_NOTIMPL` (R4 does not create them). |
| Compute (HDAO) | Done for typed texture UAVs; buffer UAV binding exists, clears of texture UAVs do not. |
| Swap chain, colour space, resize | SDR done; `ResizeBuffers` supported; fullscreen/`ResizeTarget` are no-ops. |
| D3DCompiler: `D3DReflect`, `D3DGetInputSignatureBlob`, `D3DDisassemble`; `D3DCompile` via hook | Done. |
| D3DX11: image info, `CreateTextureFromMemory` (DDS: BC1-7, legacy RGB(A), cube, volume, `FirstMipLevel`, `IMMUTABLE`/`STAGING`), `FilterTexture`, save DDS/BMP | Done. PNG/JPG saving and filtered `LoadTextureFromTexture` resizing are not. |
| Geometry shaders, hull/domain shaders | Not supported: `Create*Shader` returns `E_NOTIMPL`. |
| HDR10, tearing flags, gamma ramps, PerfHUD | Not implemented / ignored by design. |

## Known gaps and risks

1. **Tessellation and geometry shaders.** Default Anomaly does not use tessellation (`r4_enable_tessellation`, off) and only the
   3D-fluid blenders use a GS (`fluid_array`). R4 initialises `FluidManager` unconditionally, so the Apple build must skip it
   (it also needs 3D render targets with `WSize > 1`, which the shim does not provide).
2. **Pipeline creation is synchronous.** Each new (shader, state, target format) combination costs a Metal compile at first
   draw. Anomaly has thousands of permutations; expect hitches until the translator ships `.metallib` payloads and a
   `MTLBinaryArchive`/pre-warm pass exists. The pipeline cache itself works (7 created, 1,853 hits in the test).
3. **`MipLODBias` is ignored.** Metal samplers have no LOD bias; the translator would have to add it in MSL.
4. **`SampleMask` is ignored**; `D3D11_STANDARD_MULTISAMPLE_PATTERN` is accepted and does nothing.
5. **Depth bias units.** D3D biases are tuned for 24-bit UNORM; Metal applies them to a 32-bit float depth. Shadow-map bias
   values will need a visual check.
6. **Mid-pass clears cost a pass break** (the previous contents have to be stored). R4's clears follow a target switch, so
   this is rare; a clear-by-draw fallback is the fix if profiling shows otherwise.
7. **Every render-target store is `Store`.** No `DontCare` for depth that is never sampled, no memoryless MSAA/transient
   targets. This is the first place to look when bandwidth or memory is tight on the iPad.
8. **Staging textures double memory transiently.** R4 loads DDS as `STAGING` and copies to `DEFAULT` on first use. On iPad set
   `no_ram_textures` so `texture_load` creates `IMMUTABLE` textures directly (one allocation, uploaded with `replaceRegion`).
9. **Immutable textures stay in the CPU-swizzled shared layout.** `optimizeContentsForGPUAccess` is not issued yet.
10. **Render-pass breaks for copies.** R4 issues up to 36 whole-target `CopyResource` per frame; each ends the open pass.
    Fusing them into a final blit run or replacing them with sampling is a later optimisation.
11. **Implemented but not exercised by the test app:** `DrawInstanced`/`DrawIndexedInstanced`, buffer SRVs/UAVs, 1D and array
    textures, `ClearUnorderedAccessView*` (buffer fill only), occlusion queries spanning a pass break, `Present` after
    `ResizeBuffers`, HDR.
12. **Threading.** The context is single-threaded, like D3D. Resource creation is safe from other threads except a
    `DEFAULT`+`RENDER_TARGET`/`UAV` texture created with initial data, which uses the context's upload path.
13. **Windows-isms outside D3D11** (D3D9 enums/structures, D3DX9 math, `DWORD`, `HWND` plumbing) are not in this library; see
    D3D11-SURFACE.md section 7.

## Integration steps for R4

1. **Build.** For the Apple xrRender target, add `metal_d3d11_compat` to the include path before anything else and link
   `metal_d3d11`. `#include <d3d11_4.h>`, `<dxgi1_5.h>`, `<D3DCompiler.h>`, `<D3Dx11core.h>` and `<D3DX10Tex.h>` then resolve to the shim.
   If xrCore already defines `HRESULT`, `UINT`, `BOOL`, `RECT`, ... compile the shim with `MTL_D3D11_NO_WINTYPES`.
2. **D3D9 vocabulary.** Provide a small `d3d9types` header (the `D3DFMT_*`, `D3DRS_*`, `D3DPT_*`, `D3DBLEND_*`, ... listed in the survey)
   and the `D3DX` math/declaration helpers; they are shared with other renderers and outside this library.
3. **Device.** Keep `dx10HW.cpp` but replace its Win32 parts: use SDL for window mode and display lists. Before
   `CreateSwapChainForHwnd` call `mtl_d3d11::RegisterWindowLayer(m_hWnd, SDL_Metal_GetLayer(view))`; the rest of
   `CreateDevice` (`D3D11CreateDevice`, `QueryInterface` to `ID3D11Device1`/`Context1`, swap chain, annotation) works as is.
   Call `SetDisplayMode` from SDL's display mode so `fill_vid_mode_list` has something to list.
4. **Shaders.** Install the translator with `mtl_d3d11::SetShaderCompiler`. `r4.cpp::create_shader`, `D3DReflect` and
   `D3DGetInputSignatureBlob` then work unchanged, and the engine's shader cache stores containers. Do not load `hs`/`ds`/`gs`
   shaders (tessellation off, fluid skipped).
5. **Textures.** `CRender::texture_load` works unchanged through `D3DX11GetImageInfoFromMemory`/`CreateTextureFromMemory`.
   Enable `no_ram_textures` on iPad. Provide PNG/JPG encoding (ImageIO) for screenshots.
6. **Quality settings for the first boot:** `r3_msaa` off, HDR10 off, tessellation off, HDAO off (or test the compute path first).
7. **Diagnostics.** `D3D11_CREATE_DEVICE_DEBUG` (or `DeviceDesc::validation`) enables shim logging; run with `MTL_DEBUG_LAYER=1` and
   `MTL_SHADER_VALIDATION=1` during bring-up; `mtl_d3d11::GetStats` reports passes, clears, pipeline hits and memory.
8. **First milestone check** (main menu, then Cordon): watch `passBreaksByClear`/`passBreaksByCopy` in the stats and the
   pipeline-creation count per frame; both should be near zero in steady state.

## Test app results

macOS (Apple M2 Pro, `MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=1`, 2560x1600): all checks pass.

| Check | Result |
|---|---|
| Solid colour, 50% alpha blend, point-sampled texture | exact (blend 0,128,127) |
| Offscreen RT cleared long before it is sampled | exact |
| 4x MSAA target -> `ResolveSubresource` -> `CopyResource` | exact |
| Depth test, G-buffer depth reused as DSV after being sampled as SRV | pass |
| Compute shader writing a UAV texture, sampled afterwards | exact |
| Cube map faces, 3D texture slice, `UpdateSubresource` on a `DEFAULT` texture | exact |
| `STAGING` -> `CopyResource`; `DYNAMIC` `Map(DISCARD)`; BC1; DDS via `D3DX11CreateTextureFromMemory` (top mip skipped) and via staging | pass |
| `ID3D11ShaderReflection` answers the R4 constant-table queries | pass |
| Occlusion: hidden cube 0 samples, visible cube about 96k samples | pass |
| Pass economy (above) | pass |

The screenshot shows the deferred scene: a brick wall and four bricked cubes on a checkered ground lit by a sun and two
moving coloured point lights, depth fog toward a sky gradient, a translucent teal pane intersecting the cubes (depth-tested
against the G-buffer), a HUD panel with a progress bar and a checker swatch at the top left, and a row of sixteen solid-colour
swatches along the bottom, one per feature check.

iPad: see below.
