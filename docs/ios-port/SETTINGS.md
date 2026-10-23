# Graphics settings: console-like

On the iPad and the Mac the game does not ask the player to tune a PC. The device picks **a profile** for its class: texture level,
shadows, sun and lights, ambient occlusion, grass, distances, render size, anti-aliasing and the post-process set, chosen from
measurements for the best picture it keeps at 60 fps. The profile is applied on every start and **locked**: the options menu does
not show those settings, and the console, scripts and addons cannot change them. The player keeps what a console offers: a mode
(Quality or Performance), a frame rate cap, field of view, brightness, motion blur, and the HUD and crosshair.

Everything here is Apple only (`XR_PLATFORM_APPLE`). On Windows the options menu, the console and `user.ltx` behave as before
([Windows](#windows-is-unchanged)).

| Part | Where |
|---|---|
| Device detection, the profile tables, the lock rules (plain C++, tested on any host) | `src/Platform/Apple/GraphicsProfile.{h,cpp}`, hardware probe in `DeviceInfo.mm` |
| Applying the profile after `user.ltx`, the lock in the console, `gfx_mode`, `gfx_fps`, `gfx_profile` | `src/xrEngine/xr_graphics_profile.{h,cpp}`, hooks in `XR_IOConsole.cpp`, `x_ray.cpp`, `xr_ioc_cmd.cpp` |
| The frame rate cap | `src/Platform/App/main.cpp` |
| The reduced options menu | `gamedata/scripts/console_options.script`, hooked in `ui_options_modded_exes.script`; strings in `gamedata/configs/text/*/ui_mm_console_graphics.xml` |
| Tests and measurements | `tools/graphics_profile_test`, `tools/profile_measure.py`, `tools/render_harness/{measure_profile.sh,scenes/2*.script,scenes/30_*.script}` |

## Device classes and profiles

The shell detects the device at start (`AppleShell::Graphics::Hardware`) and the profile follows from it. The log says which:

```
graphics profile ipad-8gb, quality mode, 60 fps cap, locks on (ipad Apple M3, 0 GPU cores, 7.4 GiB, 2732x2048 @ 60 Hz)
```

| Profile | Chosen for | State |
|---|---|---|
| `ipad-8gb` | an iPad with 8 GB (`physicalMemory` 7.0 GiB or more, an 8 GB iPad Air reports 7.4) and an M-series chip | **measured** on the iPad Air 13-inch (M3) |
| `ipad-16gb` | 12 and 16 GB iPads (10.5 GiB or more) | not measured: the 8 GB profile with `texture_lod 0` and a larger shadow map |
| `ipad-6gb` | 6 GB or less, or an A-series chip (iPad mini, iPad 10) | not measured: one step down in textures, render size and distances |
| `mac-pro` | an Apple Silicon Mac with 11 to 26 effective GPU cores (M1 cores count 0.7): M1/M2/M3/M4 Pro, M1 Max | **measured** on a MacBook Pro 14-inch (M2 Pro, 19 GPU cores, 32 GB) |
| `mac-base` / `mac-base-8gb` | up to 10 effective cores (base M1 to M4); the second with less than 12 GB of RAM | not measured: the Pro class one tier down |
| `mac-max` | more than 26 effective cores: M2/M3/M4 Max | not measured: one tier up |
| `mac-ultra` | more than 40 effective cores | not measured |

The GPU core count comes from the I/O registry on a Mac (`gpu-core-count`); the OS does not give it on iPadOS, so an iPad is
classed by RAM and chip. An unknown iPad or Mac falls into the nearest class by those two numbers, so a new device needs no change
to work ([adding a device](#adding-a-device)). `--graphics-profile <id>` forces one (`--graphics-profile ipad` puts the iPad
profile on a Mac, to test its numbers).

**The tables of the unmeasured classes are extrapolations and say so** in the log (`(extrapolated)`) and here. They are
conservative: each is one step from a measured class, in the direction of safety for a smaller GPU and of headroom only where the
measured class had it.

### What the profiles set

`tools/graphics_profile_test --dump` prints every resolved table. The settings that matter, per profile and mode
(Q = Quality, P = Performance; the render size is `r__render_scale` of the native output, see [render size](#render-size-and-render-scale)):

| Setting | ipad-8gb | ipad-16gb | ipad-6gb | mac-pro | mac-base | mac-base-8gb | mac-max | mac-ultra |
|---|---|---|---|---|---|---|---|---|
| texture level | 1 | 0 | 2 | 1 | 1 | 2 | 0 | 0 |
| render size | scale 0.42 / 0.375 (without a scale: 1152x864 / 1024x768) | scale 0.47 / 0.42 (without a scale: 1280x960 / 1152x864) | scale 0.375 / 0.33 (without a scale: 1024x768 / 896x672) | scale 1.0 / 0.7 | scale 0.7 / 0.5 | scale 0.7 / 0.5 | scale 1.0 / 0.8 | scale 1.0 / 0.9 |
| sun/shadow quality | low | low | low | medium / low | low | low | high / medium | high / medium |
| light shadow quality | 0.5 | 0.5 | 0.5 | 1.0 / 0.7 | 0.7 / 0.5 | 0.7 / 0.5 | 1.0 | 1.0 |
| AO level (HDAO on the iPad, SSDO on the Mac) | low / off | low / off | low / off | medium / low | low / off | low / off | high / medium | high / medium |
| shadow distance m | 100 / 80 | 100 / 80 | 100 / 80 | 100 | 100 | 100 | 100 | 100 |
| wet surfaces | off | off | off | on / off | off | off | on | on |
| grass density (lower = denser) | 0.4 | 0.4 | 0.4 | 0.5 | 0.5 | 0.5 | 0.45 / 0.5 | 0.45 / 0.5 |
| grass radius m | 35 / 30 | 35 / 30 | 25 / 20 | 100 / 75 | 75 / 50 | 75 / 50 | 125 / 100 | 125 / 100 |
| view distance | 0.8 / 0.75 | 0.8 / 0.75 | 0.7 / 0.65 | 1.0 / 0.9 | 0.9 / 0.8 | 0.9 / 0.8 | 1.2 / 1.0 | 1.2 / 1.0 |
| decal life s | 30 / 20 | 30 / 20 | 30 / 20 | 50 | 50 | 50 | 50 | 50 |

All profiles share `apple` (unsupported features off, see [below](#unsupported-features-stay-off)), plus per family:

- iPad: `rs_screenmode borderless`, `rs_v_sync on`, `snd_cache_size 32`, `r__tf_aniso 4`, no MSAA, `-smap<size>` shadow map; with a render
  scale: native output, `r__upscaler bilinear`, `r__upscale_sharpness 0.25`.
- Mac: `rs_v_sync on`, `r__tf_aniso 8`, no MSAA; with a render scale: `r__upscaler metalfx`. The window and the display mode stay the
  player's (a desktop has windows).

## Modes

| | Quality | Performance |
|---|---|---|
| Goal | the best picture the device keeps at 60 fps in the worst measured scene | headroom: a steadier frame rate, or 120 fps where the display runs 120 Hz |
| How | the profile's full set at a higher render scale | cheaper shadows and AO, shorter grass and view distance, a lower render scale |
| Frame cap (Auto) | 60 | 120 on a 120 Hz Mac (`mac-pro`, `mac-max`, `mac-ultra`); 60 everywhere else |

The brief for this work assumed a 120 Hz iPad Air. **The iPad Air 13-inch (M3) panel is 60 Hz**: the shell reads
`UIScreen.maximumFramesPerSecond` and the device reports 60. On it Performance cannot mean 120 fps, so it means a lower render scale and
cheaper settings that keep 60 even in the worst scene and when the iPad is warm. The 120 Hz path exists (cap choices 30, 60, 120;
Performance defaults to 120 where the profile allows it and the display runs 120 Hz) and is unit tested, but **no 120 Hz iPad was
available to measure it**. A ProMotion iPad Pro falls in `ipad-8gb` or `ipad-16gb` and gets cap choices up to 120 with Auto at 60.

**A warm iPad is the other limit.** The iPad leaves Nominal for Serious within a minute or two of load and then runs 12 to 20 % slower on the
main thread (RENDERER.md, measured by the MetalFX session: every render scale up to 0.67 then fell to 45 to 51 fps). The two modes divide the
problem: **Quality holds 60 fps cool**, with about 10 % of the frame to spare in the worst scene, so a warm iPad may dip under it; **Performance
holds 60 fps warm**, with 25 % to spare (more than the 20 % slowdown). A player who wants an even frame rate whatever the temperature has the frame
rate cap: 30 fps paces frames evenly (the app skips every other display tick). The measurement tables below have a two-minute warm-up in the
busiest scene before the scenes and a thermal column, so they show what state each window ran in.

Switching mode in the menu applies the profile's settings for that mode and restarts the video (`vid_restart`, a second or two); the
mode is saved in `user.ltx` as `gfx_mode`. The default is Quality.

## What the player can change, and why

| Menu | Option | Console | Why it stays |
|---|---|---|---|
| Visual / Basic | Graphics mode | `gfx_mode` | the one quality choice a console offers |
| | Frame rate | `gfx_fps` | 30 / 60 / (120): the player's choice of smoothness against battery and heat. Auto follows the mode |
| | Brightness | `r__exposure` | rooms differ and so do screens; the stock menu already used this variable for R4 (its gamma ramp is ignored on Metal) |
| | Text size | `ui_text_scale` | the UI text follows the screen's height by itself ([below](#ui-text-size)); this makes it larger or smaller still, for the distance the player sits at. 0.75 to 1.5, applies at the next video restart |
| | FOV, HUD FOV | `fov`, `hud_fov` | comfort. FOV is limited to 45 to 90 |
| | Motion blur | `r2_mblur_enabled` | a taste, and cheap. Off until the player turns it on |
| | Screen mode (Mac only) | `rs_screenmode` | a desktop has windows |
| Visual / UI/HUD, Player | HUD, crosshair, effects | various | the HUD and what the player sees of their character: not a graphics quality setting |
| Visual / Weather, Night | weather and night brightness | `axr_options` | world settings, not hardware settings |
| Modded Exes / Visual | UI/HUD, Crosshair, 3D scopes | various | HUD and optics; the 3D scope switch (`r__3Dfakescope`) is removed from the page: it changes how the scene is rendered |
| Sound, Control, Gameplay, The Zone, Others | all | | untouched. Subtitles (Sound / General) and the language (Others) stay as they are |

Hidden, and why:

| Hidden | Rows | Why |
|---|---|---|
| Visual / Advanced | 42 | every one of them is what the profile sets: distances, texture level, anisotropy, MSAA, SMAA, tessellation, grass, shadows, sun, AO, volumetric lights, soft effects, depth of field, wet surfaces, volumetric smoke |
| Visual / Basic: renderer, monitor, resolution, gamma, contrast, lighting | 6 | one renderer; the profile owns the resolution; the gamma ramp does nothing on Metal; the lighting editor sets the same lights the profile sets |
| Modded Exes / Visual / Graphics, Particles | 8 + 4 | decal and particle budgets, shadow LOD, rain ranges: the profile |
| Modded Exes / Visual / HDR10 | 36 | not exercised on Metal; off in every profile |
| Modded Exes / Optimizations | 12 | engine threading and Lua garbage collection tuning: not a player's setting on a console |
| Modded Exes / Debug | 8 | logging and crash switches |

`tools/render_harness/scenes/22_options_tree.script` prints the tree the player sees, and **fails if any visible row has a locked
command**: with the profile on it prints `OPTIONS locked rows visible: 0`. With `--graphics-profile none` the same scene prints the
full PC tree (Advanced 42 rows, Graphics 8, Particles 4, HDR10 36, Optimizations 12, Debug 8).

## The locks

A profile is active when the app finds a device class for it (always, unless `--graphics-profile none`). With the locks on:

1. **Every start.** `user.ltx` is read, then the profile is applied, so stale or hand-edited values never survive. The engine reports
   what it ignored once per start: `graphics: user.ltx has 190 locked settings, ignored (the ipad-8gb profile applies); 3 of them
   differ from the profile: ...`. (`cfg_save` writes every setting, so `user.ltx` always names the locked ones.)
2. **The console.** `CConsole::ExecuteCommand` asks `xr_gfx::Refuse` before it runs a command. This is the one place that
   covers typed commands, `exec_console_cmd` from any script, `get_console():execute`, `cfg_load`, the options menu's Apply, and an
   addon or MCM. A refused line does nothing and is logged once per command:
   `! graphics: 'r2_sun_quality st_opt_extreme' refused: locked by the mac-pro profile. Further attempts on r2_sun_quality are not logged.`
   Reading a locked setting (the command without arguments) still works.
3. **What is locked.** Every setting a profile gives a value (the profile tables), the 59 other settings the old Advanced
   menu offered (they keep the game's own starting values), and the prefixes `ssfx_` and `r4_hdr10_` (features this port cannot
   run). The list for the active profile: `LockedCommands()`, 103 names on the iPad. Not locked: the player's settings, and every
   variable the game's own scripts set while it runs (night vision, mask effects, depth-of-field for scopes, `hud_*`, ...). A grep of
   the stock scripts found one runtime writer of a locked setting, `modded_exes_migration.script` (`r__detail_radius`, once), which is
   refused and logged.
4. **The game's starting values.** `default_controls` (at start, and the controls menu's Default button) holds starting values for
   many graphics settings. It may set the locked settings the profile gives no value, so they start where stock Anomaly starts, and
   it never touches a setting the profile owns (`DefaultsScope`).
5. **Mode and cap** are not locked: `gfx_mode` re-applies the profile for the new mode, `gfx_fps` sets the cap. `gfx_fps unlimited`
   exists only without locks.

**Escape hatches** for developers, on the command line or in `commandline.txt`:

| Flag | Effect |
|---|---|
| `--unlocked-graphics` | the profile still applies at start as the starting values, nothing is refused, the full PC menu is back, `gfx_fps unlimited` works. The harness sweeps use it |
| `--graphics-profile none` | no profile, no locks, no cap: PC behaviour. The render harness adds it for its `-p preset` runs |
| `--graphics-profile <id>` | force a profile |
| `gfx_profile` (console) | prints the active profile and every setting it applies |

**Addons and MCM.** An addon cannot re-enable an unsupported option: it would have to change a locked console variable, and
every route to one goes through the same refusal (checked below with scripts). An addon that adds rows to the options menu gets
the rows, but applying them is refused. MCM writes through `exec_console_cmd` and is refused the same way. An addon that ships its
own `default_controls.ltx` can only reach the settings the profile leaves at the game's defaults, never a setting a profile lists
and never `ssfx_*` or `r4_hdr10_*`.

## Unsupported features stay off

Off in every profile and both modes (`graphics_profile_test` checks all of them), locked, and not in the menu:

| Feature | Why | Set by |
|---|---|---|
| Tessellation | no hull and domain stages on Metal | `r4_enable_tessellation off` (the renderer also forces it off) |
| 3D fluid, volumetric smoke | needs geometry shaders and layered 3D targets | `r3_volumetric_smoke off` (the renderer also disables the fluid) |
| Volumetric lights | not worth their cost (iPad preset), not exercised | `r2_volumetric_lights off` |
| Screen Space Shaders pack (34 `ssfx_*` shader files) | the files are not in the game data; the renderer detects that and switches every feature off | every `ssfx_*` variable is locked at its default; no menu row |
| Heat vision | its shader is not in the game data | `r__heatvision 0` |
| HDR10 | not exercised | `r4_hdr10_on 0`; the HDR10 page is hidden |
| MSAA, SMAA, other AA | not exercised beyond MSAA 2x and 4x, costs 0.5 to 1.3 GB at native size | `r3_msaa st_opt_off`, `r2_smaa off`, `r2_aa off` |
| Global illumination | not exercised | `r2_gi off` |

## Render size and render scale

The MetalFX work (`RENDERER.md`, "Render scale and upscaling") added `r__render_scale` (0.5 to 1.0: the 3D scene renders at a fraction of the
output size, the UI at native), `r__upscaler` (`metalfx` or `bilinear`) and `r__upscale_sharpness` (0 to 1). The profiles use them **by name and only
if the console has them**, and `r__render_scale` is clamped to the range this build accepts (a value outside it is a syntax error in the console,
not a clamp: a profile asking for 0.42 would leave the scale at 1.0, the iPad at native resolution and 20 fps).

| | Without `r__render_scale` | With it |
|---|---|---|
| iPad | `vid_mode` is the render size (Quality 1152x864, Performance 1024x768 on the 8 GB class); the Metal layer scales it to the panel | `vid_mode` is the panel's native size (a sharp UI) and the scene is `r__render_scale` of it: 0.42 and 0.375, the same pixels; bilinear with sharpening 0.25 |
| Mac | the window or display size, which is the player's | Quality 1.0 (no scaling), Performance 0.7 with MetalFX (free on the Mac: 6.5 against 6.9 ms) |

**The iPad scales are below the renderer's minimum of 0.5, and below what the MetalFX session recommends (0.6 for Quality, 0.5 for Performance).** That
recommendation keeps the old preset's GPU cost (15.3 ms in a field in the rain, 56 to 59 fps). The on-device runs of this profile (below) show that a
frame of that cost does not hold 60 fps in the rain (54.6 fps at 1280x960, 1.23 Mpixel: about 15 ms a Mpixel), so the profile asks for about 1.0 Mpixel of
scene for Quality and 0.79 for Performance: scales 0.42 and 0.375 of the panel. This change lowers the renderer's minimum to 0.35 (the `0.5f` in `CMD4(CCC_RenderScale, "r__render_scale", ...)` in `xrRender_console.cpp`, as the MetalFX session offered);
a build with the old minimum would run the iPad at 0.5, which is 1.4 Mpixel (the old preset's pixel count: Quality dips under 60 in the rain), and
`gfx_profile` prints the value that was set.

## UI text size

Text in the menus and on the HUD did not follow the screen: the font atlas ladder (`GetFontTextureName`, by `Device.dwHeight`) has rungs up to 2160 for a
few HUD fonts only, most UI fonts stop at the 1200 rung, and glyphs are drawn at their atlas size. At 2048 px high the options menu's text is 16 px, the
same as at 1024 and half as tall relative to the screen (7.8 against 15.6 per mille): the "0.64 of the old text" in the render scale notes. On Apple
builds `CGameFont` now gives every font that is not device independent a multiplier of `Device.dwHeight / 1080` (`/ 2000` for an atlas authored for 2160;
1 below that; at most 3), so text keeps the height, relative to the screen, it has on a 1080p PC; the player's `ui_text_scale` multiplies it. The rule is
`apple_text_scale` in `GameFont.cpp` behind `XR_PLATFORM_APPLE`, and the engine log lists each font's multiplier at start (`* [font]
ui\ui_font_arial_14_1600: text scale 1.90`). Checked at 2732x2048 on the Mac: the options menu, the inventory, the PDA and the loading screen
(`scenes/26_options_keys.script`, `06_inventory`, `07_pda`, `01_loading`): the text is as tall relative to the screen as at 1366x1024 and nothing
overflows its box. Dialogs and the in-game HUD were not checked.

## Measurements

Method: `tools/profile_measure.py script` writes a `--script` timeline that loads the Cordon save and plays five scenes (outdoors at noon,
outdoors in rain, inside the trader's bunker with rain outside, the Rookie village at noon (a busy base: the most NPCs of the Cordon), and the
village at night with the flashlight on), each for 10 s of settling and 34 s of measurement, one launch per mode. The shell log's 10 s lines
(frames per second, the main thread's time in a frame, GPU time, footprint, thermal state) and the Metal statistics line (draw calls, texture and
render target memory) are averaged by `profile_measure.py summarize`. The Mac runs are gated: they wait for a Mac with no other game on the GPU
and drop a window that overlapped one.

### Mac: MacBook Pro 14-inch, M2 Pro (19 GPU cores), 32 GB, 3024x1964 at 120 Hz

`tools/render_harness/measure_profile.sh -u -s 3024x1964`: the `mac-pro` profile with the real `r__render_scale` (Quality 1.0, Performance 0.7 with MetalFX),
unattended and offscreen (no window, no focus), `--unlocked-graphics` and `gfx_fps unlimited` so that the frame rate is what the Mac could do, not the cap;
`RelWithDebInfo`, the Lua JIT on. One launch per mode, the five scenes above, each over its 10 s windows after 10 s of settling. A window counts only if no
other `anomaly` process ran during it (the sampler of the script; the runs below had none, and an earlier set that overlapped another session's runs was
discarded). *fps* is the mean of the windows, *slowest* the slowest window, *CPU* the time the main thread spends in a frame (it includes waiting for the
GPU), *GPU* the GPU time per frame, *footprint* the process's physical footprint at the end of the scene (it only grows over a run: the level streams in).

| Run | Scene | windows | fps | slowest | CPU ms | GPU ms | draws | footprint MB | thermal |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| **Quality** (scale 1.0) | field | 2 | 105.8 | 105.2 | 9.4 | 6.9 | 1682 | 4923 | Nominal |
| | rain | 3 | 99.4 | 96.4 | 10.0 | 7.3 | 1479 | 5001 | Nominal |
| | bunker | 3 | 94.9 | 93.2 | 10.5 | 6.9 | 924 | 5072 | Nominal |
| | village | 2 | 125.4 | 122.7 | 7.9 | 10.5 | 1687 | 5202 | Nominal |
| | night | 3 | 86.7 | 76.6 | 11.6 | 7.8 | 1397 | 5293 | Nominal |
| **Performance** (scale 0.7, MetalFX) | field | 2 | 108.9 | 108.4 | 9.1 | 6.7 | 1771 | 4682 | Nominal |
| | rain | 3 | 109.2 | 108.6 | 9.1 | 7.0 | 1751 | 4768 | Nominal |
| | bunker | 3 | 161.4 | 158.8 | 6.2 | 4.3 | 408 | 4863 | Nominal |
| | village | 2 | 157.6 | 155.5 | 6.3 | 6.4 | 1549 | 4933 | Nominal |
| | night | 3 | 148.5 | 138.1 | 6.7 | 5.8 | 1583 | 5052 | Nominal |

What it says:

- **Quality holds 60 at the native size with room to spare**: every scene is 87 to 125 fps and the slowest window of the whole run is 76.6 fps (night). The
  GPU time is 7 to 10.5 ms; the village is the GPU's worst scene.
- **Performance runs 109 to 161 fps at scale 0.7.** The 120 fps cap is met in the bunker, village and night; field and rain run at 109 fps because the
  main thread is the limit there (9 ms of CPU against an 8.3 ms budget), which a render scale cannot buy back. The GPU is at 4 to 7 ms.
- The footprint is 4.7 to 5.3 GB, 14 to 16 % of the Mac's RAM; the 8 GB class (`mac-base-8gb`) uses one texture level less.
- A first set of Quality runs read 19 to 23 ms of GPU time for the same scenes while another game ran on the GPU: the harness waits for a free GPU now and drops
  overlapped windows. An earlier edition of this table, taken with window sizes standing in for the render scale, is replaced by this one.

### iPad: Air 13-inch (M3, 8 GB), 2732x2048 at 60 Hz

**Final run of the profile** (`tools/ios/measure_profile.sh`, 2026-10-23, `RelWithDebInfo`, the `ipad_test` save, locks on, the merged render scale: native 2732x2048 output, bilinear
with sharpening 0.25, scene scale 0.42 for Quality and 0.375 for Performance, i.e. 1147x860 and 1024x768 of scene; one launch per mode). Each launch stands two
minutes in the Rookie village first, so that the scenes run warm, then plays the five scenes (the thermal column says what state each window ran in). The frame
rate is capped by the 60 Hz display, so a GPU time near 14 ms mostly says "the GPU waits for the display"; only a frame rate under 60 means the GPU or the CPU
was the limit.

| Run | Scene | windows | fps | slowest | CPU ms | GPU ms | draws | footprint MB | thermal |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| **Quality** (scale 0.42) | field | 2 | 60.0 | 60.0 | 16.6 | 13.1 | 1898 | 3976 | Nominal |
|  | rain | 3 | 59.2 | 58.4 | 16.8 | 16.6 | 2773 | 4021 | Nominal |
|  | bunker | 2 | 59.8 | 59.6 | 15.1 | 13.4 | 1493 | 4021 | Nominal |
|  | village | 2 | 59.9 | 59.8 | 16.1 | 14.6 | 1972 | 4089 | Nominal |
|  | night | 3 | 59.9 | 59.8 | 13.6 | 13.8 | 2256 | 4109 | Nominal |
| **Performance** (scale 0.375) | field | 2 | 59.9 | 59.8 | 16.6 | 12.7 | 2047 | 3914 | Nominal |
|  | rain | 3 | 59.9 | 59.7 | 8.6 | 12.1 | 2175 | 4059 | Nominal |
|  | bunker | 2 | 60.0 | 59.9 | 16.6 | 11.1 | 1501 | 4055 | Nominal |
|  | village | 2 | 60.0 | 59.9 | 16.6 | 14.2 | 1943 | 4060 | Nominal |
|  | night | 3 | 60.0 | 59.8 | 12.4 | 13.5 | 2226 | 4085 | Nominal |

- **Quality held 59.2 to 60.0 fps** in every scene; the rain is the worst (59.2, slowest window 58.4, 2773 draws, 16.6 ms of GPU).
- **Performance held 59.9 to 60.0**, slowest window 59.7, in every scene.
- The footprint is 3.9 to 4.1 GB of the 7.99 GB budget in both modes; the scene size changes the render targets (223 MB at scale 0.42, 190 MB at 0.375), not the
  textures (2.4 to 2.5 GB).
- **The iPad stayed at thermal state Nominal** through the two-minute warm-up and the four minutes of scenes in both launches, so the Serious state the MetalFX
  measurements describe (45 to 51 fps at any scale, 12 to 20 % slower main thread) was not reached here, and nothing was measured in it. What the profile
  does about it is the margin: Performance has about 25 % of the frame to spare in the worst scene, Quality about 10 % (the next table).

**Where the sizes come from.** The first on-device run of the profile (2026-10-23; before the render scale was merged, so the render size was `vid_mode`) had
Quality at 1280x960 (1.23 Mpixel) and Performance at 1152x864 (1.0 Mpixel):

| Run | Scene | windows | fps | slowest | CPU ms | GPU ms | draws | footprint MB | thermal |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| **Quality** (1280x960) | field | 2 | 60.0 | 60.0 | 13.8 | 12.9 | 1718 | 3864 | Nominal |
|  | rain | 3 | 54.6 | 53.6 | 18.1 | 15.4 | 2222 | 3929 | Nominal |
|  | bunker | 2 | 59.9 | 59.7 | 15.9 | 12.9 | 1489 | 3968 | Nominal |
|  | village | 2 | 59.8 | 59.6 | 12.2 | 13.2 | 1515 | 3971 | Nominal |
|  | night | 3 | 59.9 | 59.8 | 14.4 | 14.1 | 1890 | 4044 | Nominal |
| **Performance** (1152x864) | field | 2 | 60.0 | 60.0 | 13.2 | 11.2 | 1667 | 3808 | Nominal |
|  | rain | 3 | 59.5 | 59.2 | 10.0 | 12.4 | 2191 | 3905 | Nominal |
|  | bunker | 2 | 60.0 | 60.0 | 16.6 | 11.4 | 1484 | 3952 | Nominal |
|  | village | 2 | 59.5 | 59.0 | 15.3 | 14.0 | 1472 | 4011 | Nominal |
|  | night | 3 | 60.0 | 60.0 | 16.6 | 15.0 | 1890 | 4038 | Nominal |

Quality held 60 in four of the five scenes and **dropped to 54.6 fps in the rain** (slowest window 53.6, 18 ms a frame, 2222 draws): at 1.23 Mpixel the frame
costs about 15 ms a Mpixel there, so 1.0 Mpixel (scale 0.42 of the panel) leaves 10 % of a 16.7 ms frame and 0.79 Mpixel (0.375) leaves 25 %. The previous
preset's 1366x1024 was 49 to 55 fps in the village (`IPAD.md`; 1.4 Mpixel, about 10 ms a Mpixel). The MetalFX session's recommendation for this iPad (scale
0.6 for Quality, 0.5 for Performance, `RENDERER.md`) keeps that old cost class (15.3 ms in a field in the rain) and is not enough here; its minimum of 0.5
cannot express these sizes either, so this change lowers the renderer's minimum to 0.35.

Render size sweeps in the rain and in the village (one launch, the profile's Quality settings, one change at a time, 22 s each) were all display limited at
60 fps and showed only that 1152x864 and below hold 60 where 1280x960 did not always; the numbers are in the PR for this change.

## Verification

Checks and where their output is:

| Check | How | Result |
|---|---|---|
| Console and scripts cannot change locked settings | `tools/render_harness/scenes/20_locks.script`: `r2_sun_quality st_opt_extreme` typed, `exec_console_cmd("texture_lod 4")`, `exec_console_cmd("r4_enable_tessellation on")`, `exec_console_cmd("ssfx_ssr (1, 1, 1, 1)")` | each refused, the first refusal of each logged once (`! graphics: '...' refused: locked by the mac-pro profile. Further attempts ... are not logged.`), the second typed attempt silent; the values read back are the profile's; `fov 80` and `gfx_mode performance` accepted; `gfx_fps unlimited` refused |
| The menu shows no locked row, and the PC menu is intact | `scenes/22_options_tree.script` prints the tree | with the profile: `OPTIONS locked rows visible: 0`, 7 rows on Visual / Basic; with `--graphics-profile none`: the stock tree (Advanced 42 rows, Graphics 8, Particles 4, HDR10 36, Optimizations 12, Debug 8) |
| Locks survive a restart, stale `user.ltx` is overridden | `tools/render_harness/lock_restart.sh`: run 1 changes settings and saves; `user.ltx` gets stale values for 7 locked settings (`r2_sun_quality st_opt_extreme`, `texture_lod 0`, `r4_enable_tessellation on`, `r3_msaa 4x`, `r2_ssao st_opt_high`, `r3_dynamic_wet_surfaces on`, `r__detail_radius 250`) and `r2_mblur_enabled on`; run 2 prints the values | start log: `user.ltx has 186 locked settings, ignored (the mac-pro profile applies); 7 of them differ from the profile: ...`; read back: `r2_sun_quality st_opt_low`, `texture_lod 1`, `r4_enable_tessellation off`, `r3_msaa st_opt_off`, `r2_ssao st_opt_low`, `r3_dynamic_wet_surfaces off`, `r__detail_radius 75` (the Performance values: the mode saved by run 1); `r2_mblur_enabled on`, `fov 80.`, `gfx_mode performance`, `gfx_fps 30` kept (the player's) |
| The same on the iPad (Air M3) | `tools/ios/measure_profile.sh`: `scripts/lock_test.txt` (refusals from the console and from `run_string`), then `scripts/restart_check.txt` after a `user.ltx` with stale values | refused and logged once each (`r2_sun_quality`, `texture_lod`, `r4_enable_tessellation`, `ssfx_ssr`, `vid_mode`, `r3_msaa`), `fov 80` and `gfx_fps 30` accepted; at the next start `user.ltx has 192 locked settings, ignored (the ipad-8gb profile applies); 7 of them differ from the profile: ...` and the values read back are the profile's (`r2_sun_quality st_opt_low`, `texture_lod 1`, `r3_msaa st_opt_off`, `r2_ssao st_opt_off`, `r3_dynamic_wet_surfaces off`, `r__detail_radius 30`, Performance mode saved by the run before); `r2_mblur_enabled on`, `fov 80.`, `gfx_mode performance`, `gfx_fps 30` kept |
| The iPad menu | `scripts/settings_menu.txt` (Down three times, Return, a screenshot) | Visual / Basic with Graphics mode, Frame rate, Brightness, Text size, FOV, HUD FOV, Motion Blur and no Screen mode row; text sharp at the native output and as tall as on a 1080p PC |
| Brightness changes the picture | `scenes/24_brightness.script`, `r__exposure` 0.7 / 1.0 / 1.5 | mean pixel value 46 / 65 / 94 |
| Windows menu untouched | code review, below | every change guarded; the `--graphics-profile none` tree equals the stock one |
| Profile tables | `ctest -R graphics_profile_test` | device selection for iPad and Mac cases, the render scale clamp, unsupported features off in every profile and mode, lock rules, frame caps, MetalFX entries |

Menus were screenshotted on the Mac (Settings, Visual / Basic, Modded Exes) and on the iPad. Changing a value by clicking through a drop-down was not
scripted (the cursor starts in the middle of the screen when a dialog opens and a click on a drop-down arrow needs calibration): Apply goes through
the stock `On_Accept`, which runs `exec_console_cmd` (the console path above) and `vid_restart` for the mode row, and `gfx_mode performance` through the
console was checked to re-apply the profile (`graphics profile mac-pro applied (mode change)`, and on the iPad the scale 0.375 of Performance).

## Windows is unchanged

- Every C++ change is inside `#ifdef XR_PLATFORM_APPLE` or in a file the Windows projects do not list
  (`xr_graphics_profile.cpp` is added by `cmake/targets/xrEngine.cmake` only). The lock hook, the `default_controls` scope, the
  `user.ltx` hooks, the console commands and the Lua exports (`gfx_*`) do not exist on Windows.
- `console_options.script` returns at once unless the engine exports `gfx_locks_active` and it is true. On Windows the function is
  `nil`, so `ui_options.options` is exactly the tree the stock script and the Modded Exes scripts build.
- The strings added (`ui_mm_console_graphics.xml`) are unused there.
- The render harness shows the equivalent on a Mac: `--graphics-profile none` prints the stock tree.

## Adding a device

1. **Detection** is automatic. Run the app on the device and read the first lines of the shell log
   (`Documents/anomaly-shell.log` on an iPad, `~/Library/Logs/Anomaly` on a Mac): the `graphics profile ...` line says what was
   detected and which profile was chosen. If the chosen one is right, stop.
2. **A new class**: add a `ProfileDef` to `kProfiles` in `GraphicsProfile.cpp`, with `parent` set to the nearest measured profile
   (an entry replaces the parent's entry with the same key, mode lists are applied after `common`), and make `SelectProfileId`
   return its id for the hardware. Mark it `measured = false` until step 3.
3. **Measure**: Mac, `tools/render_harness/measure_profile.sh -u -p <id>`; iPad, `python3 -I tools/profile_measure.py script --save
   ipad_test` and `tools/ios/ipad.sh script` with `-dbg --graphics-profile <id>`, then `profile_measure.py summarize` on the shell
   log. Tune until the worst scene of Quality holds 60 fps with margin and Performance holds its target, with the footprint under
   the budget ([IPAD.md](IPAD.md)). Then set `measured = true` and add the numbers to the tables here.
4. Add the class to `TestSelection` in `tools/graphics_profile_test/main.cpp`, run `ctest -R graphics_profile_test`, and regenerate
   the tables here with `graphics_profile_test --dump`.
5. A setting that should become configurable: remove it from the locked lists, add a row to `basic_rows` in
   `console_options.script` and strings to `ui_mm_console_graphics.xml`.
