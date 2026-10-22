# Integration: the engine as one macOS app

Every module is linked statically into one app, `anomaly` (`src/Platform/App`), and runs a new game against the real
Anomaly 1.5.3 data on macOS: main menu, faction dialog, loading screens, Cordon, a minute of walking, a save, a clean quit,
a relaunch and a load of that save. Rendering is "good enough to see progress" (M2 is the correctness pass; the list is in
[Known issues](#known-issues-by-module)).

Measured on a MacBook Pro (Mac14,9, M2 Pro, 12 cores, 32 GiB, macOS 27.0.1), `RelWithDebInfo`, game data in
`game/Anomaly`, 2026-10-22.

## Build

```bash
git submodule update --init --depth 1 external/SDL external/openal-soft external/LuaJIT external/ogg external/vorbis external/theora external/SPIRV-Cross external/vkd3d external/sse2neon
cmake --preset macos-arm64-release -DANOMALY_BUILD_ALL_MODULES=ON
cmake --build --preset macos-arm64-release --target anomaly
```

The app is `build/macos-arm64-release/src/Platform/App/anomaly.app` (bundle id `com.andriishafar.anomaly`, 57 MB with
debug info). The target exists only with `ANOMALY_BUILD_ALL_MODULES=ON` (it needs xrEngine, xrGame and the R4 renderer); the
other presets are unchanged.

| Step | Time (12 cores) |
|---|---|
| configure | 78 s (Ninja) |
| all 14 modules from scratch (2196 steps, xrGame with its shared PCH) | 5 min 34 s |
| relink of `anomaly` after a change in one `.cpp` | 5 s |
| rebuild after touching an xrCore/xrEngine header | 4 to 5 min |

Debug builds work (`macos-arm64`) but xrGame alone is 2.4 GB; use `RelWithDebInfo` and lldb (`-O2 -g`: breakpoints and
backtraces work, locals are often optimised out).

### How the Windows DLL plumbing became static

Nothing had to be replaced: xrEngine already carried the static path (`STATIC_RENDERER_R4` in `EngineAPI.cpp` calls
`DllMainXrRenderR4` and `DllMainXrGame`, `xrFactory_Create` and `xrFactory_Destroy` are plain symbols of xrGame, only R4
exists on Apple). What the integration added is the app target, the entry point and the fixes in
[Crash and fix log](#crash-and-fix-log). The first link left 12 undefined symbols and no duplicates:

- `ImGui_ImplDX11_*` (6): the stock DX11 ImGui backend is now compiled into `xrImGui` (state backup/restore and the
  multi-viewport window code are compiled out on Apple; `dxImGuiRender::Render` invalidates the renderer's state cache
  instead).
- `dx10StateCache<...>::ClearStateArray` (3): explicit instantiations in `dx10StateCache.cpp` (clang emits an implicit one
  only in the translation unit that uses it).
- `CVisualMemoryManager::current_state/mask`, `CALifeMonsterBrain::smart_terrain` (3): `IC` functions defined in a `.cpp` and
  called from others; MSVC emitted them as COMDATs, clang drops an unused inline definition. They are plain functions now.

`tools/xrengine_smoke` failed the same way (`GetUICursor`, `get_binded_action`, `CUICursor::*`: xrEngine's ImGui input glue
needs xrGame) and, with the modules on, linked its own stubs next to the real ones (9 duplicate symbols). It is now added
from the root `CMakeLists.txt` after every module file has run, so its stub files drop out for the modules that exist, and it
links xrGame and the renderer when they are built.

No stub was needed in the app: multiplayer is the `mp-stub` shard, Discord and the vendor SDKs were already compiled out by the module
owners (`ANOMALY_XRENGINE_DISCORD=OFF`, NvAPI/AGS/ReShade stubs), and `xrengine_smoke`'s own stubs only apply when a module
is not built.

## Run

```bash
build/macos-arm64-release/src/Platform/App/anomaly.app/Contents/MacOS/anomaly --data-root game/Anomaly
open build/macos-arm64-release/src/Platform/App/anomaly.app --args --data-root "$PWD/game/Anomaly"
```

`--data-root` is read by the shell (APP-SHELL.md lists the other ways: `ANOMALY_DATA_ROOT`, `defaults write
com.andriishafar.anomaly AnomalyDataRoot <path>`, the default `~/Documents/Anomaly`). The data root is never written to.

### Command line

| Option | Meaning |
|---|---|
| `--data-root <path>` | the Anomaly folder (holds `db/`, `gamedata/`, `bin/`) |
| `--overlay <dir>` | the Monolith gamedata overlay (`ANOMALY_OVERLAY`); default is the bundle's `Resources/overlay` |
| `--exit-after <s>` | ask the engine to quit that many seconds after it is up |
| `--script <file>` | scripted run: console commands, synthetic keys and mouse (below) |
| `--show-launcher` | show the launcher (data status, how to copy data) even when the data is valid; it is shown by itself when the data is missing or incomplete |
| anything else | the engine's command line, as on Windows (`-nosound`, `-nojit`, `-start ...`, ...), after `commandline.txt` (`~/Library/Application Support/Anomaly/commandline.txt`) |

`-load <save>` on the command line does nothing (the console `load` needs a running ALife). To load, use what the main menu
runs: `start server(<save>/single/alife/load) client(localhost)`.

### The Monolith overlay

The engine build is Monolith's and needs the repository's `gamedata/` (161 scripts, configs and shaders: `global.lua`,
`pda_dynamic_tabs.script`, `r3/hdr10_*.ps`, `mod_system_*.ltx`, ...) next to the game data. On Windows they are copied into
the game's `gamedata/`; the game data here is read-only, so:

1. CMake copies `gamedata/` into the bundle (`Contents/Resources/overlay`; the bundle root on iOS).
2. At startup the shell packs it into `~/Library/Caches/Anomaly/overlay/monolith_overlay.db0` (an uncompressed X-Ray archive,
   `entry_point = $fs_root$\gamedata\`, rebuilt only when file names, sizes or mtimes change: 60 ms for 161 files).
3. The generated `fsgame.ltx` mounts it as `$monolith_overlay$`, after the game's own archives (so it overrides them, as the
   loose files do on Windows) and before `db/patches`, `db/addons` and the loose `gamedata/` folder (which override it).

Without it the game boots to the menu and then fails in scripts (`Failed to load script pda_dynamic_tabs`).

### Where things go

| What | Path |
|---|---|
| engine log | `~/Library/Application Support/Anomaly/appdata/logs/xray_<user>.log` (the previous run is `.bkp`; it is Windows-1251 with CRLF, use `grep -a`) |
| engine crash report | `appdata/logs/xray_<user>_<date>.crash.txt` (xrDebug's signal handler) |
| shell log, crash file | `~/Library/Logs/Anomaly/anomaly-shell.log`, `anomaly-crash.log` (`.prev` for the run before) |
| saves, `user.ltx`, screenshots | `~/Library/Application Support/Anomaly/appdata/{savedgames,user.ltx,screenshots}` |
| generated `fsgame.ltx` | `~/Library/Application Support/Anomaly/fsgame.ltx` |
| shader cache | `~/Library/Caches/Anomaly/shaders/{xlat,r4}`: 8 MB after a few sessions; deleting it costs about 2 s at the next start (and ShaderXlat translates again at each level) |
| overlay archive | `~/Library/Caches/Anomaly/overlay/` |

### Unattended runs: `--script`

SDL3's callback loop takes every event out of the queue before the engine's frame, so the app forwards them
(`xr_engine::HandleEvent`); `--script` pushes real SDL events through that path. One step per line, `#` starts a comment,
the time is seconds since the engine came up:

```
<t> console <command...>     Console->Execute (`start server(...)`, `save <name>`, `quit`)
<t> key <name> [ms]          SDL key name (Return, Space, Escape, W, ...) pressed for ms (80)
<t> keydown <name> | keyup <name>
<t> move <x> <y>             pointer, window points (starts at the window centre)
<t> relmove <dx> <dy>
<t> click <x> <y> [button]
<t> log <text...>            a line in the shell log
<t> quit                     leave through the engine's own disconnect path
```

The UI cursor follows relative mouse motion (the engine runs the pointer in relative mode), so `move` sends the delta from the
script's own pointer, which starts at the window centre where the dialogs place their cursor.

A new game from the menu (1512x982 window; Return opens the faction dialog on the highlighted "New game"):

```
5 key Return
8 move 921 865        # the Start button
9 click 921 865
36 key Space          # "Press any key to continue", twice to be safe
48 key Space
80 keydown W          # walk
95 keyup W
113 console save m1_test
215 quit
```

`console start server(all/single/alife/new) client(localhost)` also starts a game, but skips the faction dialog, whose script
sets the start position: the player stays in `fake_start`, a bare test corridor.

## Startup timeline

Wall times from the shell log and the engine log, warm caches unless noted.

| t (s) | Stage |
|---|---|
| 0.00 | SDL, `AppleShell::Init` (paths, `fsgame.ltx`, overlay check, crash handlers) |
| 0.03 | `xr_engine::Init`: command line, `Core._initialize`: 68 archives, 51,037 files in the registry, 0.23 s |
| ~0.4 | `system.ltx`/`game.ltx`, engine, SDL window, `CInput` (SDL3 + gameinput), console, `user.ltx` |
| ~0.6 | sound (OpenAL Soft, default device), renderer device on Metal, ShaderXlat, `xrGame` globals, fonts, UI |
| 2.3 to 3.4 | main menu up (`anomaly: engine up in ...`). Cold shader caches: 5.0 s (138 menu shaders, 1.3 s translating) |
| +0 | New game: faction dialog, Start |
| +1 to +7 | `fake_start` loaded: spawn registry (18,652 spawn points, 1.3 s), objects, textures (about 1,050, 0.3 s) |
| +10 to +18 | the faction dialog's script calls `ChangeLevel` to `l01_escape`: second load (the Cordon, about 1,075 textures, 1.1 GB of them) |
| +20 | "Press any key to continue", then the player is in the Cordon |

(Resident size sampled once a second; the two loads show as two peaks.)

Loading a save: "Game m1_test is successfully loaded from file ... (1.292 s)" for the ALife part; the level is up and at 4.6 GB
within 25 s of the command.

Frame rate: see [Memory and speed](#memory-and-speed).

## Memory and speed

`footprint <pid>` (what jetsam counts on the iPad; the process's resident size is about half of it, the rest is GPU and
compressed memory):

| State | Footprint |
|---|---|
| main menu | 513 to 600 MB |
| loading `fake_start` | peak about 2.7 GB |
| Cordon, 1 minute in | 4.3 GB |
| Cordon, 3.5 minutes in (a save, quit, relaunch and load cycle gives the same) | 4.6 GB, creeping up by about 30 MB per minute |

Breakdown in the Cordon (4.5 GB): `IOAccelerator` (Metal buffers and textures) 2.5 GB, malloc 1.2 GB (heap 1.0 GB in the engine's
own count, Lua 37 MB), graphics-unmapped 0.47 GB, IOSurface 0.14 GB. The engine reports 1.13 GB of D3D textures. These are
desktop settings (texture LOD 1, 3024x1964 render target); for the iPad (7.95 GB with `increased-memory-limit`, 2732x2048)
that is the starting point for the M4 memory work.

Frame rate (the app logs it every 10 s to the shell log): 60.0 fps in the Cordon at 3024x1964, steady for the two minutes
measured, in a small interior and with the post-processing missing (see Known issues); the main menu runs unthrottled at about
95 fps. 60 is the display's rate, so the headroom is not known yet; the loads run at 3 to 30 fps while textures and shaders
stream in.

## Crash and fix log

In the order they were met. Commit subjects name the module.

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | link: 12 undefined symbols | inline defs in `.cpp`, template members, missing ImGui backend | see [above](#how-the-windows-dll-plumbing-became-static) |
| 2 | `on_idle` null deref in `CUISequencer::OnRender` | `REG_PRIORITY_INVALID` is `0xfffffffful`: 64 bits on LP64, so `Prio != REG_PRIORITY_INVALID` never matched an `int -1` and removed registrations (the logo intro) kept being called | `0xffffffffu` (xrEngine `pure.h`) |
| 3 | `imgui.ini` created in the working directory under the name `\users\...` | ImGui opens the path with `fopen`; the engine's path is virtual | `xr_native_path` (xrEngine) |
| 4 | shutdown: `brk #1` in `CObjectFactory::~CObjectFactory`, then the shell's crash handler spun for minutes | clang compiles the destructor call of an abstract class without a virtual destructor (`xr_delete(CObjectItemAbstract*)`, `ParticleAction*`) to a trap; MSVC ran the base destructor | virtual destructors on both; `static_assert` in `xr_special_free` so any other site fails to compile (xrCore, xrServerEntities, xrParticles) |
| 5 | shutdown: SIGSEGV in `luabind::detail::unref` | with the virtual destructor the factory's `luabind::object`s are released, after the Lua state (owned by the ai space) was closed | the factory is deleted first (xrGame) |
| 6 | main menu solid magenta; `pipeline creation failed (vs28 ps52): Vertex shader writes point size` | the translated vertex functions carried `[[point_size]]`; Metal refuses it for triangle pipelines | `enable_point_size_builtin = false`, ShaderXlat 0.2 |
| 7 | new game: `D3DX11CreateTextureFromMemory` E_INVALIDARG | the renderer skips mip levels (texture LOD) of single-level DDS files; D3DX clamps `FirstMipLevel`, the shim failed | clamp (shim) |
| 8 | new game: `Can't open section '\x12pawn_loadout2'` | `loadout_section` pointed into a `string32` declared inside the loop body | buffer hoisted (xrGame `alife_object.cpp`) |
| 9 | first HUD item destroyed: `pointer being freed was not allocated` | `~HUD_SOUND_COLLECTION_LAYERED` called `~HUD_SOUND_COLLECTION()` on every element and `clear()` destroyed them again; MSVC's vector destructor nulls its pointers, libc++'s does not | `clear()` only (xrGame) |
| 10 | keys and mouse never reached the game | SDL3's callback loop dispatches every event to `SDL_AppEvent` before `xr_engine::Frame()` pumps the queue | `xr_engine::HandleEvent` -> `xr_window::DispatchEvent` (xrEngine, app) |
| 11 | `Failed to load script pda_dynamic_tabs`, then `level_name` missing in an archive header | the Monolith overlay was not mounted; its archive needs `level_name`/`level_ver` for `CApplication::GetArchiveHeader` | overlay archive (shell) |
| 12 | `xr_wounded.script:509: attempt to call field 'mod'` | LuaJIT 2.1 dropped `math.mod` and `string.gfind` (2.0 had them) | defined after the libraries open (xrServerEntities `script_storage.cpp`) |
| 13 | `xr_meet.script:592: attempt to index a nil value` in the first stalker's `net_Spawn` | `db.actor` unset: `xrServer::SendConnectionData` iterates an `xr_hash_map`; MSVC's iterates in creation order (the actor first), libc++'s in hash order | actor first, then by id (xrGame) |
| 14 | SIGSEGV at 0x84..., `CStateMonsterAttackOnRun<CAI_Boar>::critical_finalize` | `set_override_animation(eAnimUndefined)` indexed `m_anim_storage[u32(-1)]`; `EMotionAnim` is unsigned here (MSVC: int, one word before the buffer) | handled explicitly (xrGame) |
| 15 | exit code 134/139 after "quit (ok)": `~CRender` | quitting with a level loaded skipped the level unload | the app quits through `SDL_EVENT_QUIT`, i.e. the engine's own disconnect (app) |

Classes of bug to expect more of (all found by running the game, none by the compiler): dangling pointers to block-scoped
locals, `u32(-1)` enums used as indexes, container iteration order, explicit destructor calls followed by implicit ones,
`unsigned long` constants compared with `int`.

## Known issues by module

- **Renderer R4 / Metal shim (M2).**
  - The weather layer (rain) is drawn over interior geometry, and some UI and HUD windows show dark rectangles (screenshots in
    the PR). The scene is lit and textured, the sky and the HUD draw.
  - 34 optional post-process shaders are in no archive and fall back to `stub_default` (`ssfx_*`, `heatvision`,
    `deffer_terrain_low_flat`): SSR, SSS, bloom, TAA, volumetrics, water effects are absent.
  - `pixel shader N reads 1 interpolant(s) vertex shader M does not write ... zero-filled` is logged once.
  - Tessellation, 3D fluid (volumetric smoke) are switched off (no hull/domain/geometry stages).
  - `screenshot` and `r4_...` console paths were not exercised.
- **ShaderXlat.** A cold cache costs about 2 s at the menu (138 shaders, 1.3 s of translation); a level asks for about 165
  more. Translation is not threaded.
- **xrGame.** Every crash on the list above is a C++ behaviour MSVC tolerated. Expect more in code paths a two-minute walk
  does not reach: monster and stalker AI states, combat, inventory, the PDA, surges. Memory creeps up by about 30 MB per
  minute (not investigated).
- **xrEngine.** `-load` on the command line does nothing; `commandline.txt` is read, not exercised. A level
  load blocks the main loop for seconds; the engine pumps SDL events during loads, so the window stays responsive.
- **Shell / app.** The launcher only shows when the data is invalid. The overlay is packed from the bundle; on iOS the
  bundle copy step (`Resources` is the bundle root there) was only compiled, not run.
- **Input.** Keyboard and the UI cursor were driven with synthetic SDL events. A person has not yet played with a mouse
  (look, click, hold), and no DualSense was connected (the gameinput layer is linked and initialised).
- **Sound.** OpenAL Soft opens the default device; nothing was listened to.
- **xrNetServer / multiplayer.** In-process single-player transport only; the multiplayer entry points are the `mp-stub`
  stubs and log/fail safely.
- **Lua.** LuaJIT 2.1 with the JIT on; scripts ran for four minutes of play without a Lua error after the fixes above.

## For M2 (renderer correctness) and M4 (iPad)

M2: the rain/interior ordering and the dark rectangles are the first visible defects; then the 34 missing shader files
(decide whether to ship the Screen Space Shaders addon files or stay on the stubs), then the interpolant warning, then
tessellation/fluid if they are wanted.

M4: the in-level footprint (4.5 GB, 2.5 GB of it Metal) against 7.95 GB; texture LOD and render scale presets; the iOS bundle
copy of the overlay; archive access (`pread`, no `extended-virtual-addressing` with a personal team); lifecycle
(`xr_engine` already pauses on `on_pause`); the exit path (`SDL_EVENT_QUIT` -> disconnect) for backgrounding.

## iOS (compile and link only)

**Update:** the target now runs on the iPad, see [IPAD.md](IPAD.md). The section below is the original compile-and-link check.

The same target configures and links for `ios-arm64`, unsigned, nothing run on a device:

```bash
cmake --preset ios-arm64 -B build/ios-app -DANOMALY_BUILD_ALL_MODULES=ON     # 387 s
xcodebuild -project build/ios-app/AnomalyPort.xcodeproj -scheme anomaly -configuration Release \
  -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO build                                  # about 4 min on 12 cores
```

The result is `build/ios-app/src/Platform/App/Release-iphoneos/anomaly.app`: a 44 MB arm64 Mach-O, bundle id
`com.andriishafar.anomaly`, with the overlay in `overlay/` at the bundle root (161 files). One change was needed: the Xcode
generator builds each xrGame OBJECT library as its own static library but cannot archive their `$<TARGET_OBJECTS>` into
`libxrGame.a` (the file is never produced and the app's link fails), so under Xcode the shards are static libraries and
`xrGame` is an INTERFACE library that links them (`cmake/targets/xrGame.cmake`). The Ninja build is unchanged.
