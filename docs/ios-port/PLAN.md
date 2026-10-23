# Anomaly on iPad: porting plan

Target: STALKER Anomaly 1.5.3 running natively on an iPad Air 13" (M3, 8 GB, iPadOS 27) with full
DualSense (PS5) controller play. The same code also has to run natively on Apple Silicon Macs.

## Why a native port

- There is no existing Anomaly port for macOS, iOS or ARM. `fproverbio/xray-monolith-linux` is only
  a README on top of upstream.
- Emulating it (Wine plus an x86 translator) is not workable on iPadOS: sideloaded apps get no JIT.
- OpenXRay (`OpenXRay/xray-16`) already runs on macOS and ARM64, but it cannot run Anomaly's
  gamedata. Anomaly's scripts depend on hundreds of engine exports that exist only in the
  Anomaly/CoC lineage.

## Base and references

- **Base:** `themrdemonized/xray-monolith` ("Modded Exes"), the Anomaly 1.5.3 engine plus community
  patches, many of which addons need. This repo is a hard fork of it: the history is kept, but it
  doesn't track upstream, and `main` is the working branch. Upstream fixes can still be
  cherry-picked by hand. Port changes go behind platform `#if`s so the Windows build keeps working.
- **Reference:** OpenXRay, for portability work it has already solved: the POSIX `xrCore` layer,
  `sse2neon`, SDL input, LuaJIT 2.1 on arm64, OpenAL Soft and the CMake layout.

## Size of the job

| Module | LOC | Port work |
|---|---|---|
| xrCore | 46K | Win32 → POSIX (files, mmap, threads, SEH/crash handler, timers), x86 asm → portable |
| xrEngine | 53K | Window, DirectInput → SDL3; device and render loop |
| xrGame | 451K | Mostly MSVC-isms under clang; script exports unchanged |
| Layers (renderers) | 135K | DX9/DX10/DX11 → **new Metal backend** (largest new code) |
| xrPhysics/ODE, xrCDB, xrSound, xrParticles | ~60K | Mostly portable; OpenAL Soft already used |

Gamepad: `xr_input_xinput.*` is commented out, so the engine has no working gamepad code yet.

## Architecture

- **Build:** a new CMake build next to the existing `.sln`, with targets for `macos-arm64` and
  `ios-arm64`.
- **Platform layer:** SDL3 for the window, Metal view, input, app lifecycle and controllers.
- **Renderer:** port the R4 (DX11) path to Metal.
  - Shaders: Anomaly's HLSL (SM4/5) goes through DXC (or vkd3d-shader) → SPIR-V → SPIRV-Cross →
    MSL. This happens at runtime with an on-disk cache, so addons that replace shaders keep working.
  - Textures: Anomaly's DDS files are BC1–BC7. M-series iPads sample these natively, so no
    transcoding is needed.
  - Upscaling: the 3D scene renders at a fraction of the output size (`r__render_scale`) and is brought to the output size before the UI
    is drawn, with spatial MetalFX or a bilinear pass; the UI stays at native resolution. Done, see RENDERER.md ("Render scale and
    upscaling"). Temporal MetalFX and frame interpolation need motion vectors, jitter and a reactive mask that R4 does not produce;
    the same section lists what they would take.
  - Rejected alternative: DXVK-native → MoltenVK. It has three translation layers, no geometry
    shaders or transform feedback, and needs an HLSL→DXBC compiler anyway.
- **Scripts:** LuaJIT 2.1 on arm64. JIT stays on for the Mac build. On the iPad the interpreter
  is the baseline, and the game must be playable on it.
  - Optional iPad JIT: sideload with SideStore and launch through StikDebug. The M3 is a TXM
    device on iPadOS 27, so JIT only works for apps that implement StikDebug's handshake:
    - At boot, reserve a fixed code region and hand its address and size to the debugger.
    - The debugger touches every 16 KB page, then detaches; the region stays executable until
      the app quits.
    - Every cold launch has to go through StikDebug again.
  - LuaJIT changes this needs:
    - Its machine-code allocator (`lj_mcode.c`) must hand out code from that pre-authorized
      region.
    - The region must sit within arm64 branch range (±128 MB) of the VM.
    - Writes must go through a writable alias or toggle, because pages cannot be made writable
      and executable with `mprotect`.
  - If the handshake fails, fall back to the interpreter.
  - Upstream notes that moving from LuaJIT 2.0 to 2.1 breaks PC save compatibility, so PC saves
    probably won't load.
- **Audio:** OpenAL Soft on CoreAudio.
- **Files:** game data (~17 GB) goes into the app's Documents folder (Finder or Files). The engine
  maps `.db` archives into memory, so we need either the extended-virtual-addressing entitlement or
  `pread`-based archive reads.
- **Memory:** the 8 GB iPad needs the `increased-memory-limit` entitlement plus an iPad preset with
  lower texture LOD and draw distance. Metal textures count against the app's memory budget.

## Controller design (DualSense)

- A gamepad layer in `xrEngine` turns buttons into the existing key-binding actions, so they can be
  rebound in the stock Controls menu.
- **Movement:** the left stick is analog, with walk below a threshold and run above it. Sprint is L3.
- **Look:** the right stick uses a response curve and acceleration. Optional gyro aim, while ADS or
  always.
- **Triggers:** L2 aims (ADS), R2 fires.
- **Haptics:** adaptive trigger resistance per weapon via `GCDualSenseAdaptiveTrigger`, rumble on
  hits and blowouts, and the light bar shows health.
- **Menus** (inventory, PDA, trade, dialogs, map): stick-driven virtual cursor, face buttons for
  click and back, and the touchpad as a trackpad.
- **Hotkeys:** a radial menu for the rest of Anomaly's bindings (quick slots, heal, bandage,
  detector, NVG, flashlight, lean, prone and so on).
- **Fallbacks:** touch works as mouse input in menus; the on-screen keyboard is used for save names.

## Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 | Repo, CMake skeleton, deps (SDL3, OpenAL Soft, LuaJIT 2.1, shader toolchain) | `cmake --preset macos-arm64` configures |
| M1 | Engine runs headless on macOS arm64 with a null renderer | New game starts, scripts and AI tick, a save round-trips |
| M2 | Metal renderer on macOS | Main menu, then Cordon renders with lighting, at about PC-low quality |
| M3 | Input: SDL3 keyboard/mouse, DualSense layer, UI cursor, haptics | Full playthrough of the first quests on a pad, on the Mac |
| M4 | iPad build: app bundle, signing, data import, memory, lifecycle, LuaJIT interpreter | Plays on the iPad at a stable 30+ fps with a sensible preset |
| M5 | Polish: performance, addons via Files, adaptive triggers, crash logs; hands-on DualSense test on the iPad (checklist in IPAD.md) | Daily-driver quality |

The Mac comes first because it has the same Metal, GameController and CPU architecture as the
iPad, plus a debugger, fast iteration, and no 7-day signing or jetsam. That leaves only packaging
and limits for M4.

### M5: Lua performance on the iPad

Lua scripts aren't compiled ahead of time to native code. LuaJIT has no AOT backend, and the
only usable tool (LuaAOT) needs Lua 5.4. A move to 5.4 would break Anomaly's 5.1/LuaJIT scripts,
luabind, lua-extensions and addons, and still only reach about LuaJIT-interpreter speed. Instead,
in order:

1. **Measure first.** Use the M4 numbers: fps and frame time in the Cordon with the LuaJIT
   interpreter, plus a profile of time spent in Lua (scheduler updates, callbacks, per-frame
   scripts). If the budget holds, stop here.
2. **Precompile scripts to LuaJIT bytecode** (`luajit -b`).
   - Do it at build time for the shipped scripts and the Monolith overlay; it can also be done
     at first run for addons in `gamedata/`.
   - Keep the `.script` source as the fallback, and re-check the source mtime/hash so edited
     scripts and addons still load. This cuts startup and level-load time; runtime speed doesn't
     change.
3. **Move hot paths to C++.** Profile the interpreter on the iPad, port the few hottest script
   functions to engine exports callable from Lua, and keep the Lua behaviour identical. Re-measure
   after each move.

The JIT stays available as the opt-in route for full speed: SideStore + StikDebug, or a debugger
([IPAD-JIT.md](IPAD-JIT.md)).

State (2026-10-23, measured on the Mac and on the iPad; the method, tools and tables are in [LUA-PERF.md](LUA-PERF.md), the iPad's numbers also in [IPAD.md](IPAD.md#lua-on-the-ipad)):

1. **Measured.** Lua is 1.0 to 3.6 ms of a frame on the iPad's interpreter, 6 to 11 % of the frame budget in the Cordon, the Rookie village and the Bar, in both modes; both modes hold their cap
   (Quality 30 fps, Performance 60). The profile is flat: a quarter of the time is the collector, half is engine functions called from scripts, a fifth is bytecode.
2. **Bytecode cache.** Built (`Library/Caches/Anomaly/lua`, keyed by a hash of the exact text, any mismatch or error loads the source; tested for stale sources, addon overrides and damaged entries) and
   on by default with a read-ahead thread. Compiling is not where the load time goes: the scripts are 0.27 to 0.34 s of a 5.5 s level load on the iPad and 66 ms of that is compiling, so the gain is
   small: the Lua part of a load goes from 326-343 ms (no cache) to 270-273 ms (warm cache with the read-ahead), about 1 % of the load; without the read-ahead thread it is 318-322 ms.
3. **Hot paths: not needed**, the budget holds with room; nothing was ported.

## Risks

1. **Memory on 8 GB.** This is the most likely blocker; the mitigations are above. Measure it
   early at M2.
2. **Lua performance without JIT** on the iPad. Anomaly is script-heavy. Mitigation: profile, then
   move hot script paths to C++ exports.
3. **Shader translation edge cases** (FXC-isms in Anomaly HLSL). The M2 spike compiles every
   shader before any backend work starts.
4. **Size.** This is a multi-month project.

## Things needed from the user

- **Game files:** Anomaly 1.5.3 lives in `game/` and is gitignored. It comes from ModDB's
  `Anomaly-1.5.3-Full.7z`, fetched through the official torrent link on the ModDB page.
- **iPad:** turn Developer Mode on (Settings → Privacy & Security). Needed at M4.
- **Signing:** the current signing team is a free Personal Team.
  - Its builds expire every 7 days. SideStore can refresh them on the device, but the free
    account still caps installs at 3 apps.
  - Probe results on the iPad (2026-10-21):
    - `increased-memory-limit` signs with the free team, and `os_proc_available_memory` reports
      7.95 GB.
    - `extended-virtual-addressing` is refused for Personal Teams, so archive reads must not rely
      on mapping all ~17 GB.
  - The paid Apple Developer Program gives 1-year signing and the full set of entitlements.
