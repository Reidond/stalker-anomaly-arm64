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
(Q = Quality, P = Performance; "MetalFX" is `r__render_scale`, see [render size](#render-size-and-metalfx)):

| Setting | ipad-8gb | ipad-16gb | ipad-6gb | mac-pro | mac-base | mac-base-8gb | mac-max | mac-ultra |
|---|---|---|---|---|---|---|---|---|
| texture level | 1 | 0 | 2 | 1 | 1 | 2 | 0 | 0 |
| render size | 1280x960 / 1152x864; MetalFX 0.47 / 0.42 | 1366x1024 / 1280x960; MetalFX 0.5 / 0.47 | 1152x864 / 1024x768; MetalFX 0.42 / 0.375 | window; MetalFX 0.7 / 0.5 | window; MetalFX 0.6 / 0.45 | window; MetalFX 0.6 / 0.45 | window; MetalFX 0.8 / 0.6 | window; MetalFX 0.9 / 0.7 |
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

- iPad: `rs_screenmode borderless`, `rs_v_sync on`, `snd_cache_size 32`, `r__tf_aniso 4`, no MSAA, `-smap<size>` shadow map.
- Mac: `rs_v_sync on`, `r__tf_aniso 8`, no MSAA. The window and the display mode stay the player's (a desktop has windows).

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

Switching mode in the menu applies the profile's settings for that mode and restarts the video (`vid_restart`, a second or two); the
mode is saved in `user.ltx` as `gfx_mode`. The default is Quality.

## What the player can change, and why

| Menu | Option | Console | Why it stays |
|---|---|---|---|
| Visual / Basic | Graphics mode | `gfx_mode` | the one quality choice a console offers |
| | Frame rate | `gfx_fps` | 30 / 60 / (120): the player's choice of smoothness against battery and heat. Auto follows the mode |
| | Brightness | `r__exposure` | rooms differ and so do screens; the stock menu already used this variable for R4 (its gamma ramp is ignored on Metal) |
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

## Render size and MetalFX

The MetalFX session adds `r__render_scale` (the 3D scene renders at a fraction of the output size, the UI at native) and
`r__upscaler` (`metalfx` or `bilinear`). They are not in `main` yet. The profiles use them **by name and only if the console has
them**; until then the render size is the back buffer size, as before:

| | Without `r__render_scale` | With it |
|---|---|---|
| iPad | `vid_mode` is the render size (1366x1024 Quality, 1152x864 Performance on the 8 GB class); the Metal layer scales it to the panel | `vid_mode` is the panel's native size and `r__render_scale` the fraction |
| Mac | the window or display size, which is the player's | the scale on top of it |

The scale values in the tables are what the measurements below support at 1x scale equivalent; the MetalFX session reports its own
measured defaults and they replace these.

## Measurements

Method: `tools/profile_measure.py script` writes a `--script` timeline that loads the Cordon save and plays five scenes (outdoors at noon,
outdoors in rain, inside the trader's bunker with rain outside, the Rookie village at noon (a busy base: the most NPCs of the Cordon), and the
village at night with the flashlight on), each for 10 s of settling and 34 s of measurement, one launch per mode. The shell log's 10 s lines
(frames per second, the main thread's time in a frame, GPU time, footprint, thermal state) and the Metal statistics line (draw calls, texture and
render target memory) are averaged by `profile_measure.py summarize`. The Mac runs are gated: they wait for a Mac with no other game on the GPU
and drop a window that overlapped one.

### Mac: MacBook Pro 14-inch, M2 Pro (19 GPU cores), 32 GB, 3024x1964 at 120 Hz

`tools/render_harness/measure_profile.sh -u`: the `mac-pro` profile, unattended and offscreen (no window, no focus), `--unlocked-graphics`
and `gfx_fps unlimited` so that the frame rate is what the Mac could do, not the cap; `RelWithDebInfo`, the Lua JIT on. One launch per
mode and size, the five scenes above, each measured over its 10 s windows after 10 s of settling; a window counts only if no other `anomaly`
process ran during it. The render sizes stand in for the render scale while `r__render_scale` is not in `main`: 3024x1964 is the native
size (scale 1.0), 2116x1375 is scale 0.7, 1512x982 is scale 0.5. *fps* is the mean of the windows, *slowest* the slowest window, *CPU* the
time the main thread spends in a frame (it includes waiting for the GPU), *GPU* the GPU time per frame, *footprint* the process's physical
footprint at the end of the scene (it only grows over a run: the level streams in).

| Run | Scene | windows | fps | slowest | CPU ms | GPU ms | draws | footprint MB | thermal |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| **Quality, native** (run 1) | field | 2 | 105.8 | 105.4 | 9.4 | 6.7 | 1685 | 4836 | Nominal |
| | rain | 3 | 58.5 | **48.8** | 17.3 | 12.0 | 3013 | 4921 | Nominal |
| | bunker | 2 | 88.4 | 79.6 | 11.3 | 8.7 | 921 | 4720 | Nominal |
| | village | 2 | 111.9 | 107.1 | 8.9 | 12.4 | 1693 | 4797 | Fair |
| | night | 3 | 79.7 | 75.6 | 12.6 | 8.3 | 1279 | 4975 | Fair |
| **Quality, native** (run 2) | field | 2 | 103.8 | 103.6 | 9.6 | 6.9 | 1763 | 4879 | Nominal |
| | rain | 3 | 96.8 | 90.4 | 10.3 | 7.6 | 1586 | 4953 | Nominal |
| | bunker | 3 | 104.5 | 101.7 | 9.5 | 6.9 | 915 | 5020 | Nominal |
| | village | 2 | 82.8 | 73.3 | 12.2 | 9.9 | 1718 | 5081 | Nominal |
| | night | 3 | 83.0 | 58.7 | 12.9 | 8.2 | 1772 | 5221 | Nominal |
| **Quality, scale 0.7** | field | 2 | 110.8 | 107.9 | 9.0 | 5.8 | 1465 | 4563 | Nominal |
| | rain | 3 | 118.1 | 112.7 | 8.4 | 6.3 | 1916 | 4640 | Nominal |
| | bunker | 2 | 98.6 | 97.8 | 10.1 | 5.2 | 911 | 4460 | Nominal |
| | village | 2 | 135.2 | 128.0 | 7.4 | 4.7 | 1494 | 4561 | Nominal |
| | night | 3 | 110.4 | 93.5 | 9.2 | 4.7 | 1510 | 4705 | Fair |
| **Performance, native** | field | 2 | 102.2 | 101.4 | 9.8 | 7.0 | 1728 | 4395 | Nominal |
| | rain | 3 | 104.3 | 96.1 | 9.6 | 6.6 | 1396 | 4508 | Nominal |
| | bunker | 2 | 111.3 | 105.1 | 8.9 | 6.2 | 411 | 4661 | Nominal |
| | village | 2 | 93.3 | 93.0 | 10.6 | 13.0 | 1558 | 4762 | Nominal |
| | night | 3 | 124.4 | 115.6 | 8.0 | 9.2 | 1587 | 4863 | Nominal |
| **Performance, scale 0.5** | field | 2 | 119.2 | 116.5 | 8.4 | 4.6 | 1273 | 4397 | Nominal |
| | rain | 3 | 155.2 | 149.7 | 6.4 | 3.7 | 567 | 4482 | Nominal |
| | bunker | 3 | 158.5 | 156.1 | 6.3 | 3.4 | 411 | 4585 | Nominal |
| | village | 2 | 211.5 | 206.4 | 4.7 | 3.0 | 1078 | 4682 | Nominal |
| | night | 3 | 178.4 | 172.4 | 5.6 | 5.9 | 1115 | 4801 | Nominal |

What it says:

- **Quality holds 60 at the native size, with dips.** Two runs, 10 scene means between 58 and 112 fps; 3 of the 25 windows were under 60 (48.8 and
  58.7 in the rain of run 1, 58.7 at night in run 2). The rain is the worst scene: wet surfaces and the sun's shadows double the draw calls
  (3013 against 1396 in Performance), and the frame is main-thread bound (17 ms of CPU, 12 ms of GPU). The same scene was 97 fps in run 2,
  so a dip is an NPC or a downpour at the wrong moment, not a steady cost. With scale 0.7 every window is over 93 fps.
- **Performance holds 93 to 124 fps at native size, 117 to 212 at scale 0.5**, so the 120 fps cap of Performance on a 120 Hz Mac is met in most
  scenes with a render scale and in about half of them without. The frame is CPU bound below 8.3 ms of CPU: a render scale cannot buy more
  there. The village is GPU bound at native size (13 ms) and is where a scale helps most (4.3 ms at 0.5).
- The footprint is 4.4 to 5.2 GB, 14 to 16 % of the Mac's RAM. The 8 GB class (`mac-base-8gb`) uses one texture level less
  (about 0.7 GB, RENDER-QA.md).
- An early run with another game on the same GPU read 19 to 23 ms of GPU time for the same scenes: the harness now waits for a free GPU.

### iPad: Air 13-inch (M3, 8 GB), 2732x2048 at 60 Hz

**Measurements that set the iPad numbers** (`docs/ios-port/IPAD.md`, 2026-10-22, the first iPad preset, `RelWithDebInfo`, same Cordon scenes,
GPU ms is the sum of the command buffers' GPU time per frame; at 60 fps it includes waiting for the display):

| Render size | Mpixel | fps | GPU ms | footprint |
|---|---:|---:|---:|---:|
| 1024x768 | 0.79 | 60 | 16 (vsync) | 3.83 GB |
| 1366x1024 (the first preset) | 1.40 | 60, 55 in a few spots | 14 to 16 (vsync) | 3.8 GB |
| 1600x1200 | 1.92 | 42 to 47 | 17 to 18 | 4.09 GB |
| 1920x1440 | 2.76 | 32 to 35 | 24 | 4.2 GB |
| 2732x2048 (native) | 5.60 | 19 | 59 | 4.65 GB |

The two long soaks of that preset (48 minutes in all, thermal Nominal to Serious, no change of frame rate): open field 56 to 60 fps, the
Rookie village **49 to 55 fps at 15.2 to 16.2 ms of GPU**, footprint 3.96 to 4.10 GB of the 7.99 GB budget. GPU time grows with the pixel
count, about 10 ms per Mpixel, and the one setting with a large cost at a given size is the shadows (sun 2.3 ms, a medium shadow quality
+3 ms, wet surfaces 3.5 ms in rain at 1920x1440; SSAO 0.7 ms; grass, view distance, soft particles, soft water, sun shafts and
anisotropy not measurable).

From that: the first preset does not hold 60 in the village. **Quality is the first preset at 1280x960** (1.23 Mpixel: 16.2 ms x 1.23 / 1.40 =
14.2 ms, 15 % of a 16.7 ms frame to spare) and **Performance is 1152x864 with no AO and shorter distances** (1.0 Mpixel: about 11.5 ms, 30 % to
spare), both at the first preset's shadows (sun quality low, shadow map 1536, light shadows 0.5), no wet surfaces, texture level 1.
The footprint stays at about 3.8 GB: a render size changes the render targets, not the textures.

**On-device run of the two modes: pending.** `tools/ios/measure_profile.sh -i` makes it in one go (about 25 minutes: install, the menu
screenshot, the five scenes in each mode, the lock checks) and `profile_measure.py summarize` prints the same table as above for the iPad. It did
not run for this change because the iPad was held by another session's measurements for the whole time (a note in the shared folder said so,
and a launch from here would have killed that run); the numbers for Quality and Performance above are derived from the size sweep, not
measured on the finished profile. Treat `ipad-8gb` as "derived, to be confirmed by `measure_profile.sh`".

## Verification

Checks and where their output is:

| Check | How | Result |
|---|---|---|
| Console and scripts cannot change locked settings | `tools/render_harness/scenes/20_locks.script`: `r2_sun_quality st_opt_extreme` typed, `exec_console_cmd("texture_lod 4")`, `exec_console_cmd("r4_enable_tessellation on")`, `exec_console_cmd("ssfx_ssr (1, 1, 1, 1)")` | each refused, the first refusal of each logged once (`! graphics: '...' refused: locked by the mac-pro profile. Further attempts ... are not logged.`), the second typed attempt silent; the values read back are the profile's; `fov 80` and `gfx_mode performance` accepted; `gfx_fps unlimited` refused |
| The menu shows no locked row, and the PC menu is intact | `scenes/22_options_tree.script` prints the tree | with the profile: `OPTIONS locked rows visible: 0`, 7 rows on Visual / Basic; with `--graphics-profile none`: the stock tree (Advanced 42 rows, Graphics 8, Particles 4, HDR10 36, Optimizations 12, Debug 8) |
| Locks survive a restart, stale `user.ltx` is overridden | `tools/render_harness/lock_restart.sh`: run 1 changes settings and saves; `user.ltx` gets stale values for 7 locked settings (`r2_sun_quality st_opt_extreme`, `texture_lod 0`, `r4_enable_tessellation on`, `r3_msaa 4x`, `r2_ssao st_opt_high`, `r3_dynamic_wet_surfaces on`, `r__detail_radius 250`) and `r2_mblur_enabled on`; run 2 prints the values | start log: `user.ltx has 186 locked settings, ignored (the mac-pro profile applies); 7 of them differ from the profile: ...`; read back: `r2_sun_quality st_opt_low`, `texture_lod 1`, `r4_enable_tessellation off`, `r3_msaa st_opt_off`, `r2_ssao st_opt_low`, `r3_dynamic_wet_surfaces off`, `r__detail_radius 75` (the Performance values: the mode saved by run 1); `r2_mblur_enabled on`, `fov 80.`, `gfx_mode performance`, `gfx_fps 30` kept (the player's) |
| Brightness changes the picture | `scenes/24_brightness.script`, `r__exposure` 0.7 / 1.0 / 1.5 | mean pixel value 46 / 65 / 94 |
| Windows menu untouched | code review, below | every change guarded; the `--graphics-profile none` tree equals the stock one |
| Profile tables | `ctest -R graphics_profile_test` | device selection for 14 hardware cases, unsupported features off in every profile and mode, lock rules, frame caps, MetalFX entries |

The menu through the cursor (Settings, Visual / Basic and Modded Exes pages) was screenshotted on the Mac; changing a value by clicking
through the dropdown was not scripted (the cursor starts in the middle of the screen when a dialog opens and a click on a drop-down arrow
needs calibration): Apply goes through the stock `On_Accept`, which runs `exec_console_cmd` (the console path above) and `vid_restart` for the
mode row, and `gfx_mode performance` through the console was checked to re-apply the profile (`graphics profile mac-pro applied (mode change)`).

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
