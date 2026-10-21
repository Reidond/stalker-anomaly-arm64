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

| Feature | Why | How |
|---|---|---|
| 3D fluid simulation (`FluidManager`, volumetric smoke volumes) | needs geometry shaders (`fluid_array*.gs`) and layered 3D render targets | `CRender::create` sets `o.volumetricfog = FALSE` with a log line; `Load3DFluid` and `FluidManager.Initialize` already return early on it |
| Tessellation (`r4_enable_tessellation`, default **on**) | no hull/domain stages | `o.dx11_enable_tessellation = FALSE` with a log line if the flag was set |
| NvAPI, AGS, NVIDIA PerfHUD | vendor SDKs | compiled out |
| `.avi` textures | VFW | compiled out |
| Multi-monitor `vid_monitor` switch | one display | returns false |
| TBB | not ported | GCD |
| Gamma ramp (`SetGammaControl`) | no equivalent on `CAMetalLayer` | accepted and ignored |
| MSAA, HDR10 | off by default, not exercised | unchanged code paths; the shim implements resolve and the 10-bit layer but nothing here tests them |

## Shim gaps closed in this change

Against what R4 calls (see [D3D11-SURFACE.md](D3D11-SURFACE.md)); the rest of METAL-SHIM.md's list is still open.

* Pixel inputs the vertex shader never writes: the pixel function is rebuilt from MSL with those inputs as zero locals
  (`Context::fragmentFunctionFor`). With default settings no stock pair needs it; the patching is covered by the warm-up tool and
  `metal_d3d11_headless_test` on real and synthetic MSL.
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
