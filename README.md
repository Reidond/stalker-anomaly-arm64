# STALKER Anomaly for iPad

A native port of [S.T.A.L.K.E.R. Anomaly](https://www.moddb.com/mods/stalker-anomaly) 1.5.3 to
iPadOS and macOS on Apple Silicon, playable with a PS5 DualSense controller. The target device is
an iPad Air 13" (M3, 8 GB, iPadOS 27).

This is a native port, not emulation. The Windows engine (Monolith, the X-Ray fork Anomaly runs on)
is rebuilt for arm64:
- Metal replaces DirectX.
- SDL3 replaces Win32 and DirectInput.
- LuaJIT 2.1 replaces LuaJIT 2.0.

The repo holds source code only. You need your own copy of the Anomaly 1.5.3 game files, which are
free from ModDB.

> **Status: not playable yet.** The foundations are in place and verified on both devices: the core
> library, physics, scripting, shader translation, the Metal graphics layer, controls and the app
> shell. The rest of the engine and the game code are being ported now.

## Progress

| Milestone | Goal | State |
|---|---|---|
| M0 | Build system, dependencies, platform probe | Done. Runs on the Mac and the iPad. |
| M1 | Engine runs headless on macOS | In progress. Ported so far: xrCore (reads the real `.db` archives), luabind and ODE. The remaining modules are next ([M1-AUDIT](docs/ios-port/M1-AUDIT.md)). |
| M2 | Metal renderer | Groundwork done. 98% of Anomaly's shaders translate to Metal, and the D3D11 calls the renderer needs work on Metal. Porting the renderer itself is next. |
| M3 | DualSense controls | Controller layer done and checked on the iPad. Not yet hooked into the engine. |
| M4 | iPad app | App shell done, and the game data copies to the iPad. Packaging the engine into it is pending. |
| M5 | Polish | LuaJIT's JIT works on the iPad when launched through a debugger; otherwise it falls back to the interpreter. |

## How it works

| Area | Approach | Doc |
|---|---|---|
| Engine | `src/`, the [xray-monolith](https://github.com/themrdemonized/xray-monolith) engine ("Modded Exes"), hard-forked. Port changes sit behind platform guards (`XR_PLATFORM_APPLE`), so the Windows code stays intact. | [XRCORE.md](docs/ios-port/XRCORE.md) |
| Graphics | Anomaly's DX11 renderer is kept and runs on a D3D11 subset implemented on Metal (`src/Platform/MetalD3D11`). | [METAL-SHIM.md](docs/ios-port/METAL-SHIM.md) |
| Shaders | Translated at runtime with an on-disk cache, so addon shaders keep working (`src/Platform/ShaderXlat`). The pipeline is HLSL → vkd3d-shader → SPIR-V → SPIRV-Cross → MSL. | [SHADERS.md](docs/ios-port/SHADERS.md) |
| Controls | `src/Platform/GameInput`, over SDL3 and GameController. | [CONTROLS.md](docs/ios-port/CONTROLS.md) |
| App shell | `src/Platform/Apple`: paths, game-data checks, lifecycle, memory/thermal callbacks, launcher. | [APP-SHELL.md](docs/ios-port/APP-SHELL.md) |
| Scripts | LuaJIT 2.1. JIT on the Mac; interpreter on the iPad, or JIT through SideStore + StikDebug. | [IPAD-JIT.md](docs/ios-port/IPAD-JIT.md) |
| Audio and video | OpenAL Soft on CoreAudio; Ogg, Vorbis and Theora. | |

Controls are about more than buttons:
- analog movement, and right-stick look with a response curve;
- optional gyro aim;
- a virtual cursor (stick or touchpad) for inventory, PDA and dialogs;
- radial menus for Anomaly's many hotkeys;
- rumble, a light bar showing health, and per-weapon adaptive triggers.

Buttons can be rebound in the stock Controls menu.

The full plan, design decisions and risks are in [PLAN.md](docs/ios-port/PLAN.md).

## Repository layout

```
src/                 engine (xrCore, xrEngine, xrGame, renderers, third-party libs)
src/Common/          shared Windows-compat layer for the Apple build (Platform.h)
src/Platform/        port-only code: Apple shell, GameInput, MetalD3D11, ShaderXlat
external/            pinned submodules: SDL3, OpenAL Soft, LuaJIT 2.1, Ogg/Vorbis/Theora,
                     SPIRV-Cross, vkd3d
cmake/               build helpers and dependency recipes (incl. LuaJIT iPad-JIT patches)
tools/               smoke tests and lab apps (platform_probe, xrcore_smoke, input_lab,
                     shell_lab, jit_lab, metal_d3d11_test, shader_batch, xrdb_unpack, ...)
tools/ios/           scripts such as push_gamedata.sh (copies game files to the iPad)
docs/ios-port/       plan, design docs and audit for the port
game/, reference/    local only, gitignored: game files and the OpenXRay reference checkout
```

## Building

You need macOS on Apple Silicon, Xcode 27, CMake 3.28+ and Ninja.

```bash
git submodule update --init --depth 1 external/SDL external/openal-soft external/LuaJIT external/ogg external/vorbis external/theora external/SPIRV-Cross external/vkd3d external/sse2neon
cmake --preset macos-arm64
cmake --build --preset macos-arm64
ctest --test-dir build/macos-arm64
```

iPad builds use the Xcode generator and your own signing team, set in `CMakeUserPresets.json`. A
free Apple account works, but its builds expire after 7 days.

Details, including signing and each tool: [BUILDING.md](docs/ios-port/BUILDING.md).

The original Windows build is still `src/engine-vs2022.sln`. The port doesn't test it.

## Game files

1. Download **Anomaly 1.5.3** (`Anomaly-1.5.3-Full.7z`, 9.07 GB) from
   [ModDB](https://www.moddb.com/mods/stalker-anomaly/downloads/stalker-anomaly-153).
2. Extract it to `game/Anomaly`. Tools only ever read it.
3. For the iPad, copy it into the app with `tools/ios/push_gamedata.sh`. Finder or the Files app
   also work; see [APP-SHELL.md](docs/ios-port/APP-SHELL.md).

Saves from the PC version will probably not load, because of the LuaJIT 2.0 to 2.1 change.

## Credits and license

- **GSC Game World:** the X-Ray engine and S.T.A.L.K.E.R. The engine source is GSC proprietary code
  released for **non-commercial use only**; see [License.txt](License.txt). This project is
  non-commercial and not affiliated with GSC or the Anomaly team.
- **Anomaly Developers:** Anomaly and the Monolith engine.
- **themrdemonized and the Modded Exes contributors:** [xray-monolith](https://github.com/themrdemonized/xray-monolith),
  the base of this fork. Its original README, with the engine's feature list, is at
  [docs/xray-monolith-README.md](docs/xray-monolith-README.md).
- **[OpenXRay](https://github.com/OpenXRay/xray-16):** used as the reference for cross-platform and
  ARM64 work.
- **Third-party libraries,** each under its own license: SDL3 (zlib), OpenAL Soft (LGPL), LuaJIT
  (MIT), vkd3d (LGPL), SPIRV-Cross (Apache 2.0), Xiph Ogg/Vorbis/Theora (BSD). OpenAL Soft and
  vkd3d are LGPL and statically linked, so keep their terms in mind before sharing any build.
