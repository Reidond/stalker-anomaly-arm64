# Spike S1b: xrCore and the headless smoke tests as WebAssembly

Question: does the engine core build for wasm32 and run correctly, and what does the full platform split (milestone W1) have
to do? Answer: **yes**. xrCore, xrCDB, xrParticles, xrCPU_Pipe, ODE and xrPhysics compile with Emscripten, and the six smoke
tools pass under Node, including `xrcore_smoke` mounting and reading the real game archives (67 archives, 47 834 files, 16 GB on disk). The Apple build
is unchanged. What it took is listed below; so is what it did not prove.

## Reproduce

```bash
# once: the toolchain, inside the checkout (build/ is gitignored); nothing is installed system-wide
git clone --depth 1 https://github.com/emscripten-core/emsdk.git build/emsdk
(cd build/emsdk && ./emsdk install latest && ./emsdk activate latest)

cmake --preset web-wasm32                 # no need to source emsdk_env.sh; the preset sets EM_CONFIG
cmake --build build/web-wasm32 -j 4
ctest --test-dir build/web-wasm32         # runs every tool under the `node` on PATH
```

`xrcore_smoke_gamedata` needs the game directory. CMake looks for `game/Anomaly` in the checkout and in
`<repo>/../../../game/Anomaly` (a worktree under `.claude/worktrees`); set `-DXRCORE_SMOKE_GAME_DIR=<dir>` otherwise. It reads
the archives and writes only below its run directory (`build/web-wasm32/tools/xrcore_smoke/run`).

Tests (ctest names): `xrcdb_smoke`, `xrcdb_smoke_mt_cdb`, `xrcpu_pipe_smoke`, `xrparticles_smoke`, `xrphysics_smoke`,
`ode_smoke`, `xrcore_smoke_case_test` (`--case-test`), `xrcore_smoke_gamedata`.

## Toolchain

| | |
|---|---|
| emsdk | `emscripten-core/emsdk` at `1693ce7359fc9694f198694f902a2cc061239b86` (cloned 2026-11-10), `./emsdk install latest` = SDK `releases-6afbd83493d647051772d491124187d4e9a0cda9-64bit` |
| Emscripten | 7.0.0 (`777e6e5db7b4990bcaeb1b9998ed642cc84e6629`) |
| clang | 24.0.0git (`84ba7b37f02df5a5567e31a1bcfa0f607e578f12`), target `wasm32-unknown-emscripten` |
| Host | macOS arm64, CMake 4.4.5, Ninja |
| Node that runs the tests | v26.13.0 (the `node` on PATH; emsdk's own Node 24.19.0 is used only by `emcc` itself) |

Flags (`cmake/web/AnomalyWebFlags.cmake`, applied to every target):

| Flag | Why |
|---|---|
| `-pthread` | threads on shared memory; every object needs it |
| `-msimd128 -msse4.2` | wasm SIMD; Emscripten's `<xmmintrin.h>` ... `<nmmintrin.h>` emulate SSE up to 4.2 on it |
| `-fwasm-exceptions` | native wasm exception handling, no JavaScript glue |
| `-frtti` | on (also the clang default) |
| `-O3 -DNDEBUG` | the preset is `Release`; `-flto` is `-DANOMALY_WEB_LTO=ON`, off by default (sizes below) |
| `-sSTACK_SIZE=8MB` | main stack (`ANOMALY_WEB_MAIN_STACK_MB`) |
| `-sDEFAULT_PTHREAD_STACK_SIZE=2MB` | thread stacks; `string4096` buffers (`_math.cpp:397-416`) live on them |
| `-sPTHREAD_POOL_SIZE=12` | Workers created before `main()` (`ANOMALY_WEB_POOL_THREADS`) |
| `-sINITIAL_MEMORY=256MB -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB` | heap grows to the wasm32 limit |
| `-sMALLOC=mimalloc` | see below |
| `-sEXIT_RUNTIME=1` | `main()` returning ends the process with its exit code |
| `-sENVIRONMENT=node -sNODERAWFS=1` | the Node runner (`ANOMALY_WEB_NODE`, on); real disk access, no preload |
| `--pre-js cmake/web/node_argv.js` | `process.argv` becomes `GetCommandLine()` (`-mt_cdb`, `-max-threads`, ...) |

The linker warns that `-pthread` with `ALLOW_MEMORY_GROWTH` "may run non-wasm code slowly" (the JavaScript glue refreshes its
heap views after every growth). Its suggestion, `-sGROWABLE_ARRAYBUFFERS=2` (resizable `SharedArrayBuffer`), links, silences the
warning and `xrcdb_smoke` passes with it under Node 26; whether Chrome and Edge accept it, and what it does to speed, is W1.

### `-sMALLOC=mimalloc`: works, and is needed

It links and all eight tests pass with it. Differences from the default `dlmalloc`:

- **Alignment.** mimalloc returns 16-byte aligned blocks; dlmalloc and emmalloc return 8 on wasm32. `xrcore_smoke` asserts 16
  (`xr_malloc(1 << 20) % 16`, the contract xrMemory has on Windows and Apple) and **fails with dlmalloc**. wasm SIMD loads are
  alignment-tolerant, so nothing else broke, but the contract holds only with mimalloc. It is the preset default.
- **`mallinfo()`** does not exist in the mimalloc build, which broke the link (`xr_apple_heap_bytes_in_use`). It is a weak
  reference now; without it the heap size stands in.
- Size: +62 KB per tool (table below).
- Speed: not measured here. The plan's reason for it (a global lock in dlmalloc under contention) is not tested by a smoke
  tool.

## What was done

Everything is additive or guarded; with `ANOMALY_WEB` off, nothing changes for Apple.

### Build system

| File | Change |
|---|---|
| `CMakePresets.json` | `web-wasm32` configure, build and test presets (Ninja, Release, toolchain file under `build/emsdk`, `EM_CONFIG`) |
| `CMakeLists.txt` | `project()` languages computed first (no OBJC/OBJCXX on web); `ANOMALY_WEB` includes `cmake/web/AnomalyWeb.cmake` and returns before the Apple-only rest |
| `cmake/AnomalyPlatform.cmake` | `ANOMALY_WEB` option, `ANOMALY_PLATFORM_NAME=web`, skips the arm64 check, sanitizers, signing and entitlements |
| `cmake/AnomalyModules.cmake` | `Anomaly::EngineConfig` no longer links `sse2neon` and `metal_d3d11_compat` on web |
| `cmake/web/AnomalyWebFlags.cmake`, `AnomalyWeb.cmake`, `node_argv.js` | flags above; the module list for web (xrAPI, xrCDB, xrCPU_Pipe, xrParticles, xrPhysics, by name — the root file's glob also enables targets that need SDL or Swift); ODE, xrCore, smoke tools |
| `src/xrCore/CMakeLists.txt` | on web: `Platform/PlatformWeb.cpp`, `Platform/xrThreadPool_web.cpp`, `XR_WEB_TASK_THREADS` |
| `tools/xrcore_smoke/CMakeLists.txt` | two `add_test`s on web |

### Platform defines (`src/Common/Platform.h`)

`__EMSCRIPTEN__` → `XR_PLATFORM_WEB`, `XR_PLATFORM_POSIX` **and `XR_PLATFORM_APPLE`**; `__wasm32__` → `XR_ARCH_WASM32`.

The last define is a deliberate shortcut and the main thing W1 has to undo. xrCore uses `XR_PLATFORM_APPLE` 122 times in 36
files to mean "the POSIX port path" (the non-Windows branch), not "an Apple API". Defining it on web takes all of those
branches at once. Code that really calls an Apple API got an `XR_PLATFORM_WEB` branch placed ahead of it (the list below).
`PlatformWeb.inl` includes `PlatformApple.inl` unchanged (it is POSIX apart from one asm line).

### Seams in xrCore (each guarded by `XR_PLATFORM_WEB` or `XR_ARCH_WASM32`)

| Where | Apple/x87 code that does not exist on wasm | Web replacement |
|---|---|---|
| `_thread_types.h` | GCD: `dispatch_group_*`, `dispatch_apply_f` | `Platform/xrThreadPool_web.{h,cpp}`: fixed pool of `XR_WEB_TASK_THREADS` (4) Workers, queue + condvar; `xr_task_group` and `xr_parallel_for/foreach` keep their semantics, the waiting thread runs queued work (no starvation of a nested wait) |
| `Platform/PlatformApple.cpp` | `malloc_size`, `malloc_zone_statistics`, `task_info`, `sysctl*`, `_NSGetArgv`, `_NSGetExecutablePath`, `pthread_threadid_np`, `clock_gettime_nsec_np` | `Platform/PlatformWeb.cpp` (`malloc_usable_size`, `mallinfo`, `emscripten_get_heap_size/max`); `sysconf`, `CLOCK_MONOTONIC`, `pthread_self`, `$XR_COMMAND_LINE` as `#ifdef` branches at 9 spots |
| `Platform/xrDebug_posix.cpp` | signals, `sigaltstack`, `alarm`, `backtrace`, `dladdr` | no handlers (a wasm trap is a JavaScript exception); stack from `emscripten_get_callstack` |
| `_math.h` `CPU::GetCLK` | `mrs cntvct_el0` | `clock_gettime(CLOCK_MONOTONIC)` ns (1 GHz), defined in `PlatformWeb.cpp` |
| `_math.cpp` `_initialize_cpu_thread` | FPCR flush-to-zero (`mrs`/`msr fpcr`) | nothing: wasm cannot set it (numbers below) |
| `_math.cpp` `thread_name` | `pthread_setname_np(name)` | `emscripten_set_thread_name` (the glibc form is declared but not implemented) |
| `_math.cpp:25`, `_bitwise.h:19`, `xrMemory_pso_Copy.cpp:10`, `xrMemory_pso_Fill32.cpp:11` | fell through to MSVC x87/MMX asm | `|| defined(XR_ARCH_WASM32)` joins the portable x64/arm64 branch |
| `crc32.cpp` | `_mm_crc32_*` / `__crc32c*` | software CRC32C, slicing by 8, same contract (no final inversion) |
| `cpuid.cpp` | `__cpuid`, `sysctlbyname` | fixed answer: SSE through SSE4.2 (wasm SIMD is required to load the module), `sysconf` core count |
| `PlatformApple.inl` | `__asm__("yield")` in `YieldProcessor`; `strupr`/`strlwr` clash with Emscripten's `extern "C"` ones | compiler barrier; Emscripten's |
| `PlatformApple.cpp` | `pthread_t x = nullptr` (an opaque pointer on Apple, an integer here) | `pthread_t()` |
| `src/Common/compat/*mmintrin.h`, `intrin.h` | forwarded to `sse2neon` | `#include_next` Emscripten's header; `__unaligned` (an MSVC keyword under `-fms-extensions`, an empty macro in `Platform.h`) is renamed around it because Emscripten's `_mm_storeu_ps` uses it as a struct tag |

Not edited, because they fall through only on MSVC: `xrCore.h:195` and `_stl_extensions.h:14` (`#ifdef _M_AMD64` around a
`#pragma warning` and a macro that is defined unconditionally one line later) and `_math.h:42` (MSVC `_asm`).

One wasm-specific correctness point in `crc32.cpp`: `crc32()` takes the CRC32C branch only if `CPU::ID` reports SSE4.2, and
the plain IEEE CRC32 otherwise, so the two give different values. The web `_cpuid` reports SSE4.2 so web matches Windows and
Apple (the smoke test "crc32 is CRC32C" checks it).

### Other modules

| Where | Change |
|---|---|
| `xrParticles/particle_actions_collection.cpp` | the two `<tbb/...>` includes are guarded out: nothing from TBB is used in the file, and the bundled TBB headers stop at "Unsupported machine word size" for wasm32 |
| `tools/xrcdb_smoke/main.cpp` | the `[6] LP64` case builds a pointer that is zero in its low 32 bits; with 4-byte pointers it is skipped and says so |

Nothing in xrCDB, xrCPU_Pipe, ODE or xrPhysics needed an edit. xrCPU_Pipe's `ttapi.cpp` already runs on `xr_parallel_for`
(its Apple branch), so it uses the new pool.

### Stubs and degraded behaviour on web, all in the shared files above

- `GetCommandLine()` returns `$XR_COMMAND_LINE`: filled from `process.argv` by `node_argv.js`, or by `xr_set_command_line()`.
- `GetModuleFileName()` returns `<cwd>/anomaly`; `IsDebuggerPresent()` is false; `GetSystemInfo` reports an unknown
  processor architecture; `GlobalMemoryStatusEx` reports the heap ceiling as physical memory.
- `xr_apple_vm_info`: committed = current wasm memory, reserved = `-sMAXIMUM_MEMORY`, free = the difference.
- No crash reporter: a fault is a wasm trap and ends the process with the JavaScript error. Assertions still go through
  `xrDebug::backend`.
- File access is Emscripten's: `NODERAWFS` for the Node runner. The five `mmap` sites are Emscripten's `mmap` emulation (it
  copies the mapped window into the heap, so a window costs its size in wasm memory). The bounded-window code in
  `LocatorAPI.cpp` was written to map one archive entry at a time, which is what makes this work; mapping fails over to
  `pread` as before.
- Module-level feature gaps nobody exercised: the clipboard (`os_clipboard.cpp`) compiles and is a stub; `LoadLibrary` and
  `ShellExecute` stay the Apple stubs.

## Results

Every ctest passes, with `-sMALLOC=mimalloc`:

| ctest | Result | Notes |
|---|---|---|
| `xrcdb_smoke`, `xrcdb_smoke_mt_cdb` | pass | 68 checks (native 76: the LP64 case is skipped). SSE ray path against the double-precision reference and the scalar path: 0 mismatches. `-mt_cdb` builds the model on the engine's helper thread (`thread_spawn`) |
| `xrparticles_smoke` | pass | 18 checks. SSE noise equals the scalar reference exactly; SSE turbulence within 2.1e-7 to 3.3e-7 relative (native: 2.0e-7 to 2.7e-7; limit 2e-5). This is the wasm SIMD path |
| `xrcpu_pipe_smoke` | pass | 20 checks. `ttapi` on the new pool: 5 distinct threads did the work (the pool's 4 plus the caller) against 11 natively; skinning bit-identical to the single-threaded one |
| `xrphysics_smoke` | pass | 6 s of physics, 596 steps, resting contacts, ragdoll chain, characters |
| `ode_smoke` | pass | `dWorldStep`, `dWorldQuickStep`, `dWorldStepFast1` come to rest on a plane |
| `xrcore_smoke_case_test` | pass | `--case-test`: path layer on mixed-case folders. The volume is case-insensitive, so the sensitive half is skipped by the tool (as on the Mac) |
| `xrcore_smoke_gamedata` | pass | real data through `NODERAWFS`: mounts 67 archives (50 872 registry entries, 47 834 archived files); reads `configs\system.ltx` (62 515 bytes, stored 11 763, LZ-compressed) out of `configs.db0`; a 2.8 MB texture; a 350 KB stored entry through both archive readers (identical hash); `crc32` is CRC32C; 4 threads x 10 000 locked increments; `xr_parallel_for`; 16-byte aligned allocator |

`.wasm` sizes (Release `-O3`, no LTO, no `-g`; `.js` glue 24 to 98 KB each):

| Tool | `-sMALLOC=mimalloc` (default) | `dlmalloc` |
|---|---|---|
| `xrcore_smoke` | 661 718 | 599 612 |
| `xrcdb_smoke` | 697 954 | 635 905 |
| `xrcpu_pipe_smoke` | 609 965 | 547 918 |
| `xrparticles_smoke` | 646 276 | 584 199 |
| `xrphysics_smoke` | 1 024 948 | 962 896 |
| `ode_smoke` | 217 435 | 155 229 |

With `-DANOMALY_WEB_LTO=ON` (`-flto`, built for two tools only, tests pass): `xrcdb_smoke` 388 933 bytes (-44%),
`xrcore_smoke` 622 398 bytes (-6%; it uses most of xrCore). Compile plus link of those two took 5 min 55 s on the loaded
machine, against 2 min 14 s for the whole default build.

xrCore with its dependencies is about 0.6 MB of wasm (`xrcore_smoke`); the modules add little on top. Binary size is not a risk
for the core.

### Run times against native

Median of 7 runs after one warm-up run, `cmake/web/probes/timing.py`; native is the `macos-arm64-release` build
(RelWithDebInfo, `-O2`), wasm is the default preset under Node 26 (V8, Release `-O3`), both on the same M-series Mac.
Wall seconds, and CPU seconds (user + system, all threads):

| Tool | native wall | wasm wall | wasm wall minus start-up | native CPU | wasm CPU |
|---|---|---|---|---|---|
| `ode_smoke` | 0.013 | 0.254 | 0 (this is the floor) | 0.004 | 0.372 |
| `xrcdb_smoke` | 0.325 | 0.634 | 0.38 (1.2x) | 0.309 | 0.778 |
| `xrcdb_smoke -mt_cdb` | 0.365 | 0.673 | 0.42 (1.1x) | 0.311 | 0.797 |
| `xrparticles_smoke` | 0.128 | 0.407 | 0.15 (1.2x) | 0.114 | 0.471 |
| `xrcpu_pipe_smoke` | 0.249 | 0.607 | 0.35 (1.4x) | 0.217 | 0.676 |
| `xrphysics_smoke` | 0.132 | 0.631 | 0.38 (2.9x) | 0.093 | 0.619 |
| `xrcore_smoke` (game data) | 0.466 | 1.112 | 0.86 (1.8x) | 0.375 | 1.277 |

Read it with care:

- **The floor is Node, not the engine.** `ode_smoke` does about 4 ms of work; the wasm run's 0.25 s is Node starting, compiling
  the module and creating the 12 pre-started Workers. The "minus start-up" column subtracts it, which is a rough correction.
- **The machine was shared.** Another session's game (`pgrep -x anomaly`: 1 instance) was running throughout, because the
  guard that waits for a quiet machine gave up after 25 minutes; load average was 7 at the start. Treat the ratios as +-30%.
- Across the tools that do real work the wasm run takes 1.1 to 1.8 times the native wall time, apart from `xrphysics_smoke`
  (2.9x: ODE is scalar float code at `-ffp-contract=off` on both sides, so the gap is V8's code against clang's, and the
  tool's run-to-run variation is large). CPU time is higher than wall time for wasm because the Workers spin up threads that
  the native run does not have.
- This is the headless core. Nothing here says how the renderer-bound frame will compare.


### Numeric differences without flush-to-zero

The x64 and arm64 builds run every thread with flush-to-zero (`_initialize_cpu_thread`: MXCSR FTZ and DAZ, FPCR.FZ); wasm has
no such control, so denormals are kept, as in IEEE 754. `cmake/web/probes/denorm_probe.cpp`, built native (with and without
the FPCR bit) and for wasm, run under Node:

| Operation (float) | native, flush on (engine) | native, flush off | wasm |
|---|---|---|---|
| `FLT_MIN * 0.5` | `0` | `5.87747175e-39` | `5.87747175e-39` |
| `3e-39 + 4e-39` | `0` | `7.00000011e-39` | `7.00000011e-39` |
| `3e-39 - 4e-39` | `0` | `-9.99998814e-40` | `-9.99998814e-40` |
| `3e-39 * 1e38` | `0` (denormal operand read as 0) | `0.300000042` | `0.300000042` |
| `(1+eps-1) * 1e-30` (normal) | `1.1920929e-37` | same | same |
| `y *= 0.99`, 12 000 steps from 1 | first result below `FLT_MIN` at step 8689 becomes `0` | stops at the smallest denormal `7.0e-44`... never 0 | same bits as flush off |

So wasm equals "native with flush off", bit for bit, and differs from the shipped behaviour exactly where a result or an operand
is below `FLT_MIN` (1.18e-38): results the engine sees as 0 are small non-zero numbers on web. Loops that decay to zero
(filters, damping, `x *= 0.99`) never reach exactly 0; a test `if (x == 0.0f)` on such a value behaves differently. Timing: on
this Apple M-series host denormals cost nothing either way (95 ms flushed, 98 ms kept for 2000 x 12 000 steps natively); that
is not true of x86 hosts, where a denormal operand can cost over a hundred cycles, and the browser targets include them. Not
measured.

In the engine, the effect is not visible in the smoke tools. `xrparticles_smoke` agrees with its scalar reference to 3.3e-7.
`xrphysics_smoke`'s chain scene is chaotic: its worst link-spacing error varies from run to run on the Mac itself, so it cannot
show a flush-to-zero effect (12 runs each, min / median / max of the worst error, metres: native flush on 0.011 / 0.038 /
0.087, native flush off 0.017 / 0.026 / 0.106, wasm 0.017 / 0.038 / 0.052; the three ranges overlap and every run passes).

Rounding modes: wasm only has round-to-nearest. `_control87(_RC_CHOP, ...)` ends in `fesetround(FE_TOWARDZERO)`, which fails
there; the engine only ever leaves round-to-nearest in effect (`FPU::m24r`, `m64r`, `xrCore.cpp:50-53`), so nothing changes.


## What this means for the plan

- **The core is not the hard part.** One session got xrCore plus five modules to build, link and pass with 35 source and build
  files touched and about 800 lines added, most of them comments, the thread pool and the software CRC. W1's cost is in the other 99 files that say
  `XR_PLATFORM_APPLE` (below), the renderer, audio and Lua.
- **Plan item "Allocator: mimalloc" is confirmed and becomes a requirement**, not an optimisation: the engine assumes
  16-byte aligned blocks and only mimalloc gives them on wasm32. The plan's "Platform layer" paragraph can say so.
- **Plan item "GCD → small thread pool" is done** (`xrThreadPool_web.h`). The pool is fixed at compile time
  (`XR_WEB_TASK_THREADS`, 4) because Workers must exist before the engine blocks; the plan's "all threads created up front"
  matches. The thread budget is `-sPTHREAD_POOL_SIZE=12` for now and has to be recounted when xrEngine, OpenAL and the
  prefetcher exist (proposal: a table in this document's successor, one line per thread).
- **The flush-to-zero warning in the plan is accurate and now measured**: web equals native with flush off, bit for bit. The
  audit the plan asks for (audio, particles) is still open and should run on an x86 browser host, where the cost shows.
- **Node is a usable CI runner**: each headless tool finishes in about a second, so W1 can keep every module's smoke tool in
  `ctest` for the web preset as the module starts to link, with no browser involved.
- **Emscripten's `mmap` is acceptable for the spike and wrong for the product**: it copies each window into the heap, and wasm
  memory never shrinks (probe: a 512 MB window took 450 ms and left the heap at 532 MB after `munmap`). The plan's windowed
  read interface with a web backend is the right replacement; its minimum job is to make windows small and reuse buffers.
- Proposed change to "Platform layer": split in the order *rename macros, then move code*, as listed below, since the alias
  in `Platform.h` lets the rename be mechanical and checkable by the Apple build alone.

### Remaining work for W1

1. **Rename `XR_PLATFORM_APPLE` where it means "POSIX".** xrCore: 122 uses in 36 files (`LocatorAPI.cpp` 30, `xrSyncronize.cpp` 7,
   `_math.cpp` 7, `FS.cpp` 6, ...). Outside xrCore and the five modules built here: 99 more files (xrEngine 30, xrGame 27,
   renderer 24, xrSound 8, xrServerEntities 7, cximage 3, xrNetServer). Each use is one of three kinds: POSIX path (becomes
   `XR_PLATFORM_POSIX`), real Apple API (stays, plus a web branch), or Windows-only (unchanged). Then delete the
   `#define XR_PLATFORM_APPLE` from the web branch of `Platform.h`.
2. **Split `PlatformApple.inl` / `.cpp`** into POSIX-common, Apple and web files. The web file now holds `xr_apple_malloc_size`,
   `xr_apple_heap_bytes_in_use`, `xr_apple_vm_info` (rename them) and `CPU::GetCLK`; the 9 `XR_PLATFORM_WEB` branches in
   `PlatformApple.cpp` (clock, command line, thread id, debugger, module path, system info, memory status) move out.
3. **Modules not attempted**: xrSound (OpenAL Soft on Web Audio), xrEngine, xrServerEntities, xrGame with Lua and luabind
   (`-fwasm-exceptions` through luabind is untested), the renderer, xrNetServer, xrXMLParser.
4. **Files**: the windowed read interface for the five `mmap` sites. `CVirtualFileRW` (save games, `FS.cpp:577`) writes through
   a `MAP_SHARED` mapping; under `NODERAWFS` the write-back on `munmap` works (probe), but OPFS has no `mmap` and needs the
   interface to turn it into reads and one write on close.
5. **Threads**: decide `PTHREAD_POOL_SIZE` and `XR_WEB_TASK_THREADS` from the full thread list; `ttapi` gets the pool of 4 instead
   of `hardware_concurrency()` workers, which is a smaller degree of parallelism than the native 12. Engine on a Worker, not the
   page's main thread (plan).
6. **Start-up**: `CPU::Detect()` busy-waits 100 ms to calibrate a "cycle counter" that is the QPC clock on web; skip it.
   `GetCommandLine()` comes from `xr_set_command_line()` in the browser.
7. **Crash handling**: no handlers. `emscripten_get_callstack` is compiled in `xrDebug_posix.cpp` but never triggered; function
   names need `-g` or `--profiling-funcs`. The error screen and log download belong to the shell.
8. **`-sGROWABLE_ARRAYBUFFERS=2`** (links and runs under Node 26, not tried in a browser) and **LTO** (`ANOMALY_WEB_LTO`, not
   built here) are to be evaluated for speed and size.
9. **`ANOMALY_WEB_NODE=OFF`** (`-sENVIRONMENT=web,worker`, no `NODERAWFS`, no `node_argv.js`) is wired in
   `AnomalyWebFlags.cmake`. The `xrcore_smoke` link line with those flags was run by hand and links (662 KB wasm, 88 KB glue);
   the result was never run, and no page loads it.
10. **Memory64** stays a switch: `PlatformWeb.inl` asserts 4-byte pointers; `xrcdb_smoke`'s LP64 case would run again there.
11. **libm**: wasm uses Emscripten's musl `sinf`, `cosf`, `powf` ..., Apple uses its own; last-bit differences between native
    and web are expected in anything that is replayed or compared across the two (saves are not affected by the tools tested here).


## Not verified

- **No browser.** Everything ran under Node 26 (V8), not Chrome or Edge, and not in a Worker with cross-origin isolation.
  The `ANOMALY_WEB_NODE=OFF` output links but was never run.
- **Large archive entries.** The reader was exercised on entries up to 2.8 MB and on mounting all 67 archives (tables of
  contents). The probe shows what a 512 MB window costs, but no real `level.*` entry of that size was read through the engine.
  `CVirtualFileReader` (`FS.cpp:599`) and `CVirtualFileRW` (`FS.cpp:577`) are not reached by any tool; only the probe
  (`cmake/web/probes/mmap_probe.cpp`) covers the mapping calls they make.
- **Speed of the allocator and of the pool.** mimalloc against dlmalloc, the 4-thread pool against GCD, and
  `GROWABLE_ARRAYBUFFERS` were not benchmarked; the timings above are of whole smoke runs on a shared machine with a game
  instance running.
- **Denormal cost on x86**, and any audio or particle behaviour that depends on denormals.
- **`emscripten_get_callstack`** output (no assertion or trap was provoked) and the clipboard, `LoadLibrary` and `ShellExecute`
  stubs.
- **Anything above xrCore plus the five modules**: sound, engine, game, scripts, renderer, network.
- **Run-to-run variation of `xrphysics_smoke`** on the Mac itself was seen but not investigated.
- **The macOS result** is for this commit: `cmake --preset macos-arm64`, `cmake --build --preset macos-arm64 -j 4` and
  `ctest --test-dir build/macos-arm64` (23 of 23 passed); plus `cmake --preset macos-arm64-release` with
  `-DANOMALY_BUILD_XRAPI=ON -DANOMALY_BUILD_XRCDB=ON -DANOMALY_BUILD_XRCPU_PIPE=ON -DANOMALY_BUILD_XRPARTICLES=ON
  -DANOMALY_BUILD_XRPHYSICS=ON`, building the six smoke tools, 6 of 6 passed. The iOS preset and the sanitizer presets were not built.

