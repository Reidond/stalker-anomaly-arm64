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
