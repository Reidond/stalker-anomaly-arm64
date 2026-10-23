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
| DualSense, sound | Virtual pad on the iPad and the Mac: every binding and UI screen checked ([M5 system checks](#m5-system-checks)); the feel is the [hands-on list](#hands-on-dualsense-checklist-needs-a-person) |

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
| `--script <file>` | scripted run (INTEGRATION.md). New in the app: relative paths are looked up in `Documents/`, a trailing ` # comment` is ignored, `ballast <MB>` / `unballast`; the virtual pad, touch, lifecycle and audio-interruption commands are in CONTROLS.md "Test aids". `run_string` in a script needs `-dbg` (`ipad.sh script <file> -dbg`) |
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
- **Quick use does nothing in a scripted start** (Mac, M5): the Quick 1 wheel slot and F1 on the keyboard both reach `quick_use_1`, `CInventory__eat` called from Lua
  returns true, but the medkit is not used (count 3 stays 3, health stays). Not the pad; for the gameplay session.
- Flashlight and night vision were only seen reaching their actions: the scripted start has neither item. Check them in a normal game.
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

### What M5 changed in the controller path

- **The cursor that stopped answering** (the old known issue: the stick moves the cursor, the menu sees no hover or click). On the Mac it was SDL: it drops joystick events
  while no window has the keyboard focus, so the script pad followed the script for a few frames and then froze (the cursor ran to the edge and ignored everything after).
  Unattended runs set `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS`. Two real defects showed up behind it: a cursor *jump* (R3, a D-pad jump to a widget, a touch)
  moved the cursor without a mouse move, so no hover followed, and the main menu's click needed the sliding lens to be under the cursor, so a tap or a click right after a
  jump was dropped. Jumps now send the widgets a mouse move and the button under the cursor takes the click (`UIMMShniaga.cpp`).
- **Touch** puts the UI cursor on the finger (it integrated deltas from the last pointer position, which is not where the UI cursor is when there is no mouse).
- **L2 / R2** step the PDA's tabs; **a missing controller** pauses with a reason on screen and any key, click or tap lets go.
- **Test aids** for all of it: `pad_feedback_log`, `pad_info`, `pad_cursor`, the script commands of CONTROLS.md "Test aids".

## The UI colour bug

Symptoms: the Load game list showed no saves, the inventory no item icons. Cause (luabind, found on the iPad): Lua scripts pass UI colours to `u32` parameters as
negative numbers (`GetARGB(255, 216, 186, 140)` is -2573684, the alpha in the sign bit); luabind cast the double straight to the unsigned type, which
is undefined for a negative value: x86 wrapped it, arm64 saturates it to 0. Every Lua-set colour became `00000000`, fully transparent (the list's
glyph quads were being drawn, with vertex colour 0). Integer parameters now go through `long long`. This is very likely the Mac's "dark rectangles".

## M5 system checks

Before the second hands-on session every system the DualSense test touches was run without a person: the virtual pad of `--script` (`padon`, `pad`, `paddown`/`padup`,
`padaxis`, CONTROLS.md "Test aids") on the Mac (unattended, own state, the 2026-10-23 build of the M5 PR) and on the iPad, with logs and screenshots read for each row. The two
scripts are `tools/ios/scripts/pad_bindings.txt` (every default binding, a `STATE` and a `VIEW` line around each) and `pad_ui.txt` (every UI screen, screenshots);
`lifecycle.txt` with `tools/ios/lifecycle_check.sh` drives the app lifecycle from the Mac. The two pad scripts gave the same effects on the iPad (2026-10-23, the PR's build, run
with `-dbg`) as on the Mac; the Mac ran at a load average of 400, so only effects are judged there and the iPad's numbers are quoted where there is one.

| Item | How verified | Result |
|---|---|---|
| **Controller, in the level** | | |
| Move, strafe (left stick) | `padaxis ly/lx 1`, position after 1.5 to 2 s | OK on both: forward 5.4 m in 2 s, back 2.1 m in 1.5 s, strafe 3.8 m in 1.5 s (`forward` `back` `lstrafe` `rstrafe` reach the game) |
| Walk (gentle push) | `padaxis ly -0.6`, 2 s | OK: 2.4 m against 5.4 m running, `accel` is held. A push under about 0.52 of the stick moves nothing (dead zone 0.18, then the keys switch on at 0.42 of the rest): the walk window is 0.52 to 0.69 |
| Sprint (L3) | L3 tap while running forward | OK: 11.7 m in 2.5 s (`sprint_toggle`) |
| Look (right stick) | `padaxis rx/ry`, `VIEW` yaw and pitch sampled every 0.5 s | OK both axes (the camera yaw wraps at 180: read it in steps of less than half a turn). iPad: full push turns the view at about 310 degrees per second (220 and the 1.5x after 0.45 s at the edge), a 0.3 push at 17 |
| Crouch (Circle held), jump (Cross), lean (D-pad left / right) | eye height, y, camera roll | OK: eye 1.66 to 1.24 m, jump +1.1 m, roll -0.19 and +0.20 |
| Aim (L2), fire (R2), reload (Square) | field of view, magazine | OK: fov 75 to 63.1, 30 to 16 rounds in a 0.3 s pull, reload refills |
| Previous / next weapon (L1, R1 taps) | active slot | OK: slot 3 to 2 to 3 |
| Wheels: L1 hold (items), R1 hold (combat) | stick pick and release, action log, screenshots | Both open and pick (`wpn_3`, `quick_save` reach the game, the quicksave and its preview are written). **Quick use 1 reaches `quick_use_1` but does not use the medkit**; F1 on the keyboard does not either, so it is not the pad (see follow-ups) |
| Flashlight, night vision (D-pad up, down) | action log | Reach `torch` and `night_vision`; no visible effect: the test start has no flashlight or night-vision item |
| Use (Triangle) | action log | Reaches `use`; picking an item up was not exercised |
| Inventory (Create), PDA (touchpad), pause menu (Options) | `pad ui: dialog up / closed` log lines, screenshots | OK. Map: the stock game has no map binding, it is a PDA tab, reached with R2 / L2 |
| **Controller, UI screens** (`pad_ui.txt`) | | |
| Main menu | stick to a button, Cross; D-pad; R3 | OK: the stick hovers and clicks (the lens follows), D-pad nudges, R3 recentres. A tap or a click right after a cursor jump now takes the button under the cursor (it was dropped until the lens arrived) |
| Load game list | click a row, Cross | OK: the row is selected and the preview shows the frame of the save. Saves made before M5 have a black preview and `(no save)` shows the stock noise texture |
| Options and the console-style graphics menu | Settings, Cross on the Graphics mode combo | OK: the combo opens (Quality, Performance); the page list on the left is two list boxes: click them (L2 / R2 do not step them) |
| Inventory | drag the rifle out of its slot (Cross held, stick), Square | OK: drag and drop and the context menu work |
| PDA tabs | L2 / R2 | OK (new): the tab strip of the PDA steps through its 7 tabs |
| In-game menu and message box | Options, click Quit game | OK: "Are you sure you want to quit the game?" with Yes / No, answered with Cross. Circle (Escape) did nothing in a Yes / No box (the stock code has that case commented out): it answers No now (new, `CUIMessageBox::OnEscape`) |
| Dialog choices and trade | trader dialog, click choice 1 | OK: the trade window opens |
| Touch, no controller | `touch x y` on Settings | OK after the fix below: the cursor jumps to the finger and the tap opens Settings |
| **Controller, connection** | | |
| Disconnect and reconnect mid-game | `padoff` while walking, `padon` | Pauses at once ("Game paused", "Reconnect the controller to continue"); reconnecting resumes, walking and Create still work (bindings intact) |
| Second controller | `padon 2` (an Xbox Series pad) | Both are listed (`pad_info`); the pad touched last drives; one going away while the other stays does not pause |
| No controller at all | the last pad off, then a key, `touch` | Pauses like a disconnect; a key, a click or a tap ends the pause and the game goes on with the keyboard (new). On the iPad a game started with no pad shows no controls on screen: only the menus take touch |
| Rumble, light bar, adaptive triggers | `pad_feedback_log 1`, shots, a hit, low health, the menu | Calls go out with the CONTROLS.md parameters (shots rumble low 0.04 to 0.16 and high 0.07 to 0.27; `R2 weapon 0.40 / 0.62 / 0.55` at rest, `vibration 0.40 / 0.55 / 0.55` firing, `L2 feedback 0.25 / 0.40` aiming, both off on an empty magazine; light bar blue in a menu, a green pulse following health). The feel is for a person |
| **Other systems** | | |
| Save thumbnail | quick save, the Load game list | Fixed (Mac): `.dds` was black because the shim could not copy to a BC1 texture and the Metal back buffer is empty after Present. The preview is now captured from the presented frame, resized and BC1 encoded. An autosave made while the app is leaving the screen has no frame to capture and keeps the previous preview |
| Exit button | the pad, Exit game, Yes | The label said "Exit to Windows"; it reads "Exit game" now (new string `ui_mm_exit_game` in English and Russian, other languages keep the stock text) and quits cleanly: `anomaly: quit (ok)` 0.5 s after Yes |
| Audio start-up error | engine log | Fixed (Mac): `Max targets - 256` and `Can't create source. Error: Out of Memory.` came from OpenAL Soft's default of 256 sources against `snd_targets 1024`; with a generated `alsoft.ini` (`sources = 1024`) the log is clean |
| **App lifecycle on the iPad** (`lifecycle_check.sh`, the game standing in the Cordon at 30 fps) | | |
| Home and back, app switcher | Settings launched over the game with `devicectl` (the same transition), the game launched again | OK: `WillEnterBackground`, `ipad_autosave` written in 0.11 to 0.14 s, `DidEnterBackground` 1.0 s after the first; on the way back `WillEnterForeground`, `DidEnterForeground`, 29.7 fps in the next 10 s window, the footprint 3.9 GB, 3.73 GB in the background, 3.87 GB after; the actor was where it had stood. What is on the screen was not looked at (hands-on list) |
| Low-memory warning | `lifecycle lowmemory` (UIKit's event pushed in), then `ballast 3200` | OK: the app warning takes memory Normal to Critical and back after 5 s; real pressure at a 7.2 GB footprint with 0.99 GB available: Critical via system pressure and a genuine UIKit memory warning, 28.6 fps, no kill; ballast freed, 3.2 GB available again. `devicectl ... sendMemoryWarning` is refused by this iPad (ENOENT), hence the script's own events |
| Terminated while backgrounded | `devicectl ... signal --signal 9` (what jetsam does), then a start loading `ipad_autosave` | OK: the next start logs `previous session ended unexpectedly`, `ipad_autosave` (written at the background transition) loads, the actor and 15 inventory items are there, `ipad_test` is intact. A SIGTERM instead takes 31 s to quit (the paused engine cannot answer the quit before the 30 s limit): that is the quit hang, with the heap-stability session |
| Audio interruption | `audiointerrupt began` / `ended` (the notification a call or an alarm posts) | OK: `SOUND: audio session interrupted, device paused`, 6 s later `audio session reactivated`, the game stays up. A real one is on the hands-on list |
| Lock and unlock, Control Center, a banner | nobody can touch the iPad | Hands-on list |
| Audio start-up error | engine log of the iPad start | Fixed on the iPad: `no alsoft.ini in the game data, OpenAL Soft settings written to ...`, `snd_targets 256`, no `Max targets` and no `Can't create source`; the output route is logged (`Speaker`) |
| `vid_mode` saved at exit | `lock_test.txt` twice (`vid_mode 640x480` from the console, `cfg_save`) | Nothing to fix: the profile locks it (`vid_mode 640x480` refused, logged once), and the next start says `user.ltx has 192 locked settings, ignored (the ipad-8gb profile applies); 0 of them differ from the profile`; `vid_mode` stays `2732x2048` |
| Exit button, iPad | Exit game, Yes | OK: `quit (ok)` 0.5 s after Yes, exit code 0 |
| Save preview, iPad | the Load game list | OK: the preview shows the saved frame |
| Touch, iPad | `touch 683 688` (Settings in window points) | OK: Settings opens |
| Main menu cursor bug on the iPad | `padaxis ly 1` for 0.2 s, Cross | OK: the stick hovers Settings and Cross opens it (the baseline build was not run: the cause on the Mac was SDL dropping the script pad's events, above) |

## Hands-on DualSense checklist (needs a person)

Everything that has a log line or a screenshot above is done; what is left is how it feels, and the events nobody can trigger over USB. Load the `ipad_armed` save
(`tools/ios/scripts/pad_test.txt` makes it: AK-74 in hand, PDA in slot 8, outfit, quick slots F1 medkit, F3 bread, F4 flask) or any game.

- [ ] Sticks: the response curve and dead zone of move and look, the walk window (gentle push), acceleration after 0.45 s at the edge, look speed while aiming
- [ ] Cursor: stick speed, precision mode (L3 held), trackpad with the touchpad, D-pad nudges in the inventory
- [ ] Wheels: how quickly a slot is picked (hold L1 or R1, flick, release), page flip with D-pad left / right
- [ ] Haptics: a hit of each kind, a shot per weapon class, the blowout
- [ ] Adaptive triggers: R2 per weapon class (pistol to launcher), the vibration while an automatic weapon fires, limp on empty and during reload, the wall when jammed, L2 firming up while aiming
- [ ] Light bar: green to red with health, the pulse below 25 %, blue in menus
- [ ] Latency: stick to view, R2 to shot, Cross to click
- [ ] Gyro aiming (off by default; `gyro.mode` 1 is "ADS only")
- [ ] Hold Create for about a second (the system capture gesture) and PS (Home): the game must not misbehave afterwards
- [ ] Power the pad off and on again in the game: pause, "Reconnect the controller to continue", resume
- [ ] Lock and unlock, Control Center, a notification banner over the game: the game pauses, comes back with sound and without a black frame
- [ ] A real audio interruption (an alarm or a call) and the output moving to AirPods and back
- [ ] The on-screen taps in the menus with a finger, with and without a pad
