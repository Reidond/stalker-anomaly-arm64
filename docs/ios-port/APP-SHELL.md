# Apple app shell

Everything the game needs from iPadOS and macOS before and around the engine: where files live,
whether the game data is there, app lifecycle, memory/thermal/power callbacks, display info,
logging and crash capture, and a launcher screen. It is a static library (`anomaly_apple_shell`,
`src/Platform/Apple`) with a plain C++ interface in `Shell.h`, plus a lab app that runs it on its
own (`tools/shell_lab`).

```cpp
#include <Platform/Apple/Shell.h>      // everything below
#include <Platform/Apple/Launcher.h>   // the launcher screen
```

Link against `anomaly_apple_shell` (it pulls in SDL3 and the Apple frameworks). Call `AppleShell::Init`
after `SDL_Init`.

## Paths

| Path | iPad | macOS |
|---|---|---|
| `Paths::data_root` (the engine's `$fs_root$`, read-only) | `Documents/Anomaly` | `~/Documents/Anomaly`, or configured (below) |
| `Paths::appdata` (`$app_data_root$`: saves, `user.ltx`, screenshots) | `Library/Application Support/Anomaly/appdata` | `~/Library/Application Support/Anomaly/appdata` |
| `Paths::logs` (the engine's `$logs$`) | `…/appdata/logs` | same |
| `Paths::caches`, `Paths::shader_cache` | `Library/Caches/Anomaly[/shaders]` | `~/Library/Caches/Anomaly[/shaders]` |
| `Paths::shell_dir` (shell log, crash file) | `Documents` | `~/Library/Logs/Anomaly` |
| `Paths::crash_log` / `crash_log_previous` | `Documents/anomaly-crash.log` / `.prev.log` | `~/Library/Logs/Anomaly/…` |
| `Paths::fsgame_ltx` (generated) | `Library/Application Support/Anomaly/fsgame.ltx` | same |

- `Documents` is visible in the Files app and Finder (`UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`),
  which is how the game data gets in and how logs get out.
- On iPad the shell creates `Documents/Anomaly` and marks it excluded from backups (17 GB).
- Caches may be deleted by the OS when storage runs low, so only rebuildable data goes there (the shader cache).
- Mac data root, highest priority first: `InitParams::data_root_override`, `--data-root <path>`,
  `ANOMALY_DATA_ROOT`, `defaults write com.andriishafar.anomaly AnomalyDataRoot <path>`, then the default.
  The shell never writes into the data root, so pointing it at the repo's `game/Anomaly` is safe.

### Generated `fsgame.ltx`

`WriteFsGameLtx()` (called by `Init`, rewritten only when the content changes) writes the stock
Anomaly `fsgame.ltx` with four absolute roots on top:

```
$fs_root$        = false | false | <data_root>
$app_data_root$  = true  | false | <appdata>
$app_cache_root$ = false | false | <caches>
$shader_cache$   = false | false | <shader_cache>
```

Every other entry is unchanged and hangs off those roots. It is regenerated on every launch because the
iOS container path changes when the app is reinstalled.

What the engine needs for this: pass `Paths::fsgame_ltx` as the `fs_name` of `CLocatorAPI::_initialize`.
`searchForFsltx` accepts an absolute path, and because the file defines `$fs_root$` itself the engine's
"fsltx lives in the game folder" assumption is not needed. Two small things in `LocatorAPI.cpp` to be aware
of: roots that are not aliases are taken literally but are lower-cased (`xr_strlwr(root)`), which is fine on
the case-insensitive APFS volume of an iPad or Mac (the container's UUID is upper-case); and `$app_cache_root$` /
`$shader_cache$` are new aliases the shader-cache code can use.

## Game data discovery and validation

Sizes in the app and the scripts are binary (GiB / MiB). The Anomaly archives are 17,149,484,207 bytes
(17.1 GB in the decimal units Finder shows, 15.97 GiB here).

`ValidateGameData(root)` stats the 67 required archives of an Anomaly 1.5.3 install against
`GameDataManifest.inc` (exact byte sizes, 17,149,484,207 bytes / 15.97 GiB in all; the optional
`default-*.mhr` HRTF presets are recorded but not required). About 70 `stat` calls, so cheap enough to run
every time the launcher is shown or on a timer.

| `DataState` | Meaning |
|---|---|
| `Ok` | every required archive is present with the exact size |
| `Incomplete` | some are missing or have another size: interrupted copy, or a different Anomaly version |
| `NotFound` | no data root, or no `db/` in it |

The result carries `issues` (first 64 files with expected/actual size), `files_ok/files_expected`,
`bytes_present/bytes_expected`, `nested_root` (the game found one folder too deep, the usual Files-app
mistake) and `help`, a ready-to-print how-to-copy text: Finder, the Files app and the push script on iPad;
`--data-root` / `ANOMALY_DATA_ROOT` / `defaults` on the Mac. The launcher shows it.

Regenerate the manifest from a clean install with `tools/ios/gen_gamedata_manifest.sh [dir]`.

## Lifecycle, memory, thermal, power

Register a `Listener` with `AddListener`; every callback runs on the main thread.

```cpp
AppleShell::Listener l;
l.on_pause  = [] { /* stop simulating, silence OpenAL, stop submitting GPU work */ };
l.on_resume = [] { /* undo */ };
l.on_memory = [](const AppleShell::MemoryEvent& e) {
    if (e.level >= AppleShell::MemoryLevel::Warning) { /* drop caches, lower texture LOD */ }
};
l.on_thermal   = [](AppleShell::ThermalState now, AppleShell::ThermalState before) { /* scale quality */ };
l.on_low_power = [](bool on) { /* cap to 30 fps */ };
AppleShell::AddListener(std::move(l));
```

- **Pause / resume** come from SDL3 events, through an `SDL_AddEventWatch` so they run synchronously
  inside the OS callback (an iOS app is suspended right after `DidEnterBackground`; an event queue would be
  too late). SDL 3.4 on iPadOS reports every transition twice, a few ms apart (scene delegate and UIApplication
  notifications both fire), so the shell only forwards a transition that changes its state. `on_pause` is `SDL_EVENT_WILL_ENTER_BACKGROUND`, which on iPadOS is `sceneWillResignActive`, so
  it also fires for Control Center, system alerts and app-switcher peeks, not only for real backgrounding.
  `on_resume` is `SDL_EVENT_DID_ENTER_FOREGROUND` (`sceneDidBecomeActive`). `on_lifecycle` gets all five
  events (`Will/DidEnterBackground`, `Will/DidEnterForeground`, `Terminating`). No GPU work may be issued
  between `DidEnterBackground` and the next `DidEnterForeground`.
- **Memory** has three levels, `Normal`, `Warning`, `Critical`, from three sources that are merged
  (the reported level is the worst of them; callbacks fire on changes only):
  - `SystemPressure`: kernel memory-pressure notifications (normal / warn / critical dispatch source).
  - `AppWarning`: UIKit's memory warning (`SDL_EVENT_LOW_MEMORY`), reported as `Critical` and held for 5 s.
  - `Budget`: a watchdog on `os_proc_available_memory()` (iOS), polled every 250 ms from `AppleShell::Update()`:
    below 768 MB is `Warning`, below 384 MB is `Critical`, with 128 MB of hysteresis. The kernel signals can
    lag behind a fast allocation spike; the budget is what jetsam actually enforces. The event carries
    `available_bytes`.
- **Thermal** (`NSProcessInfo.thermalState`: Nominal, Fair, Serious, Critical) and **Low Power Mode** are
  delivered on change; their current values are in `QueryDeviceInfo()`.
- **Keep awake**: `SetKeepScreenAwake` (on after `Init`) disables the idle timer through
  `SDL_DisableScreenSaver`, and releases it while paused.
- `RecentEvents()` returns the event history (lifecycle, memory, thermal, power), which the launcher and
  any debug overlay can show.

`AppleShell::Update()` must be called once per frame from the main thread (it drives the budget watchdog).

## Display info

`QueryDisplayInfo(window)`:

| Field | Source |
|---|---|
| `native_width/height` | `UIScreen.nativeBounds` (landscape order) |
| `drawable_width/height`, `window_*`, `scale` | `SDL_GetWindowSizeInPixels`, `SDL_GetWindowSize`, `SDL_GetWindowPixelDensity` |
| `native_scale` | `UIScreen.nativeScale` |
| `refresh_hz` / `max_refresh_hz` / `promotion` | current display mode / `maximumFramesPerSecond` (120 on ProMotion iPad Pros; the iPad Air M3 is 60) |
| `safe_area_points/pixels` | `SDL_GetWindowSafeArea`, as insets from each edge |

## Logging and crashes

- `AppleShell::Log(fmt, ...)` writes `anomaly-shell.log` in `Paths::shell_dir` (the previous run's log is kept
  as `anomaly-shell.prev.log`), stderr and the unified log (`subsystem com.andriishafar.anomaly`, category `shell`).
  SDL's own log output is redirected into the same sink.
- Crash capture, installed by `Init`: `SIGSEGV/BUS/ILL/FPE/ABRT/TRAP/SYS` on an alternate stack, `std::terminate`
  and uncaught Objective-C exceptions append a report to `anomaly-crash.log`: signal, fault address, available
  memory, backtrace and the last 8 KB of log. The next launch moves it to `anomaly-crash.prev.log`. The handlers use only async-signal-safe calls and then hand the
  signal back to the system, so Xcode and the OS crash reports still work.
- Jetsam kills and force quits cannot be caught. `session.state` records `running`, `background` or `clean`;
  `PreviousSessionEndedUnexpectedly()` is true when the last run died while `running`, i.e. a crash or a
  memory kill in the foreground. `PreviousCrashLogExists()` is true when the previous run left a crash report.

## Launcher

`AppleShell::Launcher` draws with SDL_Renderer debug text: game-data status (and the copy instructions when
it is not `Ok`), memory budget, thermal state, Low Power Mode, free storage, display info, previous-crash notes
and, optionally, the live event list. Buttons: Start (enabled only with valid data), Rescan, Quit (macOS) and
any `AddButton`s. Touch, mouse, keyboard (Enter, R, Q) and gamepad (A, X). While the data is not valid it
rescans every 2 s, so a copy in progress shows up live.

```cpp
launcher = std::make_unique<AppleShell::Launcher>(window, renderer, config);
// SDL_AppEvent:   if (launcher->HandleEvent(*event) == Launcher::Action::StartGame) { /* hand over to the engine */ }
// SDL_AppIterate: AppleShell::Update(); launcher->Render();
```

## Final app files

- `platform/ios/Info.plist.in`: landscape-only (`UISupportedInterfaceOrientations~ipad`), `UIRequiresFullScreen`,
  `UIApplicationSupportsIndirectInputEvents`, `GCSupportsControllerUserInteraction`, `GCSupportedGameControllers`
  (ExtendedGamepad), `UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`, Metal and arm64 device
  capabilities, hidden status bar. `platform/macos/Info.plist.in` is the Mac counterpart.
- Entitlements: `platform/ios/Anomaly.entitlements.in` (generated into `build/<preset>/Anomaly.entitlements`, with
  `increased-memory-limit` on and `extended-virtual-addressing` opt-in) was already there; `cmake/AnomalyGameApp.cmake`
  adds `anomaly_configure_game_app(target)`, which applies the final bundle id `com.andriishafar.anomaly`, these
  plist templates and the entitlements file to a target. The engine executable should use it:

  ```cmake
  add_executable(anomaly ...)
  anomaly_configure_game_app(anomaly)
  ```

## Shell lab

`tools/shell_lab` (bundle id `com.andriishafar.anomaly`, shown as "Anomaly Shell Lab") runs the shell by itself:
data status, device and display info, and the live event list. "Alloc 512 MiB" / "Free memory" (or `--stress`)
allocate and touch memory in 128 MB chunks to watch the memory levels change, stopping 192 MB before the budget
runs out. Launch arguments, passed with `devicectl device process launch … -- <args>` or on the Mac command line:

| Argument | Effect |
|---|---|
| `--data-root <path>` | data root (macOS) |
| `--print-report` | print one `ANOMALY_REPORT key=value …` line (data status, storage, memory) and exit; `push_gamedata.sh` uses it |
| `--exit-after <s>` | quit after that many seconds |
| `--stress` | allocate automatically until the level reaches `Critical` |
| `--screenshot <name>` | save a PNG of the launcher into `Paths::shell_dir` after 1.5 s |

```bash
cmake --preset macos-arm64
cmake --build --preset macos-arm64 --target shell_lab
build/macos-arm64/tools/shell_lab/shell_lab.app/Contents/MacOS/shell_lab --data-root game/Anomaly
```

```bash
cmake --preset ios-arm64-local
xcodebuild -project build/ios-arm64/AnomalyPort.xcodeproj -scheme shell_lab \
  -destination 'generic/platform=iOS' -allowProvisioningUpdates build
xcrun devicectl device install app --device <udid> build/ios-arm64/Debug-iphoneos/shell_lab.app
xcrun devicectl device process launch --device <udid> --terminate-existing --console com.andriishafar.anomaly
```

## Pushing the game data to the iPad

`tools/ios/push_gamedata.sh` (zsh, `xcrun devicectl device copy to --domain-type appDataContainer`):

1. Reads the iPad's free storage by launching shell_lab with `--print-report`.
2. Copies a ~265 MB subset of real archives and measures MB/s.
3. Copies the rest, skipping `bin/`, `tools/` and `*.exe`, **only if the iPad has more than 40 GB free**
   (`--min-free-gb`).
4. Launches shell_lab again to validate the data on the device, and prints the timings.

The script lists what is already in the app container (`devicectl device info files`) and only copies files
that are missing or have another size, so an interrupted run is resumed by running it again (a complete data set
re-checks in about 5 s). Options: `--device`, `--source`, `--subset-only`, `--check-only`, `--dry-run`. The
target app must be installed and the iPad unlocked for the two `--print-report` launches (copying itself works
while the iPad is locked).

Measured on 2026-10-21 over USB to the iPad Air 13" M3 (512 GB, iPadOS 27.0.1), with other sessions loading the Mac:

| Step | Result |
|---|---|
| Free storage before | 339.5 GiB of 475.9 GiB (above the 40 GiB threshold) |
| Subset: 8 archives, 253 MiB | about 12.8 s, 20 MiB/s including 0.5 to 1 s of `devicectl` start-up per file; 26 to 27 MiB/s on the two 100+ MiB archives |
| Full copy, 15.7 GiB still missing | 553 s (9 min 13 s), about 29 MiB/s overall |
| per folder | `db/meshes` 1372 MiB in 43 s (31.6 MiB/s), `db/sounds` 1040 MiB in 31 s (33.6), `db/textures` 7090 MiB in 238 s (29.8), `db/levels` 6827 MiB in 239 s (28.6) |
| Verification on the device | shell_lab: 67 of 67 archives with the exact sizes, 15.97 GiB |
| Free storage after | 323.1 GiB |
| Re-run on the complete set | 5.6 s, nothing copied |

The one-file-per-`devicectl`-call overhead is only visible on the small archives; the large ones run at the link
speed, which is USB-limited at about 30 MiB/s here.

## Verified on the iPad Air M3

shell_lab (bundle id `com.andriishafar.anomaly`, signed with the free Personal Team) on the iPad, 2026-10-21:

| Area | Observed |
|---|---|
| Device | `iPad15,5`, iPadOS 27.0.1, 8 cores, 7.44 GiB RAM visible to the process |
| Memory budget | `os_proc_available_memory()` 7.99 GiB at launch (about 7.8 GiB once the UI is up), so `increased-memory-limit` is active |
| Display | native 2732x2048, 1366x1024 pt at 2x, 60 Hz, max 60 (no ProMotion on the Air), safe area bottom inset 20 pt |
| Data | 67 of 67 archives valid after the copy; the container path changed on reinstall (`6EC14A93…` to `34D418F3…`) and the data came with it, `fsgame.ltx` was regenerated for the new path |
| Lifecycle | launching another app gives `WillEnterBackground`, `DidEnterBackground` about 0.9 s later, then `WillEnterForeground` and `DidEnterForeground` on return, each exactly once |
| Memory pressure | `--stress` (128 MiB chunks, touched) triggered `Normal -> Critical via system pressure` at 1.47 GiB still available, then the UIKit memory warning; the kernel jumped straight to critical, with no separate warning step |
| Crash capture | `--crash-after 2` wrote `anomaly-crash.log` with signal, fault address, available memory, backtrace and recent log; the next launch reported "previous session ended unexpectedly" and moved the report to `anomaly-crash.prev.log` |
| Thermal / Low Power | initial state read correctly (Nominal, off); change callbacks are registered but a transition was not provoked on the device |
| Keep awake | idle timer disabled through SDL while active; not observed over a long idle period |

Not tested: the 768 MiB / 384 MiB budget watchdog (the kernel signal arrived first), touch input on the launcher,
and gamepad input on the launcher. On the Mac the memory-pressure callback fired on its own while the machine was
under heavy build load, and crash capture, the data states (`Ok`, `Incomplete`, `NotFound`, nested folder) and the
launcher were checked against the real data and against hand-made partial folders.

## What the engine has to call

1. `SDL_Init(...)`, then `AppleShell::Init({argc, argv})`.
2. Pass `GetPaths().fsgame_ltx` to the file system as the fsgame.ltx; use `GetPaths().appdata/logs/caches` for
   anything else it writes.
3. Show the launcher (or at least call `ValidateGameData(GetPaths().data_root)`) before touching the file system,
   and refuse to start unless the state is `Ok`.
4. Register a `Listener` for pause/resume, memory, thermal and low-power.
5. `AppleShell::Update()` every frame.
6. `AppleShell::Shutdown()` on exit.
