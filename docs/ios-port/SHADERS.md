# Shader path: HLSL to Metal

Anomaly's D3D11 renderer (R4, shader sources under `shaders/r3`) compiles ~370 HLSL files into
thousands of permutations at runtime. This document records how those shaders reach Metal, what
was measured, and the API the Metal shim calls.

```
HLSL (SM5, FXC dialect)
  --[ShaderXlat FXC-compat hook]--> HLSL
  --[vkd3d-shader 2.1]----------->  DXBC (SM5)  : RDEF + ISGN/OSGN = D3D11 reflection
  --[vkd3d-shader 2.1]----------->  SPIR-V       : explicit descriptor bindings
  --[SPIRV-Cross]---------------->  MSL 3.0      : macOS and iOS flavours
```

Everything runs in-process inside one static library (`src/Platform/ShaderXlat`): no helper
process, no JIT, no threads of its own. An on-disk cache sits in front of it.

## Result

Sweep of every `.vs/.ps/.gs/.hs/.ds/.cs` file under `shaders/r3` (shaders.db0 overlaid with this
repo's `gamedata/shaders`), 4 macro profiles taken from `CRender::shader_compile` (MSAA off), plus
the five skinning variants of the 15 skinned model vertex shaders.

| | |
|---|---|
| Files swept | 370: 4 geometry + 3 tessellation (not translatable), 363 vertex/pixel/compute, of which 2 are include-only fragments excluded from the rate |
| Jobs | 1555; 14 on include-only fragments (no `main`) and 28 on GS/HS/DS are not counted below |
| **Translated** | **1482 / 1513 = 97.95 %** |
| Files that translate in every profile | 351 of 361 evaluated |
| Engine-requested entries (blenders + `.s` scripts) that translate everywhere | 208 / 210 (the two failures are 3D-fluid shaders, see below) |
| MSL accepted by `xcrun metal` (`-std=metal3.0`) | macOS **1482 / 1482**, iOS **1482 / 1482** |
| Skinned VS variants (`SKIN_0..4`) | 75 / 75 |
| Extra profile, MSAA 4x (`USE_MSAA`, `MSAA_SAMPLES=4`) | 427 / 434, Metal 427 / 427 on both |
| Extra profile, SSAO quality 4 + HDAO (the only user of the compute shader) | 421 / 436, Metal 421 / 421 on both |

Profiles (`tools/shader_batch/profiles.cpp`): `low`, `medium` (plain SSAO, soft effects),
`high_hbao` (HBAO, blur, vectorised code), `ultra_hdao` (HDAO, 4096 shadow map, min/max shadow map,
all SSFX at maximum), then `msaa4` and `ssao4_hdao` on request. Each macro list mirrors what
`r4.cpp` emits, including which macros are *absent* when a setting is 0.

Numbers are from `build/final-default` runs of `shader_batch`, Apple M-series, Release build.

## DXC or vkd3d-shader: vkd3d-shader

Both were built from source and run over the same sweep through the same SPIRV-Cross back end.

| | DirectXShaderCompiler v1.9.2609 (`-spirv`) | vkd3d-shader 2.1 |
|---|---|---|
| Translated | 91.4 % (1383 / 1513) | **97.95 %** (1482 / 1513) |
| Why the rest fails | 92 jobs (23 files): `Offsets to texture access operations must be immediate values`; 4 jobs: `input semantic 'TEXCOORD0' used more than once`; the shipped-data failures below | only the shipped-data failures below |
| FXC-isms it needs help with | HLSL 2016 mode (`-HV 2016`), otherwise 650 errors on vector `?:` conditions; lower-case `texture2D`; non-literal sample offsets; duplicate input semantics. The offset case is not fixable with a text hook: `shadow.h` passes loop variables of an `[unroll]` loop and every deferred light shader includes it | one hook: `register(ps, t0)` (see below) |
| Reflection | none: SPIR-V keeps names but not D3D register numbers, cbuffer `used` flags, or HLSL types, they would have to be rebuilt | the DXBC it emits has RDEF/ISGN/OSGN, i.e. what `D3DReflect` returned for FXC |
| Translation time, mean, 1 thread, `medium` profile | 7.0 ms (`-O3`), 5.5 ms (`-O0`) | 10.8 ms |
| macOS build | 7 min on 10 cores; `libdxcompiler.dylib` 27.9 MB | 5 s; `libvkd3d-shader.a` 2.4 MB |
| iOS arm64 | all 1275 objects compile (12 min); the shared-library link fails because LLVM's CMake passes GNU `ld` flags (`-z`, `--gc-sections`) when `CMAKE_SYSTEM_NAME=iOS`; 52 static archives, 60 MB; there is no static `dxcompiler` target, it would need patching | builds with stock CMake and Apple clang; whole translator (vkd3d + SPIRV-Cross + glue) links into a **3.7 MB** iOS arm64 binary |
| Licence | NCSA / Apache-2.0 with LLVM exception | LGPL-2.1 |

Decision, in order of weight:

1. **FXC compatibility.** vkd3d's HLSL front end is an FXC work-alike (inline, unroll, fold, then
   check), so shaders written for FXC mostly just compile. DXC rejects a quarter of the lighting
   path for reasons that are not source-patchable.
2. **Static library on iOS.** vkd3d-shader is ~25 C files; DXC is LLVM and has no static target.
3. **D3D11-shaped reflection for free.** The engine's constant tables are built from
   `ID3D11ShaderReflection`; vkd3d hands us that data in the DXBC.

Costs of the choice, all handled in the library:

- **LGPL-2.1.** Fine for a sideloaded personal build. If the app is ever distributed, link
  `libvkd3d-shader` as a dynamic framework or ship the relinkable objects; nothing else in the tree
  is LGPL. SPIRV-Cross is Apache-2.0.
- **The HLSL front end is young.** Pinned at tag `vkd3d-2.1`. It has a NULL dereference in error
  recovery (e.g. an undeclared identifier inside an expression; hit by SSAO quality 4) and an
  infinite loop on a self-including file. The library caps opens per include file and runs every
  vkd3d call under a SIGSEGV/SIGBUS guard (`FrontendVkd3d.cpp`), so a broken addon shader fails to
  compile instead of killing the game. The guard leaks the abandoned allocations and chains to any
  previously installed handler.
- **Build-time tools.** Its parsers need bison 3 and flex, the headers need Wine's `widl`. Those
  outputs are checked in under `cmake/deps/vkd3d-generated/` with a `regenerate.sh`, so building
  needs none of them.
- **Names.** vkd3d's direct HLSL to SPIR-V path drops all names (`cb0_0`, `t0`, `V`). The pipeline
  therefore goes HLSL to DXBC (names, registers, layouts) and then DXBC to SPIR-V with bindings
  derived from the registers. Same pass rate as the direct path, half the time of compiling twice.
- vkd3d logs `Invalid swizzle ... for scalar value` for `SampleCmp(...).x`. The generated code is
  correct in the cases inspected (the scalar is broadcast); the library captures vkd3d's log into `Result::log` and never
  treats log lines as failure.

## FXC-compat hooks

"Handle FXC-isms with minimal preprocessing hooks inside the translator, never by editing the
game's shaders": hooks rewrite the in-memory text handed to the front end (root source and every
`#include`), fire only when their pattern is present, and are reported in `Result::applied_hooks`.

| Hook | Why | Reach |
|---|---|---|
| `register-stage-qualifier`: `register(ps, t0)` becomes `register(t0)` | FXC accepts a stage qualifier; vkd3d stops with `not yet implemented: Reservation shader target`. `shadow.h` declares `s_smap` this way | 200 of 1500 jobs without it (every shader that includes `shadow.h`) |

That is the whole list. Everything else FXC accepts and DXC does not is handled by vkd3d itself:
non-immediate texture offsets, vector ternary conditions, duplicate input semantics, `texture2D`,
the legacy `sampler` type, `$Globals` for loose uniforms, global `uniform` without a cbuffer.

## Failure categories (the 31 failing jobs, 10 files, plus the 2 excluded fragments)

All are unreachable from the engine or broken in the shipped data; none needed a translator fix.

| Category | Files | Notes |
|---|---|---|
| Fragment files included by real entries | `ssao.ps`, `ssao_hbao.ps`, `ssao_hdao_new.ps`, `ssdo.ps`, `gather.ps`, `ssao_blur.ps` (failing); `ssao_hdao.ps`, `accum_sun_near_old.ps` (excluded) | They use declarations from the includer (`s_smap`, `smp_nofilter`, `jitter0`, ...). Compiled standalone they report either "Entry point main is not defined" (excluded from the rate) or an undeclared identifier (counted as failures) |
| Self-including file | `deffer_tree_s_bump_d.vs` | The file is two lines and includes itself. FXC fails with "include nested too deeply"; vkd3d recursed forever until the open-count cap |
| Typo in shipped data | `accum_volumetric_sun_normal .ps` | `#unfdef`, and the name has a space. Not requested by the engine |
| 3D fluid sim leftovers | `fluid_draw_texture.ps`, `fluid_obststaticbox.ps` | Undeclared `textureNumber`; cbuffer member redeclared. The blender for the second one is commented out. The rest of the fluid sim needs geometry shaders anyway |

Two failure groups in the very first sweeps were harness bugs, kept here because they will recur
for anyone writing macro sets:

- `SUN_QUALITY=0` / `SSAO_QUALITY=0` must be *absent*, not defined as 0 (`r4.cpp` only emits them
  when non-zero); defining them made 50 lighting jobs fail with `undeclared identifier
  PCSS_NUM_SAMPLES`.
- `SSAO_QUALITY=4` (the "ultra" token) has no branch in `ssao.ps`, which leaves `RINGS`
  undeclared in `combine_1.ps`, `ssao_calc*.ps` and `ssao.ps` itself (7 jobs in `ssao4_hdao`). FXC
  would reject the same text, so ultra SSAO is only reachable if an addon replaces `ssao.ps`.

### Missing files the engine requests (needs the user)

The engine asks for **76 names that exist in neither `shaders.db0` nor this repo's overlay**. 32
are the Screen Space Shaders (SSFX) addon: `ssfx_ssr*.ps`, `ssfx_sss*.ps`, `ssfx_il*.ps`,
`ssfx_ao*.ps`, `ssfx_taa*.ps`, `ssfx_bloom*.ps`, `ssfx_fog_scattering*.ps`, `ssfx_motion_blur.ps`,
`ssfx_rain.ps`, `ssfx_water_*.ps`, `ssfx_volumetric_*.ps`, `ssfx_sky_mv.{vs,ps}`. The matching shader
scripts (`ssfx_*.s`, which the blenders instantiate by name) are missing from the tree as well. The stock shaders already use the `SSFX_*` macros, so the addon is part of the Anomaly
1.5.3 stack. No archive under `game/Anomaly/db` contains them, and `game/Anomaly/gamedata` is
empty. Without those files the original engine also fails (a missing pixel shader is a hard error
in `CResourceManager::_CreatePS`). The other 44 are DX9-era leftovers (`lmapE`, `impl_*`,
`add_point`, `tree_*`, ...) and `null.*`, which the engine special-cases; a missing vertex shader falls
back to `stub_default.vs`. The full list is the "Engine-requested
names" section of the `shader_batch` summary. **Action: provide the loose `gamedata/shaders/r3/ssfx_*`
files (and the `.s` scripts) from the working Windows install so they can be swept too.**

## Runtime cost

Single thread, Release, `--no-validate`, per translated shader (HLSL to MSL + reflection):

| | mean | p50 | p90 | p99 | max |
|---|---|---|---|---|---|
| all four profiles (1482 jobs) | 12.9 ms | 6.4 ms | 23.8 ms | 82 ms | 1.44 s |
| `low` | 8.7 ms | | | | |
| `medium` | 10.8 ms | | | | |
| `high_hbao` | 20.7 ms | | | | |
| `ultra_hdao` | 11.3 ms | | | | |
| vertex / pixel / compute | 7.2 / 15.3 / 22.7 ms | | | | |

- Where the time goes: about 90 % HLSL to DXBC to SPIR-V (vkd3d), about 10 % SPIRV-Cross and reflection. Metal's own compile of the
  MSL is not included; it happens when the pipeline is created.
- The outlier is `combine_1*.ps` with HBAO (~1.4 s alone, ~8 s with every core busy); everything
  else is under 0.7 s.
- MSL size: median 3.7 KB, max 91 KB.
- **Cache** (`TranslationCache`): a full 427-shader `medium` set takes ~10 s cold and 0.33 s warm
  (< 1 ms per hit). A touched include invalidates exactly the entries that read it (changing
  `common_defines.h` re-translated all but 32 of 445 jobs).
- At 8 to 25 ms per shader, a cold start that needs 1000 permutations is 10 to 20 s of CPU, which
  parallelises across cores; with the cache it happens once. Ship a pre-translated cache for the
  stock shaders with the app if first-launch time matters.

## Stages Metal does not have

Source: `shader_batch` summary sections "Stages Metal cannot run directly" and the engine scan.

**Geometry shaders: 4 files, 3 requested.** `fluid_array.gs`, `fluid_array_oobb.gs`,
`fluid_array_dyn_oobb.gs` (`test.gs` is unused). They exist only for the 3D fluid simulation
(`xrRenderDX10/3DFluid`, `FluidManager.Initialize(70, 70, 70)` runs at renderer start): the GS
routes each full-screen triangle to a slice of a 3D render target through
`SV_RenderTargetArrayIndex`. Metal equivalent: draw one instance per slice and write
`[[render_target_array_index]]` from the vertex function (3D textures support layered rendering),
or one render pass per slice. `translate()` returns `Category::UnsupportedStage` for GS/HS/DS
without touching them. Nothing in Anomaly's normal rendering depends on these; the first
milestone can leave `FluidManager` uninitialised.

**Tessellation: 3 files** (`dx11/tess.hs`, `tess.ds`, `tess_shadow.ds`). Requested only from
`uber_deffer.cpp:100` and `:276` when `o.dx11_enable_tessellation` (console flag
`r4_enable_tessellation`, on in the default flags) and the material has `TessMethod != 0`; the
build then adds `TESS_PN`, `TESS_HM`, `USE_LM_HEMI`, `USE_TDETAIL`, `USE_TDETAIL_BUMP` through
`addShaderOption`. Recommendation: force `dx11_enable_tessellation = false` on Metal. The non-tess
paths for the same materials are in the sweep and translate.

**Compute: 2 files** (`ssao_hdao.cs`, `ssao_hdao_msaa.cs`), requested by `dx11HDAOCSBlender.cpp`
through `r_ComputePass` and only when HDAO is on *and* SSAO quality is 4. They translate to real
kernels: 32x32 threads, 12.5 KB threadgroup memory, one `RWTexture2D` (`[[texture(64)]]`), a
cbuffer of 31 vectors. Both validate with `xcrun metal` on macOS and iOS.

## The API the Metal shim calls

`src/Platform/ShaderXlat/include/ShaderXlat/ShaderXlat.h`. Library: `ShaderXlat` (CMake target),
C++17, thread safe.

```cpp
#include "ShaderXlat/ShaderXlat.h"
using namespace shaderxlat;

// 1. The engine's includer: "<shader dir>/<name>" first ("r3/" for R4), then "<name>"; loose
//    gamedata files shadow archive contents. Backslashes are already turned into '/'.
struct EngineIncludes : IncludeProvider {
    bool open(const std::string& name, const std::string& from,
              std::string& resolved, std::string& contents) override;
};

// 2. One cache per process, in the app's cache directory.
TranslationCache cache(cachePath);

// 3. Per shader. `macros` is exactly the D3D_SHADER_MACRO list CRender::shader_compile builds.
Options o;
o.stage    = Stage::Pixel;            // from the file extension
o.platform = Platform::IOS;           // or MacOS
// entry = "main", profile derived from the stage, row-major matrices (D3D10_SHADER_PACK_MATRIX_ROW_MAJOR)
Result r = cache.translate(hlslText, "accum_sun.ps", macros, o, &includes);
if (!r.ok) { log(r.log); /* r.category: FrontendError, CrossError, UnsupportedStage, ... */ }

// 4. Metal
id<MTLLibrary>  lib = [device newLibraryWithSource:@(r.msl.c_str()) options:nil error:&err];
id<MTLFunction> fn  = [lib newFunctionWithName:@(r.entry_point.c_str())];   // "main0"
```

Free functions: `translate()` (no cache), `retarget(result, options)` (re-run SPIR-V to MSL for
another platform/version from `Options::keep_spirv`), `checkStageInterface(vsReflection,
psReflection)`, `version()`, `frontend_name()`.

### Binding convention

| D3D | MSL | Notes |
|---|---|---|
| constant buffer `b#` | `[[buffer(cbuffer_base + #)]]` | stock shaders use at most b0..b2 (`static_globals`, `dynamic_transforms`, `dynamic_light`, `$Globals`) |
| SRV texture `t#` | `[[texture(#)]]` | at most 11 textures, slots <= 10 |
| UAV texture `u#` | `[[texture(uav_texture_base + #)]]` (64) | |
| sampler `s#` | `[[sampler(#)]]` | at most 4 samplers, slots <= 3 |
| vertex streams | pipeline vertex descriptor, buffer index `vertex_stream_base + n` (16, same as the Metal D3D11 shim) | the MSL uses `[[stage_in]]` attributes |
| structured / raw buffers | `[[buffer(storage_buffer_base + ordinal)]]` (24..30) | unused by the stock shaders |

Every `msl_index` is also in the reflection, so the shim never recomputes these. `slot` is the
real D3D register from the HLSL compiler, which is what the engine's reflection returned.

### Reflection (`Result::reflection`)

Looked up by *name*, like `R_constant_table::parse` does today.

| Field | Content | Metal use |
|---|---|---|
| `constant_buffers[]` | name (`$Globals` for loose uniforms), `slot`, `msl_index`, `size` (multiple of 16) and `members[]`: name, absolute byte `offset`, `size`, `type`, `rows`/`columns`, `array_elements`/`array_stride`, `row_major`, `used`, HLSL `type_name`, nested `members[]` for structs | CPU-side shadow buffer with the same offsets the engine already writes; one `MTLBuffer` range per cbuffer. Layout is D3D11 packing (16-byte registers, no straddling) and matrices stay row-major as compiled with `PACK_MATRIX_ROW_MAJOR` |
| `textures[]` | name, `slot`, `msl_index`, `dim`, `sampled_type`, `depth`, `multisampled`, `is_uav` (+ read/write only) | `depth` marks textures sampled with a comparison sampler: bind a depth-format texture (the MSL casts through `depth2d`) |
| `samplers[]` | name, `slot`, `msl_index`, `comparison` | `comparison` => create the `MTLSamplerState` with `compareFunction` (shadow maps: `smp_smap`) |
| `sampler_texture_pairs[]` | (sampler, texture) names used together by a sample instruction | informational; MSL keeps them separate |
| `inputs[]` / `outputs[]` | `semantic`, `semantic_index`, `reg`, `mask`, `location`, `type`, `components`, `system_value`, `active` | vertex inputs: match D3D input-layout semantics to `[[attribute(location)]]`. Interstage: `location` is derived from the semantic, so a VS output and a PS input with the same semantic share `[[user(locnN)]]` |
| `threads[3]`, `writes_depth`, `uses_discard`, `uses_sample_index` | | compute dispatch size, pipeline setup |

Stock shader limits for sizing: largest cbuffer 3952 bytes, at most 10 inputs / 10 outputs.

### Things the shim must still handle

1. **Pixel inputs the vertex shader never writes.** D3D reads zero; Metal refuses to build the
   pipeline. `checkStageInterface()` finds them. The shim handles it now: `Context::fragmentFunctionFor` rebuilds the pixel
   function with the missing inputs as zero locals (see METAL-SHIM.md). The stock set has 9 pairs (22 combinations):
   `model_def_lplanes` to `model_scope_lense` (TEXCOORD1) and `models_lfo_light_dot_weapons`
   (TEXCOORD1, TEXCOORD2), `model_distort` / `model_distort_inv` to `particle_distort` (TEXCOORD1),
   and `water_{regular,ryaska,studen,underground}` VS to PS (TEXCOORD7). Widths always match
   otherwise (680 pairs checked, 0 mismatches). Fix on the shim side: drop the unmatched member
   from the fragment `main0_in` struct and declare it as a zero local.
2. **Comparison sampling needs depth textures.** The MSL `sample_compare` goes through
   `spvDepthCast(texture)` because vkd3d declares the image as non-depth.
3. **`discard`.** Enabled `check_discarded_frag_stores`: HLSL `discard` ends the invocation, Metal's
   `discard_fragment()` does not. 116 of the 1482 translations use it.
4. **Clip distances.** Four volumetric-light vertex shaders (`accum_volumetric*.vs`,
   `fluid_grid*oobb.vs`) write `SV_ClipDistance`; it maps to `[[clip_distance]]`.
5. **Tessellation, geometry shaders:** see above.

## Tools

`tools/xrdb_unpack` (standalone, independent of xrCore):

```bash
build/macos-arm64/tools/xrdb_unpack/xrdb_unpack list    game/Anomaly/db/shaders.db0
build/macos-arm64/tools/xrdb_unpack/xrdb_unpack extract game/Anomaly/db/shaders.db0 -o build/shaders-extract
```

Reads the archive layout from `CLocatorAPI::LoadArchive` (INI header chunk 666, LZ-Huffman file
table, LZO1X payloads). Verified on `shaders.db0` (956 entries, raw), `files.db0` (LZO) and a
texture archive (529 files, all 260 DDS headers valid). It refuses to write below any
directory named `game`.

`tools/shader_batch`:

```bash
build/macos-arm64/tools/shader_batch/shader_batch \
    --root build/shaders-extract/shaders --root gamedata/shaders \
    --engine-src src/Layers/xrRender --engine-src src/Layers/xrRenderDX10 --engine-src src/Layers/xrRenderPC_R4 \
    --out build/shader-batch --reflect
```

`--root` may repeat (earlier roots win, like loose files over archives), `--profile NAME` selects
profiles (`msaa4` and `ssao4_hdao` are off by default), `--filter` limits files, `--no-validate`
skips `xcrun metal`, `--cache DIR` exercises the cache, `--keep-msl` keeps all MSL. Outputs
`summary.md` (pass/fail per file and stage, failure groups, engine-request and interface checks),
`results.jsonl` (one record per job with timings and hooks), `failures.log`, and with `--reflect`
one JSON reflection per translation.

`shaderxlat_selftest` (`ctest`) covers translation, reflection offsets, includes, retargeting,
failure paths and the cache without needing game data.
