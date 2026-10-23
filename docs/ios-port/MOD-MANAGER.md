# Mod manager

A mod manager inside the Anomaly app for the iPad and the Mac: a SwiftUI launcher is shown first, mods are stored
apart from the game and mounted at engine start in load order, Mod Organizer 2 style, and nothing in the game folder is
overwritten. Four sessions built it: engine mounting, import and FOMOD, downloads and links, and the launcher. This file has
one section per part, each written by the session that built it.

## Launcher

The launcher is the first screen of an ordinary start, on the iPad and on the Mac: native SwiftUI, shown before the engine exists,
with **Play**, **Mods** and **Add Mod**. Play writes `modlist.json` and continues into the engine in the same process. Code:
`src/Platform/Launcher`.

```
SDL_AppInit ─ SDL_Init, AppleShell::Init ─ launcher? ─ yes ─▶ SDL window (no renderer) + SwiftUI hosted in it
                                                    └─ no ──▶ xr_engine::Init   (--script, --exit-after, --unattended, --no-launcher)
SDL_AppIterate: AppleShell::Update, poll anomaly_launcher_result
    Play ─▶ anomaly_launcher_close (views leave the window) ─▶ SDL_DestroyWindow ─▶ StartEngine (the engine makes its own window)
    Quit ─▶ SDL_APP_SUCCESS
```

### Look

It is a system app, not a themed one: the stock SwiftUI controls, so that it looks like iPadOS 27 (and macOS) and follows what the
system does. A `NavigationStack` with large titles and the back button, inset-grouped lists (a grouped `Form` on the Mac), system
switches, `LabeledContent`, `ContentUnavailableView` for empty and result states, disclosure groups for the file conflicts, swipe
actions and context menus, system sheets for the import flow and the remove confirmation, SF Symbols, Dynamic Type text styles and
semantic colors (light and dark follow the system). The primary buttons (Play, Add Anyway) use the Liquid Glass styles on iPadOS and
macOS 26 and later, and the bordered-prominent style before. The only custom things are the amber accent of the game's menus and the
Anomaly logo on the home screen: the generated app icon (`tools/make_app_icon.sh`: the logo of `AnomalyLauncher.exe` when the game
data was there at configure time, the repository's fallback artwork otherwise), copied into the bundle as `launcher-logo.png`; a
plain mark is drawn without it. The Anomaly artwork is never committed.

### Screens

| Screen | What it does |
|---|---|
| Home | The logo, **Play** (disabled until the game data is valid) with the number of mods that are on, **Mods** (n of m on), **Add Mod**, Quit (Mac). When the game data is missing or incomplete it shows the shell's check (`ValidateGameData`) with the how-to-copy text and a Rescan, and looks again by itself every 2 s, so a copy in progress shows up live. This replaces the old debug-text launcher, which stays in the shell for `shell_lab` and as the fallback when the SwiftUI one cannot be hosted. A previous crash is shown as a notice. |
| Mods | The load order, first (lowest priority) to last (wins a file conflict): number, name, version, size and file count, a conflict count, an info button and a switch. Drag to reorder (Edit on the iPad, direct on the Mac), swipe or context menu to remove, tap the row for the details. |
| Mod details | About, version, author, size, files, load position, source (file, link or browser), date added, and the file conflicts from `ModLibrary.conflicts()`: which files it overrides in which mod and which are overridden by which, with the paths. Switch on/off, load later / earlier, remove (asks first). |
| Add Mod | Files (`.fileImporter`: zip, 7z, rar, or a folder), Browse (`DownloadsView`: ModDB, GitHub) and a link field with Paste and Download (`LinkDownloader`). |
| Import sheet | For all three sources: *reading* (`inspect`), the warnings the importer found ("Contains Windows DLLs", ...) with *Add Anyway* / *Cancel*, the FOMOD wizard (`FomodWizardView`) when the mod has one, *installing* with progress, then *added* (switch on, open Mods) or *failed* with the importer's message. A download that ends up here is deleted afterwards (the mod is stored). |

Everything the launcher changes goes through `ModLibrary` at once (`setEnabled`, `move`, `remove` replace `modlist.json` atomically);
Play writes the file once more so that it exists and lists every installed mod, also on a first run. Changes apply at the next engine
start; there is no in-process engine restart (iOS cannot relaunch an app).

### Controls

Touch, and the mouse and keyboard on the Mac, work on the stock controls as on any app. On top of that the whole launcher can be
driven from a pad: a layer that knows the reachable rows of the screen (the model builds them: `LauncherModel.focusItems`), moves a
focus ring along them and runs the row's action. It is data-driven, so it does not depend on what the views draw, and the cards that
sit on top (the import sheet, the remove confirmation) take the focus until they are gone.

| Input | Pad (DualSense, Xbox) | Keyboard (Mac; hardware keyboard on the iPad) | Touch / mouse |
|---|---|---|---|
| Move focus | D-pad up/down, left stick | arrow keys | tap / click |
| Select, switch a mod on/off | Cross | Return, Space | tap the row's control |
| Back | Circle | Esc | back button, swipe from the edge |
| Details of the focused mod | Triangle | I | tap the row |
| Reorder | hold Square, then D-pad up/down | Option + arrow up/down | Edit, drag the handle |
| Play | Options (home, mods) | P | the Play button |
| Remove | Remove in the details | Delete | swipe, context menu |

The focus ring and the hint bar at the bottom (pad glyphs or key caps, for what the buttons do on this screen) appear when a pad or the
keyboard is used and go away at the next touch or click, so the touch and mouse UI stays the plain system one. The list scrolls to the
focused row, going back returns to the row that was open, and the stick repeats like the D-pad (after 0.35 s, then every 0.09 s). The system
file picker and the in-app browser are other views: touch there, not the pad. [The FOMOD wizard takes the pad through its own session; see below.]

### Handover to the engine

The launcher is hosted in an SDL window that has no renderer and no Metal view, so SwiftUI draws and SDL only supplies the window and
the events: a child view controller of the window's root view controller on iPadOS, a subview of the content view on macOS. The shell
maps SDL gamepad events (and, on the iPad, key events) onto the launcher's buttons; the Mac keyboard is read by the launcher itself
(`NSEvent` local monitor, so a key is never handled twice).

When Play is pressed the launcher only sets its result. `SDL_AppIterate` sees it, calls `anomaly_launcher_close` (the hosting view
controller or view is removed and the model released: timers and tasks stopped), destroys the window, and calls `StartEngine`, which
creates the engine's own window exactly as in a start without the launcher. This happens from the SDL callback, not inside a SwiftUI
action, so the engine's start never runs inside a UIKit or AppKit event handler. Gamepads the launcher opened are closed first (and the
background-events hint it set is reset); the game's input layer enumerates the connected pads itself.

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
| `--launcher-script <file>` | shown, also on an unattended run (its window is then hidden), driven by the script |
| game data missing, not unattended | shown (Home explains it), also with `--no-launcher` |

`--launcher-script` takes the pad commands of the engine script (`padon`, `pad <button> [ms]`, `paddown`, `padup`, `padaxis`, `padoff`),
plus `snapshot <name>` (a PNG of the launcher into the shell directory), `log` and `quit`; the time is seconds since the launcher came up.
The virtual pad is an SDL virtual joystick, so the launcher sees it as a DualSense. `ANOMALY_LAUNCHER_AUTOPLAY_MS=<n>` presses Play after
n ms.

```
# drive the launcher: Mods, switch the second mod, reorder it, details, back, Play
1.5 padon
2.5 snapshot 01-home
3.0 pad down
3.5 pad cross
4.5 snapshot 02-mods
5.0 pad down
5.5 pad cross
6.5 paddown square
7.0 pad down
7.5 padup square
8.5 pad triangle
10.0 pad circle
10.5 pad circle
11.5 pad options
```

### Where things are

| | |
|---|---|
| `src/Platform/Launcher/*.swift` | `LauncherModel` (state, and the rows the pad can reach), `Focus` (pad layer), `HomeView`, `ModsView`, `DetailsView`, `AddModView`, `ImportOverlay` (the sheets) + `ImportCoordinator` (the flow), `ScreenChrome`, `Components`, `Theme`, `LauncherEntry` (C entry, hosting) |
| `src/Platform/Launcher/Stubs.swift` | stand-ins for `ModManagerCore` / `ModDownloads` while they are not on `main` (each block compiles out when `canImport` finds the real module); deleted when they land |
| `src/Platform/App/main.cpp` | opening, input mapping, script and handover (`OpenLauncher`, `LauncherEvent`, `RunLauncherScript`, `SDL_AppIterate`) |
| `cmake/targets/launcher.cmake`, `src/Platform/Launcher/CMakeLists.txt` | `enable_language(Swift)`, the `anomaly_launcher` static library |
| `tools/launcher_snapshot` | renders every screen offscreen; `--selftest` drives the pad logic; `--fixtures` makes a mods root of invented mods |

Build notes: the library is Swift 5 mode, built for the deployment target (CMake does not pass it to `swiftc`), and `anomaly` links with the
C++ driver (`LINKER_LANGUAGE CXX`) because CMake would pick `swiftc` otherwise, which does not take `-fsanitize` and the other C++ link
flags; the Swift runtime's search paths come with the library's link options.

### Testing

```bash
cmake --build --preset macos-arm64-release --target launcher_snapshot
T=build/macos-arm64-release/tools/launcher_snapshot/launcher_snapshot
$T build/launcher-shots --logo build/macos-arm64-release/app_icon/icon-1024.png   # the screens as the Mac draws them
$T --selftest build/launcher-selftest                                              # pad focus, reordering, sheets, what Play writes
```

The first hosts the real views in a window that is never shown and draws them into a bitmap (the Mac's controls; the iPad's look
comes from the device, below). The second presses the pad's buttons the way the shell does and checks the focus, the reordering, the
confirmation sheet, the import warning and what Play writes.

On the Mac the whole path, launcher, pad script, Play, engine, exit, runs without a window:

```bash
T=build/macos-arm64-release/tools/launcher_snapshot/launcher_snapshot; $T --fixtures /tmp/state/Mods
open -g -j -W -n --env ANOMALY_STATE_ROOT=/tmp/state build/macos-arm64-release/src/Platform/App/anomaly.app \
  --args --launcher-script launcher.txt --exit-after 10 -nosound
```

(`--launcher-script` on an unattended run keeps the launcher's window hidden; `snapshot` still draws it.) On the iPad, `tools/ios/ipad.sh
put launcher.txt launcher.txt` and `tools/ios/ipad.sh start --launcher-script launcher.txt` do the same on the device, and the snapshots come
back with `tools/ios/ipad.sh logs`.
