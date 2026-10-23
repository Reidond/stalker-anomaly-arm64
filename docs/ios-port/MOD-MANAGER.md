# Mod manager

A mod manager inside the Anomaly app, Mod Organizer 2 style: every mod is kept in its own folder, mounted at engine start in
load order, and nothing in the game data is overwritten. The pieces:

| Piece | Where | Section |
|---|---|---|
| Engine mounting of the mods | `src/Platform/Apple`, `src/xrCore` | written by the mod-mount session |
| Library, import, FOMOD | `ModManagerCore` | written by the mod-core session |
| In-app browser and link downloads | `ModDownloads` | written by the mod-downloads session |
| The launcher (first screen, mod list, add mod) | `src/Platform/Launcher` | [Launcher](#launcher) |

The shared contract (mods root, `mod.json`, `modlist.json`, mount order, the Swift API of the two modules) is in the first message
of each of those sessions; the parts the launcher relies on are repeated below.

## Launcher

The launcher is the first screen of an ordinary start, on the iPad and on the Mac: native SwiftUI, shown before the engine exists,
with **Play**, **Mods** and **Add mod**. Play writes `modlist.json` and continues into the engine in the same process.

```
SDL_AppInit ─ SDL_Init, AppleShell::Init ─ launcher? ─ yes ─▶ SDL window (no renderer) + SwiftUI hosted in it
                                                    └─ no ──▶ xr_engine::Init                 (--script, --exit-after, --no-launcher, ...)
SDL_AppIterate: AppleShell::Update, poll anomaly_launcher_result
    Play ─▶ anomaly_launcher_close (views leave the window) ─▶ SDL_DestroyWindow ─▶ StartEngine (the engine makes its own window)
    Quit ─▶ SDL_APP_SUCCESS
```

### Screens

| Screen | What it does |
|---|---|
| Home | Play (disabled until the game data is valid), the number of enabled mods, Mods, Add mod, Quit (Mac). When the game data is missing or incomplete it shows the shell's check (`ValidateGameData`) with the how-to-copy text and a Rescan; it looks again by itself every 2 s, so a copy in progress shows up live. This replaces the old debug-text launcher, which stays in the shell for `shell_lab` and as the fallback when the SwiftUI one cannot be hosted. A previous crash is shown as a notice. |
| Mods | The list in load order (lowest priority first, the last mod wins a file conflict), with an on/off switch, drag handle, an info button, a conflict count and a context menu (switch, details, move up/down, remove). |
| Mod details | Name, version, author, description, load position, size, file count, source (file / link / browser), install date, and the file conflicts from `ModLibrary.conflicts()`: which files it overrides in which mod, which are overridden by which, with the paths. Buttons: switch on/off, load later / earlier, remove (asks first). |
| Add mod | Files (`.fileImporter`: zip, 7z, rar, or a folder), Browse (`DownloadsView`: ModDB, GitHub) and a link field (paste, then download with `LinkDownloader`). |
| Import flow | A card on top of everything, for all three sources: *reading* (`inspect`), the warnings the importer found ("Contains Windows DLLs", ...) with *Add anyway* / *Cancel*, the FOMOD wizard (`FomodWizardView`) when the mod has one, *installing* with progress, then *done* (switch on, open Mods) or *failed* with the importer's message. A download that ends up here is deleted afterwards (the mod is stored). |

Everything the launcher changes is written at once through `ModLibrary` (`setEnabled`, `move`, `remove` replace `modlist.json`
atomically), and Play writes the file once more so that it exists and lists every installed mod, also on a first run. Changes apply
at the next engine start; there is no in-process engine restart (iOS cannot relaunch an app).

### Controls

The look follows the game: dark, amber focus ring, the Anomaly logo (the generated app icon, see below). It is console-style, so
everything is reachable from the pad; touch and the Mac's mouse and keyboard work on the same views.

| Input | Pad (DualSense, Xbox) | Keyboard (Mac; hardware keyboard on the iPad) | Touch / mouse |
|---|---|---|---|
| Move focus | D-pad, left stick | arrow keys | tap / hover |
| Select, switch a mod on/off | Cross | Return, Space | tap the row |
| Back | Circle | Esc | the arrow at the top left |
| Details of the focused mod | Triangle | I | the (i) button |
| Reorder | hold Square, then D-pad up/down | Option + arrow up/down | drag the handle |
| Play | Options (home, mods) | P | the Play button |
| Remove | Remove in the details | Delete | context menu, details |

The hint bar at the bottom shows the glyphs of the device last used (pad glyphs, key caps) and hides itself for touch. Focus
moves to the nearest item in the direction pressed (`Focus.swift`), the list scrolls to keep it in view, going back returns to the
item that was open, and the cards that sit on top (import, remove confirmation) take the focus until they are gone. The stick
repeats like the D-pad (after 0.35 s, then every 0.09 s). The system file picker, the in-app browser and the FOMOD wizard are other
views (system, `ModDownloads`, `ModManagerCore`): touch and mouse there, not the pad.

### Handover to the engine

The launcher is hosted in an SDL window that has no renderer and no Metal view, so SwiftUI draws and SDL only supplies the window and
the events: a child view controller of the window's root view controller on iPadOS, a subview of the content view on macOS. The shell
maps SDL gamepad events (and, on the iPad, key events) onto the launcher's buttons; the Mac keyboard is read by the launcher itself
(`NSEvent` local monitor, so a key is never handled twice).

When Play is pressed the launcher only sets its result. `SDL_AppIterate` sees it, calls `anomaly_launcher_close` (the hosting view
controller or view is removed and the model released: timers and tasks stopped), destroys the window, and calls `StartEngine`, which
creates the engine's own window exactly as in a start without the launcher. This happens from the SDL callback, not inside a SwiftUI
action, so the engine's start never runs inside a UIKit or AppKit event handler. Gamepads the launcher opened are closed first; the game's
input layer enumerates the connected pads itself.

The C interface (`src/Platform/Launcher/AnomalyLauncher.h`, implemented with `@_cdecl` in `LauncherEntry.swift`):

| Function | |
|---|---|
| `anomaly_launcher_open(native_window, config_json, data_status, user)` | hosts the launcher in a `UIWindow*` / `NSWindow*` (SDL's window properties); `config_json` carries the mods root, platform, notices, logo path; `data_status` is the callback for the game-data check |
| `anomaly_launcher_result(l)` | 0 running, 1 Play, 2 Quit |
| `anomaly_launcher_button(l, button, down, keyboard)`, `anomaly_launcher_axis(l, axis, value)` | pad and key input |
| `anomaly_launcher_snapshot(l, path)` | PNG of the launcher's view |
| `anomaly_launcher_close(l)` | removes it from the window and releases it |

### When it is shown

| Start | Launcher |
|---|---|
| ordinary start (the icon, `open`, no flags) | shown |
| `--script`, `--exit-after`, `--unattended`, `--no-launcher`, `ANOMALY_NO_LAUNCHER=1` | skipped: straight to the engine, mods as `modlist.json` has them (`--mods-off` mounts none); the sweep, harness and iPad scripts are unchanged |
| `--show-launcher` | shown, whatever else is given |
| `--launcher-script <file>` | shown, also on an unattended run (its window is then hidden) and driven by the script |
| game data missing, not unattended | shown (Home explains it), also with `--no-launcher` |

`--launcher-script` takes the pad commands of the engine script (`padon`, `pad <button> [ms]`, `paddown`, `padup`, `padaxis`, `padoff`),
plus `snapshot <name>` (a PNG into the shell directory), `log` and `quit`; the time is seconds since the launcher came up. The virtual
pad is an SDL virtual joystick, so the launcher sees it as a DualSense. `ANOMALY_LAUNCHER_AUTOPLAY_MS=<n>` presses Play after n ms.

```
# drive the launcher: Mods, switch the second mod, back, Play
1.0 padon
1.5 pad down
2.0 pad cross
2.5 pad down
3.0 pad cross
3.5 snapshot mods
4.0 pad circle
4.5 pad options
```

### Where things are

| | |
|---|---|
| `src/Platform/Launcher/*.swift` | the launcher: `LauncherModel` (state), `Focus` (pad focus), `HomeView`, `ModsView`, `DetailsView`, `AddModView`, `ImportOverlay` + `ImportCoordinator` (the flow), `Components`, `Theme`, `LauncherEntry` (C entry, hosting) |
| `src/Platform/Launcher/Stubs.swift` | stand-ins for `ModManagerCore` / `ModDownloads` while they are not on `main` (each block compiles out when `canImport` finds the real module); deleted when they land |
| `src/Platform/App/main.cpp` | opening, input mapping, script and handover (`OpenLauncher`, `LauncherEvent`, `RunLauncherScript`, `SDL_AppIterate`) |
| `cmake/targets/launcher.cmake`, `src/Platform/Launcher/CMakeLists.txt` | `enable_language(Swift)`, the `anomaly_launcher` static library |
| `tools/launcher_snapshot` | renders every screen offscreen; `--selftest` drives the pad logic |

Build notes: the library is Swift 5 mode, built for the deployment target (CMake does not pass it to `swiftc`), and `anomaly` links with the
C++ driver (`LINKER_LANGUAGE CXX`) because CMake would pick `swiftc` otherwise, which does not take `-fsanitize` and the other C++ link
flags; the Swift runtime's search paths come with the library's link options. The logo is `icon-1024.png` of the generated app icon
(`tools/make_app_icon.sh`: the Anomaly launcher's logo when the game data is there at configure time, the repository's fallback
artwork otherwise), copied into the bundle as `launcher-logo.png`; without it the launcher draws a plain mark. The Anomaly artwork is never
committed.

### Testing

```bash
cmake --build --preset macos-arm64-release --target launcher_snapshot
build/macos-arm64-release/tools/launcher_snapshot/launcher_snapshot build/launcher-shots --logo build/macos-arm64-release/app_icon/icon-1024.png
build/macos-arm64-release/tools/launcher_snapshot/launcher_snapshot --selftest build/launcher-selftest
```

The first renders the screens with `ImageRenderer` (no window; a made-up mods root with invented mods). The second presses the pad's
buttons the way the shell does and checks the focus, the reordering, the confirmation cards, the import warning and what Play writes.
