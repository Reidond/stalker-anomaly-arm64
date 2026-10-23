# The R4 renderer on Metal

Anomaly's DX11 renderer (`src/Layers/xrAPI`, `xrRender`, `xrRenderDX10`, `xrRenderPC_R4`) is kept as it is and runs on the
D3D11 subset implemented over Metal in `src/Platform/MetalD3D11` ([METAL-SHIM.md](METAL-SHIM.md)). Its shaders are translated
from HLSL to MSL at run time by `src/Platform/ShaderXlat` ([SHADERS.md](SHADERS.md)). This document is about the glue between
the three, what was changed in the layers, what is switched off, and how to check it without the engine.

```
CRender / CHW / CResourceManager (R4, unchanged logic)
   │ ID3D11*, IDXGI*, D3DCompile, D3DReflect, D3DX11*          -- headers named like the Windows SDK's (metal_d3d11_compat)
   ▼
metal_d3d11 (Objective-C++)  ◄── shaderxlat_metal::install()  -- D3DCompile hook, container = MSL + reflection
   │ MTLDevice, MTLRenderPipelineState, CAMetalLayer            ▲
   ▼                                                            │ HLSL + R4 macro list
Metal                                                      ShaderXlat (vkd3d-shader → SPIR-V → SPIRV-Cross → MSL)
```

## Building

Both layers are opt-in modules (`cmake/targets/xrAPI.cmake`, `xrRender_R4.cmake`). The source lists still come from
`xrAPI.vcxproj` and `xrRender_R4.vcxproj` (227 files, plus `AppleWindow.cpp` and `r4_shader_xlat.cpp` listed in `xrRender_R4.cmake`), so the Windows build and CMake stay in step.

```bash
cmake -S . -B build/r4 -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
      -DANOMALY_BUILD_XRAPI=ON -DANOMALY_BUILD_XRRENDER_R4=ON
cmake --build build/r4 --target luajit_build        # once: the Lua headers
cmake --build build/r4 --target xrAPI xrRender_R4   # libxrAPI.a, libxrRender_R4.a (no engine needed to compile)

cmake -S . -B build/r4-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DANOMALY_BUILD_XRAPI=ON -DANOMALY_BUILD_XRRENDER_R4=ON
cmake --build build/r4-ios --target xrAPI xrRender_R4 --config Debug -- CODE_SIGNING_ALLOWED=NO
```

`xrRender_R4` links `xrAPI`, `luabind`, LuaJIT, SDL3 (window glue) and `ShaderXlatMetal`; `Anomaly::EngineConfig` brings the Metal
shim's headers. `xrCPU_Pipe` is a link dependency that is skipped while its module is off. The archives are not linkable into a
program yet: they need xrEngine, xrCDB and xrParticles (see "Link-time needs").

## Header collisions (task 1)

The Windows SDK defines the `D3D11_*` names as aliases of the `d3dcommon.h` ones (`typedef D3D_PRIMITIVE_TOPOLOGY
D3D11_PRIMITIVE_TOPOLOGY`, `#define D3D11_PRIMITIVE_TOPOLOGY_X D3D_PRIMITIVE_TOPOLOGY_X`), and the Metal shim copies that
layout. `xrRenderDX10/DXCommonTypes.h` mapped the other way (`#define D3D_PRIMITIVE_TOPOLOGY_X D3D11_PRIMITIVE_TOPOLOGY_X`). A
conforming preprocessor stops a self-referencing macro at the second round, so a `D3D11_` name expanded to itself and was
undeclared (68 identical errors at `dx10R_Backend_Runtime.h:294`; MSVC's older preprocessor tolerated it).

Fix, in the layer's header: the reverse aliases are skipped when the D3D11 header already defines the forward ones
(`#ifndef D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED`). On Windows the same guard skips the block when the Windows SDK's `d3d11.h` has
those macros, which gives the same meaning as before. The shim keeps no knowledge of R4.

Audit of every `D3D_X → D3D11_X` alias in `DXCommonTypes.h` against the shim headers:

| Family | Shim | Result |
|---|---|---|
| `PRIMITIVE_TOPOLOGY_*` | typedef + forward macros (SDK layout) | guarded in `DXCommonTypes.h` |
| `SRV_DIMENSION_*` | **was two unrelated enums** (`D3D_SRV_DIMENSION` in `d3d11shader_metal.h`, `D3D11_SRV_DIMENSION` in `d3d11_metal.h`), which made `mtl_shader.h` fail after the aliases | shim now has the `d3dcommon.h` enum and the SDK-style `D3D11_` typedef and macros; guarded in `DXCommonTypes.h` |
| `COMMONSHADER_SAMPLER_SLOT_COUNT`, `REQ_TEXTURE2D_U_OR_V_DIMENSION`, `STANDARD_MULTISAMPLE_PATTERN` | numeric macros | fine (the alias expands to a number) |
| `CBUFFER_TYPE`, `DRIVER_TYPE` | typedef of the `D3D_` type | fine (same type declared twice) |
| everything else | `D3D11_` enumerators only | fine |
| `QUERY_SO_*` (8 names) | missing | harmless: the macro is never expanded |

Other things MSVC accepts that clang does not, fixed in the layer sources (all behaviour-neutral on Windows): `L#Name` in
`PIX_EVENT` (now `L"" #Name`), missing `typename` (`ShaderResourceTraits.h`, `ResourceManager*.cpp`,
`dx10ResourceManager_Resources.cpp`), `friend class CSkeletonX` without a declaration (`SkeletonCustom.h`),
`type_info::raw_name()` (`Blender_Palette.cpp`), `_time32` (`Blender.cpp`).

## Platform glue

| File | What it does on Apple |
|---|---|
| `Layers/xrRender/AppleWindow.{h,cpp}` | `Device.m_hWnd` is the engine's `SDL_Window*` (created with `SDL_WINDOW_METAL`). `AttachMetalLayer` makes the SDL Metal view once, hands its `CAMetalLayer` to `mtl_d3d11::RegisterWindowLayer` and sets the shim's display mode from SDL; `GetClientSize`, `GetDisplaySize`, `ApplyWindowMode`, `Raise/Minimize/Restore` replace the Win32 calls. On iOS the window always covers the screen and only the drawable size is used. |
| `xrRenderDX10/dx10HW.cpp` | `#if XR_PLATFORM_APPLE` branches: adapter 0 / output 0 instead of `MonitorFromWindow`, no NVIDIA PerfHUD search, `AttachMetalLayer` before `CreateSwapChainForHwnd`, SDL window sizing, `exit(1)` instead of `TerminateProcess`. The DXGI code (swap chain, colour space, `ResizeBuffers`, annotation) is unchanged. |
| `xrRender/dxRenderDeviceRender.cpp` | `SwitchOutputMonitor` returns false with a log line (one display). |
| `xrRenderPC_R4/r2_test_hw.cpp` | the hardware probe creates a device without a window. |
| `xrRender/HWCaps.cpp` | NvAPI/AGS vendor paths are compiled out; one GPU. |
| `xrRenderDX10/dx10SH_Texture.cpp` | `.avi` textures (VFW) are compiled out; Theora and sequence textures stay. |
| `xrRender/ResourceManager.cpp` | `DeferredUpload/Unload` use `xr_parallel_foreach` (GCD) instead of TBB. |

Contract with the engine session (xrEngine, SDL): `Device.m_hWnd` is the `SDL_Window*`, the engine does **not** create the Metal
view itself (a second view on the same window would stack), and it provides `GetMonitorResolution` / `GetMonitorPosition`
(`dx10HW.cpp` calls them). ReShade (`use_reshade`, `init_reshade`, `unregister_reshade`, `render_reshade_effects`) stays
referenced from `dx10HW.cpp`; the engine side stubs it (M1-AUDIT section 4). The cursor grab (`ClipCursor`, `ShowCursor`) is left
to the input layer.

## Shaders: D3DCompile, the macro list and the caches (task 2)

`CRender::create()` calls `r4_install_shader_xlat()` (`xrRenderPC_R4/r4_shader_xlat.cpp`) first. That installs the
`ShaderXlatMetal` bridge (`Platform/ShaderXlat/include/ShaderXlat/MetalBridge.h`) as the shim's D3DCompile hook:

1. `D3DCompile(source, defines, includer, "main", "ps_5_0", ROW_MAJOR)` arrives with R4's own macro list. The bridge turns it into
   ShaderXlat `Macro`s and the `ID3DInclude` (R4's `includer`, reading through the virtual file system) into an `IncludeProvider`
   (`/` becomes `\` again for the engine).
2. ShaderXlat translates (`TranslationCache` on disk) and the bridge maps the reflection to the shim's container:
   `cbuffers[i].d3dSlot == i` (R4 binds the cbuffer at reflection index *i* to `b<i>`, holes get placeholders), original names and
   D3D packing, vertex inputs that survive in the MSL, `SV_Target` count, depth/comparison flags.
3. `D3DReflect`, `D3DGetInputSignatureBlob` and `Create{Vertex,Pixel,Compute}Shader` read the container; the shim compiles the MSL
   with Metal in `Create*Shader`.

The macro list is unchanged R4 code: the block of `CRender::shader_compile` that builds `defines[]` and the cache id moved verbatim
to `r4_shader_macros.inl`, `#include`d in place (and by the warm-up tool).

Caches, both under the app shell's `$shader_cache$` (`Library/Caches/Anomaly/shaders`; `$app_data_root$\shaders_cache` when the
alias is missing):

| Cache | Path | Content | Keyed by |
|---|---|---|---|
| engine's own | `$shader_cache$\r4\<shader>.<ps/vs>\<cache id>` | the container the hook returned | the macro-encoded cache id and `getShaderSourceCrc32` (source plus includes), as on Windows |
| translation | `$shader_cache$\xlat` | MSL and reflection | source, macros, options, library version; every include is re-hashed |

A container from an older shim fails `D3DReflect`, R4 falls through to `D3DCompile`, and the translation cache answers, so a format
change costs a few milliseconds per shader, not a re-translation. Geometry, hull and domain shaders fail with `E_NOTIMPL` and
one log line (`! Metal: gs_5_0 shader skipped (no geometry stage on Metal)`, at most 8 lines).

## Switched off or changed on Apple

What was checked per option, and the cost of each, is in [RENDER-QA.md](RENDER-QA.md).

| Feature | Why | How |
|---|---|---|
| 3D fluid simulation (`FluidManager`, volumetric smoke volumes) | needs geometry shaders (`fluid_array*.gs`) and layered 3D render targets | `CRender::create` sets `o.volumetricfog = FALSE` with a log line; `Load3DFluid` and `FluidManager.Initialize` already return early on it |
| Tessellation (`r4_enable_tessellation`, default **on**) | no hull/domain stages | `o.dx11_enable_tessellation = FALSE` with a log line if the flag was set |
| NvAPI, AGS, NVIDIA PerfHUD | vendor SDKs | compiled out |
| `.avi` textures | VFW | compiled out |
| Multi-monitor `vid_monitor` switch | one display | returns false |
| TBB | not ported | GCD |
| Gamma ramp (`SetGammaControl`) | no equivalent on `CAMetalLayer` | accepted and ignored |
| MSAA | 2x and 4x work; the GPU refuses 8x | `r3_msaa` is lowered until `CheckMultisampleQualityLevels` accepts it, with a log line (RENDER-QA.md) |
| HDR10 | off by default, not exercised | unchanged code paths; the shim implements the 10-bit layer but nothing here tests it |
| Screen Space Shaders pack | its 34 shader files are not in the game data | the features are off; one log line lists them; the far terrain falls back to the near terrain's shader (RENDER-QA.md) |

## Shim gaps closed in this change

Against what R4 calls (see [D3D11-SURFACE.md](D3D11-SURFACE.md)); the rest of METAL-SHIM.md's list is still open.

* Pixel inputs the vertex shader never writes: the pixel function is rebuilt from MSL with those inputs as zero locals
  (`Context::fragmentFunctionFor`); a pixel input of another width than the vertex output at the same location (`waterd`: FOG packed
  next to TEXCOORD2) is rebuilt with the vertex shader's type and a swizzled or zero-extended local (`AdaptFragmentInputs`). Covered
  by the warm-up tool, which lists the four stock pairs that need it and compiles the adapted variants, and `metal_d3d11_headless_test`
  on real and synthetic MSL. A pair that needs it is normal, counted in the render stats and not logged.
* Render targets and depth buffers are cleared to zero when they are created, as in D3D11 (a scope's fake-scope target was read
  before any write and showed magenta noise).
* Screenshots: `D3DX11SaveTextureToMemory/File` encode JPG, PNG, TIFF and GIF through ImageIO (R4's `screenshot` token 0/1).
* `D3DX11LoadTextureFromTexture` between different sizes (savegame and level thumbnails) filters on the CPU.
* `D3D11_SRV_DIMENSION` is the `d3dcommon.h` enum, `LPD3DBLOB` / `LPD3D10BLOB` exist.

Still open and relevant to R4: `MipLODBias` is ignored (`tf_mipbias` default 0), depth bias units differ from D24, pipeline
creation is synchronous (the first frame in a new area will hitch; the warm-up numbers below are the cost of one pass), gamma.

### Shim additions (commit `shim: ...`)

The shim owner session is archived, so the orchestrating session had this session add the D3D9 names R4 needs to `src/Common`
itself, in a separate commit (additive, guarded by `XR_D3D9COMPAT_QUERY_RTYPE` / `XR_D3DX9COMPAT_VECTOR_OPS`):

* `d3d9compat.h`: `D3DBLEND_SRCCOLOR2 = 16`, `D3DBLEND_INVSRCCOLOR2 = 17`, `D3DRESOURCETYPE` (`D3DRTYPE_TEXTURE`, ...),
  `D3DQUERYTYPE` (`D3DQUERYTYPE_OCCLUSION`, ...).
* `d3dx9compat.h`: `+ - * /` and compound assignment on `D3DXVECTOR2` and `D3DXVECTOR4`, `float * D3DXVECTOR2/3/4`
  (`r2_R_sun.cpp`).

## tools/r4_shader_warmup (task 4)

```bash
cmake --build build/r4 --target r4_shader_warmup
build/r4/tools/r4_shader_warmup/r4_shader_warmup [--game-dir DIR] [--passes 2] [--cold] [--no-metal] [--filter TEXT]
                                                  [--dump-msl DIR] [--json FILE] [--list] [--strict]
```

It reads `game/Anomaly` (read-only; its own `fsgame.ltx` redirects `$app_data_root$` to `build/.../run`), mounts the archives
through xrCore's file system and requests what the engine requests with the stock data: the `r_Pass("vs", "ps")` calls of the R4,
xrRender and xrRenderDX10 blenders plus the `shader:begin("vs", "ps")` lines of the `r3\*.s` scripts, and the five skinning
variants of every vertex shader that uses `SKIN_*`.

**Why `CRender::shader_compile` itself is not called.** It is a member of `CRender`, and `CRender` needs the whole engine: the
`Device` and `Console` globals, `CResourceManager`, `g_pGamePersistent`, `CEnvironment`, `CGameFont`, the spatial DB, xrCDB, xrParticles.
Linking the finished archive reports 232 undefined symbols (below). What the tool keeps of the real path:

* the macro list and cache id are `r4_shader_macros.inl`, the same text `shader_compile` compiles, evaluated with the default settings
  (stand-ins for `o`, `HW.Caps`, the `ps_*` console variables and the options `CRender::create` derives, with the values and source
  lines in `tools/r4_shader_warmup/r4_macros.cpp`): 2048 shadow maps with HW PCF, FP16, no MSAA, no HDR10, plain SSAO, soft water
  and particles, DoF, steep parallax, motion blur, water reflections, SSFX at its console defaults;
* sources and includes come through `FS.r_open("$game_shaders$", "r3\\...")` with R4's `includer` rules and entry/target selection
  from `_CreateVS/_CreatePS`;
* `D3DCompile`, `D3DReflect`, `D3DGetInputSignatureBlob` and `Create{Vertex,Pixel,Compute}Shader` are the shim's, so the hook, the
  container and the Metal library compile are exercised;
* every reflection is checked against what `R_constant_table::parse` accepts (variable types, vector/matrix classes and sizes,
  `uint` only on `bus_*`, `BindCount == 1`, cbuffer sizes); vertex/pixel pairs are checked for unwritten inputs.

Results on an M2 Pro (macOS 27, Debug build, other sessions building on the same machine), `--cold --passes 2`:

| | Cold (empty translation cache) | Warm |
|---|---|---|
| Requests | 334: 253 ok, 76 missing from the data, 5 skipped on purpose | same |
| Wall time, 10 threads | 6.1 s | 0.1 s |
| `D3DCompile` (HLSL → container), sum / mean / p50 / p90 / p99 / max | 64.1 s CPU / 253 / 121 / 535 / 3,369 / 3,620 ms | 0.7 s / 2.8 / 1.4 / 7.1 / 18.9 / 27.9 ms (253 cache hits) |
| `D3DReflect` + signature, p50 / p99 | 0.03 / 5.6 ms | 0.03 / 0.33 ms |
| `Create*Shader` (Metal compiles the MSL), sum / mean / p99 | see below | 28 ms / 0.1 / 1.3 ms |

The Metal compile depends on the system's shader cache: with the MSL never seen before the first run measured 14.0 s for the 253
shaders (mean 55 ms, p99 230 ms, max 413 ms; the 15 `accum_sun` shaders took about 1 s each on a loaded machine while Metal's
compiler service started up); with the MSL cached it is 0.2 s. A cold first launch therefore costs roughly 64 s + 14 s of CPU, 8 to
10 s of wall time on all cores, once.

Outcome details:

* **76 missing**: requested by the blenders and scripts but in neither `shaders.db0` nor the repo overlay: the 32 Screen Space
  Shaders addon files (`ssfx_*`) and 44 DX9 leftovers (`lmap*`, `impl_*`, `null.*`, ...). The original engine also lacks them
  (SHADERS.md, "Missing files the engine requests").
* **5 skipped**: `fluid_array.gs`, `fluid_array_oobb.gs`, `fluid_array_dyn_oobb.gs` (no geometry stage on Metal) and
  `fluid_draw_texture.ps`, `fluid_obststaticbox.ps` (shader bugs in the 3D fluid simulation, which is switched off here).
* **R4 contract**: all 253 reflections satisfy `R_constant_table::parse`.
* **Pairs**: 168 vertex/pixel pairs compile on both sides (71 involve a missing file); none leaves a pixel input unwritten with the
  default settings. A stress test drops the interpolants of every pixel shader (all of them, and the last one): 272 patched
  variants, all compile in Metal.


## Render scale and upscaling

The 3D scene renders at a fraction of the output size (`r__render_scale`, 0.5 to 1.0) and is brought to the output size before the
UI is drawn, so the HUD, the menus and the text stay at the display's native resolution. Spatial MetalFX
(`MTLFXSpatialScaler`) or a bilinear pass does the upscaling. At 1.0 nothing changes: no extra target, no extra pass, the same
code path as before.

### Where the 3D scene ends

One frame of the engine, as R4 runs it (`CRenderDevice::on_idle`):

```
Device.Begin()                               back buffer bound (RCache.OnFrameBegin: pBaseRT + the UI depth), native viewport
seqRender.Process()
  CLevel::OnRender()                          (priority 0x22222222: the level; the other seqRender users run after it)
    3D PDA: UI drawn into the back buffer, RenderToTarget(rtPDA) copies it to rt_ui_pda      <- output size
    inherited::OnRender()  =  Render->Calculate(); Render->Render()
      CRender::Render():  render_main, phase_scene_prepare / begin / end, lights,
        phase_combine:  AO, skybox, combine_1, water, rain, forward geometry (HUD weapons: r_dsgraph_render_hud),
                        bloom, DoF, sunshafts, LUT, gas mask, night vision, SMAA, combine_2 (tone mapping, AA),
                        phase_pp()   <- the last scene pass: colour grading, noise, blur; writes the back buffer   *** the split ***
    Game().OnRender(), BulletManager().Render()      bullet tracers: 3D, depth-tested against the scene
    Render->FinishScene()                             <- new hook, called here: the scene is complete
    RenderToTarget(rtSVP) in a scope-picture frame     copies the finished scene into rt_secondVP
    HUD().RenderUI()                                   HUD, crosshair, hit marker, dialogs: UI
  console, cursor, stats, ImGui (seqRender, lower priorities)                                 UI
Device.End()  ->  CRender::FinishScene() (safety net)  ->  Present
```

`phase_pp()` is the split point: everything before it is the scene (G-buffer, lighting, bloom, `rt_Generic*`, SMAA, tone mapping),
everything after it draws into the back buffer. With a render scale `phase_pp()` writes `rt_SceneOut` (scene size) instead, and
`CRender::FinishScene()` → `CRenderTarget::phase_upscale()` (`r4_rendertarget_phase_upscale.cpp`) turns it into the back buffer's
image. The call sits in the engine, not at the end of `CRender::Render()`, because bullet tracers (and other 3D that the level draws
after `Render()`) belong to the scene: they are depth-tested against `pBaseZB`. `FinishScene()` is a virtual of
`IRender_interface` with an empty default; the call sites are `CLevel::OnRender` (before `HUD().RenderUI()`),
`CMainMenu::OnRender` (menu over a scene) and `dxRenderDeviceRender::End()` (a frame nobody finished still reaches the screen).

### Two sizes

| | Size | Members |
|---|---|---|
| **Scene** | `HW.SceneW() x HW.SceneH()` = output x scale, rounded down to an even number | G-buffer, accumulators, `rt_Generic_*`, bloom/blur/DoF/SMAA chains, `rt_Position`, the scene's depth `HW.pBaseZB`, `rt_SceneOut`, `rt_secondVP`, every full-screen quad's vertices and viewport, the jitter/noise tiling, `screen_res` and `pos_decompress_params` while a scene target is bound |
| **Output** | `Device.dwWidth x Device.dwHeight` = the swap chain | `HW.pBaseRT`, `rt_Upscaled` (MetalFX's destination), `rt_ui_pda`, the async screenshot texture, the UI, fonts (the font atlas is chosen by `Device.dwHeight`), the cursor, the loading screen, the console, the main menu, screenshots |

Without a render scale `SceneW() == Device.dwWidth`, `pUIZB` is `pBaseZB`, and the code does what it did. The other renderers' `CHW`
(R2, R3) have `SceneW()` too, equal to their back buffer, so the shared files that now say `HW.SceneW()` should still build there (the
Windows projects were not compiled in this session).

* `Device.dwWidth/dwHeight` stay the output size, which is what everything outside the renderer wants: UI scaling
  (`ui_core`), cursor and touch mapping, `GetClientSize`, the font and HUD code, `render_device` in Lua.
* **Shader constants.** `screen_res` (pixel to clip conversion of the pre-transformed quads, and the texel size most pixel shaders read)
  is the output size while the back buffer is the bound target and the scene size otherwise (`cl_screen_res`: one compare of
  `RCache.get_RT()` with `HW.pBaseRT`). `pos_decompress_params*` are scene only.
* **Viewports** follow `CRenderTarget::dwWidth/dwHeight` as before (`rmNormal`); `phase_scene_prepare` now ends with `rmNormal()` so
  the scene starts with the scene's viewport, and `rebind_output()` sets the output's again.
* **Depth** (`CHW::UpdateViews`): `pBaseZB` is the scene's and has the scene's size. What binds `pBaseRT` (`OnFrameBegin`, the loading
  screen, `render_menu`, the upscale pass) uses `HW_UIZB`: with a render scale that is no depth buffer at all (the UI is 2D; a
  2732x2048 `D24S8` is 22 MB and a load and a store in every UI pass), without one it is `pBaseZB` as before. Attachments of one pass
  have to be one size, so `pBaseZB` cannot go with `pBaseRT`. `phase_occq`, which used `pBaseRT` as a dummy colour target next
  to the depth, runs depth-only when scaled.
* **HUD weapons and 3D models** are drawn by `r_dsgraph_render_hud` in the forward stage, so they are scene content at the scene's
  resolution; the weapon's own screens (`r_dsgraph_render_hud_ui`) too.
* **Main menu.** Its UI is drawn through the "PP-UI" path into the generic targets, which are scene sized. With a render scale
  `render_menu()` draws it straight into the back buffer instead (cleared to black first), so the menu is at the output's
  resolution; the distortion mask (`OnRenderPPUI_PP`) is not applied then.
* **3D PDA.** The PDA's screen is UI drawn into the back buffer before the scene and copied into `rt_ui_pda`, which is
  output-sized.
* **Scope picture (second viewport).** It is a whole scene rendered for the lens texture; with a render scale its result stays in
  `rt_SceneOut` (never upscaled, never presented) and `RenderToTarget(rtSVP)` copies from there.
* **Screenshots** read the back buffer after the UI, i.e. the output size: `screenshot` and the thumbnails.

### The upscaler (MetalD3D11)

`src/Platform/MetalD3D11/include/mtl_d3d11/mtl_upscale.h`, implemented in `mtl_upscale.mm` (links MetalFX):

```cpp
bool mtl_d3d11::SpatialUpscalerAvailable(ID3D11Device*);                       // MTLFXSpatialScalerDescriptor supportsDevice:
bool mtl_d3d11::EncodeSpatialUpscale(ID3D11DeviceContext*, ID3D11Texture2D* src, ID3D11Texture2D* dst);
```

* `EncodeSpatialUpscale` ends the open render pass (`Context::externalCommandBuffer`: lands the folded clears of both textures, ends the
  encoder, hands out the command buffer), encodes an `MTLFXSpatialScaler` and returns. The next draw opens a new pass that loads
  what is there. One scaler is cached and rebuilt when a size or format changes.
* **Colour processing mode: perceptual.** R4's output is LDR in gamma space (tone mapping and the display transform happen inside
  `combine_2`), which is what the perceptual mode expects.
* **Formats and usage.** Input `rt_SceneOut`: `R8G8B8A8_UNORM` (`R10G10B10A2` with HDR10), render target + shader resource.
  Output `rt_Upscaled`: the same format at the output's size, private storage, render target + shader resource. The scaler states the
  usage it needs (`colorTextureUsage`, `outputTextureUsage`) and the shim checks it before encoding: a texture without it is refused with
  one log line instead of a Metal validation error. On the M2 Pro and the M3 that is `RenderTarget` for the output and `ShaderRead` for
  the input, so no unordered-access bind (and no `ShaderWrite` usage, which would cost the texture its lossless compression) is needed;
  a GPU that asks for more gets the log line and the bilinear path.
  The drawable cannot be the output: a `CAMetalLayer` drawable is framebuffer-only, so `phase_upscale()` draws `rt_Upscaled` into
  the back buffer as a pixel-for-pixel quad (`upscale_present_mfx.s`, one pass; the back buffer is cleared first so the drawable's old
  contents are not loaded).
* **Fallback.** No MetalFX (framework or GPU), a refused texture, or `r__upscaler bilinear`: `phase_upscale()` draws `rt_SceneOut`
  into the back buffer through `smp_rtlinear` (`upscale_present.s`). At scale 1.0 none of this exists. Off Apple `CHW::UpdateSceneSize`
  forces the scale to 1.0, so the Windows build runs the old path only.
* **Sharpening.** `upscale_present.ps` has a 5-tap contrast-adaptive sharpen (`r__upscale_sharpness`, 0 to 1, 0 = off = no extra
  samples). MetalFX has no sharpness setting of its own, so it applies to both paths.
* GPU time of the upscale is in the command buffer's total (`gpu ms` in the shell log); MetalFX is encoded between two render
  passes, so it shows in neither pass counter but in `passBreaksByCopy`.

### Settings

| Console variable | Range, default | Applied |
|---|---|---|
| `r__render_scale` | 0.35 to 1.0, 1.0 (0.5 to 1.0 in the MetalFX measurements; the settings session lowered the minimum for the iPad profile) | at once, with a device reset (like `vid_mode`: new render targets, about 0.2 s); saved in `user.ltx` |
| `r__upscaler` | `metalfx` or `bilinear`, `metalfx` | next frame; saved |
| `r__upscale_sharpness` | 0 to 1, 0 | next frame; saved |

C++: `ps_r__render_scale`, `ps_r__upscaler` (`UPSCALER_BILINEAR`, `UPSCALER_METALFX`), `ps_r__upscale_sharpness` in
`xrRender/xrRender_console.h`; `HW.SceneScaled()`, `HW.SceneW()`, `HW.SceneH()`.
The swap chain is the output: with `rs_screenmode borderless` its size comes from `vid_mode` when that fits in the window
(`CHW::selectResolution`), otherwise from the window. For native output `vid_mode` must be the panel's size (or larger).

### Measurements

Scene: the Cordon, `tools/render_harness/scenes/20_scale_cordon.script` on the Mac (clear noon, the barn; blind dogs wander in and
bite, so the draw count moves between 1,700 and 2,700 from run to run and GPU times within about 3 ms of each other are noise) and
`tools/ios/scripts/scale_sweep.sh` on the iPad (the `ipad_test` save, a field in the rain, `-dbg`). Both with the game's "Default" quality
preset and, on the iPad, the device preset of this branch (version 4: `texture_lod 1`, low shadows, `r3_dynamic_wet_surfaces off`, ...).
GPU ms is the sum of the command buffers' GPU time per frame. **It includes the wait for the display when a frame is held back by vsync**
(iPad: below the 16.7 ms frame time the GPU figure only says "not the limit"), so differences below the cap are small by construction.
CPU ms is the engine's frame time on the main thread, which on the iPad includes spinning on occlusion-query results, i.e. waiting for the GPU.

**Mac** (M2 Pro, macOS 27, `RelWithDebInfo`, offscreen 3024x1964, no vsync, one run each, `HARNESS_FOOTPRINT=1`):

| scale | upscaler | scene | GPU ms | CPU ms | draws | render targets MB | Metal MB | footprint MB |
|---|---|---|---:|---:|---:|---:|---:|---:|
| 0.5 | metalfx | 1512x982 | 6.57 | 11.71 | 2176 | 485 | 2262 | 3546 |
| 0.5 | bilinear | 1512x982 | 6.53 | 10.73 | 2238 | 485 | 2225 | 3689 |
| 0.6 | metalfx | 1814x1178 | 7.14 | 10.79 | 2236 | 644 | 2400 | 4071 |
| 0.6 | bilinear | 1814x1178 | 6.96 | 10.96 | 2359 | 644 | 2371 | 4015 |
| 0.67 | metalfx | 2026x1316 | 6.48 | 9.11 | 1736 | 772 | 2512 | 4150 |
| 0.67 | bilinear | 2026x1316 | 6.92 | 11.02 | 1974 | 772 | 2500 | 4108 |
| 0.75 | metalfx | 2268x1472 | 7.01 | 9.91 | 1742 | 936 | 2682 | 4267 |
| 0.75 | bilinear | 2268x1472 | 6.70 | 10.25 | 1686 | 936 | 2666 | 4211 |
| 0.85 | metalfx | 2570x1668 | 10.27 | 12.46 | 2454 | 1166 | 2920 | 4368 |
| 0.85 | bilinear | 2570x1668 | 7.09 | 10.09 | 1744 | 1166 | 2917 | 4400 |
| 1.0 | - | 3024x1964 | 9.64 | 13.54 | 2683 | 1477 | 3188 | 4501 |

The M2 Pro renders the Cordon at the native 3024x1964 in 9.6 ms (about 100 fps), so the Mac's default stays at 1.0 and the render scale
is there for bigger or slower Macs and for 120 Hz. Footprint grows with the scale (render targets 485 MB at 0.5, 1,477 MB at 1.0).

**iPad Air 13" (M3, iPadOS 27, 60 Hz)**, native output 2732x2048, one launch per row, four 10 s samples starting about a minute after
the teleport, `SWEEP_COOLDOWN` 240 s between launches. The iPad leaves Nominal for **Serious** about 40 to 80 s into a run at any of these
settings, and then runs the main thread 12 to 20% slower, so every row is split by thermal state. The first line is the old preset (the whole
frame at 1366x1024, no render scale) in the same session.

| scale | upscaler | output | thermal | samples | fps | CPU ms | GPU ms | footprint MB |
|---|---|---|---|---:|---:|---:|---:|---:|
| 1.0 | bilinear | 1366x1024 | Nominal | 4 | 59.9 | 16.5 | 15.7 | 3977 |
| 0.5 | bilinear | 2732x2048 | Nominal | 4 | 58.0 | 17.2 | 15.3 | 4052 |
| 0.5 | metalfx | 2732x2048 | Nominal | 4 | 57.7 | 17.3 | 16.6 | 4104 |
| 0.6 | bilinear | 2732x2048 | Nominal | 3 | 56.7 | 17.6 | 15.7 | 4146 |
| 0.6 | bilinear | 2732x2048 | Serious | 1 | 50.8 | 19.6 | 15.6 | 4149 |
| 0.6 | metalfx | 2732x2048 | Nominal | 2 | 58.1 | 17.1 | 17.7 | 4190 |
| 0.6 | metalfx | 2732x2048 | Serious | 2 | 50.0 | 19.9 | 17.5 | 4197 |
| 0.67 | bilinear | 2732x2048 | Nominal | 2 | 56.6 | 17.4 | 16.6 | 4227 |
| 0.67 | bilinear | 2732x2048 | Serious | 2 | 47.3 | 20.8 | 16.9 | 4234 |
| 0.67 | metalfx | 2732x2048 | Nominal | 1 | 54.2 | 18.3 | 19.4 | 4238 |
| 0.67 | metalfx | 2732x2048 | Serious | 3 | 45.7 | 21.6 | 22.5 | 4253 |
| 0.75 | bilinear | 2732x2048 | Nominal | 1 | 42.0 | 22.9 | 23.0 | 4291 |
| 0.75 | bilinear | 2732x2048 | Serious | 3 | 39.4 | 24.2 | 22.7 | 4313 |
| 0.75 | metalfx | 2732x2048 | Nominal | 1 | 47.6 | 20.5 | 22.9 | 4316 |
| 0.75 | metalfx | 2732x2048 | Serious | 3 | 39.4 | 24.2 | 26.5 | 4336 |
| 0.85 | bilinear | 2732x2048 | Serious | 4 | 34.8 | 26.6 | 29.4 | 4465 |
| 0.85 | metalfx | 2732x2048 | Serious | 4 | 31.9 | 28.6 | 34.5 | 4504 |
| 1.0 | metalfx (off at 1.0) | 2732x2048 | Serious | 4 | 26.9 | 34.6 | 41.7 | 4640 |

<details><summary>First sweep (120 s cool-down, the build before the last two GPU changes, one launch per row)</summary>

| scale | upscaler | output | thermal | samples | fps | CPU ms | GPU ms | footprint MB |
|---|---|---|---|---:|---:|---:|---:|---:|
| 0.5 | metalfx | 2732x2048 | Serious | 2 | 56.5 | 17.7 | 16.5 | 4127 |
| 0.5 | metalfx | 2732x2048 | Nominal | 2 | 57.0 | 17.5 | 16.5 | 4124 |
| 0.5 | bilinear | 2732x2048 | Nominal | 4 | 59.4 | 16.7 | 15.2 | 4118 |
| 0.6 | metalfx | 2732x2048 | Nominal | 2 | 55.1 | 18.1 | 18.1 | 4216 |
| 0.6 | metalfx | 2732x2048 | Serious | 2 | 47.4 | 20.8 | 18.0 | 4225 |
| 0.6 | bilinear | 2732x2048 | Nominal | 3 | 58.0 | 17.2 | 15.8 | 4199 |
| 0.6 | bilinear | 2732x2048 | Serious | 1 | 52.8 | 18.7 | 15.3 | 4202 |
| 0.67 | metalfx | 2732x2048 | Nominal | 2 | 54.5 | 18.4 | 19.4 | 4265 |
| 0.67 | metalfx | 2732x2048 | Serious | 2 | 46.8 | 21.1 | 21.8 | 4276 |
| 0.67 | bilinear | 2732x2048 | Nominal | 2 | 58.5 | 17.0 | 16.6 | 4275 |
| 0.67 | bilinear | 2732x2048 | Serious | 2 | 48.2 | 20.5 | 16.8 | 4283 |
| 0.75 | metalfx | 2732x2048 | Nominal | 2 | 45.5 | 21.5 | 23.6 | 4354 |
| 0.75 | metalfx | 2732x2048 | Serious | 2 | 37.0 | 25.2 | 24.9 | 4375 |
| 0.75 | bilinear | 2732x2048 | Nominal | 1 | 54.8 | 18.1 | 18.1 | 4323 |
| 0.75 | bilinear | 2732x2048 | Serious | 3 | 45.0 | 21.9 | 21.4 | 4336 |
| 0.85 | metalfx | 2732x2048 | Serious | 4 | 32.2 | 28.4 | 34.4 | 4552 |
| 0.85 | bilinear | 2732x2048 | Nominal | 1 | 39.5 | 23.8 | 26.4 | 4487 |
| 0.85 | bilinear | 2732x2048 | Serious | 3 | 34.9 | 26.4 | 29.5 | 4500 |
| 1.0 | metalfx (off at 1.0) | 2732x2048 | Serious | 4 | 27.5 | 34.2 | 40.7 | 4651 |

</details>

Within one launch (alternating the setting every 10 s, so both settings see the same thermal state; iPad at Serious, 0.67 bilinear as
the base): MetalFX instead of bilinear, 24.2 / 24.5 ms against 19.1 / 18.4 ms of GPU time (+5.5 ms); `r__upscale_sharpness` 0.25 instead
of 0, 18.9 / 18.9 against 18.9 / 18.8 ms (no measurable cost). Rows with one sample are unreliable (a sample taken in the first seconds
after the teleport, or a thermal change in the window).

Reading the table:

* The old preset's scene (1366x1024, 1.4 Mpixel) is the scene of **scale 0.5** at native output, and 0.5 with bilinear costs the same GPU
  time (15.3 against 15.7 ms; 58.0 to 59.4 fps in the two sweeps against 59.9): native output (three 22 MB drawables, the upscale pass,
  the UI at 5.6 Mpixel) is practically free, and the UI is crisp.
* With bilinear the frame rate is the same within the noise from 0.5 to 0.67 (56.6 to 59.4 fps Nominal over both sweeps; GPU 15.3 to 16.6 ms)
  and then the GPU becomes the limit: 0.75 gives 42 to 55 fps (18 to 23 ms), 0.85 35 to 40 fps (26 to 29 ms), native 27 fps (42 ms).
  Above 0.75 the GPU time grows by about 9 ms per Mpixel of scene.
* **MetalFX costs GPU time that grows with the scene size**: +1.3, +2.0 and +2.8 ms over the bilinear pass at 0.5, 0.6 and 0.67 at
  Nominal (the pass that puts `rt_Upscaled` into the back buffer is the same quad as the bilinear path's, so this is the scaler itself).
  The frame rate is the same as bilinear at 0.5 and 0.6 and lower at 0.67 (54 against 57).
* The iPad leaves Nominal for **Serious** within about a minute of load, whatever the settings, and then the main thread runs 12 to 20% slower:
  every scale up to 0.67 falls to 45 to 51 fps there with the GPU time unchanged. The remaining gap to 60 fps is the CPU side
  (17.5 ms on the main thread at Nominal, 20 ms at Serious, of which the occlusion-query spin is GPU waiting), not the render scale.
* Footprint: 4.05 GB at 0.5 up to 4.65 GB at 1.0 (budget 7.99 GB); the old preset is 3.98 GB.
* After a live change of the scale, the next seconds ran slower than a launch that started at that scale (0.67 bilinear: 43 fps in the
  window after the change against 57 in the sweep; measured with the leaking build below, not repeated since): a scale change is
  a device reset and the settings screen should not be used to compare frame rates.

Recommendations (the settings session bakes them into the presets):

* **iPad Air M3 with this branch's preset (version 4):** `r__render_scale 0.6`, `r__upscaler bilinear`, `r__upscale_sharpness 0.25`: 1.44 times the old
  preset's scene pixels at the same GPU cost class (15.7 ms, 56.7 to 58.1 fps Nominal against 59.9), crisp native UI. For "performance"
  0.5. 0.67 is possible where the profile has made the frame cheaper elsewhere (GPU 16.6 ms, no headroom left in this scene).
  MetalFX only at 0.5 (+1.3 ms, the same frame rate, a sharper image than bilinear); not at 0.6 and above. No scale reaches a steady
  60 fps once the iPad is at Serious: that is the main thread.
* **Mac (M2 Pro, 3024x1964):** `r__render_scale 1.0`: 9.6 ms (about 100 fps). 0.67 to 0.75 with MetalFX or bilinear (6.5 to 7 ms) is headroom
  for a slower or bigger Mac or a 120 Hz target.
* `r__render_scale` accepts 0.5 to 1.0; a value outside is a syntax error, not a clamp.

### Device reset leak (fixed here)

Applying the scale at run time is a `Device.Reset()`, and every reset leaked render targets: `CopyResource`/`ResolveSubresource` took
`rt->pTexture->surface_get()`, which adds a COM reference that nobody released, once per surface per frame, so `$user$generic`,
`generic0/1` (and their MSAA twins), `generic_temp` and `ssfx_temp/2` of every earlier render-target set stayed alive: about 170 MB per
reset at 2732x2048 (`vid_restart` 16 times: footprint 3.7 to 7.3 GB; so every `vid_mode` change leaked before this). The calls use
`rt->pSurface` now. `24_scale_toggle.script` toggles the scale 16 times: the footprint stays at 3.6 to 3.85 GB.

### Image quality

`tools/render_harness/scenes/08_far_terrain.script` (far trees from 38 m up, clear noon), Mac, 2732x2048 output, crops at 2x
(nearest), top-left native render, top-right MetalFX, bottom-left bilinear, bottom-right the old preset (1366x1024 whole frame, enlarged by
a bilinear filter as the Metal layer would): `scene_quality_scale_0.67.png`, `scene_quality_scale_0.5.png` in the PR.

* MetalFX at 0.67 is clearly closer to the native render than bilinear (leaf and branch edges keep their contrast; bilinear is soft);
  at 0.5 it is sharper than bilinear and the old preset but keeps the stair-steps of the low-resolution edges (it is a spatial scaler:
  no temporal anti-aliasing).
* Bilinear plus `r__upscale_sharpness` 0.25 to 0.5 lands near MetalFX's contrast at no measurable GPU cost (MetalFX: +1.3 to +5.5 ms on
  the iPad); above 0.5 foliage turns gritty.
* UI text: at native output the text is a native-resolution rasterisation of the 2160 font set, the old preset's is the 768 set
  scaled 1.33x and then 2x by the Metal layer: sharp against blurred (`ui_text_inventory_native_vs_old.png`,
  `ui_text_pda_native_vs_old.png`). The font ladder (`GetFontTextureName`, by `Device.dwHeight`) makes the native text smaller on the
  screen: about 0.64 of the old text height. That is the engine's font scaling, not the renderer; a UI or font scale option would
  have to compensate if the text is too small on the iPad.

### What was checked

On the Mac, at 0.67 and 0.5 with both upscalers: the main menu (render_menu path), the loading screen, the in-game HUD, the inventory,
the PDA dialog, the pause menu (all at native resolution: `22_scale_ui.script`), the 3D PDA in the hand (`25_scale_pda3d.script`:
the map on its screen is the same at 1.0 and 0.67), changes of `r__render_scale`, `r__upscaler` and
`r__upscale_sharpness` at run time (each scale change is a 150 to 200 ms device reset, `21_scale_live.script`), a return to 1.0, the
screenshot console command (the output size) and the cursor (`23_scale_cursor.script`: it ends up in the same place at 1.0 and 0.67; the
scripted pointer does not land on the coordinates it is given at either scale, which was not investigated). On the iPad: the game start,
100 s in the Cordon at every scale, one on-device screenshot (the frame and the HUD bars are native-sharp); the menus, inventory and PDA
were looked at on the Mac only.
Not exercised: a scoped weapon (second viewport) with a render scale, MSAA, HDR10, the Windows build (the new code
is Apple-only or no-op there), touch input on the iPad (the pointer and cursor code is unchanged and works on `Device.dwWidth`).

### Temporal upscaling and frame interpolation: what R4 would have to provide

The spatial scaler needs nothing but the finished frame, which is why it fits R4 as it is. `MTLFXTemporalScaler` (and
`MTLFXFrameInterpolator`, `MTLFXTemporalDenoisedScaler`) reconstruct detail from previous frames and need, every frame:

| Input | What R4 has | What it would take |
|---|---|---|
| **Jitter** | Nothing is applied. The projection matrix is the plain camera one (`Device.mProject`); the SSS pack's TAA has a 4-phase `ssfx_jitter` shader constant (`Blender_Recorder_StandartBinding.cpp`, now in scene pixels) but its `ssfx_taa*.ps` are among the shaders that are not in the game data, so it is off. | A Halton (2,3) sub-pixel offset of the scene's projection, 8 to 16 phases scaled by the render scale, added in `CRenderDevice::on_idle` where `mProject` is built (`mProject`, `mProjectHud` and the shadow cascades' camera must not jitter differently). Every shader that uses the projection or reconstructs positions from depth (`pos_decompress_params`, the `m_P` users) then sees the jittered one; the HUD (weapon) projection is jittered separately or left alone. The same offset (in pixels, -0.5..0.5) is passed to the scaler each frame (`jitterOffsetX/Y`). |
| **Depth** | `HW.pBaseZB`, D24S8, standard Z (near plane at `VIEWPORT_NEAR`), plus the view-space position in `rt_Position` (G-buffer). | A `Depth32Float` or `R32Float` copy of the scene depth at the scene's size (the shim already stores D24S8 as `Depth32Float_Stencil8`), the near/far values for `MTLFXTemporalScalerDescriptor`, and the rule for the weapon: it is drawn with a different depth range (`rmNear`, 0 to 0.02) and has to be masked or given its own depth. |
| **Motion vectors** | `rt_ssfx_motion_vectors` exists but is only written by the SSFX forward effects (sky, rain); the deferred geometry passes write none. Camera motion can be rebuilt from depth and the previous frame's view-projection (`Matrix_previous`, the motion-blur code does it in `combine_2`), moving objects and skinned characters cannot. | An extra G-buffer output in the deferred geometry shaders (`deffer_*.vs/ps`: the previous world-view-projection per object, the skinning of the previous pose for `SKIN_*`; 200+ shader permutations, and `CBackend` has to keep the previous transform per renderable), or a velocity pass that re-draws moving objects. Particles and the sky need their own. |
| **Colour** | The finished LDR image after `combine_2` (tone mapped, gamma), or `rt_Generic_0` earlier in the chain. | The scaler wants the colour before tone mapping and before the effects that must run at the output's resolution (bloom, DoF, motion blur, film grain, SMAA/TAA). In R4 tone mapping is inside the combine pass, so the split would move from after `phase_pp` to after `combine_1` and everything from `phase_bloom` on would run at the output's size: its cost (bloom/blur chains, DoF, sunshafts, SMAA, `combine_2`, `phase_pp`) is paid at the full resolution again. Upscaling the tone-mapped LDR image instead works and is cheaper, but ghosts on bright highlights and bloom. |
| **Reactive / transparency mask** | None. | A mask of pixels whose content is not described by depth or motion (rain streaks, smoke and fire particles, water refraction, the distortion pass, the weapon's glass, the scope picture). R4 draws these in the forward stage (`render_forward`, `r_dsgraph_render_distort`, `phase_ssfx_rain`), so one R8 target written there would do. |
| **Exposure** | A 1x1 luminance target (`rt_LUM_pool`), the engine adapts it itself. | `exposureTexture` (a 1x1 `R16F`/`R32F` with the pre-exposure) if the scaler gets HDR input, or `autoExposureEnabled` for LDR. |
| **History reset** | `m_bFirstFrameAfterReset`, level loads, the second viewport. | Call the scaler's `reset` on a camera cut, a teleport, a device reset and when a scope picture is switched on. |

Cost: only the spatial scaler is measured here (above: +1.3 to +2.8 ms on the iPad Air M3 at 0.5 to 0.67, growing with the scene
size). A temporal scaler takes more inputs and has to be assumed to cost at least as much; the jitter is free; a velocity target
is one more 8 to 16 bit target at the scene's size plus an output in every deferred geometry shader; moving the split in front of
bloom, DoF, sunshafts, SMAA and `combine_2` puts those passes at the output's resolution, i.e. back at the cost this change
removes. Frame interpolation (`MTLFXFrameInterpolator`) additionally needs two consecutive finished frames and a UI texture
composited after the interpolation (R4 already has the UI as a separate stage after `FinishScene`, which is the point of the split in
this change), and adds a frame of latency; with the iPad main-thread bound at 47 to 51 fps at Serious (above) it would not help there.

The order of work if it is wanted: (1) the camera-motion vectors from depth plus the jitter, which gives a working temporal path
with ghosting on moving objects only; (2) the reactive mask for the forward effects; (3) per-object velocity. (1) and (2) are renderer
only (`src/Layers`, `mtl_upscale.mm` gets a second scaler class next to the spatial one); (3) touches the shader set.

## Link-time needs (task 5)

`libxrAPI.a` and `libxrRender_R4.a` force-loaded with xrCore, the shim, ShaderXlat, luabind, LuaJIT and SDL leave 232 undefined
symbols (`ld` output with the archives and `clang++ -Wl,-force_load`). Grouped by the module that defines them:

**xrEngine** (132 symbols): `Device`, `Console`, `CConsole` (`AddCommand`, `Execute`, `RemoveCommand`, `ScriptCaller`),
`IConsole_Command` (LRU helpers, vtable), `CApplication` (`LoadBegin/LoadEnd`), `IGame_Persistent::GrassBendersUpdateAnimations`,
`g_pGameLevel`, `g_pGamePersistent`, `g_hud`, `g_SpatialSpace`, `g_pMotionsContainer`, `g_bRendering`, `g_bootComplete`,
`g_screenmode`, `g_dedicated_server`, `g_current_font_scale`, `pApp`, `GMLib`, `PSGP`, `psDeviceFlags`, `psCurrentVidMode`,
`psTextureLOD`, `ps_r__Supersample`, `r2_advanced_pp`, `r2_sun_static`, `refresh_rate`, `discord_gameinfo`, `g_SASH` /
`xrSASH::OnConsoleInvalidSyntax`, `GetMonitorResolution`, `is_enough_address_space_available`, `mbhMulti2Wide`, `imf_Process`;
the `scope_*` lens parameters (`scope_brightness`, `scope_ca`, `scope_radius`, `scope_scrollpower`, `scope_innerblur`,
`scope_outerblur`, `scope_fog_attack/interp/mattack/radius/sharp/travel`); render interfaces `IRender_interface`, `IRender_Light`,
`IRender_Glow`, `IRenderable` (`renderable_ROS`) with their vtables and typeinfo; `CRenderDevice::CSecondVPParams::IsSVPFrame`,
`CStats::OnDeviceCreate`, `CEnvironment` (`RenderSky`, `RenderClouds`, `RenderFlares`, `RenderLast`), `CLensFlare::Render`,
`CEffect_Rain` (`Born`, `Hit`, `RayPick`, `RenewItem`, `p_free`), `CGameFont` (11), `CGIFResource` (7), `CTheoraSurface` (8),
`CObjectSpace` (`RayPick`, `RayTest`), `CBoneData::CalculateM2B`, `CMotionDef::StopAtEnd`, `CPartition::load`, `motions_container`
(4), `motions_value::bone_motions`, `shared_motions::create` (2), `CObject` typeinfo, `ShaderBus` (14: `declare`, `find`, `get`,
`force`, `refuse`, `release`, `at`, `count`, `version`, `texture_get`, `texture_serial`, `value_text`, `note_legacy_write`,
`hotness_lane`); ReShade stubs `use_reshade`, `init_reshade`, `unregister_reshade`, `render_reshade_effects`.

**xrCDB** (28): `CDB::MODEL` (ctor, dtor, `build`), `CDB::COLLIDER` (`box_query`, `frustum_query`, `ray_query`),
`CDB::Collector` (`add_face_packed_D`, `calc_adjacency`), `xrXRC` (ctor, dtor), `CFrustum` (8: `CreateFromMatrix`,
`CreateFromPortal`, `ClipPoly`, `_add`, `testAABB`, `testSAABB`, `testSphere`, `testSphere_dirty`), `ISpatial` (ctor, dtor, `spatial_*`,
vtable), `ISpatial_DB` (`q_box`, `q_frustum`, `q_sphere`).

**xrParticles** (60): the 29 `PAPI::PA*::Save` actions and their vtables, `PAPI::ParticleManager()`, `PAPI::pDomain::pDomain`.

**xrGame** (3): `IK_ALWAYS_CALC_DIST`, `IK_CALC_SSA`, `current_player_hud_sect`.

**3rd party/imgui** (8): `ImGui::SetAllocatorFunctions`, `ImGui::SetCurrentContext`, `ImGui_ImplDX11_{Init, NewFrame, RenderDrawData,
Shutdown, CreateDeviceObjects, InvalidateDeviceObjects}` (the stock backend, built with the engine's ImGui).

**lua-extensions** (1): `luaopen_lua_extensions`.


Notes: TBB, NvAPI, VFW, `d3d11.dll`, DXGI and D3DX11 are not in the list: the first three are compiled out, the rest resolve in the
Metal shim. `ImGui_ImplDX11_*` is the stock DX11 backend, which works on the shim as it is (`D3DCompile` goes through the same hook).
`xrXRC`, `CFrustum` and `ISpatial*` are xrCDB, not xrEngine. The xrGame entries are globals `CKinematics` and the HUD code read
(`IK_*`, `current_player_hud_sect`).

## Next steps

* Engine-core: provide the items above, create the window with `SDL_WINDOW_METAL`, set `Device.m_hWnd`, do not create the Metal view.
* First on-device frame: `r3_msaa` 0, HDR10 off, `no_ram_textures` on iPad (METAL-SHIM.md, "Integration steps").
* Pipeline pre-warm: compile the engine's pipeline permutations on a background queue (`MTLBinaryArchive`) before the first frame.
* The 76 requested shaders that exist in neither `shaders.db0` nor the repo overlay (`ssfx_*`, DX9 leftovers) are listed by the tool.
