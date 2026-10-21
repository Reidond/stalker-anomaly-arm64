# LuaJIT's JIT on the iPad

The JIT runs on the iPad Air M3 when a debugger authorizes a code region at startup. Without one,
LuaJIT is the interpreter it was before, and nothing breaks. On the test iPad (iPad15,5, iPadOS 27.0.1)
the benchmarks ran with the JIT on and stayed stable for 60 s after the debugger had detached.

## Summary

- The M3 is a TXM device. On iPadOS 26/27 a process cannot make memory executable by itself, even
  with `get-task-allow` and a debugger attached. A debugger has to write to every 16 KB page of a code
  region, and that write authorizes the page.
- With `-DANOMALY_IOS_LUAJIT_JIT=ON` the LuaJIT build copy is patched so it runs its JIT from such a
  region. The app calls `luajit_txm_init()` before creating the first Lua state. If the region is not
  authorized, the JIT stays off.
- The default build (`ANOMALY_IOS_LUAJIT_JIT=OFF`) is unchanged: interpreter only. The Mac build keeps
  stock LuaJIT with its JIT on.
- The JIT is 2.7–3.2× faster than the interpreter across the benchmarks (geometric mean) and up to 10×
  on numeric and metatable-heavy code, but not faster on calls between Lua and C.
- Every cold launch has to be authorized again. The authorization belongs to the debugger session.

## How it works

### The TXM handshake

Sources, and what each one established:

- [StikJIT `INTEGRATION.md`](https://github.com/StikDebug/StikJIT/blob/main/INTEGRATION.md) is the
  documented app side: `brk #0xf00d`, command in `x16`, `JIT26PrepareRegion`, `JIT26Detach`, wait for
  `CS_DEBUGGED` before the first `brk`, and "an unserviced `brk` crashes the process".
- [StikDebug `universal.js`](https://github.com/StikDebug/StikDebug/blob/main/StikDebug/Scripts/universal.js)
  is the debugger side: `vAttach`, a `c` loop that decodes the `brk` immediate, `x16` command dispatch,
  and `P0=` to hand the address back in `x0`.
- [`JSDebugSupport.swift`](https://github.com/StikDebug/StikDebug/blob/main/StikDebug/JSSupport/JSDebugSupport.swift)
  shows what "prepare" does: one `M<addr>,1:69` write per 16 KB page, with the address formatted as
  nine hex digits.
- [iCube / DolphiniOS](https://github.com/Provenance-Emu/iCube/blob/develop/Source/Core/Common/MemoryUtil_iOS_LuckTXM.cpp)
  ([guide](https://github.com/Provenance-Emu/iCube/issues/11)) is a shipping allocator: the app maps its
  own R-X region, hands the address to the debugger, makes a `vm_remap` alias, runs a guarded execute
  test, detaches, and keeps working. Its notes also record two hard-won facts: addresses above 64 GiB
  are blessed at the wrong page, and StikDebug authorizes only part of a large request in one call.
- iCube's [`dolphin_jit_lldb.py`](https://github.com/Provenance-Emu/iCube/blob/develop/Source/iOS/App/Project/Scripts/dolphin_jit_lldb.py)
  and RetroArch's [`lldb_jit_bless.py`](https://github.com/libretro/RetroArch/blob/master/pkg/apple/lldb_jit_bless.py)
  do the same from lldb, so Xcode can stand in for StikDebug.
- [Play! PR 34](https://github.com/jpd002/Play--CodeGen/pull/34) measured on an A17 Pro (iOS 26.4.2) that
  `mmap(MAP_JIT)` fails with `EPERM`, `mprotect(PROT_EXEC)` is stripped, and `CS_DEBUGGED` alone is not
  enough. [ARMSX2](https://github.com/SgtBilko76/ARMSX2-3D/blob/main/common/Darwin/DarwinMisc.cpp)
  maps its own R-X region and passes it to the debugger, like iCube.

The sequence `luajit_txm_init()` runs:

1. **Wait for a debugger**: `csops(CS_OPS_STATUS)` must report `CS_DEBUGGED`. Without one, stop here.
2. **Reserve the region**: `mmap(PROT_READ|PROT_EXEC, MAP_ANON)` below 64 GiB, preferably within branch
   range of the VM.
3. **Ask the debugger**: `brk #0xf00d`, `x16` = command, arguments in `x0`/`x1`, in 16 MiB steps.

   | `x16` | meaning | arguments |
   |---|---|---|
   | 1 | prepare region | `x0` = address, `x1` = length; the address comes back in `x0` |
   | 0 | detach | none |

   The debugger writes one byte into each 16 KB page, advances `pc` past the `brk` and continues.
4. **Make a writable alias**: `vm_remap` of the same pages, then `mprotect(PROT_READ|PROT_WRITE)` on the
   alias. The RX alias is never writable; the RW alias is never executable.
5. **Verify**: execute a two-instruction function from a sample of pages (all pages with
   `LUAJIT_TXM_PROBE_ALL`). An unauthorized page fails here, behind a `SIGBUS`/`SIGSEGV` guard, not in
   the middle of the game.
6. **Detach**: `brk #0xf00d` with `x16` = 0. The pages stay authorized until the app quits.

Two conventions exist for step 3. This implementation uses the one StikJIT documents for engines that
create their own mapping: the app passes the address. The other passes `x0` = 0 and lets the debugger
allocate the region; the AetherPS4 notes report lost permission after detach with that variant, and it
gives no control over placement. It is not used here, and detach worked in every run below.

### What the LuaJIT patches change

The patches in `cmake/deps/patches/luajit/` are applied to the build copy; the submodule stays pristine.

| Patch | Change |
|---|---|
| `0001` | The arm64 backend (`lj_emit_arm64.h`, `lj_asm_arm64.h`) writes and reads every machine code word through `LJ_MCW(p)`, which adds the distance to the RW alias. LuaJIT keeps recording RX addresses, so branch offsets, trace addresses and exit stubs are computed for the address the CPU executes. |
| `0002` | `lj_txm.c`: the handshake above, a page allocator that `lj_mcode.c` uses instead of `mmap`/`mprotect` (areas never change protection), cache maintenance for the alias pair (`sys_dcache_flush` on RW, `sys_icache_invalidate` on RX), and the API in `luajit_txm.h`. |
| `0003` | `jit_init` and `jit.on()` keep the JIT off unless a region is ready. `jit.on()` does not raise an error in that case; `jit.status()` reports the truth. |

If a store into machine code were missed, it would fault at once, because the RX alias is not writable.
The same writer runs on the Mac with `-DANOMALY_LUAJIT_TXM_EMULATE=ON` (RX mapping plus `vm_remap` alias,
no debugger and no `brk`), which is how most of the work was tested before the iPad.

**Placement is a preference, not a requirement.** The task assumed the region must sit within ±128 MB of
the VM. LuaJIT's arm64 backend already falls back to indirect branches for exit stubs, C calls and the
interpreter exit (`asm_exitstub_setup`, `emit_call`, `asm_tail_fixup`), and macOS deliberately maps its
machine code far from the VM. The only hard limit is that all machine code areas reach each other with a
26-bit branch, so the region is capped at 120 MiB. On the Mac, far placement measured about 1% slower. On
the iPad the kernel does not hand out addresses near the executable (the space below the image is
reserved and the space above is occupied), so the region lands about 200 MB away, which is fine.

### Using it from the engine

```cpp
#include <luajit_txm.h>   // only exists in the patched build

// Before the first luaL_newstate(). wait_ms > 0 lets a late debugger (StikDebug) attach.
int rc = luajit_txm_init(32u << 20, 0, wait_ms);
// rc == 0: JIT usable. Otherwise the JIT stays off; luajit_txm_get_info() says why.
```

- `luajit_txm_init(size, flags, wait_ms)`: `size` 0 means 32 MiB; clamped to 64 KiB–120 MiB. Flags:
  `LUAJIT_TXM_KEEP_DEBUGGER`, `_NO_PROBE`, `_NO_BRK` (non-TXM devices), `_VERBOSE`, `_PROBE_ALL`.
- Suggested setting: **JIT: Off / Auto / Wait for StikDebug**. Auto uses `wait_ms` = 0: the JIT only
  starts if a debugger is already attached. Wait uses 30–60 s and shows a message, because the game is
  frozen while the debugger authorizes pages.
- Raise `maxmcode` (default 2 MiB) with `jit.opt.start("maxmcode=...")` to use more of the region. The
  authorized pages are resident: a 32 MiB region costs 32 MiB of the app's memory budget.
- `xrServerEntities` already turns the JIT off for `-nojit` and for the Lua Studio debugger
  (`script_storage.cpp`, `script_engine.cpp`); nothing there needs to change.
- Not covered: FFI callbacks (`lj_ccallback.c` still uses `mmap` + `mprotect`, which fails on TXM). The
  engine does not use FFI callbacks today. If an addon needs them, give their trampolines pages from the
  same region.

## Running it

### Developer: Xcode or lldb

The authorization is done by lldb with `tools/jit_lab/txm_jit_lldb.py`, the equivalent of StikDebug's
`universal.js`. It installs a stop hook that answers `brk #0xf00d`.

```bash
# Build with the JIT, install, launch stopped, attach lldb, run to the end:
cmake --preset ios-arm64-local -DANOMALY_IOS_LUAJIT_JIT=ON
xcodebuild -project build/ios-arm64/AnomalyPort.xcodeproj -scheme jit_lab \
  -destination 'generic/platform=iOS' -allowProvisioningUpdates build
tools/jit_lab/run_ipad.sh lldb --exit        # debugger stays attached
tools/jit_lab/run_ipad.sh detach --exit      # debugger really detaches after authorization
tools/jit_lab/run_ipad.sh plain --exit       # no debugger: must fall back to the interpreter
```

For your own scheme in Xcode: Scheme → Run → Options → LLDB Init File, pointing at a file that contains
`command script import <repo>/tools/jit_lab/txm_jit_lldb.py`. By default the script answers a detach
request but stays attached, so the Xcode session keeps its console and breakpoints; set
`TXM_HONOR_DETACH=1` to detach for real. Without the script, an attached debugger stops at the `brk`
and the JIT stays off once SIGTRAP is passed to the app (tested), so run with `--no-jit` or add the script.
(The command-line flow was tested; the Xcode GUI uses the same stop hook but was not run.)

### Player: SideStore and StikDebug

1. Install Anomaly with SideStore. The app must carry `get-task-allow`; a development signature has it.
   Apps signed without it cannot be debugged and never get JIT.
2. Install StikDebug (through SideStore) and finish its one-time setup: pairing file, LocalDevVPN, and
   Developer Mode enabled in Settings → Privacy & Security.
3. In StikDebug, use the bundled script **`universal.js`** for Anomaly (Scripts tab). No custom script is
   needed: the engine speaks the universal protocol (`brk #0xf00d`; `x16` 0 = detach, 1 = prepare
   region). Community walkthroughs for TXM devices also recommend turning on StikDebug's keep-alive
   options (silent audio, always run scripts); the names may differ between StikDebug versions.
4. Set Anomaly's JIT setting to **Wait for StikDebug**, then start the game from StikDebug's app list.
   StikDebug launches it, attaches and runs the script. The game freezes for a few seconds while the
   debugger writes the pages, then the debugger detaches and the game continues with the JIT on.
   (An app can also start the flow itself by opening
   `stikdebug://enable-jit?bundle-id=<id>&pid=<pid>&script-name=universal.js`; `jit_lab --stikdebug`
   does this.)
5. Starting the game normally, or after iPadOS quits it, gives the interpreter. That is the supported
   fallback and the game is playable on it; launch through StikDebug again to get the JIT back.

StikDebug is not installed on the test iPad, so this path was not run. The lldb script performs the same
steps (one-byte writes per page, `x0` returned, `pc` advanced, detach) and the app follows INTEGRATION.md,
including 16 MiB steps and the 64 GiB limit.

## Results

iPad Air 13" M3 (iPad15,5), iPadOS 27.0.1, Debug build, debugger detached after authorization. One
process runs every benchmark with the JIT off and then on; times are the best of two warm runs, "cold"
includes trace compilation. Full log: `jit_lab-ipad-final-run.log` in the session notes.

| group | benchmark | interpreter ms | JIT cold ms | JIT warm ms | speedup |
|---|---|---:|---:|---:|---:|
| numeric | nbody | 144.2 | 15.2 | 14.9 | 9.7× |
| numeric | mandelbrot | 62.6 | 12.9 | 12.7 | 4.9× |
| numeric | sieve_loops | 141.7 | 50.0 | 49.5 | 2.9× |
| numeric | fib_recursion | 8.0 | 1.2 | 1.1 | 7.1× |
| table/string | table_sort_stack | 80.0 | 58.4 | 56.5 | 1.4× |
| table/string | hash_strings | 11.7 | 8.1 | 8.0 | 1.5× |
| table/string | string_ops | 27.9 | 18.6 | 18.3 | 1.5× |
| anomaly-like | oop_metatables | 17.5 | 2.7 | 2.4 | 7.3× |
| anomaly-like | closures_varargs | 15.6 | 3.2 | 3.2 | 4.8× |
| anomaly-like | event_bus | 37.9 | 11.9 | 11.8 | 3.2× (13.5× in another run) |
| anomaly-like | ai_update | 20.8 | 2.6 | 2.4 | 8.8× |
| anomaly-like | pcall_errors | 15.6 | 12.2 | 12.2 | 1.3× |
| c-interop | lua_to_c_calls | 42.0 | 47.9 | 47.9 | 0.9× |
| c-interop | c_calls_lua | 13.9 | 12.5 | 12.2 | 1.1× |
| c-interop | nested_c_lua_c | 4.5 | 4.4 | 4.5 | 1.0× |

Geometric mean: 2.75× (3.24× in the previous run; `event_bus` switches between two trace shapes from
run to run, and it did the same on the Mac). Every JIT checksum matched the interpreter.

- **Calls between Lua and C do not speed up**, and Lua loops that call a C function (`lua_to_c_calls`) are
  about 10% slower with the JIT: LuaJIT cannot compile calls to ordinary C functions. The engine's script
  exports go through luabind, so the real game gains the most in script code that stays in Lua (AI state
  machines, class hierarchies, event dispatch, math) and little in code that is mostly engine calls.
- **Stability run**: 60.0 s, 181,468 iterations, 181,468 generated functions, 361,453 traces, 5 flushes,
  0 mismatches against the interpreter, 0 errors, with the debugger detached since startup. It mixes
  the benchmarks with freshly generated code (new traces and new machine code areas every iteration),
  flushes the JIT every 12 s, and runs the garbage collector. A second Lua state created after
  `lua_close` also compiled and ran correctly.
- **Authorization cost**: 32 MiB took 1.2–4.3 s under lldb (one byte per page, depending on host load),
  with the game frozen meanwhile. 2,048 pages became resident.
- **Mac (stock LuaJIT, JIT on)**: the same suite passes with a 2.9–3.4× geometric mean; the Mac was
  heavily loaded while measuring, so only the ratios are meaningful. The Mac build still has the JIT on.

Scenarios run on the iPad:

| scenario | result |
|---|---|
| no debugger attached | JIT off, `luajit_txm_init` returns `-NO_DEBUGGER`, all benchmarks pass |
| lldb with broker, stays attached | JIT on |
| lldb with broker, real detach | JIT on, 60 s stable (the run above) |
| app waits 60 s, debugger attaches 12 s after launch | JIT on |
| debugger attached without broker, SIGTRAP passed | handshake fails cleanly, JIT off, benchmarks pass |
| default build (`ANOMALY_IOS_LUAJIT_JIT=OFF`) | interpreter only, `jit.status()` false, no TXM API |

Not tested: StikDebug itself, the Xcode GUI, the app going to the background and returning, jetsam
under memory pressure, other iPadOS versions.

## Risks and limits

- **OS changes.** Everything relies on the debugger-write behavior of TXM that StikDebug and the
  emulators use today. Apple can close it; the interpreter fallback keeps the game playable.
- **Every launch needs the debugger.** SideStore's 7-day signing, iPadOS killing the app, and a full
  quit all drop the JIT until the next launch through StikDebug.
- **StikDebug dependence.** The wait for the debugger is a window: if StikDebug is slow or gets killed
  by iPadOS, the game falls back. The `brk` itself is only issued after `CS_DEBUGGED` is set, because an
  unserviced `brk` kills the process.
- **Memory.** The authorized region is resident (32 MiB by default). Machine code for Anomaly's scripts
  is a few MiB; the region can shrink once measured in the real game.
- **No W^X toggling.** The RW alias stays writable. That is acceptable for a sideloaded game; if it ever
  matters, `vm_protect` the alias read-only between compilations.
- **Trace nondeterminism.** Some benchmarks swing between runs because LuaJIT picks different traces.
- **Unwinding.** iOS builds use LuaJIT's internal unwinder (`LJ_NO_UNWIND`), so JIT frames need no unwind
  registration and Lua errors thrown through them work (tested with `pcall_errors`). C++ destructors
  between a `lua_error` and its `pcall` are skipped, as in the interpreter-only build today.
- **FFI callbacks** do not work on TXM devices (see above).
- **Debug build numbers.** The benchmark app was built in Debug; LuaJIT is built with its Makefile
  defaults either way, so the engine's Release numbers will differ in the C++ parts only.

## Files

- `cmake/deps/patches/luajit/0001`–`0003`: the patches. `cmake/deps/LuaJIT.cmake`: the options
  `ANOMALY_IOS_LUAJIT_JIT` and `ANOMALY_LUAJIT_TXM_EMULATE`.
- `tools/jit_lab`: the app (`main.cpp` UI, `lab.cpp` runner, `lua/bench.lua` workloads),
  `txm_jit_lldb.py` (lldb broker), `run_ipad.sh`.
