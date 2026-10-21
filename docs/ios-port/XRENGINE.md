# xrEngine on Apple platforms

`src/xrEngine` builds as a static library (`xrEngine`, `cmake/targets/xrEngine.cmake`) for macOS and iOS/iPadOS, arm64
only, behind `-DANOMALY_BUILD_XRENGINE=ON`. The Windows project (`xrEngine.vcxproj`, `WinMain`) is unchanged: every port
change sits behind `XR_PLATFORM_APPLE`, and the Windows code stays in the same files. This page covers the core of the
engine: the window and device, the entry point, and what the rest of the module needed. Input (`Xr_input.cpp` & co.) and
the media decoders belong to the engine-input session, the renderer to renderer-r4.

```bash
cmake --preset macos-arm64 -DANOMALY_BUILD_XRENGINE=ON
cmake --build --preset macos-arm64 --target xrEngine
# the smoke test (needs the game data, see below)
cmake --build --preset macos-arm64 --target xrengine_smoke
build/macos-arm64/tools/xrengine_smoke/xrengine_smoke --data-root game/Anomaly --exit-after 10
# iOS: archive only, no signing
cmake --preset ios-arm64 -DANOMALY_BUILD_XRENGINE=ON
xcodebuild -project build/ios-arm64/AnomalyPort.xcodeproj -scheme xrEngine -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO build
```

## What is in the build

83 sources: the 105 `.cpp` of the project, minus the 14 Discord files (below), minus `Text_Console*.cpp` (a Win32 console
window, dedicated server only), minus the 8 engine-input files that do not compile yet (below), plus
`xr_window_sdl.cpp` and `xr_platform_stubs_apple.cpp`. It also builds two third-party libraries the engine needs and
the Windows solution has projects for: `xrImGui` (Dear ImGui without its DX backends) and `giflib`.

| Option / variable | Effect |
|---|---|
| `ANOMALY_XRENGINE_DISCORD` (OFF) | compiles the Discord Rich Presence glue (`3rd party/discord`) and defines `XR_USE_DISCORD`. Off: `discord_gameinfo`, `discord_strings`, `rsDiscord`, `updateDiscordPresence()` and `xr_ToUTF8()` (CP1251 to UTF-8 through the shim, no ICU) stay, so the game's script exports still link. |
| `XRENGINE_INPUT_PORTED` (derived) | ON when `src/xrEngine/xr_input_sdl.cpp` exists, i.e. when the engine-input port has landed. OFF: `Xr_input.cpp`, `IInputReceiver.cpp`, `line_edit_control.cpp`, `edit_actions.cpp`, `imgui_base_input.cpp`, `MonitorList.cpp`, `tntQAVI.cpp` and `xr_input_xinput.cpp` are left out of the library (they are written against DirectInput, VFW and the Win32 monitor API). It flips by itself; nobody edits `xrEngine.cmake`. |
| `cmake/targets/xrEngine_input.cmake` | the engine-input session's fragment. The root glob includes it after `xrEngine.cmake`, so it can `target_sources(xrEngine ...)` and `target_link_libraries(xrEngine ...)` directly. |

`stdafx.h` includes `Common/Platform.h` first and no longer defines `INGAME_EDITOR` in non-`NDEBUG` builds on Apple (that
is `editor.dll`, Windows only). The `-DANOMALY_ENGINE_DEBUG=ON` flavour (`DEBUG` defined) builds too. `OutOfVirtError.RES`, `resource.rc` and the splash bitmaps are not compiled.

## The window and the device

The engine window is an SDL3 window and **`HWND` is its `SDL_Window*`** (`CRenderDevice::m_hWnd`). Code that only passes
the handle around is unchanged. Everything lives in `xr_window.h` / `xr_window_sdl.cpp`.

```
SDL_Init(VIDEO)                         xr_engine::Init -> InitEngine() -> Device.Initialize()
  -> xr_window::Create()                  SDL_CreateWindow(METAL | HIGH_PIXEL_DENSITY), hidden on macOS,
                                          the whole screen on iPadOS (landscape only)
CRenderDevice::Create()                 -> xr_window::SetMode(g_screenmode, psCurrentVidMode, startup monitor)
renderer: CreateSwapChainForHwnd(Device.m_hWnd)
                                        -> xr_window::MetalLayer() made the SDL Metal view on demand and registered
                                          the CAMetalLayer with the D3D11 shim under that handle (RegisterWindowLayer)
```

Screen modes (`g_screenmode`, console `screenmode`): `windowed` is a resizable Mac window sized to the render resolution
(1 render pixel per window pixel); `borderless` and `fullscreen` are both a desktop-size fullscreen window (SDL
borderless fullscreen, no display mode change; the Metal layer scales the render resolution to the display). On iPadOS the
mode is ignored: always the whole screen. A user resize of the Mac window changes `psCurrentVidMode` and queues
`Device.Reset()` for the next frame.

Units. SDL reports points; the iPad is 2x. Everything Win32-shaped is in **pixels of the drawable**, and "screen"
coordinates are the window's client coordinates (a window covers its own screen on iPadOS, and the engine only uses
screen coordinates in pairs with client ones). `Device.clientWidth/Height` come from `GetClientRect`, so the UI cursor
scaling (`UI_BASE_WIDTH / clientWidth`) works as it does on a Windows borderless window whose back buffer is a different
size. The default render resolution (`psCurrentVidMode`, when unset) is the display's native pixel size, 2732x2048 on
the iPad Air.

### Win32 names that exist on Apple

`xr_window.h` declares these (all `ENGINE_API`, so xrGame and the renderer link them from xrEngine) so the 100-odd call
sites written against Win32 compile unchanged. Use `xr_window::` directly in new code.

| Win32 name | Behaviour |
|---|---|
| `GetClientRect`, `GetWindowRect` | `{0, 0, drawable width, drawable height}` in pixels |
| `ClientToScreen`, `ScreenToClient`, `MapWindowPoints` | identity (same space) |
| `GetCursorPos`, `SetCursorPos` | `SDL_GetMouseState` / `SDL_WarpMouseInWindow`, in pixels |
| `ClipCursor` | `SDL_SetWindowMouseRect` (null releases) |
| `ShowCursor(BOOL)` | `SDL_ShowCursor` / `SDL_HideCursor`; a flag, not Win32's counter |
| `IsIconic`, `SetForegroundWindow`, `SetActiveWindow` | `SDL_WINDOW_MINIMIZED` / `SDL_RaiseWindow` |
| `HMONITOR`, `MonitorFromPoint`, `MonitorFromWindow`, `GetMonitorInfoA`, `MONITORINFO` | an `SDL_DisplayID` stored in a pointer (`xr_monitor_from_display` / `xr_display_from_monitor`); the monitor rectangle is in pixels. `MonitorFromPoint` returns the window's display: there is no global desktop space to hit-test. |

Not provided, because nothing outside the removed code uses it: `WNDCLASS`, `CreateWindowEx`, `SetWindowLong(Ptr)`,
`SetWindowPos`, `WM_*`, `DefWindowProc`. `GetMonitorResolution`, `GetMonitorPosition`, `GetStartupMonitor` (in
`device.cpp`) are SDL-based; the monitor list for the `vid_monitor` console command (`MonitorList.cpp`) is engine-input's.

### Events

The engine is the single consumer of the SDL queue. `xr_window::PumpEvents()` runs once per frame (from
`CRenderDevice::RunFrame`) and from `CApplication::LoadDraw`, so the queue keeps moving during the multi-second level
loads. Every event goes first to the handlers registered with `xr_window::AddEventHandler(fn, user)` (input registers
one; it must not call `SDL_PollEvent` itself), then `CRenderDevice::on_sdl_event` (`Device_wndproc.cpp`, the counterpart
of `WndProc`) reacts:

| SDL | Engine |
|---|---|
| `SDL_EVENT_QUIT`, `WINDOW_CLOSE_REQUESTED` | `Engine.Event.Defer("KERNEL:disconnect")`, `("KERNEL:quit")`; `CApplication::OnEvent` then calls `xr_window::RequestQuit()` (was `PostQuitMessage`) |
| `FOCUS_GAINED/LOST`, `MINIMIZED`, `RESTORED`, `SHOWN`, `HIDDEN` | `SyncActiveState()`: `OnAppActive(focused && visible && !system_paused)` (the body of `OnWM_Activate`: `seqAppActivate/Deactivate`, cursor, clipping). On iPadOS focus is taken as given; the shell's pause is the authority. `Init` calls it once after the device exists, because the window's first focus event predates the device. |
| `PIXEL_SIZE_CHANGED` (windowed Mac window) | new `psCurrentVidMode`, `Device.Reset()` next frame |
| `WINDOW_DISPLAY_CHANGED`, `DISPLAY_ADDED/REMOVED/MOVED/CURRENT_MODE_CHANGED` | `g_monitor_list_dirty = 1` |

## The entry point

`xr_engine_app.h` is the replacement of `WinMain`/`WinMain_impl` (`x_ray_apple.inc`, included by `x_ray.cpp`). The
app owns `main`; with SDL3 callbacks it is a few lines (`tools/xrengine_smoke/main.cpp` is a working example):

```cpp
SDL_AppInit:    SDL_Init(VIDEO | EVENTS); AppleShell::Init({argc, argv});
                // the launcher until the user presses Start, if the app has one
                xr_engine::Init(params, error)      // returns once the first frame can run
SDL_AppIterate: xr_engine::Frame() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS
SDL_AppEvent:   nothing (Frame() pumps the queue)
SDL_AppQuit:    xr_engine::Shutdown(); AppleShell::Shutdown()
```

`Init` does what `WinMain_impl` did, in the same order, minus the Windows-only parts:

1. checks `AppleShell::IsInitialized()` and `ValidateGameData(Paths::data_root)`; anything but `Ok` returns
   `GameDataInvalid` with the shell's one-line summary (the launcher shows the details);
2. builds the command line: `argv` (minus the program name and the shell's `--data-root <path>`), then
   `commandline.txt` (iPad: `Documents/commandline.txt`, visible in the Files app; macOS: `~/Library/Application Support/Anomaly/commandline.txt`;
   one or more lines, `#` starts a comment line), or `InitParams::extra_command_line` instead of the file;
3. `DllMainXrPhysics(PROCESS_ATTACH)` (ODE allocator hooks), `Debug._initialize`, `compute_build_id`,
   `Core._initialize("xray", ..., Paths::fsgame_ltx)`: the file system runs on the shell's generated `fsgame.ltx`, with
   `$fs_root$` the data root and `$app_data_root$`, `$logs$`, saves and `user.ltx` in `Library/Application Support`;
   `Core.Params` is then replaced with the command line (`TODO(shim)`: `xr_set_command_line`, see below);
4. `InitSettings` (system.ltx, game.ltx), `FPU::m24r`, `InitEngine` (`Engine.Initialize` and `Device.Initialize`, which
   creates the window), `InitInput`, `InitConsole`;
5. `CreateRendererList`, the `renderer` line of `user.ltx`, `Engine.External.Initialize` (the renderer and the game are
   static libraries; `EngineAPI.cpp` calls `DllMainXrRenderR4` / `DllMainXrGame` directly, `LoadLibrary` is not used);
6. `Startup_Create` (the half of `Startup()` before the main loop: sound, `user.ltx`, `Device.Create`, application,
   game persistent object, spatial DBs) and `Device.RunBegin` (threads, `seqAppStart`).

`Frame()` is `AppleShell::Update()` (memory-budget watchdog), `xr_window::PumpEvents()`, one `CRenderDevice::on_idle()`
(a pending resize first), and returns false once the engine has asked to quit. `Shutdown()` is `RunEnd`, the rest of
`Startup()` (`Startup_Destroy`), `Core._destroy()` and destroying the window. `Startup()` for Windows is
`Startup_Create(); Device.Run(); Startup_Destroy();`, and `Device.Run()` is `RunBegin(); message_loop(); RunEnd();`.

`InitParams::stop_before_renderer` stops after the console and `user.ltx` and shows the window at 1280x720 points: the
state the smoke test uses while the other modules do not link.

### Lifecycle, memory, thermal

`Init` registers one `AppleShell::Listener`:

| Shell callback | Engine |
|---|---|
| `on_pause` (WillEnterBackground, which on iPadOS is also Control Center, system alerts and app-switcher peeks) | `CRenderDevice::OnSystemPause(true)` -> `OnAppActive(false)`: `b_is_Active` is off (so `on_idle` renders nothing, and no GPU work is issued), `seqAppDeactivate` runs (the game pauses its timers and sound emitters), the cursor is released |
| `on_resume` (DidEnterForeground) | `OnSystemPause(false)` -> `OnAppActive(true)` |
| `on_memory` Warning / Critical | `CRenderDevice::OnMemoryPressure(1 / 2)`: logs and `Memory.mem_compact()`. Dropping renderer caches and lowering texture LOD is the renderer's to hook in later (M2). |
| `on_thermal`, `on_low_power` | logged only |

Keep-awake is the shell's (`SetKeepScreenAwake` is on after `Init`, released while paused). Level loads call
`PumpEvents()` per load stage, so lifecycle events are not stuck behind a 10 second load.

### What changed in the other files

| File | Change |
|---|---|
| `device.cpp` / `device.h` | `Run` split into `RunBegin`/`message_loop`/`RunEnd`; `RunFrame`; `OnWM_Activate` split into the Win32 shell and `OnAppActive(bool)`; monitor functions over SDL displays; `GetMonitorRefresh` from the display mode; Discord thread behind `XR_USE_DISCORD`; `WINDOWINFO` check replaced by `b_is_Active` |
| `Device_Initialize.cpp`, `Device_create.cpp`, `Device_destroy.cpp` | window creation, `SetWindowLongPtr/SetWindowPos` replaced by `xr_window::SetMode` |
| `Device_wndproc.cpp` | `WndProc` stays for Windows; `on_sdl_event`, `OnSystemPause`, `OnMemoryPressure` for Apple |
| `x_ray.cpp` | Discord/ICU behind `XR_USE_DISCORD`; `Startup()` split; `WinMain`, the splash dialog, `damn_keys_filter` (sticky keys, screensaver), Optimus exports, the stack-overflow SEH filter and the single-instance mutex are Windows only; `PostQuitMessage` -> `RequestQuit`; `LoadDraw` pumps events |
| `Engine.cpp`, `EngineAPI.cpp` | unchanged: the PSGP pipeline (`xrBind_PSGP`, `ttapi_Done`) and the renderer/game entry points are already static calls |
| `Environment*.cpp`, `xrSheduler.h`, `xr_object.cpp`, `imgui_base.cpp`, `pure_relcase.h` | clang-only fixes (const heap comparator, `nullptr` for `false` as a `CInifile*`, `IUnknown` from the shim's base header, `volatile LONG*` for the interlocked compare, a named iterator, a C-style cast to a private derived class) |
| not compiled | `Text_Console*.cpp`, the in-game editor (`INGAME_EDITOR`), the Discord SDK glue, the debug-build mailslot console (`mailSlot.cpp`, `msParse`; behind `XR_USE_MAILSLOT`, i.e. `DEBUG` on Windows only). OpenAutomate (`xrSASH.cpp`) is compiled but `-openautomate` is not a flag of the Apple entry point. |

ReShade is a Windows add-on (`src/ReShadeCompat`); `xr_platform_stubs_apple.cpp` defines `init_reshade()`,
`unregister_reshade()` and `render_reshade_effects()` as no-ops, and `use_reshade` stays false.

## The renderer and game boundary

`EngineAPI.cpp` is already static (`STATIC_RENDERER_R4`). From xrEngine's side, a renderer has to provide:

- `BOOL DllMainXrRenderR4(HANDLE, DWORD, LPVOID)` and `extern "C" bool SupportsDX11Rendering()`;
- `dxRenderFactory RenderFactoryImpl` (`Layers/xrRender/dxRenderFactory.cpp`): xrAPI's `RenderFactory` points at it, so
  even the console's UI shader goes through it. `CRenderDevice::ConnectToRender()` then calls
  `RenderFactory->CreateRenderDeviceRender()`;
- the console variables the engine reads: `psDeviceFlags2`, `ps_ssfx_grass_interactive`, `ps_ssfx_wind_trees`;
- the window: `Device.m_hWnd` is the `SDL_Window*`, its layer is already registered with the shim, so the unmodified
  `CHW::CreateDevice(m_hWnd)` -> `CreateSwapChainForHwnd(device, m_hWnd, ...)` works. `Win32` window calls in
  `HW.cpp`/`dx10HW.cpp` (`SetWindowLong`, `SetWindowPos`, `ShowWindow`, `MonitorFromWindow`) are the renderer session's to
  replace; `xr_window::SetMode` is the call for screen modes and `GetClientRect`/`ClientToScreen`/`ClipCursor` exist.
  `CHW::OnAppDeactivate` minimising the window is wrong on iPadOS: the engine already stops rendering.

From the game: `DllMainXrGame`, `xrFactory_Create/Destroy`, `updateCurrentScope()`, `g_ai_unlimited_ammo`,
`g_crosshair_color`, `g_dispersion_base`, `g_dispersion_factor`, `g_fTimeFactor`. From xrPhysics: `DllMainXrPhysics`.

## Smoke test: `tools/xrengine_smoke` (macOS)

Links xrCore, xrEngine and the Apple shell; `xr_engine::Init` runs with `stop_before_renderer`. It then draws a changing
colour into the engine window through the D3D11-on-Metal shim (`D3D11CreateDevice`, then
`CreateSwapChainForHwnd(device, xr_engine::WindowHandle(), ...)`: no `RegisterWindowLayer` call in the tool, the engine
registered the layer), until the window is closed or `--exit-after <s>` elapses. `--full` runs the whole `Init` and needs
every module; with the renderer stubbed it stops, as expected, at `CreateRendererList`: "No valid renderer found".
Output lines start with `xrengine_smoke:`; the exit code is 0 when all checks passed.

Checked on the Mac (M2 Pro, macOS 27.0.1), game data `game/Anomaly`, with xrCDB, xrXMLParser and xrAPI linked for real:

| Stage | Result |
|---|---|
| SDL3, `AppleShell::Init`, `ValidateGameData` | pass |
| `Core._initialize` on the generated `fsgame.ltx` | 67 archives mounted, 50,876 files cached, 0.65 s |
| `system.ltx`, `game.ltx` | loaded (`pSettings`) |
| `Engine.Initialize`, `Device.Initialize`, window | window created, 2560x1440 px at density 2.0, then shown at 1280x720 points |
| `InitInput`, `InitConsole`, `user.ltx` | pass (input is the stub below; no `user.ltx` in the fresh appdata; `default_controls` is an xrGame command, so the console logs "Unknown command" for it) |
| Metal layer, swap chain from the engine's `HWND`, clear and present | pass; 478 frames in 4.0 s |
| Shutdown | clean: `Engine.Destroy`, `Core._destroy`, window destroyed |

The shell has no way to move the appdata, so the tool writes the generated `fsgame.ltx`, the engine log and the shell
log into the real `~/Library/Application Support/Anomaly` and `~/Library/Logs/Anomaly`. Stop mode never saves `user.ltx`.

### Stubs, and the symbols they stand in for

xrEngine's link line names every module, and most are still being ported, so `tools/xrengine_smoke` links whichever
modules are enabled (`-DANOMALY_BUILD_XRCDB=ON` ...) and takes the rest from `tools/xrengine_smoke/stubs/stubs_<target>.cpp`,
each compiled only while that target does not exist. A module that lands drops its stub out of the build with no edit.
Unresolved symbols with only xrCDB, xrXMLParser and xrAPI real (141), by owner:

| Owner | Count | Symbols |
|---|---|---|
| engine-input (xrEngine files) | 52 | `CInput` (ctor, dtor, `get_exclusive_mode`, vtable), `pInput`, `psMouseSens`, `psMouseSensVerticalK`, `psMouseInvert`; `IInputReceiver::IR_Capture/IR_Release/IR_OnActivate/IR_OnDeactivate` (+ vtable, typeinfo); `text_editor::line_edit_control` (ctor, dtor, `on_key_press/hold/release`, `on_frame`, `clear_states`, `assign_callback`, `set_edit`), `text_editor::remove_spaces`, `split_cmd`; `xr_imgui::ide` input half (`EnableInput`, `OnAppActivate/Deactivate`, `IR_Capture/Release`, `IR_OnMouse*`, `IR_OnKeyboard*`, 10 thunks); `fill_vid_monitor_list`, `free_vid_monitor_list`, `refresh_vid_monitor_list`, `ResolveSelectedMonitor`, `vid_monitor_name`, `vid_monitor_token`, `g_monitor_list_dirty` |
| xrSound | 41 | `CSound_manager_interface::_create/_destroy`, `Sound`, `soundSmoothingParams::*` (7), `snd_device_name`, `snd_devices_token`, `snd_efx_environment_change_time`, `reverb_overwrite`, 22 `psReverb*`, `psSoundFlags`, `psSoundTargets`, `psSoundCacheSizeMB`, `psSoundOcclusionScale`, `psSoundVEffects`, `psSoundVMusic` |
| renderer R4 | 34 | `DllMainXrRenderR4`, `SupportsDX11Rendering`, `RenderFactoryImpl`, 28 `dxRenderFactory::Create*/Destroy*`, `psDeviceFlags2`, `ps_ssfx_grass_interactive`, `ps_ssfx_wind_trees` |
| xrGame | 9 | `DllMainXrGame`, `xrFactory_Create`, `xrFactory_Destroy`, `updateCurrentScope`, `g_ai_unlimited_ammo`, `g_crosshair_color`, `g_dispersion_base`, `g_dispersion_factor`, `g_fTimeFactor` |
| xrNetServer | 2 | `allow_to_include_path`, `fill_auth_check_params` |
| xrCPU_Pipe | 2 | `xrBind_PSGP`, `ttapi_Done` |
| xrPhysics | 1 | `DllMainXrPhysics` |

At the time of writing (main at `6f73fdd5`) xrSound, xrNetServer, xrPhysics, xrCPU_Pipe (and xrParticles, which needs it) and
lua-extensions did not compile yet, which is why they are stubbed; xrCDB, xrXMLParser, xrAPI, ImGui, giflib, luabind and
the shell do.

## Requests and known issues for other modules

- **Shim** (appended to `shim-requests.md`): `void xr_set_command_line(const char*)` so `Core.Params`, xrDebug and
  `memory_monitor` all see `commandline.txt`; `REFIID` / `IID` (`xrNetServer/net_shared.h:61`). `x_ray.cpp` carries the
  `REFIID` typedef locally until then.
- **xrServerEntities** (`object_destroyer.h:150`, `CHelper4<T>::template delete_data<...>`): needed by `Environment_misc.cpp` and
  `xr_efflensflare.cpp`; the xrgame-triage session has the fix in its tree.
- **engine-input**: the contract in the Events and Win32 tables above; `g_monitor_list_dirty` should be `volatile LONG`
  (`volatile long` is 64 bits on Apple and `InterlockedExchange` takes `LONG*`).
- **xrCDB `ispatial.h:151`**: `ptrt(children[i])` truncates pointers to `u32` (clang warns `-Wpointer-to-int-cast`); the
  `_empty()` test can misreport a node whose child pointers all have zero low 32 bits.
- **xrCore**: `AppleShell`'s crash handler and `xrDebug`'s (`xrDebug_posix.cpp`) both install signal handlers; the one
  installed later wins. Not checked here.
