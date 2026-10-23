# Anomaly on the iPad

The `anomaly` app (bundle id `com.andriishafar.anomaly`) runs the full game on an iPad Air 13" (M3, 8 GB, iPadOS 27.0.1): main menu,
a new game in the Cordon, loading and saving, 60 fps at the iPad preset. Measured 2026-10-22 on the test iPad (`iPad15,5`), signed
with the free Personal Team, `RelWithDebInfo` build, game data in `Documents/Anomaly`.

| Milestone | State |
|---|---|
| App boots, finds the data and the overlay | Yes. Engine up 3.5 s after launch (7.8 s the very first time) |
| Main menu | Yes, 60 fps |
| New game (faction dialog, Start, two loads, the Cordon) | Yes, about 45 s from "Start" to the first world frame |
| Play | Walking, rain, AI ticking, outdoors and interiors, teleporting around the level: 60 fps in the field, 49 to 55 at the village ([long runs](#long-runs-stability-and-thermal)) |
| Save, load | Yes (`save`, the menu, and an automatic save when the app loses the screen) |
| Background and return | Yes: the game pauses, saves first, resumes at 60 fps |
| DualSense, sound | Virtual pad on the iPad: yes. First hands-on session: sound and look fine; the rest is [deferred to M5](#deferred-to-m5-hands-on-dualsense-test) |

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
`xcodebuild -allowProvisioningUpdates` needs your Apple ID signed in to Xcode. A new build applies the graphics profile at its first start like any other. The free account allows 3 apps installed at a time.

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

## The iPad profile

The first iPad preset (a table merged into `user.ltx` once) is replaced by the device graphics profiles of [SETTINGS.md](SETTINGS.md): the
shell detects the device, the profile for its class is applied on every start after `user.ltx`, and what it sets is locked. The iPad Air
13-inch (M3, 8 GB) gets `ipad-8gb`, in two modes (Quality, Performance); the options menu keeps the mode, the frame rate, field of view,
brightness, motion blur and the HUD. `--graphics-profile none` (or `--unlocked-graphics`) runs without the locks, and the settings sweeps
below use `--unlocked-graphics`. The old `--device-preset none|ipad` still works as `--graphics-profile none|ipad`; `reset` does nothing now
(the profile is applied on every start). The one engine flag the console cannot set, the shadow map size (`-smap1536`), is added to the
command line by the profile.

The measurements that led to the first preset are kept below: they are the base of the Quality numbers in SETTINGS.md.

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

GPU time grows with the pixel count (about 10 ms per Mpixel). The render scale of RENDERER.md ("Render scale and upscaling") keeps the
output at the panel's size and the UI crisp while the scene renders at a fraction of it; its iPad measurements (scale 0.5 to 1.0, MetalFX
and bilinear, fps, GPU ms, footprint) are in that section. The footprint is what the OS charges the app (`phys_footprint`, Metal and
compressed memory included) against 7.99 GB of budget with `increased-memory-limit`. At the native size one `texture_lod` step is about 1 GB
(LOD 2 3.9 GB, LOD 1 4.65 GB, LOD 0 5.7 GB). The footprint is 0.3 to 0.5 GB in the main menu, 1.9 GB when a load starts, peaks about 4.3 GB
during a load, and creeps up by about 10 MB per minute in the world.

Cost of single settings at 1920x1440 (GPU ms, one setting changed at a time, noise about 2 ms): wet surfaces 3.5, sun 2.3, shadow quality
medium +3, SSAO 0.7, grass, view distance, soft particles, soft water, sun shafts, anisotropy: not measurable. `tools/ios/scripts/sweep_gpu.txt` and
`res_sweep.sh` repeat these runs.

The profile's settings cannot be changed by hand: `user.ltx` is read and then the profile is applied on top (SETTINGS.md); for experiments
use `--unlocked-graphics`. The game saves `user.ltx` at exit (a scripted `vid_mode` included; the profile puts the right one back at the next start).

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
  running frames until the file is written: 0.07 to 0.10 s of wall time here, complete before `DidEnterBackground`. The engine is held active for
  those frames (`xr_engine::HoldActive`): SDL sends a window-minimize before `WillEnterBackground`, the engine's `Frame()` pumps it, and without the
  hold the engine stopped mid-save (seen once with Control Center). Not while loading, in the main menu,
  or within 30 s of the previous one. So a jetsam kill or a force quit of the backgrounded app loses nothing.
- **Terminate**. `SDL_EVENT_TERMINATING` (rare: a suspended app is killed without being told) leaves the level through the engine's disconnect
  (`KERNEL:disconnect` and `quit`), for up to 4 s. A normal quit (the menu's Exit, the script's `quit`) goes the same way and was clean in every run.
- **Sound output**. The session is `Ambient` (mixes with other apps, yields to calls). When the sound is "gone" look at the route first: the engine log says
  `SOUND: audio session ready, output route: <port name> (<port type>)` at start (the speaker is `Speaker`, in the iPad's language, then `(Speaker)`) and `SOUND: output route changed: ...` when a device appears or goes away. The
  one report of lost sound was the iPad's output moving to AirPods Max that had connected on their own; the speaker was silent because nothing was sent to it.
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

## Long runs: stability and thermal

Two soaks with the virtual DualSense on the `ipad_armed` save (god mode on, `-dbg`): `tools/ios/scripts/soak.txt` (27 minutes) and `soak2.txt` (21 minutes).
Both ended with exit code 0: no crash, no memory warning, no lifecycle event, the screen stayed on.

- **Soak 1**: walks west from the village for 10 minutes (220 m), then stands against the edge of the walkable area for 16 more, so it is mostly one scene. 162
  samples of the 10 s line: 60.0 fps on average (one 52 fps window while the level was still streaming in), GPU 14.4 ms (max 16.5), footprint 3.94 to 4.10 GB
  (about 100 MB of growth after the first 3 minutes, 4 MB a minute), at least 4.09 GB available, thermal Nominal the whole time.
- **Soak 2**: teleports to eight spots in turn, walks, opens the PDA and the inventory, and (in the script as run) was meant to re-equip the AK-74 with the combat
  wheel and fire. The wheel part failed because of the script, not the game: it let the stick go 1 s before R1 and a pick lasts 0.3 s after the stick is back at the
  centre, so the hands stayed empty after the first PDA and nothing fired. The script is fixed, `pad_test.txt` shows the whole path (PDA, Circle, R1 pick of slot 3,
  R2 empties the magazine). 125 samples: 55.2 fps on
  average, 45 at worst, footprint 3.96 to 4.07 GB. The frame rate depends on the place and repeats from lap to lap: the open field is 56 to 60 fps, the
  village is 49 to 55 (GPU 15.2 to 16.2 ms against a 16.7 ms frame, max 22). So the preset is not a flat 60: the heaviest spots are GPU bound just under it.
- **Thermal**: Nominal to Serious after 4.6 minutes in soak 2 (the iPad had been run for an hour), and it stayed Serious for the remaining 16. The same spot
  gave the same frame rate before and after (village 51.9 Nominal, then 55.6 and 55.1 Serious; the next spot 49.1, then 51.6 and 50.9), and the GPU time did not
  move, so no throttling shows at Serious. Soak 1 started cool and never left Nominal. Critical was never seen.
- Four more runs with `MallocGuardEdges`, `MallocScribble` and `MallocStackLogging` (`ANOMALY_LAUNCH_ENV`) loaded the save, played a minute and exited cleanly:
  the heap abort of Known issues did not show in them or in the two soaks, but one later run (a plain 100 s script) aborted in `CloseLog` at a normal quit (Known issues).

## Launch arguments

| Argument | Meaning |
|---|---|
| `--graphics-profile <id\|none>`, `--unlocked-graphics` | the device graphics profile ([SETTINGS.md](SETTINGS.md)); `--device-preset none\|ipad` is the old spelling |
| `--script <file>` | scripted run (INTEGRATION.md). New in the app: relative paths are looked up in `Documents/`, a trailing ` # comment` is ignored, `ballast <MB>` / `unballast` |
| `-dbg` | engine flag: enables the console's `run_string`, `run_script`, `jump_to_level`, `g_god`; `gamedata/scripts/ipad_debug.script` has `dump_smarts()` and `tp(x, y, z)` for test runs |
| `--exit-after <s>`, `--show-launcher`, `--data-root` | as on the Mac |

Test scripts in `tools/ios/scripts/`: `new_game.txt` (menu, faction dialog, Start, the Cordon, a walk, a save: makes the `ipad_test` save),
`load_game.txt`, `tour.txt` (four places of the Cordon), `res_sweep.sh`, `sweep_gpu.txt`, `memory_pressure.txt`. The shell log line every 10 s has
fps, CPU ms inside `Frame()`, GPU ms, footprint (peak), available memory and the thermal state. `soak.txt` and `soak2.txt` are the long runs (below).

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
- The Load game list was not screenshotted again after the colour fix (it uses the same Lua colours as the inventory list, which was). In a script the virtual
  pad's analog stick moves the cursor sprite but the menu does not see a hover or a click from it; the D-pad nudge (96 UI units a step) does click.
- The save screenshot (`.dds`) next to a save is black (the Load game preview shows black or noise).
- Four `SIGABRT`s from libmalloc (`pointer being freed was not allocated`) in about 90 runs, at different places (vkd3d; `CloseLog` at shutdown, twice): a
  double free or a foreign pointer somewhere, passed to the crash-sweep session. The latest, with the merged tree: `pick.txt`-style 100 s run, normal `quit`,
  `anomaly: script asked to quit`, then `xr_engine::Shutdown` -> `xrCore::_destroy` -> `CLocatorAPI::_destroy` -> `CloseLog` -> `LogFile.clear()` frees a bad
  block (`libsystem_malloc` abort). `clear()` walks every log line's string, so it trips over a heap corruption made anywhere earlier. Also worth a guard:
  `AddOne` does `LogFile.erase(LogFile.end()-1)` for a repeated line, which is undefined if the `clear_log` console command emptied the vector first.

## Controller

The DualSense layer (`src/Platform/GameInput`, CONTROLS.md) is hooked into the engine. A first hands-on session found that only the menu cursor and
the look stick worked: the engine had no names for the pad keys and no binding slot for them, so no button reached a game action. Now
`keyboards[]` has the `gp_*` keys, `_binding` has a third slot (`bind_pad`, `unbind_pad`; `default_controls` runs the default DualSense layout; `bind_list`
shows it), and every bind is mirrored into the layer's own map.

The virtual DualSense of `--script` (`padon`, `pad <button> [ms]`, `paddown`/`padup`, `padaxis <lx|ly|rx|ry|l2|r2> <value>`) drives the whole path without a
person: left stick up walks 11 m in 4 s, Create opens the inventory, the touchpad raises the PDA (with the PDA in its slot), R2 fires and empties the
magazine. `tools/ios/scripts/pad_test.txt` does it all, makes the `ipad_armed` save (AK-74 in hand with 30 rounds, 120 spare, PDA and outfit in their
slots, medkits, bandages) and logs `STATE` lines; `ipad_debug.script` has `give`, `give_ammo`, `equip`, `state`, `slots`.

The radial wheels (hold L1 or R1) are drawn by the game, not the layer: `xr_pad_render_radial()` in `xr_level_controller.cpp`, called from
`CUIGameCustom::Render()` before the dialogs, built from `CUIStatic` parts so it uses the engine's own UI scaling and scissor. Eight flat dark tiles on
a ring over a dimmed world, each with a 1 px edge and a soft shadow; the picked tile is amber, a little larger and drawn last. Weapon, quick-use and
detector slots show the item's inventory icon (cut from the same atlas as the inventory grid) with its slot number (1 to 6, F1 to F4); an empty slot is a
dash and its kind; actions without an item (Quick save, Drop, Safe mode, ...) are plain labels. The hub in the middle names the picked tile
(item name, then what it is) and shows the page dots. Open is a 120 ms ease-out (a fade and a settle from 94 %, never from nothing), the hot tile
follows the stick with a frame-rate independent lerp, and it closes instantly because the wheel is used often. A label is always on screen, so nothing
depends on the motion.

Two things that look like bugs and are not. The touchpad opens the PDA from the game, but while the PDA is up the pad is in the menu context, where the
touchpad press is a click: **Circle** (`ESC`) closes it (Create does the same for the inventory). And closing the PDA leaves the hands empty, as in stock Anomaly
(the PDA is the held item of slot 8): take the weapon out again with the combat wheel (R1, push the stick to slot 3, release R1) or its slot key. A wheel pick lasts
0.3 s after the stick is back at the centre, so flick and release R1 together, or release R1 while still pushing. Pressing the key of the slot that is already
in hand puts the weapon away (stock). `pad_test.txt` does the PDA, Circle, the pick and the shooting; `pick` runs of knife, AK-74 and pistol by wheel all took effect.

Not done: the Controls screen has no Gamepad column. A game started by script leaves the PDA and the outfit in the backpack (`slots(true)` moves the PDA to its
slot; a normal new game from the menu was not checked), and the 3D PDA only rises from its slot.

## The UI colour bug

Symptoms: the Load game list showed no saves, the inventory no item icons. Cause (luabind, found on the iPad): Lua scripts pass UI colours to `u32` parameters as
negative numbers (`GetARGB(255, 216, 186, 140)` is -2573684, the alpha in the sign bit); luabind cast the double straight to the unsigned type, which
is undefined for a negative value: x86 wrapped it, arm64 saturates it to 0. Every Lua-set colour became `00000000`, fully transparent (the list's
glyph quads were being drawn, with vertex colour 0). Integer parameters now go through `long long`. This is very likely the Mac's "dark rectangles".

## Deferred to M5: hands-on DualSense test

First session (before the controller fix): menu and loading fine, the look stick smooth and a stable frame rate, sound good (it went to the AirPods Max once the
iPad had them connected), background and return fine. Movement, Create, touchpad, L1 and R1 did nothing, the list text and the item icons were missing:
all of it fixed above, and re-checked with the virtual DualSense. The second session, with a real pad and the fixed build, moves to M5 (PLAN.md).
Load the `ipad_armed` save (`tools/ios/scripts/pad_test.txt` makes it: AK-74 in hand, PDA in slot 8, outfit, quick slots F1 medkit, F3 bread, F4 flask).

- [ ] Menu: virtual cursor with the left stick, Cross to click, Circle to go back
- [ ] Move, look, jump, crouch, sprint
- [ ] Aim (L2) and shoot (R2) with the AK-74; reload
- [ ] Inventory (Create), PDA (touchpad press, press again to close); drag and drop with the cursor
- [ ] Radial wheels: hold L1 (items) and R1 (combat), pick with the right stick, flip pages with D-pad left/right, release to use
- [ ] Rumble on a hit, light bar colour, adaptive trigger resistance
- [ ] Sound (effects, music, ambience); PS button or Control Center and back: pause, resume, sound
