# Spike S2: Anomaly's shaders as WGSL

Question from `PLAN.md` (risk 1): does the SPIR-V that vkd3d-shader makes from Anomaly's HLSL reach valid WGSL through
Tint? **Yes.** Tint as it is accepts 84.7 % of the jobs that reach SPIR-V. With three small SPIR-V rewrites (below,
implemented) it accepts **99.5 %** over every profile and **every shader the engine requests at default settings except
two 3D-fluid pixel shaders**, which the web build does not run. Nothing found needs a different front end.

| Set | Jobs | Reach SPIR-V (vkd3d) | Valid WGSL, Tint as is | Valid WGSL, with the rewrites |
|---|---|---|---|---|
| All files × `low`, `medium`, `high_hbao`, `ultra_hdao` (+ skinning variants) | 1517 | 1486 | 1259 (84.72 %) | **1478 (99.46 %)** |
| All files × `msaa4`, `ssao4_hdao` | 872 | 850 | | **846 (99.53 %)** |
| Runtime set: `r4_shader_warmup` requests at default settings | 334 requests: 258 in the game data, 8 more only in the repo overlay, 68 missing | 253 + 8 overlay | | **251 / 253 (99.21 %), overlay 8 / 8** |

"Valid" means what Chrome checks per shader: the WGSL parses and resolves with Tint's WGSL reader (what
`createShaderModule` runs) and lowers to Tint IR (what Dawn does before generating backend code), with only the
features a web build would request. Tint is the revision Chrome 157 stable ships. Entry-point limits (bind groups,
interstage locations, workgroup size) are checked separately against WebGPU's default limits; only the HDAO compute
shader exceeds one.

## What was built

| Piece | Where |
|---|---|
| Dawn, pinned at **`3c24c91e70eaf701ae92ab8cc414669cc4c38892`**: the Dawn revision of Chrome 157.0.8103.51 (stable on Mac on 2026-11-10, branch `chromium/8103`). Shallow submodule; only Tint's SPIR-V reader, WGSL writer and WGSL reader are built, plus abseil and SPIRV-Tools (three nested submodules). No Dawn backend, no samples, no tests. | `external/dawn`, `cmake/deps/Tint.cmake` |
| CMake option `ANOMALY_SHADERXLAT_WGSL`, **off by default**. With it off nothing new is configured or built. | `src/Platform/ShaderXlat/CMakeLists.txt` |
| The WGSL back end: SPIR-V decoration patches (bind groups, interstage locations, UAV formats), the SPIR-V rewrites, Tint SPIR-V → WGSL, validation, reflection (Tint's inspector) and the WebGPU entry-point limit checks. | `src/Platform/ShaderXlat/include/ShaderXlat/Wgsl.h`, `src/SpirvToWgsl.cpp` |
| `shaderxlat_wgsl_selftest` (only with the option): bindings, locations, comparison sampling, each rewrite, the HDAO UAV case. | `src/Platform/ShaderXlat/tests/wgsl_selftest.cpp` |
| `shader_wgsl`: one SPIR-V file → WGSL + a line report. `shader_batch --target wgsl` runs it once per job in a child process, because Tint calls `abort()` on an internal compiler error (it did, 42 times, before the rewrites). | `tools/shader_batch/wgsl_main.cpp` |
| `shader_batch --target wgsl` (WGSL section in `summary.md`, WGSL fields in `results.jsonl`, failure logs, failing WGSL and SPIR-V kept) and `--requests` (replay an exported request list with its exact macros). | `tools/shader_batch/main.cpp` |
| `r4_shader_warmup --export-requests FILE`: every request with the entry point, profile and macro list `CRender::shader_compile` builds at default settings. Additive; nothing else in the tool changed. | `tools/r4_shader_warmup/main.cpp` |
| The sweep as one command. | `tools/shader_batch/wgsl_sweep.sh` |

The SPIR-V comes from the existing pipeline unchanged (HLSL → vkd3d → DXBC → vkd3d → SPIR-V, `Options::keep_spirv`).
The WGSL path only reads it, so the MSL path is untouched.

## How to reproduce

```bash
git submodule update --init --depth 1 external/dawn
```

```bash
git -C external/dawn submodule update --init --depth 1 third_party/abseil-cpp third_party/spirv-headers/src third_party/spirv-tools/src
```

```bash
cmake --preset macos-arm64-release -B build/wgsl -DANOMALY_SHADERXLAT_WGSL=ON -DANOMALY_BUILD_XRRENDER_R4=ON
```

```bash
tools/shader_batch/wgsl_sweep.sh build/wgsl /path/to/Anomaly
```

The script builds what it needs (Tint takes about 5 minutes at `-j 4` on an M-series Mac), unpacks `db/shaders.db0`,
exports the runtime requests, and runs three sweeps into `build/wgsl/wgsl-sweep/{all-profiles,extra-profiles,runtime}`.
Each has `summary.md` (the "WGSL (Tint)" section is this spike's), `results.jsonl`, `failures.log`, and the WGSL and
SPIR-V of any failing job. The game data is only read. `ANOMALY_BUILD_XRRENDER_R4=ON` is only for `r4_shader_warmup`;
the script builds that target, not the renderer.

Roots, as in `SHADERS.md`: the repo's `gamedata/shaders` overlay first (it shadows the archive), then `shaders.db0`.
`--uav-format 0=r16float` tells the back end HDAO's UAV format (see below). The "Tint as is" column is
`shader_batch --target wgsl --wgsl-no-polyfill` without `--uav-format`: bind groups and locations are still patched (they
only renumber decorations), nothing else.

## Results

### Per profile (with the rewrites)

| Profile | Jobs | Reach SPIR-V | Valid WGSL | % of jobs |
|---|---|---|---|---|
| `low` (+ 75 skinning variants) | 433 | 428 | 426 | 98.38 |
| `medium` | 361 | 353 | 351 | 97.23 |
| `high_hbao` | 362 | 353 | 351 | 96.96 |
| `ultra_hdao` | 361 | 352 | 350 | 96.95 |
| `msaa4` | 435 | 428 | 426 | 97.93 |
| `ssao4_hdao` | 437 | 422 | 420 | 96.11 |

Every job that does not reach SPIR-V fails in the HLSL front end, identically for the Metal path (`SHADERS.md`: include-only
fragments compiled alone, the self-including file, the `#unfdef` typo, the 3D-fluid leftovers, plus `ssao_hbao.ps`
standalone). Per stage over the four default profiles: vertex 487 / 487, pixel 983 / 991, compute 8 / 8.

### Runtime set

`r4_shader_warmup` scans the R4 blenders and the `.s` scripts the way the engine does and finds 334 requests (skinning
variants included). Exported with their exact macro lists (`SMAA_QUALITY=3`, `SSAO_QUALITY=3`, no HBAO/HDAO, ...) and
replayed through the same pipeline, every one is accounted for:

| Class | Requests | Which |
|---|---|---|
| Valid WGSL | 251 | |
| Valid WGSL, file only in the repo's `gamedata/shaders` overlay (the warmup mounts only the game data, so it calls them missing) | 8 | `fakescope.ps`, `hdr10_bloom_*`, `hdr10_lens_flare_*` |
| WGSL fails: `clip_distances` as a fragment input | 2 | `fluid_obst_static_oobb.ps`, `fluid_obst_dynamic_oobb.ps` |
| HLSL front end fails (also on Metal) | 2 | `fluid_draw_texture.ps` (undeclared `textureNumber`), `fluid_obststaticbox.ps` (redeclared `boxLBDcorner`) |
| Geometry shader, no such stage | 3 | `fluid_array.gs`, `fluid_array_oobb.gs`, `fluid_array_dyn_oobb.gs` |
| Missing from the data (as on Metal: SSFX addon files, DX9-era names, `null.*`) | 68 | listed in `runtime/summary.md` |

The **253 requests in the game data that reach SPIR-V** are the set the brief names: 251 give valid WGSL, the two
`fluid_obst_*_oobb.ps` do not.

The four 3D-fluid failures (two front end, two WGSL) and the three geometry shaders are the fluid simulation, which the
Metal build already turns off (`CRender::create`) and the plan locks off on web.

### Speed

Tint (decoration patch, rewrites, SPIR-V reader, WGSL writer, re-parse, lowering, inspector) took a mean of 7.8 ms and a
median of 5.7 ms per shader on the unpatched run, slowest about 180 ms. The final runs measured a mean of 9 to 12 ms and
a median of 6.5 ms, slowest 270 ms, but the machine was shared with other sessions and one or two running game
instances (load average 20 to 60 on 12 cores), so these are indicative only. A sweep of all six profiles plus the
runtime set takes a few minutes. Either way it is small next to vkd3d's own 10 to 20 ms per shader, and it happens
once, on the Mac, when the pack is built.

## Failure classes of Tint as it is

Of the 1486 jobs that reach SPIR-V (four default profiles), 227 fail in Tint. Four classes, all in the SPIR-V reader:

| Class | Jobs | Files | Example | Cause | Fix |
|---|---|---|---|---|---|
| `IsNan is not supported because NaNs cannot be represented in WGSL` | 169 | 50 | `accum_sun_near.ps` | vkd3d pattern. D3D defines float → int conversion of NaN as 0, so vkd3d emits `OpIsNan` + `OpSelect` around every `ftoi`/`ftou`; also for `isfinite` and the `ord`/`unord` compares. Tint rejects `OpIsNan` and `OpIsInf` outright. | **Done:** rewrite to integer tests on the bits, `(bitcast<u32>(x) & 0x7fffffff) > 0x7f800000` (`== 0x7f800000` for Inf). Exact, and no compiler can fold it away the way it can fold `x != x` under fast math. |
| Tint internal compiler error, `TINT_ASSERT(std::isfinite(v.value))` (`core/constant/scalar.h:59`): the process aborts | 42 | 16 | `hud_default.ps`, `yuv2rgb.ps`, `effects_sun.ps`, `postprocess.ps` | vkd3d pattern: **the NaN-pattern booleans**. DXBC registers are typeless, so a comparison result (`0xffffffff`) that lands in a float register becomes `OpConstant %float -nan`. Also the shaders' own `1.#INF`/NaN literals (`postprocess.ps` has `0x7fc00000` and `0xff800000`, one inside a vector constant). Tint asserts on any non-finite constant, and in WGSL a non-finite const-expression is a shader-creation error anyway. | **Done:** each such constant becomes a load from a `var<private> : u32` initialised with the same bits, bitcast to `f32` where it is used; vector constants holding one are rebuilt at the use. Run-time values, so Tint never folds them. The bits survive as on the Metal path (`bitcast` is `as_type` in MSL). |
| `SPIR-V capability 'StorageImageWriteWithoutFormat' is not supported` | 8 | 2 | `ssao_hdao.cs` | WebGPU limit: a D3D `RWTexture2D<float>` carries no format; WGSL's `texture_storage_2d` needs one. | **Done:** the caller passes the UAV format (`Options::uav_formats`; HDAO's target is `R16F`, `r4_rendertarget.cpp:980`). The patch fills it in, drops the `*WithoutFormat` capabilities and adds `StorageImageExtendedFormats` for `r16float`. |
| `clip_distances cannot be used on a fragment shader input` | 8 | 2 | `fluid_obst_static_oobb.ps` | WebGPU limit: these pixel shaders read `SV_ClipDistance0/1` as interpolated inputs (the box test in `fluid_common.h`). WebGPU has no clip-distance fragment input. | Not done (3D fluid is off on web). If it is ever needed: the vertex shader writes the same values to an extra `@location`, the pixel shader reads that. |

Ten of the 42 ICE jobs (8 files) also need the `OpIsNan` rewrite. Over the default profiles the `OpIsNan` rewrite fires
in 183 jobs (52 files, HDAO included) and the constant rewrite in 42 jobs (16 files).

With the rewrites the first two classes are gone, the third is gone given the format, and the fourth stays: 8 jobs, the
2 fluid files, in every profile.

## How each D3D feature maps

### Comparison samplers and depth textures

The MSL path learns which sampler compares and which texture is a depth texture from SPIRV-Cross's analysis after
compiling (`variable_is_depth_or_compare`, `SpirvToMsl.cpp`), because vkd3d declares the image as non-depth and the
sampler as a plain `OpTypeSampler`. **Tint's SPIR-V reader does the same inference by itself**: an image used with
`OpImageSampleDref*` becomes `texture_depth_2d`, its sampler `sampler_comparison`. The sweep compared the two on every
valid job: **1478 of 1478 agree** (comparison sampler and depth texture in 158 jobs, 43 files: every shader that does
`SampleCmp` on `s_smap`). So the WGSL itself carries the flags; the pack's reflection takes them from Tint's inspector
(`Binding::kind` = `depth_texture` / `sampler_comparison`), and the WebGPU backend creates the bind group layout entry
(`sampleType: "depth"`, `type: "comparison"`) and the sampler (`compare: "less-equal"` for `smp_smap`) from that.

### Depth textures sampled as float with a filtering sampler

WebGPU cannot filter a depth texture with a filtering sampler. The sweep lists every sampler/texture pair that reads a
depth resource (`s_smap`, bound to `$user$smap_depth`, the only depth-format SRV R4 binds) and the 32-bit float targets
(`s_smap_minmax`, R32F; `s_half_depth`, R32F on NVIDIA):

| Resource | Declared in WGSL as | Read with | Files |
|---|---|---|---|
| `s_smap` (shadow map, depth format) | `texture_depth_2d` | `smp_smap`, comparison | 43 |
| `s_smap` | `texture_2d<f32>` | `smp_nofilter`, point | 4: `accum_sun{,_msaa,_nomsaa}.ps`, `create_minmax_sm.ps` |
| `s_smap_minmax` (R32F) | `texture_2d<f32>` | `smp_nofilter`, point | 8: `accum_sun_far*.ps`, `accum_sun_near*.ps` |
| `s_half_depth` (R16F, R32F on NVIDIA) | `texture_2d<f32>` | `smp_nofilter`, point | 3: `combine_1{,_msaa,_nomsaa}.ps` |

**No stock shader reads a depth texture through a filtering sampler.** The four that read the shadow map without
comparison (`accum_sun*.ps` in the luminance pass, `create_minmax_sm.ps`) use `smp_nofilter` (point, clamp): legal as a
`texture_2d<f32>` binding with `sampleType: "unfilterable-float"` and a `"non-filtering"` sampler. 23 files also
`textureLoad` the shadow map, which needs no sampler. The 32-bit float targets, which WebGPU cannot filter without the
`float32-filterable` feature, are also read only with `smp_nofilter`, so that feature is not needed. The backend must
still create these layouts itself (`sampleType: "unfilterable-float"`, `type: "non-filtering"`) from the pack's
reflection plus the engine's sampler names (`smp_nofilter`, `smp_jitter` and `smp_point` are point-sampled, per
`Blender_Recorder_R3.cpp`), since the texture's real format is not visible in the shader.

### Bindings: registers to bind groups

D3D11 registers are per stage (a vertex shader's `b0` and a pixel shader's `b0` are different buffers); WebGPU bindings
are per pipeline layout. The back end therefore gives each stage its own bind group and keeps the register number
inside it, at a fixed base per register class (`BindingLayout`, changed by patching the SPIR-V decorations from the
`Internal.h:11-14` numbering, set 0 and `class base 0/256/512/768 + register`):

| | Vertex | Pixel | Compute |
|---|---|---|---|
| `@group` | 0 | 1 | 0 |
| `b#` constant buffer | `@binding(#)`, 0..15 | same | same |
| `t#` SRV | `@binding(16 + #)`, 16..47 | same | same |
| `s#` sampler | `@binding(48 + #)`, 48..63 | same | same |
| `u#` UAV | `@binding(64 + #)` | same | same |

The stock shaders use at most `b2`, `t10`, `s3`, `u0` and 15 bindings in one shader, well under WebGPU's per-stage
limits (12 uniform buffers, 16 sampled textures, 16 samplers) and its 4 bind groups. Two groups leave group 2 and 3
free, for example for a per-frame group if profiling later shows rebinding `static_globals` costs. The D3D11 shim then
maps "set constant buffer / SRV / sampler at slot N of stage S" straight to a bind group entry, and caches bind groups
by the resources bound. Constant buffers fit `@binding` uniform buffers with dynamic offsets into one ring per frame (the
plan's CPU shadow rings); WebGPU allows 8 dynamic uniform buffers per pipeline layout, and R4 shaders use at most 3 per
stage, 6 per pipeline.

### `SV_ClipDistance`

Four vertex shaders write it: `accum_volumetric.vs` (volumetric lights, in the runtime set), `accum_volumetric_nomsaa.vs`
and `fluid_grid_oobb.vs`, `fluid_grid_dyn_oobb.vs` (fluid, off). Tint maps it to `@builtin(clip_distances)
array<f32, 6>` with `enable clip_distances;` (16 jobs). The device must be created with the **`clip-distances`**
feature. Six distances take two of the 16 interstage slots (Dawn counts `ceil(6 / 4)`); the volumetric shaders use far
fewer, so that fits. Without the feature, the fallback is the D3D9 way: a `discard` in the pixel shader on the
interpolated distances.

### `discard` and the NaN-pattern booleans

`discard` (HLSL `clip()` and `discard`) appears in 116 jobs, 29 files, and becomes WGSL `discard`, which demotes the
invocation to a helper (derivatives keep working). That matches what the MSL path gets with
`check_discarded_frag_stores`. Nothing else is needed.

The NaN-pattern booleans are the ICE class above: vkd3d stores comparison results (`0xffffffff`) in float registers.
After the rewrite those values exist only at run time, so WGSL's rule against non-finite constants no longer applies.
Two cautions: WGSL lets an implementation assume NaNs are absent, so a backend could in principle canonicalise a NaN
held in an `f32`. Dawn's MSL, HLSL and SPIR-V outputs use plain bitcasts and the same bit patterns already work on the
Metal port, but this should be checked on each Dawn backend with the image diff (W2). The rewrite also leaves real float
NaN arithmetic alone.

### Fragment inputs the vertex stage does not write

WebGPU fails `createRenderPipeline` when a fragment input has no vertex output at the same `@location`, **and also when
the component count, base type or interpolation differ** (`RenderPipeline.cpp`, exact match). The sweep checked every
(vertex, pixel) pair that blenders and `.s` scripts request, with the WGSL of each profile and of the runtime set:

| | Default profiles (676 combinations) | Runtime set (174 combinations) |
|---|---|---|
| Link as they are | 650 | 164 |
| Pixel input with no vertex output | 22 (9 pairs): `model_def_lplanes` → `model_scope_lense` (`@location(1)`), → `models_lfo_light_dot_weapons` (1, 2); `model_distort`, `model_distort_inv` → `particle_distort` (1); `water_{regular,ryaska,studen,underground}` (7) | 7 (7 pairs): the five `model_*` ones, and `stub_notransform_aa_AA` → `combine_2_AA`, `combine_2_AA_D` (`POSITIONT`, 14) |
| Component count differs | 8 (2 pairs): `waterd` (vec3 written, vec2 read, 2), `wmark` → `simple` (vec3 / vec2, 0) | 4 (4 pairs): those two, and `stub_notransform_aa_AA` → `accum_sun_msaa`, `accum_sun_nomsaa` (vec2 written, vec4 read, 4) |
| Base type or interpolation differs | 0 | 0 |

These are the cases the Metal shim handles at draw time (`PatchFragmentInputs`, `AdaptFragmentInputs` in
`mtl_shader.mm`, called from `Context::fragmentFunctionFor`): the pairs that read unwritten inputs, and the `waterd`,
`wmark` + `simple` and `accum_sun_*` pairs whose D3D register packing gives different vector widths (`METAL-SHIM.md`
item 13). Which pairs need it depends on the macros, so the pack has to check per profile.

**Where the equivalent goes:** the shader pack builder, not the browser. Without addons the pair list is fixed and known
on the Mac (this sweep already derives it), so the builder emits a patched pixel variant per pair that needs one, and the
WebGPU backend picks the variant by (vertex, pixel) at pipeline creation, the same key `fragmentFunctionFor` uses. The
patch fits best at SPIR-V level in this back end, before Tint: an unwritten input becomes a `Private` variable
initialised to zero (storage class and pointer type change, `Location` and the interface entry go); a width mismatch is
fixed by giving the pixel input the vertex side's vector type and narrowing after the load. A WGSL-text patch like the
MSL one would also work (Tint's output is regular), but SPIR-V keeps it independent of Tint's printing. A pair the
pack did not anticipate is logged and falls back to the same patch done at run time, which needs Tint in the browser,
so the backend should rather refuse the pipeline and log it.

A simpler alternative: make every interstage variable `vec4<f32>` on both sides (D3D registers are four wide anyway).
That removes all width mismatches in one rule; only the unwritten inputs then need variants.

Interstage locations follow D3D's semantic matching through a fixed table (`wgslInterstageLocation` in
`shader_batch`): `TEXCOORD0..9` → 0..9, `COLOR0..1` → 10..11, `FOG` → 12, `POSITION` → 13, `POSITIONT` → 14,
`NORMAL` → 15. The stock shaders link exactly `TEXCOORD0..7`, `COLOR0..1`, `FOG`, `POSITION` and `POSITIONT` between
stages, so everything fits WebGPU's 16 interstage locations; the MSL table (`interstageLocation`, up to 32) does not.

### HDAO compute and its `R16F` storage texture

`ssao_hdao.cs` and `ssao_hdao_msaa.cs` (used only with HDAO at SSAO quality 4) translate to valid WGSL once the format is
known: `texture_storage_2d<r16float, write>` at `@group(0) @binding(64)`, workgroup 32 × 32 × 1. Two WebGPU limits
remain, both at pipeline creation:

- **1024 invocations per workgroup**, over the default `maxComputeInvocationsPerWorkgroup` of 256 (and
  `maxComputeWorkgroupSizeX/Y` of 256 are fine). Apple and desktop adapters typically offer 1024, so the device can
  request it **(verify per browser and GPU)**; otherwise the shader needs a 16 × 16 rewrite.
- **`r16float` as a storage format** is not in core WebGPU; it comes with the `texture-formats-tier1` feature
  **(verify availability in Chrome 157 on Mac and Windows)**. Fallback: create the target as `r32float`, which is a core
  storage format (`--uav-format 0=r32float`), at twice the memory.

Group-shared memory: 12,544 bytes in the configuration that really uses it (`ssao4_hdao`; the other profiles compile the HDAO body out), under the default `maxComputeWorkgroupStorageSize` of 16,384. The plan already locks HDAO compute off on web, so none of this blocks.

### Border sampler address mode

Only one place in R4 sets `D3DTADDRESS_BORDER`: `blender_light_direct.cpp:80` and `:218`, the sun's far cascade
(`SE_SUN_FAR`), which gives `smp_smap` a white border so that lookups outside the far shadow map count as lit. The
shaders are `accum_sun_far_nomsaa.ps` (runtime set) and `accum_sun_far_msaa.ps` (MSAA). Both translate; the WGSL
sampler gets `clamp-to-edge`, so samples outside the map would take the edge texel instead of "lit". The fix is a
bounds test in the shader: in these two shaders, compare the projected coordinate against `[0, 1]` and return 1.0 outside,
either as an HLSL-side macro when building the pack for web or as a SPIR-V patch. No other blender uses border
addressing; `Blender_Recorder_R2.cpp:114` and `R3.cpp:78` are only the setters.

### Derivatives in non-uniform control flow

HLSL lets implicit-LOD sampling and `ddx`/`ddy` sit in branches that differ per pixel; WGSL's uniformity analysis
rejects that. Tint's writer can emit `diagnostic(off, derivative_uniformity);`, valid WGSL that Chrome accepts
(diagnostic filtering is part of WGSL). The sweep validates each shader again without it: 105 jobs, 33 files need it
(the steep-parallax `deffer_base_*steep*-hq.ps`, `combine_1*`, `effects_sun.ps`, the HDR10 blur passes, `fakescope.ps`,
`pda_overlay.ps`, ...). D3D gives the same undefined-derivative behaviour there, so this changes nothing on screen.

## Naga and DXC

Not run. The brief asks for them if Tint rejects a large share; with the rewrites Tint rejects 8 jobs of 1486 (two fluid
files, a WebGPU limit that any translator hits), so neither could change the answer. Two notes for the record:

- Installing `naga-cli` from crates.io was blocked in this session's sandbox. To try it: `cargo install naga-cli`, then
  `naga <job>.spv <job>.wgsl` on the SPIR-V that `shader_batch` keeps in `wgsl_failed_spirv/` (run with
  `--wgsl-no-polyfill` to get the 227 unpatched failures).
- DXC as the front end was measured in `SHADERS.md`: it loses 8.6 % of jobs at the HLSL stage (non-immediate sample
  offsets in `shadow.h`), which is worse than everything Tint rejected before the rewrites, and it drops the D3D
  reflection the engine needs.

## Recommendation

Use the pipeline as it stands with Tint as the last step: **HLSL → vkd3d → DXBC → SPIR-V → (patch) → Tint → WGSL**,
ahead of time on the Mac, pinned to the Dawn revision of the Chrome stable the web build targets. Ship the WGSL plus the
reflection (D3D names, registers and cbuffer layouts from the DXBC; WGSL bindings, depth/comparison flags and
interstage locations from Tint) in the pack.

Remaining work, roughly in order:

1. **Pack builder** (W2): a tool on top of `ShaderXlatWgsl` that runs the engine's request set per graphics profile,
   writes WGSL + reflection keyed like `TranslationCache`, and fails the build on any non-fluid shader that does not
   validate. `shader_batch --requests` is most of it.
2. **Pipeline linking variants**: the unwritten-input and width-mismatch pairs above, found per profile, as pixel
   variants in the pack (or the all-`vec4` rule plus zero variants).
3. **Device features and limits**: `clip-distances`, `texture-compression-bc`; for HDAO only, 1024 invocations and
   `texture-formats-tier1`. Check at start-up.
4. **Explicit bind group layouts** from the pack's reflection (not `layout: "auto"`), so the shadow map binds as
   `unfilterable-float` with a non-filtering sampler where it is read without comparison.
5. **Border emulation** in the two `accum_sun_far*` shaders.
6. **Pixel check on a Dawn backend** (W2's image diff): the NaN-pattern values and `discard` behave as on Metal.
7. Keep `ShaderXlatWgsl` in sync with Dawn: a Chrome stable bump means a new pin and a sweep; the numbers above take
   about ten minutes to reproduce.

## Proposed plan changes

- Risk 1 (shader translation) can drop to low: measured, three rewrites needed, all done.
- Renderer table: add "interstage component counts must match exactly" next to the unwritten-input row, and note the
  fixed interstage location table.
- Renderer table, HDAO row: name the two blockers (1024 invocations, `r16float` storage) rather than "missing or
  restricted".
- Shaders section: the pack also needs the per-pair pixel variants; the browser never runs Tint either.
- Add `clip-distances` to the required features (volumetric lights in the default runtime set use it).

## Not verified

- No WGSL ran on a GPU. Validity is Tint's (parse, resolve, lower) plus the entry-point limit checks Dawn makes;
  pipeline-level checks (bind group layouts against the shim's bindings, vertex buffer layouts, colour target formats)
  need the WebGPU backend.
- The rewrites are checked by validation and by `shaderxlat_wgsl_selftest`, not by comparing images.
- The device features and limits named above are from Dawn's source and the WebGPU spec, not from querying a browser's
  adapter on the target machines.
- The runtime set is the warmup's view at default settings. Other console settings produce other macro lists; the six
  profiles cover the main ones.
- The SSFX addon shaders the engine asks for are not in the data (`SHADERS.md`), so they are missing here too.
- Timings were taken on a heavily shared machine.
