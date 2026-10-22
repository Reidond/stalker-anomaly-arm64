# Anomaly on the iPad

The `anomaly` app (bundle id `com.andriishafar.anomaly`) runs the full game on an iPad Air 13" (M3, 8 GB, iPadOS 27.0.1): main menu,
a new game in the Cordon, loading and saving, 60 fps at the iPad preset. Measured 2026-10-22 on the test iPad (`iPad15,5`), signed
with the free Personal Team, `RelWithDebInfo` build, game data in `Documents/Anomaly`.

| Milestone | State |
|---|---|
| App boots, finds the data and the overlay | Yes. Engine up 3.5 s after launch (7.8 s the very first time) |
| Main menu | Yes, 60 fps |
| New game (faction dialog, Start, two loads, the Cordon) | Yes, about 45 s from "Start" to the first world frame |
| Play | Walking, rain, AI ticking, outdoors and interiors, teleporting around the level: 55 to 60 fps |
| Save, load | Yes (`save`, the menu, and an automatic save when the app loses the screen) |
| Background and return | Yes: the game pauses, saves first, resumes at 60 fps |
| DualSense, sound | See [Hands-on](#hands-on-session) |

## Install

You need the signing setup of [BUILDING.md](BUILDING.md) (Xcode 27, `CMakeUserPresets.json` with `ANOMALY_IOS_TEAM_ID`, your Apple ID in
Xcode, Developer Mode on the iPad) and the game data pushed into the app's `Documents/Anomaly` with
`tools/ios/push_gamedata.sh` ([APP-SHELL.md](APP-SHELL.md); 67 archives, 16 GiB, about 10 minutes over USB). Installing over the existing app
keeps `Documents`, so the data survives every update. **Never uninstall the app**: that deletes the data.

```bash
git submodule update --init --depth 1 external/SDL external/openal-soft external/LuaJIT external/ogg external/vorbis external/theora external/SPIRV-Cross external/vkd3d external/sse2neon
cmake --preset ios-arm64-local -DANOMALY_BUILD_ALL_MODULES=ON      # a few seconds when the build folder exists, 6 minutes from scratch
tools/ios/ipad.sh build                                            # xcodebuild, RelWithDebInfo: about 15 minutes from scratch, a minute after one .cpp
tools/ios/ipad.sh install                                          # over com.andriishafar.anomaly
```

Then tap **Anomaly** on the iPad (unlock it first), or launch with the console attached:

```bash
tools/ios/ipad.sh run [app args]           # devicectl --console: the shell log on stdout (it arrives when the app exits)
tools/ios/ipad.sh script <file> [args]     # copies a --script timeline to Documents/ and runs it (INTEGRATION.md: --script)
tools/ios/ipad.sh logs [dir]               # shell log, crash logs, engine log
tools/ios/ipad.sh shot <file.png>          # screenshot of the iPad (devicectl device capture)
```

`ANOMALY_LAUNCH_ENV='{"MTL_HUD_ENABLED":"1"}'` in front of `run`/`script` shows Apple's Metal HUD (GPU ms, memory) on the screen.

## Updating to a new build (7-day signing)

A free Personal Team's profile is valid for 7 days (this one: 2026-10-21 to 2026-10-28). After that the app no longer launches
("Unable to Verify App"). To refresh it, rebuild and install over the old one (`tools/ios/ipad.sh build && tools/ios/ipad.sh install`):
Xcode re-signs with a new profile, and `Documents` (data, `commandline.txt`, logs) and `Library` (saves, settings, shader cache) stay.
`xcodebuild -allowProvisioningUpdates` needs your Apple ID signed in to Xcode. A new build also runs the preset logic below, which merges
only when the preset's version number was raised. The free account allows 3 apps installed at a time.

## Where things are on the iPad

| What | Path (in the app container) |
|---|---|
| game data (read-only) | `Documents/Anomaly` (`db/`, ...) |
| shell log and crash files | `Documents/anomaly-shell.log` (`.prev.log`), `anomaly-crash.log` (`.prev.log`) |
| extra engine command line | `Documents/commandline.txt` (one flag per line or all on one line, `#` comments) |
| engine log | `Library/Application Support/Anomaly/appdata/logs/xray_mobile.log` (Windows-1251, CRLF; `.bkp` is the run before); crash reports `xray_<user>_<date>.crash.txt` |
| saves, `user.ltx`, screenshots | `Library/Application Support/Anomaly/appdata/{savedgames,user.ltx,screenshots}` |
| generated `fsgame.ltx` | `Library/Application Support/Anomaly/fsgame.ltx` (rewritten at every launch) |
| shader cache | `Library/Caches/Anomaly/shaders/{xlat,r4}` (rebuilt when missing; 276 shaders for a level take 77 ms from the cache, 1.3 s translated) |
| Monolith overlay archive | `Library/Caches/Anomaly/overlay/monolith_overlay.db0` (packed from the bundle's `overlay/` at launch: 161 files, 0.1 s) |

`tools/ios/ipad.sh get "<path>" <local>` and `put` copy files in and out through `devicectl`.

## What had to be fixed to boot

In the order they were met on the device.

1. **`system.ltx` missing: no archives found** (xrCore). The engine lower-cases virtual paths and resolves them again case by case, but the iOS
   sandbox does not let it list the parents of the app container, so the container's own spelling (`Containers/Data/Application/<UUID>`) is
   restored from `$HOME` first. `$HOME` is `/private/var/mobile/...` while `NSSearchPath` (and so every root in the generated `fsgame.ltx`) is
   `/var/mobile/...`, the same directory through a symlink; only the first form was restored. Both are now.
2. **Resolution**. The iOS renderer path took the window's pixel size (2732x2048) as the render size and ignored `vid_mode`; it is the
   render size only when `vid_mode` does not fit in the window (xrRenderDX10).
3. **30 fps at 2.6 ms of GPU** (MetalD3D11). With `maximumDrawableCount` 2 the display-link callback waited two vsyncs in `nextDrawable` every
   frame. Three drawables: 60 fps. (The engine's own menu limiter, `ECO_RENDER`, also cost a vsync and a core; it is off on iOS.)
4. **`r2_sunshafts_mode screen`** is not a token (it is `screen_space`); the first preset had it wrong.

Archive access needed no change: xrCore reads archives with `pread` and bounded `mmap` windows, never a whole archive, so
`extended-virtual-addressing` is not needed. Sound (OpenAL Soft on CoreAudio with an `AVAudioSession`, xrSound), Metal, the shader cache in
`Library/Caches` and the overlay archive packing all worked on the first launch that got past the file system.

## The iPad preset

`src/Platform/Apple/DevicePreset.cpp`. On iOS the shell merges these lines into `appdata/user.ltx` the first time, and again whenever the preset's
version number is raised (the version applied is in `Library/Application Support/Anomaly/device-preset.version`). The player's other settings,
bindings and later changes to settings the preset does not name are kept. `--device-preset reset` (on the command line or in
`commandline.txt`) applies it again at once, `--device-preset none` skips it, `--device-preset ipad` applies it on a Mac too. The one engine flag
the console cannot set, `-smap1536` (shadow map size), is added to the engine's command line.

| Setting | Value | Why |
|---|---|---|
| `vid_mode` | `1366x1024` | render size, 4:3 like the panel (2732x2048); the Metal layer scales it 2x. 1.4 Mpixel: 60 fps |
| `texture_lod` | `1` | 1 GB less than LOD 0, 0.9 GB more than LOD 2 (numbers below); the render size does not show more |
| `r__tf_aniso` | `4` | |
| `r2_sun_quality` / `-smap1536` / `r2_ls_squality` | `st_opt_low` / 1536 / `0.5` | shadows: the one setting with a large GPU cost (medium is +3 ms at 1920x1440) |
| `r3_dynamic_wet_surfaces` (+ `_sm_res 1024`) | `off` | 3.5 ms of GPU at 1920x1440 in the rain |
| `r2_volumetric_lights`, `r4_enable_tessellation`, `r3_volumetric_smoke`, `r2_mblur_enabled` | `off` | not available on Metal or not worth their cost |
| `r2_sunshafts_mode` / `r2_ssao` | `screen_space` / `st_opt_low` | |
| `r__detail_density` / `r__detail_radius` / `rs_vis_distance` | `0.4` / `35` / `0.8` | grass and view distance |
| `snd_cache_size` | `32` | MB of decoded sound kept |
| `rs_screenmode`, `rs_v_sync`, `r__supersample` | `borderless`, `on`, `1` | |

### Render size, speed and memory

Same spot of the Cordon (a field, rain), `RelWithDebInfo`, Lua interpreter, steady state; GPU ms is the sum of the command buffers' GPU time per
frame (`mtl_d3d11::GpuTimeNsTotal`). Below 60 fps the GPU is the limit, at 60 the figure includes waiting for the display, so it only says the
real GPU time is lower.

| `vid_mode` | Mpixel | fps | GPU ms | footprint |
|---|---:|---:|---:|---:|
| 1024x768 | 0.79 | 60 | 16 (vsync) | 3.83 GB |
| **1366x1024 (preset)** | 1.40 | 60 (55 in a few spots) | 14 to 16 (vsync) | 3.8 GB |
| 1600x1200 | 1.92 | 42 to 47 | 17 to 18 | 4.09 GB |
| 1920x1440 | 2.76 | 32 to 35 | 24 | 4.2 GB |
| 2732x2048 (native) | 5.60 | 19 | 59 | 4.65 GB |

GPU time grows with the pixel count (about 10 ms per Mpixel). The footprint is what the OS charges the app (`phys_footprint`, Metal and
compressed memory included) against 7.99 GB of budget with `increased-memory-limit`. At the native size one `texture_lod` step is about 1 GB
(LOD 2 3.9 GB, LOD 1 4.65 GB, LOD 0 5.7 GB). The footprint is 0.3 to 0.5 GB in the main menu, 1.9 GB when a load starts, peaks about 4.3 GB
during a load, and creeps up by about 10 MB per minute in the world.

Cost of single settings at 1920x1440 (GPU ms, one setting changed at a time, noise about 2 ms): wet surfaces 3.5, sun 2.3, shadow quality
medium +3, SSAO 0.7, grass, view distance, soft particles, soft water, sun shafts, anisotropy: not measurable. `tools/ios/scripts/sweep_gpu.txt` and
`res_sweep.sh` repeat these runs.

To change the preset by hand edit `appdata/user.ltx` (copy it out with `ipad.sh get`) or use the in-game options; the game saves `user.ltx`
at exit, so whatever you set in a session is what the next one starts with (a scripted `vid_mode` included: run `--device-preset reset` after).

## Memory warnings and the watchdog

The shell watches `os_proc_available_memory()` every 250 ms (warning below 768 MB, critical below 384 MB) and the kernel's pressure
notifications and UIKit's memory warning. `tools/ios/scripts/memory_pressure.txt` fills memory with 64 MB blocks (`ballast <MB>` in a script) while
the game runs. Observed in the Cordon (3.75 GB footprint to start):

| Footprint | Available | Event |
|---|---:|---|
| 5.2 GB | 2.9 GB | nothing |
| 6.2 GB | 2.0 GB | nothing |
| 6.98 GB | 1.2 GB | kernel: Normal -> Critical, then UIKit's memory warning (the watchdog's own thresholds had not been reached) |
| 7.63 GB | 0.56 GB | still running at 60 fps; freed again, back to 3.75 GB |

The engine's response (`CRenderDevice::OnMemoryPressure`) runs a full Lua collection (`lua_collect_garbage`, 40.0 MB to 39.4 MB) and asks malloc
to give back free pages (`malloc_zone_pressure_relief`); the old `Memory.mem_compact()` is a no-op outside the debug memory manager. This returned
nothing measurable: the footprint is textures, render targets and Metal heaps (the engine counts 1.45 GB of textures), and dropping those
would need the renderer to reload them. With 4.2 GB of headroom at the preset the game never gets near the limit, so no further response was added.

## Lifecycle

- **Pause**. `WillEnterBackground` (which iPadOS also sends for Control Center, alerts and the app switcher) pauses the engine (`OnSystemPause`):
  nothing renders, the sound emitters pause (xrSound's `AVAudioSession` handling), the cursor is released. `DidEnterForeground` resumes: the
  game was back at 59.7 fps in the next 10 s window after 15 s in Settings.
- **Save on the way out**. The app's listener runs before the engine pauses and saves `ipad_autosave` (a normal save, in the Load menu),
  running frames until the file is written: 0.07 s of wall time here, complete before `DidEnterBackground`. Not while loading, in the main menu,
  or within 30 s of the previous one. So a jetsam kill or a force quit of the backgrounded app loses nothing.
- **Terminate**. `SDL_EVENT_TERMINATING` (rare: a suspended app is killed without being told) leaves the level through the engine's disconnect
  (`KERNEL:disconnect` and `quit`), for up to 4 s. A normal quit (the menu's Exit, the script's `quit`) goes the same way and was clean in every run.
- **Screen awake**. `SetKeepScreenAwake` keeps the idle timer off while the game is in the foreground. A 6-minute session with no touch
  stayed lit (see the idle result below).
- **Frame pacing**. The app runs on SDL's `CADisplayLink` at the display's rate and presents with three drawables. The iPad Air's panel is 60 Hz
  (no ProMotion), so 60 fps is the ceiling; on a 120 Hz iPad Pro SDL asks for 120 and the same code applies, the renderer would need to be twice
  as fast. Below 60 fps frames are not paced to 30: expect 42 to 47 fps style judder at higher render sizes.

## Performance: where the time goes

Main thread in the Cordon field at the preset, Time Profiler over 8 s (`xctrace`, `tools/ios/tp_summary.py`): the thread is on a CPU 8.4 s of 8 s,
but 4.9 s of that is `light::vis_update` -> `R_occlusion::occq_get` spinning on occlusion query results, i.e. waiting for the GPU. The rest is
about 7.5 ms per frame: rendering the scene graph (visibility, static geometry, constant and texture binding) about 4.6 ms, `FrameMove` (the
scheduler, AI, Lua) about 2.8 ms. Lua's `lua_pcall` is 0.66 s, 8% of the main thread, about 1.4 ms per frame, with the interpreter; the JIT
(2.7 to 3.2x on script-only code, IPAD-JIT.md) could save about 1 ms of that, so it was not worth a debugger-authorized run: the game is limited by the GPU, not
by Lua. A secondary engine thread uses 36% of a core. So: **GPU bound** at the native size, at 1600x1200 and above; at the preset GPU and
vsync, with the CPU at about 45% of a frame.

## Launch arguments

| Argument | Meaning |
|---|---|
| `--device-preset reset|none|ipad` | see above |
| `--script <file>` | scripted run (INTEGRATION.md). New in the app: relative paths are looked up in `Documents/`, a trailing ` # comment` is ignored, `ballast <MB>` / `unballast` |
| `-dbg` | engine flag: enables the console's `run_string`, `run_script`, `jump_to_level`, `g_god`; `gamedata/scripts/ipad_debug.script` has `dump_smarts()` and `tp(x, y, z)` for test runs |
| `--exit-after <s>`, `--show-launcher`, `--data-root` | as on the Mac |

Test scripts in `tools/ios/scripts/`: `new_game.txt` (menu, faction dialog, Start, the Cordon, a walk, a save: makes the `ipad_test` save),
`load_game.txt`, `tour.txt` (four places of the Cordon), `res_sweep.sh`, `sweep_gpu.txt`, `memory_pressure.txt`. The shell log line every 10 s has
fps, CPU ms inside `Frame()`, GPU ms, footprint (peak) and available memory.

## Known issues

- **Intermittent abort in ShaderXlat** (renderer session). `malloc: pointer being freed was not allocated` in vkd3d's `preproc_free_macro` /
  `preproc_yyparse` while a shader is translated at run time: seen once, non-fatal, at start-up, and twice as a `SIGABRT` (once at quit, once
  mid-run), both times translating a `hud_font` shader requested by `CConsole::OnRender`. It never shows on the Mac. Shaders already in the cache
  do not go through vkd3d, so it shows when new shaders are needed (the first run of a level, the console font). Reported to the M2 session.
- The renderer's known defects (INTEGRATION.md): rain over interior geometry, the 34 missing `ssfx_*` shaders, no tessellation. One
  `pipeline creation failed (vs206 ps205): Fragment input user(locn2) mismatching` per load.
- `sound: OpenAL: Can't create source. Error: Out of Memory.` once at start-up with 256 targets (the engine continues); `bin/alsoft.ini` is not on the
  iPad (the push script skips `bin/`), so OpenAL Soft runs on its defaults.
- Runtime changes of `vid_mode` through the console work, but the engine saves the last one at exit, and it becomes the next start's size.
- The main menu's "Exit to Windows" quits the app.
- The save screenshot (`.dds`) next to a save was not checked.

## Hands-on session

Results of the checks with a DualSense on the iPad are in the PR description and are added here when they come in.
