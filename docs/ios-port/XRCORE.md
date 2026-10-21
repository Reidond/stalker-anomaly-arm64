# xrCore on Apple platforms

`src/xrCore` builds as a static library (`Anomaly::xrCore`, `src/xrCore/CMakeLists.txt`) for macOS and iOS/iPadOS,
arm64 only. The Windows build (`xrCore.vcxproj`) is unchanged; every port change sits behind
`XR_PLATFORM_APPLE` / `XR_ARCH_ARM64`.

`tools/xrcore_smoke` is the headless check (macOS only): it mounts the real game archives, reads
`configs\system.ltx` out of one and reads textures through both archive read paths.

```bash
cmake --preset macos-arm64
cmake --build --preset macos-arm64 --target xrcore_smoke
build/macos-arm64/tools/xrcore_smoke/xrcore_smoke
```

`xrcore_smoke --crash-test segv|assert|abort` checks the crash reporting instead: stack trace to stderr and the log,
`<log name>_<timestamp>.crash.txt` next to the log, and the process still dies with the original signal.

It finds the game data through `--game-dir`, `$ANOMALY_GAME_DIR` or the CMake cache variable
`XRCORE_SMOKE_GAME_DIR` (auto-detected next to the main checkout). It writes only into
`build/macos-arm64/tools/xrcore_smoke/run/` (generated `fsgame.ltx`, `appdata/`, logs).

`-DANOMALY_ENGINE_DEBUG=ON` defines `DEBUG` for the engine (the shipped Windows Release leaves it undefined, and so
does this build by default); xrCore and the smoke tool pass in both modes.

## The shim: `src/Common/Platform.h`

Include it before anything else. On Windows it only defines markers; on Apple it pulls in `PlatformApple.inl`:

- `XR_PLATFORM_WINDOWS | XR_PLATFORM_APPLE | XR_PLATFORM_IOS | XR_PLATFORM_MACOS | XR_PLATFORM_POSIX`,
  `XR_ARCH_X64 | XR_ARCH_ARM64`.
- MSVC keywords as macros: `__forceinline`, `__declspec(x)` (noinline, align, dllexport, thread, ... mapped to
  attributes), `__stdcall`/`__cdecl` (empty), `__interface`, `__int64`.
- Win32 types (`DWORD`, `BOOL`, `HANDLE`, `LARGE_INTEGER`, `RECT`, ...), `HRESULT` helpers, `Sleep`,
  `GetTickCount`, `QueryPerformanceCounter` (nanoseconds, frequency 1e9), `Interlocked*`, `MessageBox` (stderr plus an
  optional hook, `xr_set_message_box_hook`), `IsDebuggerPresent`, `GetModuleFileName`, `GetCommandLine`, ...
- The MSVC CRT: `stricmp`, `strupr`, `strcpy_s` & co., `_snprintf`, `_vsnprintf`, `_splitpath(_s)`, `_findfirst` family,
  `_open/_read/_mkdir/_stat`, `_fpclass`, `_control87` (rounding mode only), `MultiByteToWideChar` /
  `WideCharToMultiByte` for UTF-8 and Windows-1251.

Compile flags that go with it (set PUBLIC on `xrCore`, so dependents get them by linking `Anomaly::xrCore`):
`-fdelayed-template-parsing` (MSVC parses template bodies at instantiation), `-fno-operator-names` (`or` is an
identifier), `-Wno-register`, `-fno-strict-aliasing`, `-ffp-contract=off`. Do **not** add `src/xrCore` to an include
path: it has `_types.h`, `_math.h`, ... that would shadow Apple SDK headers. Include xrCore headers as
`"../xrCore/xxx.h"`, as the engine already does.

## Path model (important for every module)

The engine keeps **virtual Windows-style paths** everywhere: `\` separators, lower case, and the registry
(`CLocatorAPI`) is case-insensitive by construction. Roots such as `$fs_root$` are absolute virtual paths
(`\users\me\game\anomaly\`). This keeps the ~500K lines of game code that build paths with `"\\"` working unchanged.

Convert only where a path reaches the OS:

- `xr_native_path(virtual)` returns an OS path. It swaps `\` for `/` and, if the path does not exist as spelled,
  resolves each component case-insensitively (iOS volumes can be case-sensitive; the game data is mixed case). Paths
  that do not exist yet are resolved as far as they go. The result is in a per-thread ring buffer: use it at once.
- `xr_virtual_path(native, out, size)` goes the other way.
- Wrappers that take virtual paths: `xr_fopen`, `xr_stat`, `xr_rename`, `xr_unlink`, `_open`, `_mkdir`, `_access`,
  `_findfirst`, `GetFileAttributes`, ... On Windows the `xr_*` ones are macros for the plain CRT calls.

`Core.ApplicationPath` / `Core.WorkingPath` are virtual paths too.

## Archives (`.db*`)

Archives are never mapped whole. Each mounted archive keeps one file descriptor (`CLocatorAPI::archive::fd()`). An
entry is read with `pread()` into a heap buffer when it is under 256 KB; larger entries get a page-aligned, read-only
`mmap` window covering just that entry, released when the reader is destroyed (`CPackReader`). `CStreamReader` (level
geometry) keeps its existing 1 MB sliding window, now an `mmap` window onto the descriptor. Address space therefore
stays proportional to what is open, not to the ~17 GB of data, and no `extended-virtual-addressing` entitlement is
needed. The page size is 16 KB on Apple silicon (`FS.dwAllocGranularity`).

## What is replaced

| Windows | Apple |
|---|---|
| `CRITICAL_SECTION`, `SRWLOCK` | `std::recursive_mutex`, `std::shared_mutex` |
| PPL (`_thread_types.h`) | Grand Central Dispatch (`xr_parallel_for`, `xr_task_group`) |
| `rdtsc`, QPC | `cntvct_el0` for `CPU::GetCLK`, `clock_gettime_nsec_np` for QPC |
| CPUID | `sysctl` (`hw.physicalcpu`, brand string); the SSE feature bits are reported as present, to be backed by sse2neon |
| `_aligned_malloc` front end, `xrMemory_subst_msvc.cpp` | `Platform/xrMemory_apple.cpp` (`calloc`/`realloc`/`free`; the PURE_ALLOC path) |
| SEH, DbgHelp, BugTrap, StackWalker, `blackbox/` | `Platform/xrDebug_posix.cpp`: signal handlers on an alternate stack, `backtrace()` + `dladdr` + demangling, crash report next to the log |
| SSE4.2 `crc32` | ARMv8 `__crc32c*` (same CRC32C) |
| x87 control word | FPCR rounding mode and flush-to-zero |
| `CreateFile`/`MapViewOfFile` readers | `open`/`mmap` |
| Win32 clipboard, comdlg32 dialogs | process-local clipboard; dialogs stubbed (editor-only) |

## For module sessions

Every other engine module (xrCDB ... xrGame) is a CMake static library, defined in its own file under
`cmake/targets/` and **off by default** until its session makes it compile. The root `CMakeLists.txt` includes every
`cmake/targets/*.cmake`, so a module session edits only its own file.

| Module | Option | File | Target | Source list read from |
|---|---|---|---|---|
| xrCDB | `ANOMALY_BUILD_XRCDB` | `xrCDB.cmake` | `xrCDB` | `src/xrCDB/xrCDB.vcxproj` |
| xrXMLParser | `ANOMALY_BUILD_XRXMLPARSER` | `xrXMLParser.cmake` | `xrXMLParser` | `src/xrXMLParser/xrXMLParser.vcxproj` |
| xrParticles | `ANOMALY_BUILD_XRPARTICLES` | `xrParticles.cmake` | `xrParticles` | `src/xrParticles/xrParticles.vcxproj` |
| xrCPU_Pipe | `ANOMALY_BUILD_XRCPU_PIPE` | `xrCPU_Pipe.cmake` | `xrCPU_Pipe` | `src/xrCPU_Pipe/xrCPU_Pipe.vcxproj` |
| xrSound | `ANOMALY_BUILD_XRSOUND` | `xrSound.cmake` | `xrSound` | `src/xrSound/xrSound.vcxproj` |
| xrPhysics | `ANOMALY_BUILD_XRPHYSICS` | `xrPhysics.cmake` | `xrPhysics` | `src/xrPhysics/xrPhysics.vcxproj` |
| xrNetServer | `ANOMALY_BUILD_XRNETSERVER` | `xrNetServer.cmake` | `xrNetServer` | `src/xrNetServer/xrNetServer.vcxproj` |
| xrEngine | `ANOMALY_BUILD_XRENGINE` | `xrEngine.cmake` | `xrEngine` | `src/xrEngine/xrEngine.vcxproj` |
| xrAPI | `ANOMALY_BUILD_XRAPI` | `xrAPI.cmake` | `xrAPI` | `src/Layers/xrAPI/xrAPI.vcxproj` |
| R4 renderer layers | `ANOMALY_BUILD_XRRENDER_R4` | `xrRender_R4.cmake` | `xrRender_R4` | `src/Layers/xrRenderPC_R4/xrRender_R4.vcxproj` (xrRender + xrRenderDX10 + R4, 227 files) |
| xrGame (+ 58 xrServerEntities files) | `ANOMALY_BUILD_XRGAME` | `xrGame.cmake` | `xrGame` | `src/xrGame/xrGame.vcxproj` |
| lua-extensions | `ANOMALY_BUILD_LUA_EXTENSIONS` | `lua_extensions.cmake` | `lua_extensions` | `src/3rd party/lua-extensions/lua_extensions.vcxproj` |

`ANOMALY_BUILD_ALL_MODULES=ON` turns them all on (the per-module options are then ignored), and the aggregate target
`anomaly_modules` builds whatever is enabled.

```bash
cmake -S . -B build/probe -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DANOMALY_BUILD_ALL_MODULES=ON            # or -DANOMALY_BUILD_XRSOUND=ON for just your module
cmake --build build/probe --target luajit_build  # once: the Lua headers live in the LuaJIT build copy
cmake --build build/probe --target xrSound -- -k 0
tools/syntax_probe.py build/probe --module xrSound --all-files      # -fsyntax-only, no objects, errors grouped by origin
```

`anomaly_vcxproj_sources(<var> <vcxproj> [EXCLUDE regex...] [INCLUDE regex...])` (cmake/AnomalyModules.cmake) turns the
project's `<ClCompile>` entries into the source list (paths are matched on a case-insensitive file system, entries with
`ExcludedFromBuild` are skipped, regexes are written in lower case). Your module file can exclude files, add its
own (`SOURCES ${list} extra.cpp`), add `LINK` dependencies, define macros and so on; it stays the only file you edit
for the build. No precompiled header is configured: every file starts with `#include "stdafx.h"` (or
`pch_script.h`), so the MSVC `/Yu` behaviour is preserved by the includes themselves.

### What every module gets: `Anomaly::EngineConfig`

Linking a module (it is done for you by `anomaly_add_module`) brings, on top of `Anomaly::xrCore`'s flags above:

- `-fms-extensions` (`\` in `#include`, `__declspec`, `__super`, the `i64` suffix) and `-iquote src/xrCore`. The
  module's own include roots are also passed as `-iquote` ahead of it, because `-iquote` directories are searched
  before every `-I`: `pch_script.h` includes `"stdafx.h"` and must find the module's, not xrCore's.
- `src/Common/compat` first on the include path, then `src`, `src/3rd party`, `src/Include`; the sse2neon include root
  and the Metal D3D11 headers (`metal_d3d11_compat`). `src/xrCore` itself is **never** an include directory, and neither
  is `sdk/include` (its DirectX, DirectPlay and `cs` headers are the real Windows ones; `sm_boost` comes through luabind).
- `NDEBUG` (unless `ANOMALY_ENGINE_DEBUG`), `dSINGLE`, `MTL_D3D11_NO_WINTYPES` (the Win32 types come from the shim, the
  Metal shim's fallback typedefs stay out of the way).
- Warning suppressions for what MSVC `/W3` never reported (`-Wno-microsoft`, unknown pragmas, writable strings,
  narrowing in initializer lists, `offsetof` on non-POD, ...). C++ exceptions stay on (the sources contain `try`/`catch`,
  and Lua error unwinding needs them).
- `ANOMALY_MS_COMPAT=ON` adds `-fms-compatibility -fms-compatibility-version=19.30 -D__GNUC__=4` (the define restores the Apple
  SDK headers, which break without it). It makes clang accept dependent-base lookup, template-parameter shadowing and
  forward-declared enums. Measured at P0 on the same 60 sampled TUs per module: xrGame unique module error sites 541 without it,
  350 with it (total errors 2,680 against 706), xrPhysics 28 against 25, xrEngine and the renderer unchanged; but 34 luabind
  `type_info` sites then appear in every script TU, so no xrGame TU is clean until luabind is patched, and the diagnostics it
  hides stay hidden. It is off by default; the xrgame-triage session decides whether xrGame builds with it while the
  shared template headers are fixed.

### What the shim provides

`Common/Platform.h` (+ `PlatformApple.inl`, implemented in `xrCore/Platform/PlatformApple.cpp`): the Win32 types
(`DWORD` is `uint32_t`; `GUID`, `LUID`, `FILETIME`, `SYSTEMTIME`, `WAVEFORMATEX`, `SIZE`, `HMONITOR`, GDI handles), `HRESULT`
codes, the MSVC keyword macros, the secure CRT (`*_s`, `sscanf_s` that drops the size arguments), `_snprintf`, `_splitpath`,
`_alloca`, `_aligned_malloc`, 64-bit time functions, `ZeroMemory` & co. as macros, `CRITICAL_SECTION` (recursive),
`Interlocked*` and `_Interlocked*`, events, mutexes, semaphores, `CreateThread`/`_beginthread(ex)` with `WaitForSingleObject`
/ `WaitForMultipleObjects` / `CloseHandle`, `GetLocalTime`, `GetSystemInfo`, `GlobalMemoryStatusEx`, environment variables,
`ShellExecute` (always fails), `LoadLibrary`/`GetProcAddress` (always fail). It also defines `interface` as `struct` in C++ (not
Objective-C++) translation units, like `objbase.h`.

`Common/d3d9compat.h` + `d3dx9compat.h` (based on OpenXRay's `d3d9compat.hpp`): the D3D9 enums, flags and structs the engine
and the renderer still name, the D3D9 interfaces as incomplete types, `D3DXVECTOR*`/`D3DXMATRIX`/`D3DXPLANE` with the matrix,
vector and plane functions in use, `D3DXGetDeclLength`/`D3DXGetDeclVertexSize`/`D3DXGetFVFVertexSize`/`D3DXDeclaratorFromFVF`,
`D3DXFLOAT16`. It has no D3D10/11/DXGI names, so it coexists with the Metal shim's headers in one translation unit
(checked on the R4 sources). Not provided: D3DX texture/surface/shader-compiler calls (the renderer port replaces them).

`Common/dinput_compat.h`: `DIK_*` with the DirectInput numbering (Anomaly's Lua scripts and `user.ltx` depend on it; map SDL
scancodes to these, never the other way), plus the DirectInput device types as declarations so `xr_input.h` parses.
`Common/compat/DPlay/dplay8.h`: the DirectPlay subset xrGame and xrEngine name (`DPNID`, `DPNSEND_*`, application/connection
descriptors, the interface names as incomplete types). `external/sse2neon` through `<xmmintrin.h>`, `<emmintrin.h>`,
`<intrin.h>`, ... in `Common/compat` (sources need no edits).

**Not** in the shim, on purpose: the window system (`WNDCLASS`, `CreateWindow`, `GetClientRect`, `ClipCursor`, `WM_*`), COM,
DirectInput/DirectPlay/DirectSound/WASAPI calls, the registry, VFW. Those code paths are replaced by the module sessions that
own them (engine-core, engine-input, netserver, sound).

Known leftovers that block specific modules and belong to them: the circular `D3D_PRIMITIVE_TOPOLOGY_*` /
`D3D11_PRIMITIVE_TOPOLOGY_*` macros between `Layers/xrRenderDX10/DXCommonTypes.h` and the Metal shim's `d3d11_metal.h` (hits every R4 file
at `dx10R_Backend_Runtime.h:294`; drop the engine-side `#define` block for Apple or the shim-side one), `IUnknown` for
`xrEngine/Environment.cpp` (comes from the Metal shim's headers), `REFIID`, the `luaL_reg` name for lua-extensions.

### Requesting shim additions

`src/Common/**`, `src/xrCore/**`, `cmake/AnomalyPlatform.cmake` and `cmake/AnomalyModules.cmake` are owned by the orchestrating
session after P0 merges. If you are blocked on a missing name, append a line to `shim-requests.md` in the shared notes folder
(the name, the file and line that needs it, the Win32 signature) and carry on with the next error. If it blocks you for good, add it
yourself as an **additive** change in `Common/PlatformApple.inl` (declaration) and `xrCore/Platform/PlatformApple.cpp`
(implementation), in its own commit at the start of your branch, and say so in your PR: additive-only keeps the merges between
module branches trivial. Do not "fix" a Windows name by changing the module's source when the shim can provide it; do not put
Apple-only code in a module without `XR_PLATFORM_APPLE`.

