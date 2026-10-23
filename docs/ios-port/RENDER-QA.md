# R4 on Metal: correctness pass, option support, performance and memory

What the M2 pass found by looking at the renderer through fixed scenes, what was fixed, which Anomaly graphics options the Metal
renderer supports, and what a frame costs. Measured on an Apple M2 Pro (macOS, Release build, unattended offscreen runs); numbers
are for comparing settings with each other, not absolute iPad figures (see "iPad").

## The scene harness

`tools/render_harness` runs `--script` scenes of the real game unattended (offscreen, in the background, own state root per run, see
INTEGRATION.md) and collects screenshots, the engine log and the Metal statistics under `build/render-harness/<tag>/<scene>/`.

```bash
tools/render_harness/run_scene.sh --newgame                 # once: new game, saved as h_cordon (the base of the scenes)
tools/render_harness/run_scene.sh -t mytag 02_cordon_day 05_rain
tools/render_harness/run_scene.sh -t mytag -p low -s 2732x2048 02_cordon_day   # a preset (presets/*.ltx), a back buffer size
tools/render_harness/sweep.sh -t sw "minimum low default" "02_cordon_day 03_bunker"   # presets x scenes, prints the perf table
tools/render_harness/perf_report.sh mytag                   # the Metal statistics of runs as a markdown table
```

`HARNESS_CONSOLE="gfx_mode performance;r__upscaler metalfx"` runs console commands before the level loads (the iPad's profile on a Mac:
`HARNESS_ARGS="--graphics-profile ipad-8gb --unlocked-graphics"` and `-s 1366x1024`). The helpers the scenes call are in
`gamedata/scripts/xr_harness.script` (`env`, `tp`, `talk`, `particle`, `blowout`, `nightvision`, `weapon`, `zoom`, ...).

Environment: `HARNESS_COLD_SHADERS=1` (empty shader cache), `HARNESS_ENV="VAR=value ..."` (passed to the app, e.g. `MallocScribble=1`),
`HARNESS_FOOTPRINT=1` (physical footprint of the process at about 40 s), `HARNESS_SIZE=WxH` (sweep.sh).

| Scene | What it shows |
|---|---|
| `00_menu` | main menu |
| `01_loading`, `01b_loadbar` | the loading screen when the level is ready; one frame per load stage while a save loads (`load_shots`) |
| `02_cordon_day`, `03_bunker`, `03b_interior_rain` | outdoors at noon, the trader's bunker, the starting hut in heavy rain |
| `04_night_flashlight` | night with the player's torch on (a spot light with shadows) |
| `05_rain` | outdoors in heavy rain |
| `06_inventory`, `07_pda`, `07b_pda_map` | inventory, PDA (plain dialog; the 3D PDA needs the hand animation) |
| `08_far_terrain` | four views from 35 m up: the far landscape, sky and fog |
| `09_dialog` | the talk window with the Cordon's trader |
| `10_resize` | `vid_mode` / `rs_screenmode` changes at run time (device reset) |
| `11_emission`, `12_emission_night` | the blowout's sky and light by day (red sky, and the white flash) and at night |
| `13_fire_smoke` | the camp fire's flame and smoke, sample fire / smoke-with-distortion / radial-distortion effects of `particles.xr` |
| `15_scope` | a Dragunov with its 1P29: the weapon and hands at the hip, then aimed through the scope |
| `16_nightvision` | night vision on, at night in the Rookie village |

Debugging commands the harness uses (all Apple-only console commands of the renderer):

* `screenshot <name> [frames]`: the next presented frame (the Metal back buffer is only readable at Present, see METAL-SHIM.md).
* `load_shots <prefix>`: a screenshot of every loading-screen frame that shows a new load stage. The load blocks the main loop, so a
  script step cannot ask for it.
* `rt_dump <target> <file> [stage] [mode] [lo hi] [x y w h]`: an intermediate target (`$user$position`, `depth`, `smap`, or a loaded
  texture as `tex:ui\ui_icon_equipment`) at a point of the frame, as a PNG with min/max/mean in the log.
* `mtl_trace <n>`: the next n draws with shader names, render targets, depth/stencil/blend/cull/scissor state, topology and bound textures.
* The shim logs `metal: gpu ... cpu ...` every 10 s (the source of the tables below), shaders and render targets carry names.

## Defects found and fixed

| Where | What was wrong | Root cause | Fix |
|---|---|---|---|
| Inventory, Load game list, any Lua-coloured UI | item icons and text invisible ("dark rectangles") | luabind converted a negative Lua number (`GetARGB` returns the colour as a signed int) to `unsigned` through `static_cast<unsigned>(double)`: x86 wraps, arm64 saturates to 0, so every Lua-set colour was `0x00000000` | `ca9cf35d` luabind: through `long long` (found on the iPad by the M4 session, taken here) |
| Rain in interiors | drops fell through roofs and showed in every room | `CEffect_Rain::Born` called `RenewItem(dest, height, RayPick(..., height, ...))`; the ray shortens `height`, but arm64 clang reads the argument before the call (order of evaluation is unspecified), so every drop was timed for the full 30 m and never stopped at the geometry it hit | `fcd8bdaa` the ray is its own statement |
| Loading screen | the progress bar was always full, then dark at the end (the "blink") | `iFloor`/`iCeil` (xrCore `_bitwise.h`) shift by up to 158 bits and by negative counts; x86 masks the count, clang folds a constant argument to poison on arm64: `color_argb_f(1,1,1,1)` returned `0x0000ffff`. Everything built from `color_argb_f`/`color_rgba_f` with constants was affected (progress colours, light animations, UI tints) | `5e4b9635` `floorf`/`ceilf` on Apple |
| Water | the water distortion pass never drew; `pipeline creation failed ... user(locn2) mismatching` in the log | `waterd.vs` packs `FOG` next to `TEXCOORD2` (float3 at one location), `waterd.ps` reads a float2 there; D3D links by semantic and component, Metal wants the same type | `fc780483` the pixel function is rebuilt with the vertex shader's type and a swizzled local (`AdaptFragmentInputs`) |
| Terrain beyond 240 m | drawn with the stand-in shader that writes one colour target instead of the G-buffer | landscape element 3 asks for `deffer_terrain_low_flat.ps`, which only the SSFX shader pack has | `a8541b35` without the pack it uses the near terrain's shader |
| Start-up with `r3_msaa 8x` | crash in `CRT::create` | Apple GPUs refuse 8 samples; the failed texture was dereferenced | `402c29fb` the setting is lowered until the device accepts it, with a log line |
| One shader pair | `E5013` (an input semantic used twice with different widths) killed the game | FXC accepts it, vkd3d-shader does not | `eea900f1` an alias hook in ShaderXlat; a shader that still fails to translate skips its draws |
| Screenshots | the Metal back buffer is empty after Present | capture is a blit in the Present command buffer; its state outlives the swap chain, rows are 256-byte aligned, a failed blit is not published | `679605d7` and the earlier harness commits |
| ShaderXlat crash guard | replaced any alternate signal stack with an unfreed 64 KB block | | `fd371d5f` keeps an existing one and frees its own |
| Scoped weapons, aimed | the whole screen magenta and purple, a magenta lens | the fake scope (`fakescope.ps`) samples `$user$scopeRT`, which nothing in the engine draws into; D3D11 hands out a render target zeroed (no overlay), a private Metal texture shows what its memory held | the shim clears a render target or depth buffer created without data (`Context::zeroInitialise`). The magenta came back in a later run of scenes under heavy machine load (load average 400) with Metal's fast math and not with safe or relaxed math: the library is now compiled with relaxed math (METAL-SHIM.md item 19); the cause of that second magenta is not pinned down |
| Skinned models' tangent frame | `float-cast-overflow` in `q_N` while a model loads (SANITIZERS.md open item 4: "a NaN normal") | not a normal and not NaN in the file: 779 of 25.5 M skinned vertices in 139 models (`item_merger`, `mwolf`, `rotan`, `mini_psevdogigant`, stalker suits and masks, weapons and their variants) carry a binormal of (inf, inf, inf), and 5,512 vertices of weapons an infinite texture coordinate; `normalize_safe` turns infinity into NaN (inf * 0), the conversion to a byte is undefined (x64: `INT_MIN` and a clamp, arm64: 0 or saturated) | `FSkinned.cpp` replaces a non-finite component with 0 before it is normalised and quantised, `q_N` maps NaN to the middle of the range |
| Shader sharing | `ShaderElement::equal(ShaderElement*)` tested `this` against null, `&*E[4]` and `*val_pTransform` bound references to null | clang may fold `this == 0` to false (only xrGame is built with `-fno-delete-null-pointer-checks`): two shaders without a technique compared unequal | `equal_elements` on the pointers, `E[4]._get()`, `IsValuableToRender` takes the matrix by pointer and reads it only for dynamic geometry |
| ShaderXlat crash guard, again | `AltStack` left the (freed) stack installed when its thread exited | Darwin refuses `sigaltstack(SS_DISABLE)` with a size of 0 (`ENOMEM`) and keeps the stack | the disable carries the size and is checked; the guard installs nothing under a sanitizer (the runtime owns the thread's stack and unmaps it at exit); a thread test in `shaderxlat_selftest` |
| Log line per pair | `pixel shader ... zero-filled, 1 of another width adapted` (and, before `fc780483`, `pipeline creation failed ... user(locn2) mismatching`) once per level load | D3D links interpolants by semantic and component; the stock set has four pairs that pack a register differently (`waterd`, `wmark` + `simple`, `stub_notransform_aa_AA` + `accum_sun_msaa`/`_nomsaa`) | adapting is the normal path: not logged, counted (`Stats::linkAdaptedPairs`); `tools/r4_shader_warmup` lists the pairs and compiles each adapted variant; only a pair that cannot be adapted is logged |

Checked and fine in the scenes: sky, fog, sun and spot-light shadows, particles (fire, smoke), decals, HUD, water-free terrain,
the PDA map, text in all fonts, the main menu, the loading screen. Not exercised: SMAA/other AA modes, HDR10, the 3D PDA, weapon
HUD models (the scenes carry no weapon in hand), MP maps.

### Open

* **Heap corruption in `preproc_free_macro`** reported from the iPad (about 4 in 25 runs): not reproduced on the Mac. About 7,000
  translations under ASan, a Debug ASan+UBSan+pattern-init build, and the game with a cold cache under `MallocScribble`/`GuardEdges`
  found nothing in ShaderXlat/vkd3d. A macro freed at the end of preprocessing is the first free of everything allocated during it,
  so an overflow or use-after-free by other code shows up there first. Next: the same malloc environment on the iPad, a whole-engine
  ASan run, shutdown ordering (the fatal case was translating at quit). Details in the shared notes folder.
* **Unspecified evaluation order and other x86-only undefined behaviour** is a class: two instances (`Born`, `iFloor`) explained
  four visible defects. `-Wunsequenced` does not catch a by-reference change inside a call; a sweep of `f(x, g(&x))` patterns and of
  bit tricks (`*(int*)&f`, large shifts) would find more.
* The torch key (`Hit_TorchToggle`) and some hand animations use Lua exports this port does not provide (`stop_hud_motion`); the
  harness switches the torch with `enable_torch`.
* `vid_mode` only sizes the back buffer in `borderless` mode (windowed follows the window, fullscreen the display); a resize resets
  the device without damage (`10_resize`).

## Graphics options on Metal

| Option | Console | Status |
|---|---|---|
| Quality presets Minimum / Low / Default / High / Extreme | `_preset`, `rspec_*.ltx` | all run (tables below); Extreme's tessellation request is ignored |
| Tessellation | `r4_enable_tessellation` | **off**: no hull/domain stages; forced off with a log line |
| 3D fluid / volumetric smoke | `r3_volumetric_smoke` | **off**: needs geometry shaders and layered 3D targets; forced off |
| Screen Space Shaders features (ssr, sss, bloom, taa, fog, water, ao, il, volumetric, motion blur, terrain, glass, rain) | `ssfx_*` | **off**: the pack's 34 shader files are not in the game data (`ssfx_*.ps` x 31, `heatvision.ps`, `ssfx_sky_mv.vs`, `deffer_terrain_low_flat.ps`); one log line lists the features that are off, the passes that would use a stand-in are skipped with the feature |
| Heat vision | `r__heatvision` | off (shader file absent) |
| Ambient occlusion | `r2_ssao_mode` disabled / default / hdao / hbao | all four render; +0.2 to +0.5 ms GPU at 720p |
| MSAA | `r3_msaa` off / 2x / 4x / 8x | off, 2x, 4x work (GPU 3.9 / 4.6 / 6.0 ms at 720p); **8x is lowered to 4x**; MSAA alpha test not exercised |
| Sun and light shadow quality | `r2_sun_quality`, `r2_ls_squality`, `r2_sun` | all values render; no measurable memory or GPU difference at 720p; `r2_sun off` gives flat light |
| Grass density and radius | `r__detail_density`, `r__detail_radius` | work (dense: +350 draws, +0.3 ms) |
| Texture quality | `texture_lod` 0-3 | works; the largest memory lever (below) |
| Textures not kept in system memory | `r__no_ram_textures` | no measurable footprint change on the Mac |
| Soft water / soft particles / steep parallax / detail bump | `r2_soft_water` ... | in the presets, render without artefacts |
| Wet surfaces | `r3_dynamic_wet_surfaces` | work; rain checked outdoors and in interiors |
| Window modes and resolution | `rs_screenmode`, `vid_mode`, `vid_restart` | windowed, borderless, fullscreen and resizes reset the device cleanly |
| Gamma | `r__gamma` | the gamma ramp is ignored on `CAMetalLayer` (a shader pass would have to apply it) |
| HDR10, SMAA, other AA | `r4_hdr10_on`, `r2_smaa`, `r2_aa` | not exercised |

## Performance and memory

`gpu` is the GPU time per frame (command buffer start to end), `cpu` the wall time per frame minus time blocked on the GPU; `Metal`
is `MTLDevice.currentAllocatedSize`, `footprint` the process's physical footprint from `footprint(1)`. The CPU numbers move by
several ms between runs on a shared machine (the game is single-thread bound on its own logic); the GPU, memory and draw numbers are
stable.

### Presets, 1280x720, three scenes

| Preset | Scene | GPU ms | CPU ms | draws | passes | textures MB | Metal MB | footprint MB |
|---|---|---|---|---|---|---|---|---|
| Minimum | day | 2.96 | 6.96 | 1031 | 28.5 | 1356 | 1611 | 2837 |
| Minimum | bunker | 2.32 | 5.20 | 388 | 29.1 | 1349 | 1599 | 2900 |
| Minimum | rain | 2.83 | 6.88 | 818 | 29.4 | 1357 | 1615 | 3008 |
| Low | day | 3.15 | 7.68 | 1310 | 28.6 | 1352 | 1608 | 2848 |
| Low | bunker | 2.21 | 5.29 | 404 | 29.1 | 1351 | 1598 | 2793 |
| Low | rain | 3.21 | 7.80 | 1076 | 30.3 | 1362 | 1618 | 3096 |
| Default | day | 4.64 | 8.80 | 1499 | 31.5 | 1701 | 1958 | 3207 |
| Default | bunker | 4.01 | 7.55 | 958 | 36.1 | 1695 | 1947 | 3130 |
| Default | rain | 5.38 | 11.63 | 1796 | 36.1 | 1704 | 1967 | 3306 |
| High | day | 6.18 | 12.12 | 1712 | 31.5 | 2580 | 2832 | 4503 |
| High | bunker | 5.76 | 9.43 | 962 | 36.1 | 2548 | 2798 | 4030 |
| High | rain | 5.01 | 7.82 | 2022 | 33.4 | 2572 | 2826 | 4164 |
| Extreme | day | 5.82 | 10.70 | 1702 | 31.5 | 3664 | 3919 | 5335 |
| Extreme | bunker | 5.87 | 10.10 | 959 | 36.1 | 3628 | 3880 | 5215 |
| Extreme | rain | 7.38 | 13.31 | 2066 | 36.1 | 3627 | 3885 | 5579 |

### Render targets: 1280x720 and the iPad Air's 2732x2048, day scene

| Size | Preset | GPU ms | textures MB | of which render targets | Metal MB | footprint MB |
|---|---|---|---|---|---|---|
| 1280x720 | Minimum | 3.16 | 1351 | 177 | 1608 | 3225 |
| 1280x720 | Low | 3.23 | 1350 | 177 | 1604 | 3239 |
| 1280x720 | Default (2x MSAA) | 5.04 | 1699 | 258 | 1956 | 3446 |
| 1280x720 | High (4x MSAA) | 6.26 | 2558 | 391 | 2810 | 4091 |
| 1280x720 | Extreme (4x MSAA) | 6.24 | 3644 | 391 | 3899 | 5564 |
| 2732x2048 | Minimum | 5.46 | 2100 | 903 | 2351 | 3669 |
| 2732x2048 | Low | 6.02 | 2094 | 903 | 2345 | 3749 |
| 2732x2048 | Default (2x MSAA) | 8.45 | 2859 | 1394 | 3094 | 4436 |
| 2732x2048 | High (4x MSAA) | 18.70 | 4420 | 2205 | 4636 | 5768 |

### Single options on top of Default (1280x720, day scene)

| Run | GPU ms | textures MB | Metal MB |
|---|---|---|---|
| AO off | 4.44 | 1705 | 1965 |
| AO default / HDAO / HBAO | 4.65 / 4.95 / 4.94 | 1701 / 1698 / 1705 | 1959 / 1954 / 1967 |
| shadows low / extreme | 5.03 / 5.06 | 1717 / 1699 | 1979 / 1955 |
| sun off | 5.03 | 1713 | 1973 |
| grass dense / sparse | 5.26 / 4.89 | 1695 / 1695 | 1947 / 1948 |
| MSAA off / 2x (Default) / 4x | 3.91 / 5.04 / 5.99 | 1633 / 1699 / 1838 | 1896 / 1956 / 2091 |
| `texture_lod` 3 / 2 (Default) / 0 | 5.07 / 5.04 / 5.11 | 1432 / 1699 / 3489 | 1682 / 1956 / 3747 |
| `r__no_ram_textures on` | 4.83 | 1704 | 1965 (footprint 3673 vs 3446) |

What cuts GPU memory most, in order: `texture_lod` (3 to 2 is +0.27 GB, 2 to 1 +0.73 GB, 1 to 0 +1.09 GB, from the texture memory without render targets), the back buffer size
(render targets are 177 MB at 720p and 903 MB at 2732x2048 without MSAA), MSAA (2x adds 0.5 GB and 4x 1.3 GB at 2732x2048). Shadow,
AO, grass and sun settings change memory by less than 40 MB. The process footprint is the Metal memory plus about 1.5 to 2 GB of game
data, Lua and audio.

## iPad (Air 13", M3, 8 GB)

The Mac figures above are an M2 Pro (19 GPU cores); the iPad Air's M3 has 9. Expect the GPU times to roughly double; the memory
figures carry over (unified memory, same allocator). The M4 session owns the on-device measurements; this is the shape of the
decision:

* **Memory decides first.** The foreground limit of an iPadOS app on an 8 GB device is about 5 GB (more with the
  `increased-memory-limit` entitlement). At the native 2732x2048: Low 3.7 GB, Default 4.4 GB, High 5.8 GB. High and Extreme do
  not fit at native size.
* **Recommended default for the iPad: Low's options with one more texture level.** `_preset Low`, then `texture_lod 2`: about
  4.0 GB at native size (Low alone is 3.7 GB with `texture_lod 3`) and 6 ms of M2 Pro GPU time per frame (an estimated 12 ms on
  the M3). Low is MSAA off, no AO, low sun quality, anisotropy 1.
* **Medium** (headroom permitting): `_preset Default` with `r3_msaa st_opt_off`: render targets drop from 1394 MB to 903 MB
  (about 4.0 GB in total); 7 ms on the M2 Pro.
* **Do not ship MSAA 4x or Extreme on the iPad**; at native size 4x MSAA costs 1.3 GB and doubles the GPU time.
* A lower `vid_mode` (borderless) is the cheapest way to buy GPU time and memory: render targets scale with the pixel count.
* Keep `texture_lod` at 2 or 3 on 8 GB; `r__no_ram_textures`, shadow, AO and grass settings do not move the memory.
* Everything depending on the SSFX pack is off on every device; tessellation and 3D fluid stay off.
