# Graphics settings: console-like

On the iPad and the Mac the game does not ask the player to tune a PC. The device picks **a profile** for its class: texture level,
shadows, sun and lights, ambient occlusion, grass, distances, render size, anti-aliasing and the post-process set, chosen from
measurements for the best picture it keeps at a steady frame rate. The profile is applied on every start and **locked**: the options menu does
not show those settings, and the console, scripts and addons cannot change them. The player keeps what a console offers: a mode
(Quality or Performance), a frame rate cap, field of view, brightness, motion blur, and the HUD and crosshair.

Everything here is Apple only (`XR_PLATFORM_APPLE`). On Windows the options menu, the console and `user.ltx` behave as before
([Windows](#windows-is-unchanged)).

| Part | Where |
|---|---|
| Device detection, the profile tables, the lock rules (plain C++, tested on any host) | `src/Platform/Apple/GraphicsProfile.{h,cpp}`, hardware probe in `DeviceInfo.mm` |
| Applying the profile after `user.ltx`, the lock in the console, `gfx_mode`, `gfx_fps`, `gfx_profile` | `src/xrEngine/xr_graphics_profile.{h,cpp}`, hooks in `XR_IOConsole.cpp`, `x_ray.cpp`, `xr_ioc_cmd.cpp` |
| The frame rate cap and its pacing, the frame interval histogram in the log | `src/Platform/App/main.cpp`, `src/Platform/Apple/FramePacer.h` |
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
| render size | scale 0.6 / 0.42 (without a scale: 1152x864 / 1024x768) | scale 0.67 / 0.47 (without a scale: 1280x960 / 1152x864) | scale 0.375 / 0.33 (without a scale: 1024x768 / 896x672) | scale 1.0 / 0.7 | scale 0.7 / 0.5 | scale 0.7 / 0.5 | scale 1.0 / 0.8 | scale 1.0 / 0.9 |
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
| Goal | the best picture the device keeps at a steady rate: 40 fps on an iPad whose display shows it evenly (120 Hz), 30 on a 60 Hz one, 60 on a Mac | a locked 60 fps, 120 on a 120 Hz Mac |
| How | the profile's full set at a higher render scale | cheaper shadows and AO, shorter grass and view distance, a lower render scale |
| Frame cap (Auto) | iPad 40 (30 on a 60 Hz panel), Mac 60 | 60; 120 on a 120 Hz Mac (`mac-pro`, `mac-max`, `mac-ultra`) |

**The frame rate cap shows only rates the display holds evenly.** A frame is shown at a display refresh, so its length is a whole number of
refreshes or the frame times alternate. 30 fps on a 60 Hz panel is every second refresh, 40 fps on a 120 Hz panel every third, 60 on either
every refresh or every second one: all even. **40 fps on a 60 Hz panel is 1.5 refreshes**: frames alternate 16.7 and 33.3 ms (25 ms on average, no
frame at 25 ms), a judder that a cap cannot fix. `FrameCaps()` therefore offers 30 and 60 on a 60 Hz display, and 30, 40, 60 and 120 on a 120 Hz one,
and the mode's default is the largest offered rate that is not above the profile's: **the iPad's Quality asks for 40 and is 30 on a 60 Hz iPad**
(the iPad Air 13-inch M3 is 60 Hz: the shell reads `UIScreen.maximumFramesPerSecond` and the device reports 60, it is not a ProMotion panel),
40 on a ProMotion iPad Pro. `--unlocked-graphics` accepts any rate from 10 to 240 (`gfx_fps 40`), which is how the uneven 40 on the Air is measured
below. The 120 Hz iPad path is unit tested (the pacer is replayed against simulated 60 and 120 Hz displays) but **no 120 Hz iPad was available to measure it**.

**Frame pacing.** The app's loop is called at every display refresh (the display link); `FramePacer` runs a frame at the first tick at or after
the time the last frame's schedule says, and skips a tick that is more than 5 ms early, so the frames fall on whole numbers of refreshes and
the schedule does not drift. The engine's own limiter (`r__framelimit`) busy-waits and is compiled out on iOS: it stays 0. The shell log has, every 10 s,
the share of frames by the time since the previous frame started (`frame intervals`), and `profile_measure.py summarize` prints it as a second table.

**A warm iPad is the other limit.** The iPad leaves Nominal for Serious within a minute or two of load and then runs 12 to 20 % slower on the
main thread (RENDERER.md, measured by the MetalFX session: every render scale up to 0.67 then fell to 45 to 51 fps). The two modes face it
differently. **Quality has room**: with no cap at scale 0.6 the iPad ran 54 to 60 fps in Serious ([below](#ipad-air-13-inch-m3-8-gb-2732x2048-at-60-hz)),
so 30 fps keeps about 45 % of the frame to spare even then (40 fps a quarter at full clocks, less under the cap: see the uneven 40 below).
**Performance asks 60 of a 1.0 Mpixel scene and has almost none in the heaviest rain**: 57.5 fps there, cool (the other four scenes held 59.5 to 59.9), and it was not measured in Serious. A player
who wants an even frame rate whatever the temperature has the frame rate cap: 30 fps paces frames evenly (the app skips every other display
tick). The measurement tables below have a two-minute warm-up in the busiest scene before the scenes and a thermal column, so they show what
state each window ran in.

Switching mode in the menu applies the profile's settings for that mode and restarts the video (`vid_restart`, a second or two); the
mode is saved in `user.ltx` as `gfx_mode`. The default is Quality.

## What the player can change, and why

| Menu | Option | Console | Why it stays |
|---|---|---|---|
| Visual / Basic | Graphics mode | `gfx_mode` | the one quality choice a console offers |
| | Frame rate | `gfx_fps` | 30 / 60 (and 40 / 120 on a 120 Hz display): the player's choice of smoothness against battery and heat, only rates the display shows evenly. Auto follows the mode |
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
| iPad | `vid_mode` is the render size (Quality 1152x864, Performance 1024x768 on the 8 GB class); the Metal layer scales it to the panel | `vid_mode` is the panel's native size (a sharp UI) and the scene is `r__render_scale` of it: Quality 0.6 (1639x1229, 2.0 Mpixel), Performance 0.42 (1147x860, 1.0 Mpixel); bilinear with sharpening 0.25 |
| Mac | the window or display size, which is the player's | Quality 1.0 (no scaling), Performance 0.7 with MetalFX (free on the Mac: 6.5 against 6.9 ms) |

**Quality's 0.6 is what the MetalFX session recommended for this iPad** (RENDERER.md) and is inside the renderer's old range. **Performance's 0.42 is
below its minimum of 0.5**, so this work lowers the minimum to 0.35 (the `0.5f` in `CMD4(CCC_RenderScale, "r__render_scale", ...)` in `xrRender_console.cpp`,
as the MetalFX session offered); a build with the old minimum would run Performance at 0.5, which is 1.4 Mpixel (the old preset's pixel count),
and `gfx_profile` prints the value that was set. What the sizes cost on the iPad Air: at 0.6 (2.0 Mpixel) a frame took at most 18.5 ms with no cap, in Serious, so it fits
30 fps with room and 40 fps only just; at 0.42 (1.0 Mpixel) the heaviest rain is at the edge of 16.7 ms (57.5 to 59.7 fps), and the cost of a frame is not simply proportional
to its pixels (the measurements below).

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

**Final run of the profile** (2026-10-23, `RelWithDebInfo`, the `ipad_test` save, locks on, native 2732x2048 output, bilinear with sharpening 0.25, Quality at
scene scale 0.6 (1639x1229) and Performance at 0.42 (1147x860); one launch per mode). Each launch stands two minutes in the Rookie village first, so that
the scenes run warm, then plays the five scenes (the thermal column says what state each window ran in). Quality's cap is **30 fps on this 60 Hz panel** (its
default is 40, which the panel cannot show evenly: [below](#the-uneven-40-on-the-60-hz-panel)). The frame rate is capped by the cap or the display, so the GPU time
mostly says "the GPU waits for the next frame"; only a frame rate under the cap means the GPU or the CPU was the limit.

| Run | Scene | windows | fps | slowest | CPU ms | GPU ms | draws | footprint MB | thermal |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| **Quality** (scale 0.6, cap 30) | field | 2 | 30.0 | 30.0 | 17.7 | 20.0 | 2129 | 4037 | Nominal |
|  | rain | 3 | 30.0 | 30.0 | 11.0 | 18.3 | 2201 | 4076 | Nominal |
|  | bunker | 2 | 30.0 | 30.0 | 16.1 | 18.4 | 1492 | 4081 | Nominal |
|  | village | 2 | 29.9 | 29.9 | 10.9 | 20.7 | 1975 | 4158 | Nominal |
|  | night | 3 | 30.0 | 30.0 | 21.5 | 24.1 | 2260 | 4151 | Nominal |
| **Performance** (scale 0.42, cap 60) | field | 2 | 59.9 | 59.7 | 16.6 | 13.0 | 1994 | 3962 | Nominal |
|  | rain | 3 | 57.5 | 57.4 | 17.2 | 15.7 | 2777 | 4001 | Nominal |
|  | bunker | 2 | 59.6 | 59.5 | 15.9 | 11.4 | 1505 | 4013 | Nominal |
|  | village | 2 | 59.9 | 59.8 | 16.4 | 14.1 | 1945 | 4060 | Nominal |
|  | night | 3 | 59.5 | 59.2 | 14.2 | 13.3 | 1653 | 4149 | Nominal |

Frame intervals, % of the frames by the time since the previous frame started (the shell log's `frame intervals` line; 16.7 ms is one refresh of this panel,
33.3 two):

| Run | Scene | under 14 ms | 14 to 19 (17) | 19 to 22 | 22 to 28 (25) | 28 to 31 | 31 to 37 (33) | 37 to 55 | over 55 | longest ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **Quality** (scale 0.6, cap 30) | field | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 100.0 | 0.0 | 0.0 | 34 |
|  | rain | 0.0 | 0.0 | 0.2 | 0.0 | 0.0 | 99.6 | 0.2 | 0.0 | 47 |
|  | bunker | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 100.0 | 0.0 | 0.0 | 34 |
|  | village | 0.0 | 0.3 | 0.0 | 0.0 | 0.0 | 99.2 | 0.3 | 0.1 | 67 |
|  | night | 0.0 | 0.0 | 0.0 | 0.5 | 0.1 | 99.2 | 0.2 | 0.0 | 50 |
| **Performance** (scale 0.42, cap 60) | field | 0.1 | 99.5 | 0.1 | 0.0 | 0.1 | 0.0 | 0.0 | 0.1 | 66 |
|  | rain | 0.6 | 86.4 | 10.7 | 2.1 | 0.0 | 0.1 | 0.1 | 0.0 | 50 |
|  | bunker | 0.2 | 99.2 | 0.1 | 0.1 | 0.0 | 0.1 | 0.0 | 0.2 | 80 |
|  | village | 0.4 | 98.9 | 0.5 | 0.0 | 0.1 | 0.0 | 0.1 | 0.0 | 45 |
|  | night | 0.8 | 97.7 | 0.7 | 0.5 | 0.0 | 0.1 | 0.1 | 0.1 | 148 |

- **Quality ran at 30.0 fps in all five scenes, 99.2 to 100 % of the frames 31 to 37 ms apart**: the frame time is even. The longest interval of a window was
  34 to 67 ms (an occasional hitch of one or two frames).
- **Performance held 59.5 to 59.9 fps in four scenes and fell to 57.5 in the rain** (slowest window 57.4; 13 % of the frames took 19 to 28 ms; 2777 draws,
  15.7 ms of GPU). The previous profile's 0.375 held 59.9 there ([below](#the-earlier-profile)); at 0.42 it is the one place
  where the locked 60 is not met.
- The footprint is 4.0 to 4.3 GB of the 7.99 GB budget; the scene size changes the render targets (390 MB at scale 0.6, 223 MB at 0.42), not the textures (2.4 to
  2.7 GB).
- **Thermal state stayed Nominal** through the warm-up and the scenes in both launches. The two no-cap launches that followed read Serious, the uneven-40 and
  sweep launches after them Nominal again. What happens to Performance in Serious was not measured; Quality's capacity in Serious was
  ([next](#what-the-ipad-could-do-at-scale-06-without-a-cap)).

#### What the iPad could do at scale 0.6 without a cap

`--unlocked-graphics` and `gfx_fps unlimited` (the display still limits at 60), the same warm-up and scenes, **in thermal state Serious** (the fourth and fifth launches of the session). Bilinear with sharpening 0.25 is the profile's; MetalFX is the same scale with `r__upscaler metalfx`:

| Run | Scene | windows | fps | slowest | CPU ms | GPU ms | draws | footprint MB | thermal |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| **bilinear** 0.25 (no cap) | field | 2 | 59.9 | 59.7 | 7.2 | 13.1 | 1981 | 4031 | Serious |
|  | rain | 3 | 56.0 | 54.1 | 17.7 | 16.3 | 2376 | 4117 | Serious |
|  | bunker | 3 | 59.7 | 59.3 | 16.4 | 17.6 | 1475 | 4150 | Serious |
|  | village | 2 | 57.6 | 57.6 | 17.3 | 24.2 | 1968 | 4185 | Serious |
|  | night | 3 | 56.1 | 54.1 | 17.8 | 15.0 | 2256 | 4155 | Serious |
| **MetalFX** (no cap) | field | 2 | 56.8 | 56.8 | 17.4 | 16.7 | 2087 | 4107 | Serious |
|  | rain | 3 | 56.0 | 55.9 | 17.8 | 26.0 | 2186 | 4183 | Serious |
|  | bunker | 2 | 59.5 | 59.3 | 16.6 | 17.1 | 1484 | 4183 | Serious |
|  | village | 2 | 48.0 | 47.8 | 20.8 | 30.2 | 1972 | 4210 | Serious |
|  | night | 3 | 48.2 | 47.5 | 20.3 | 17.4 | 2260 | 4312 | Serious |

- With bilinear the iPad ran 54.1 to 59.9 fps in Serious: at full clocks a frame is at most 18.5 ms, so 30 fps leaves 45 % of the frame and 40 fps 26 %. A capped GPU
  runs at lower clocks, so the real margin at 40 is smaller: the uneven-40 run below missed 40 by 0.5 fps in the field.
- MetalFX costs about 3 ms of frame time in the village and at night (48 against 56 to 58 fps; GPU 30.2 against 24.2 ms in the village) and 1 ms in the field; the rain and
  the bunker are display limited either way.
  Screenshots of the same frame (field scene, crops of the same region, `ab_bilinear` and `ab_metalfx` in the PR) show MetalFX a little sharper in foliage and ground detail.
  The profile keeps bilinear: Performance cannot afford MetalFX, and for Quality the sharper picture was not measured warm at its cap. To try it, put
  `r__upscaler metalfx` and `r__upscale_sharpness 0` into `ipad-8gb`'s Quality list in `GraphicsProfile.cpp`.

#### The uneven 40 on the 60 Hz panel

For the record, the cap the brief asked for, on this panel: `--unlocked-graphics` and `gfx_fps 40` (not offered otherwise), Quality at scale 0.6, locks off, Nominal:

| Run | Scene | windows | fps | slowest | CPU ms | GPU ms | draws | footprint MB | thermal |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| **Quality**, uneven 40 (scale 0.6, cap 40) | field | 2 | 39.5 | 39.2 | 22.8 | 27.6 | 2647 | 4076 | Nominal |
|  | rain | 3 | 40.0 | 40.0 | 11.0 | 21.9 | 1965 | 4134 | Nominal |
|  | bunker | 2 | 40.0 | 40.0 | 16.6 | 20.4 | 1505 | 4172 | Nominal |
|  | village | 2 | 39.8 | 39.6 | 10.3 | 27.2 | 1646 | 4249 | Nominal |
|  | night | 3 | 39.8 | 39.7 | 9.0 | 18.4 | 1918 | 4265 | Nominal |

Frame intervals (time between frame starts in the app's loop):

| Run | Scene | under 14 ms | 14 to 19 (17) | 19 to 22 | 22 to 28 (25) | 28 to 31 | 31 to 37 (33) | 37 to 55 | over 55 | longest ms |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **Quality**, uneven 40 | field | 0.0 | 5.2 | 11.0 | 66.5 | 0.4 | 15.4 | 1.6 | 0.0 | 55 |
|  | rain | 0.0 | 0.2 | 46.7 | 6.5 | 46.6 | 0.0 | 0.1 | 0.0 | 46 |
|  | bunker | 0.0 | 44.0 | 0.8 | 10.6 | 0.0 | 44.6 | 0.0 | 0.0 | 33 |
|  | village | 1.1 | 42.8 | 1.1 | 9.0 | 1.6 | 43.9 | 0.5 | 0.0 | 50 |
|  | night | 0.0 | 45.5 | 1.7 | 4.9 | 1.8 | 45.7 | 0.4 | 0.1 | 65 |

It runs at 39.5 to 40.0 fps, but **it cannot be even**: the panel shows a frame at a refresh, so with two frames to three refreshes the frames are on screen for 16.7
and 33.3 ms in turn (the bunker, village and night rows: 43 to 46 % each); none is shown for 25 ms. In the field (66 % at 22 to 28 ms) and the rain (21 and 29 ms) the
loop's start times do not follow that pattern, but the panel still shows each frame at a refresh, so what is on screen is 16.7, 33.3 or 50 ms per frame, never 25. The same
scenes at 30 fps are 99 to 100 % at 33 ms. This is why a 60 Hz panel is offered 30, not 40.

#### Performance's scale in the rain

The rain is where Performance's 60 is marginal. One launch, the profile's Performance settings, only the render scale changed, 22 s each (one or two of the shell log's 10 s windows: 0.4 and 0.375 have one each), `--unlocked-graphics`, Nominal:

| Performance scale | windows | fps | slowest | CPU ms | GPU ms | draws | footprint MB | 14 to 19 ms | 19 to 28 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.42 (the profile) | 2 | 59.7 | 59.6 | 15.6 | 14.1 | 2003 | 3999 | 96.4 | 2.4 |
| 0.4 | 1 | 60.0 | 60.0 | 16.6 | 11.4 | 1815 | 3964 | 100.0 | 0.0 |
| 0.375 | 1 | 60.0 | 60.0 | 16.6 | 10.5 | 1822 | 3931 | 99.7 | 0.2 |

0.42 is within 0.3 fps of 60 here and 2.5 under in the full run above; 0.4 and 0.375 held 60.0. One window is thin evidence for 0.4, but the earlier full run at 0.375
held 59.9 in all five scenes ([below](#the-earlier-profile)). The profile asks for 0.42 as chosen; **0.4 or 0.375 would make the locked 60 hold in the rain**.

#### The earlier profile

The profile before this change (Quality 0.42 and Performance 0.375, both at 60 fps; the same method, 2026-10-23, Nominal throughout):

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

The sizes came from a first run before the render scale existed (Quality 1280x960, 1.23 Mpixel: 54.6 fps in the rain, slowest window 53.6, 18 ms a frame; Performance 1152x864,
1.0 Mpixel: 59.5 in the rain), which gave about 15 ms a Mpixel in the rain. The previous preset's 1366x1024 was 49 to 55 fps in the village (`IPAD.md`; 1.4 Mpixel).
Render size sweeps in the rain and in the village were all display limited at 60 fps and showed only that 1152x864 and below hold 60 where 1280x960 did not always.

## Verification

Checks and where their output is:

| Check | How | Result |
|---|---|---|
| Console and scripts cannot change locked settings | `tools/render_harness/scenes/20_locks.script`: `r2_sun_quality st_opt_extreme` typed, `exec_console_cmd("texture_lod 4")`, `exec_console_cmd("r4_enable_tessellation on")`, `exec_console_cmd("ssfx_ssr (1, 1, 1, 1)")` | each refused, the first refusal of each logged once (`! graphics: '...' refused: locked by the mac-pro profile. Further attempts ... are not logged.`), the second typed attempt silent; the values read back are the profile's; `fov 80` and `gfx_mode performance` accepted; `gfx_fps unlimited` refused |
| The menu shows no locked row, and the PC menu is intact | `scenes/22_options_tree.script` prints the tree | with the profile: `OPTIONS locked rows visible: 0`, 8 rows on Visual / Basic on the Mac (7 on the iPad); with `--graphics-profile none`: the stock tree (Advanced 42 rows, Graphics 8, Particles 4, HDR10 36, Optimizations 12, Debug 8) |
| Locks survive a restart, stale `user.ltx` is overridden | `tools/render_harness/lock_restart.sh`: run 1 changes settings and saves; `user.ltx` gets stale values for 7 locked settings (`r2_sun_quality st_opt_extreme`, `texture_lod 0`, `r4_enable_tessellation on`, `r3_msaa 4x`, `r2_ssao st_opt_high`, `r3_dynamic_wet_surfaces on`, `r__detail_radius 250`) and `r2_mblur_enabled on`; run 2 prints the values | start log: `user.ltx has 186 locked settings, ignored (the mac-pro profile applies); 7 of them differ from the profile: ...`; read back: `r2_sun_quality st_opt_low`, `texture_lod 1`, `r4_enable_tessellation off`, `r3_msaa st_opt_off`, `r2_ssao st_opt_low`, `r3_dynamic_wet_surfaces off`, `r__detail_radius 75` (the Performance values: the mode saved by run 1); `r2_mblur_enabled on`, `fov 80.`, `gfx_mode performance`, `gfx_fps 30` kept (the player's) |
| The same on the iPad (Air M3) | `tools/ios/measure_profile.sh`: `scripts/lock_test.txt` (refusals from the console and from `run_string`), then `scripts/restart_check.txt` after a `user.ltx` with stale values | refused and logged once each (`r2_sun_quality`, `texture_lod`, `r4_enable_tessellation`, `ssfx_ssr`, `vid_mode`, `r3_msaa`), `fov 80` and `gfx_fps 30` accepted; at the next start `user.ltx has 192 locked settings, ignored (the ipad-8gb profile applies); 7 of them differ from the profile: ...` and the values read back are the profile's (`r2_sun_quality st_opt_low`, `texture_lod 1`, `r3_msaa st_opt_off`, `r2_ssao st_opt_off`, `r3_dynamic_wet_surfaces off`, `r__detail_radius 30`, Performance mode saved by the run before); `r2_mblur_enabled on`, `fov 80.`, `gfx_mode performance`, `gfx_fps 30` kept |
| The iPad menu | `scripts/settings_menu.txt` (Down three times, Return, a screenshot) | Visual / Basic with Graphics mode, Frame rate, Brightness, Text size, FOV, HUD FOV, Motion Blur and no Screen mode row; text sharp at the native output and as tall as on a 1080p PC |
| Brightness changes the picture | `scenes/24_brightness.script`, `r__exposure` 0.7 / 1.0 / 1.5 | mean pixel value 46 / 65 / 94 |
| Windows menu untouched | code review, below | every change guarded; the `--graphics-profile none` tree equals the stock one |
| Profile tables | `ctest -R graphics_profile_test` | device selection for iPad and Mac cases, the render scale clamp, unsupported features off in every profile and mode, lock rules, frame caps (40 on 120 Hz only; Quality's 40 is 30 on 60 Hz), MetalFX entries |
| Frame pacing is even where the display allows | the same test replays `FramePacer` against simulated 60 and 120 Hz displays with 0.3 ms of jitter | 30 on 60 Hz, 60 on both, 40 and 30 on 120 Hz: every frame a whole, equal number of refreshes at the cap's rate; 40 on 60 Hz alternates 1 and 2 refreshes |
| The cap on the iPad | `scripts/lock_test.txt` plus `gfx_fps 40`, `gfx_fps 25`, with locks on | `gfx_fps 40` and `25`: `Invalid syntax` (not on offer on 60 Hz); `gfx_fps 30` accepted; `gfx_fps unlimited` refused; a `user.ltx` that saved `r__upscaler metalfx` from an unlocked run: `1 of them differ from the profile: r__upscaler metalfx`, ignored |

Menus were screenshotted on the Mac (Settings, Visual / Basic, Modded Exes) and on the iPad. Changing a value by clicking through a drop-down was not
scripted (the cursor starts in the middle of the screen when a dialog opens and a click on a drop-down arrow needs calibration): Apply goes through
the stock `On_Accept`, which runs `exec_console_cmd` (the console path above) and `vid_restart` for the mode row, and `gfx_mode performance` through the
console was checked to re-apply the profile (`graphics profile mac-pro applied (mode change)`, and on the iPad the scale 0.42 of Performance).

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
