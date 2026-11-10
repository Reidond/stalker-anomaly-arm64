# Spike S1a: Anomaly on plain Lua 5.1

The web build cannot use LuaJIT (no wasm target), so the scripts have to run on PUC Lua 5.1. This spike adds that VM as a build
option on the Mac, checks what the scripts need from LuaJIT, runs the game on it and measures the cost against LuaJIT with and
without its JIT. The cost in wasm is **not** measured here (that needs the S1b toolchain).

## Verdict

**Go.** Anomaly runs on PUC Lua 5.1 on the Mac: a new game in the Cordon, the `LUA-PERF.md` scenes (Cordon field, Rookie
village, a level change to the Bar) in both graphics modes, with the same warnings as LuaJIT and no script errors. It costs no
more per frame than LuaJIT: 0.74 to 2.04 ms of Lua per frame against 0.86 to 2.01 for LuaJIT's interpreter and 1.05 to 1.72
with its JIT, the same places, two runs each. That is because the frame's Lua time is mostly engine functions and the collector:
on bytecode-heavy code Lua 5.1 is 1.5 to 2.1 times slower than LuaJIT's interpreter and about 5.5 times slower than its JIT, but
bytecode is only 20 to 30 % of the game's Lua time.

It does not work with stock Lua 5.1. The vendored copy carries eight changes that make it behave as LuaJIT where the scripts or the
engine depend on it (`src/3rd party/lua-5.1.5/README.anomaly.md`). Two of them, and a fix in `lmarshal.c`, were found only by
running the game: a `for` loop with a step of 0 that froze every new game, C code that pushes without `lua_checkstack`
(`marshal` crashes on deeply nested tables), and `marshal` crashing on a save of the other VM. The coroutine risk of the plan is not real for the game: no stock script yields,
and no script thread runs.

What it costs besides time:

- **Memory:** the Lua heap is 25 to 30 % larger (54 to 59 MB against 43 to 46 MB; peaks 68 against 58), 5.1's values and table
  slots being twice LuaJIT's size on arm64.
- **Saves:** a save made by one VM does not load on the other (`game.CTime` in every save is kept as bytecode). The web build
  cannot import a save from the Mac or the iPad as things stand.
- **Errors** are C++ throws: about 8 times a `longjmp`, the same cost as LuaJIT's errors on this Mac. They are rare in play.
- Script compiles take 36 % longer (83 against 61 ms for the stock scripts), about 20 ms of a 3 to 9 s level load.

Not measured: anything in wasm (the VM compiled to wasm32, its exceptions, its allocator), the iPad, long play, fights.

## What was built

- `ANOMALY_LUA=luajit|lua51` (`cmake/Dependencies.cmake`; default `luajit`, which builds and behaves as before). Every target
  links `Anomaly::Lua` instead of `LuaJIT::luajit`; code that differs tests `ANOMALY_LUA51`, which the Lua 5.1 target defines.
- `src/3rd party/lua-5.1.5`: PUC Lua 5.1.5 and LuaBitOp 1.0.3, MIT, in-tree (no submodule), built by `cmake/deps/Lua51.cmake`
  **as C++**: `lua_error` throws a C++ exception instead of `longjmp`. The first commit has the upstream files unchanged; the
  changes are marked `ANOMALY` and listed in `README.anomaly.md` there.
- `-DANOMALY_LUA=lua51` needs no `external/LuaJIT`; `tools/jit_lab` (a LuaJIT JIT lab) is not built then.
- Outside the spike's own files: `cmake/Dependencies.cmake` (the option, its own commit), one guard each in
  `xrRenderDX10/dx10ResourceManager_Scripting.cpp` and `tools/jit_lab/CMakeLists.txt`, the link line of
  `tools/platform_probe/CMakeLists.txt`, and `lmarshal.c` (a load check that changes LuaJIT builds too: an error instead of a crash).
- `lua_profile` prints `[lua-stall]` stacks (below) and `lua_perf`'s period line gains the Lua heap; both VMs.

Checks, with the options at their defaults and with `lua51`: `ctest` passes in the default `macos-arm64` preset (LuaJIT, Debug,
23 of 23), in `macos-arm64-release` with every module (LuaJIT, 32 of 32) and in the same with `-DANOMALY_LUA=lua51` (32 of 32).

```bash
cmake --preset macos-arm64-release -B build/macos-arm64-release-lua51 -DANOMALY_LUA=lua51 -DANOMALY_BUILD_ALL_MODULES=ON
cmake --build build/macos-arm64-release-lua51 -j 4
ctest --test-dir build/macos-arm64-release-lua51
```

## The audit

### LuaJIT in the engine, the tools and the overlay scripts

| Where | What | With `lua51` |
|---|---|---|
| `script_storage.cpp` `CScriptStorage::reinit` | `luaL_openlibs` (LuaJIT's also opens `bit`, `jit`, `ffi`); `-nojit` calls `luaJIT_setmode` | `luaL_openlibs` of the vendored Lua opens `bit` (LuaBitOp, the same API) as well; no `jit`, no `ffi`; `-nojit` is ignored |
| `script_perf.cpp` | `luaJIT_profile_start/stop` (the `lua_profile` sampler) | Same sampler clock; the signal handler reads the VM state the vendored VM keeps and arms a count hook that walks the stack at the next instruction, so the `[lua-prof]` tables have the same columns |
| `script_bytecode_cache.cpp` | LuaJIT version string and 5-byte dump header as the VM id | `LUA_RELEASE` and the 12-byte 5.1 header (endianness, sizes of int, size_t, Instruction, lua_Number). Still off by default |
| `lua-extensions/base.cpp` | `luaopen_jit`, `luaopen_ffi` when `lua_debug` is on | Skipped |
| `lua_extensions.cmake` | `luaL_reg=luaL_Reg` (LuaJIT 2.1 dropped the 5.0 name) | Not needed: 5.1's `lauxlib.h` has it |
| `xrRenderDX10/dx10ResourceManager_Scripting.cpp:351` (not in the plan's list) | `luaopen_jit` on the renderer's own Lua state for the `.s` shader scripts | Skipped; none of the 167 stock and repository `.s` files uses `jit` or `bit` |
| `script_engine.cpp:98,391` | `luaJIT_setmode` | Only under `USE_LUA_STUDIO` (Windows); not compiled here |
| `gamedata/scripts/global.lua:2` | `jit.opt.start(2)`, unguarded | `if jit then ... end`. The file only runs with `lua_debug 1` |
| `gamedata/scripts/axr_main.script:21` | `jit.os == "OSX"` turns on the appdata copy of `axr_options.ltx` | Without `jit`, a `/` in `package.config` (a POSIX build: the Mac, and the web build) turns it on; LuaJIT builds take the old branch unchanged |
| `gamedata/scripts/imgui_shader_bus.script:9` | `bit.bor` | Works unchanged with LuaBitOp |
| `tools/luabind_smoke`, `lua_ext_smoke`, `lua_cache_test` | LuaJIT version strings, `luajit.h`, the LuaJIT bytecode header | Pass on both VMs. `lua_ext_smoke` also checks, with values taken from LuaJIT, every LuaJIT behaviour the vendored Lua reproduces; `luabind_smoke` that a Lua error runs the destructors of the C++ frames it crosses; `lua_cache_test --check` compiles a folder of scripts |
| `tools/platform_probe` | Links LuaJIT to report the JIT state | Links `Anomaly::Lua`; on 5.1 it reports that there is no `jit` module |
| `tools/jit_lab` | LuaJIT's JIT on the iPad | Not built with `lua51` |
| `luabind` | Lua 5.1 API only | No change |

### What the scripts expect from LuaJIT

The stock scripts (414 files of `db/configs/scripts.db0`, unpacked with `tools/xrdb_unpack`) and the repository's overlay
(`gamedata/scripts`, 75 `.script` and 4 `.lua`) were searched for every LuaJIT 2.1 extension of Lua 5.1, then compiled with
the vendored Lua (`lua_cache_test --check`, which wraps each file in the namespace header `CScriptStorage` uses):

| LuaJIT behaviour | Used by | Stock 5.1 | Vendored 5.1 |
|---|---|---|---|
| `\xXX` escapes | 1,355 in `imgui_helper.script` (UTF-8 icon glyphs) | Compiles, but `"\xef"` is the text `xef`: wrong strings, no error | Patched in (`llex.c`), with `\z` |
| `string.format("%s", v)` for any `v` | Not countable statically: log and UI strings pass booleans and `nil` | Error "string expected, got boolean" | Patched in (`lstrlib.c`): `__tostring`, then `tostring`'s text |
| `math.random` / `math.randomseed` | 683 calls, 9 seeds | Another generator (C `rand()`), bounds truncated to int, "interval is empty" error when m > n (LuaJIT returns a number) | LuaJIT's generator and argument handling ported (`lmathlib.c`): a seed gives LuaJIT's numbers |
| `bit.*` | `imgui_shader_bus.script` | No `bit` | LuaBitOp, opened by `luaL_openlibs` |
| `jit.*` | `global.lua`, `axr_main.script`, `LuaPanda.lua` (debugger) | No `jit` | Guarded (above) |
| `[[` inside a long string | none found | Error when nested | Accepted, as LuaJIT (`LUA_COMPAT_LSTR` off) |
| `goto`, `::label::`, `\u{}`, `ffi`, `table.move/pack`, `math.log(x, base)`, `string.rep(s, n, sep)`, `xpcall(f, h, args)`, `load(string)`, `select(-n)` | none (`slaxml.script` has `unpack or table.unpack`) | | Not added |
| Yield across `pcall`, metamethods, iterators, C calls | see the coroutine section | Error | Not added |

Compile check: every one of the 492 files compiles on the vendored 5.1, on stock 5.1 and on LuaJIT except
`lua_help.script`, which is API documentation rather than Lua and is rejected by LuaJIT too (it is never loaded).

Differences left in, none of which shows as an error:

- **`pairs` order and `#` of a table with holes** follow 5.1's table layout and string hash, not LuaJIT's. Code that depends on
  the order of `pairs` (which squad or item comes first) can pick differently; with the same seed the run is not the same game
  as on LuaJIT.
- **`coroutine.running()`** in the main thread returns `nil` (LuaJIT: the main coroutine and `true`). Only `LuaPanda.lua` asks.
- **Numbers to text** use `%.14g` in both; integer-valued doubles print the same.
- **The allocator** is the system `malloc` through Lua's `l_alloc` (LuaJIT has its own).
- **C++ exceptions**: a C++ exception thrown by an engine function and not caught before `lua_pcall` becomes the error
  "C++ exception" (`ldo.c` patch), as on LuaJIT.

### The coroutine question

Plain 5.1 cannot yield across `pcall`, a metamethod, a `for ... in` iterator or a C function; LuaJIT can (except through C
functions such as `table.sort`'s comparator). Shown on both VMs with the same chunk:

| Yield inside | LuaJIT 2.1 | Lua 5.1 |
|---|---|---|
| `pcall(function() coroutine.yield() end)` | works | "attempt to yield across metamethod/C-call boundary" |
| an `__index` function | works | same error |
| a `for ... in` iterator function | works | same error |
| `table.sort`'s comparator | "attempt to yield across C-call boundary" | same error |
| a plain Lua call chain | works | works |

Where coroutines are used:

- **Engine.** `CScriptThread` (a `lua_newthread` resumed once a frame by `CScriptProcess`) is the only C++ `lua_resume`. Its
  threads come from `script.ltx` (`[single] script =` is empty in 1.5.3) and from a level's `[level_scripts]` (none of the 34
  `level.ltx` files has one), so no script thread runs in Anomaly; LUA-PERF.md measured the scheduler section at 0.000 ms.
  luabind's `resume_function` and its `yield` policy are not used by any binding.
- **Stock scripts.** `coroutine.*` appears only in `_g.script`'s `wait()` and `wait_game()`, inside a `--[[ does not work? ... --]]`
  comment. No stock script yields.
- **Repository overlay.** `LuaPanda.lua` (the VS Code debugger, `lua_debug 1` only) wraps `coroutine.create` and reads
  `coroutine.running()`'s second value; not tested on 5.1. `sweep_driver.script` and the `sweep_check_*` phases
  (`tools/sweep`, test tooling) run each phase as a coroutine and yield inside `pcall` (`try()`, `wait_for()`, `pcall(function()
  press(...) end)`): **these break on 5.1** with the error above. They are not part of the game.
- **In the game**: no "attempt to yield across" error in any Lua 5.1 run (two new games, four measurement launches, the profiler run).

**coxpcall** (a `pcall` that runs the function in its own coroutine and passes yields through) fixes the sweep driver's case.
Measured on this Mac, 10^6 calls of a trivial function, best of three:

| | `pcall` | `copcall` | extra per call |
|---|---:|---:|---:|
| Lua 5.1 (vendored, C++) | 0.043 µs | 0.469 µs | 0.43 µs |
| LuaJIT interpreter | 0.010 µs | 0.173 µs | 0.16 µs |
| LuaJIT JIT | ~0 | 0.145 µs | 0.15 µs |

Since nothing in the game yields across a boundary, the fallback is only needed for the sweep driver (a local `copcall` there).

### What running the game found

The static audit missed three differences; running a new game and the measurement scenes on Lua 5.1 found them. Each one is
fixed in the vendored Lua or in `lmarshal.c` and has a check in `lua_ext_smoke`, which fails on stock 5.1:

1. **A `for` loop with a step of 0 never ended.** Every new game froze on the actor's first update, the main thread spinning in
   bytecode. The `[lua-stall]` report (below) pointed at `item_radio.script:408`, `for i=1,size,(size/5)`, which runs with
   `size = 0` for a level with 15 or fewer stashes. LuaJIT takes a step of 0 as positive, so `for i=1,0,0` runs no iteration; 5.1
   takes it as negative, and the loop never ends. `lvm.c` now uses LuaJIT's rule. (A NaN step still differs: nobody writes one.)
2. **C code that pushes without `lua_checkstack` overruns the stack.** LuaJIT grows the stack on every push from C; 5.1 gives a C
   function 20 slots and trusts it. `lmarshal.c` pushes a few slots per level of nested tables and never checks: found while
   chasing the save crash below, a round trip of 40 nested tables crashes stock 5.1 and passes on LuaJIT. `api_incr_top` in
   `lapi.c` now grows the stack like LuaJIT. This also covers any other C code (engine exports, luabind) that relies on LuaJIT
   here; none was found to, but nothing checks it either. Whether a real save nests deep enough is not known.
3. **Saves are not portable between the two VMs.** `_g.script` gives `game.CTime` a `__persist` method, so every save's `.scoc`
   holds CTime restore closures, and `marshal` writes a closure as **bytecode of the VM that saved it**. Lua 5.1 refuses LuaJIT
   bytecode and the other way round; `lmarshal.c` ignored the failure of `lua_load`, decoded on with the error message where the
   function should be, and crashed (`SIGSEGV` in `mar_decode_value`). It now raises "marshal: cannot load a function written by
   another Lua VM", which the engine treats as a fatal script error: the load stops (the runs below).
   The same was already true between the Windows build (LuaJIT 2.0) and this port (LuaJIT 2.1 GC64), whose bytecode formats differ.

The `[lua-stall]` report is new in `script_perf.cpp` (both VMs): with `-lua_profile` on, a call into Lua that has run for more than
5 s prints its Lua stack to the log once. lldb could not attach to the hung game; this is how the `for` loop was found.

### Runs on Lua 5.1

| Run | Result |
|---|---|
| New game from the main menu (`Start` at the unattended 1280x720 window), save as `h_cordon` | Level ready after 10.8 to 13.4 s, saved (before the `for` fix: frozen on the first update) |
| `LUA-PERF.md` scenes, Quality and Performance, two launches each, on Lua 5.1's own save | All places and the change to the Bar played; the same warnings as LuaJIT (`stop_hud_motion`, 2 per launch on both VMs), no script errors, no `[lua-stall]`, no "yield across" |
| Profiler run, Quality | Same as above |
| The LuaJIT build loads Lua 5.1's save, and the other way round | Fatal script error at the load, "marshal: cannot load a function written by another Lua VM" (before the `lmarshal.c` fix: `SIGSEGV`) |

## Measurements

The method and scenes of `docs/ios-port/LUA-PERF.md`, on this Mac (MacBook Pro 14", M2 Pro), `RelWithDebInfo`, unattended
(`tools/render_harness/lua_measure.sh`, 34 s per place after 10 s of settling, Quality capped at 60 fps and Performance at 120).
The same timers in all three configurations: the `lua_perf` timers around every call from C++ into Lua (main thread, CPU time),
the `[lua-load]` counters, and the `lua_profile` sampler (1 ms, only while the main thread is inside a call into Lua). The Lua
heap is new in the period line (`heap_kb`, `heap_max_kb`: `LUA_GCCOUNT` at the end of each frame). Two launches per
configuration and mode, 04:22 to 05:12; no other game was running when a launch started, the load average was 3.5 to 22.

**The saves differ.** A save of one VM does not load on the other, so LuaJIT (with and without the JIT) ran on a new game saved
by LuaJIT and Lua 5.1 on a new game saved by Lua 5.1, both made the same way a few minutes apart. The scenes teleport the player
to fixed places with fixed weather, but the A-Life population differs: calls into Lua per frame vary as much between the two saves
as between the runs of one save.

### Lua per frame (main thread, CPU ms; two launches)

| Place | LuaJIT, JIT on | LuaJIT, `-nojit` | Lua 5.1 |
|---|---:|---:|---:|
| Quality, Cordon field | 1.61 / 1.64 | 1.54 / 1.67 | 1.16 / 1.30 |
| Quality, Rookie village | 1.61 / 1.63 | 1.61 / 1.69 | 1.57 / 1.80 |
| Quality, Bar | 1.72 / 1.66 | 2.01 / 1.72 | 1.63 / 2.04 |
| Performance, Cordon field | 1.18 / 1.05 | 1.06 / 0.93 | 0.85 / 0.74 |
| Performance, Rookie village | 1.14 / 1.10 | 1.18 / 0.96 | 0.86 / 0.93 |
| Performance, Bar | 1.41 / 1.05 | 1.24 / 0.86 | 0.75 / 0.79 |
| **Mean of the 12** | **1.40** | **1.37** | **1.20** |
| Lua p95 per frame, ms | 2.6 to 4.3 | 2.5 to 4.9 | 2.1 to 4.2 |
| Calls into Lua per frame (Quality / Performance) | 66 to 130 / 46 to 90 | 66 to 149 / 40 to 80 | 85 to 141 / 47 to 66 |
| Collector steps on the second thread, ms per frame | 2.3 to 3.3 | 1.9 to 3.4 | 1.2 to 2.2 |
| Lua heap, mean of a place (largest), MB | 43.5 to 45.9 (55.5) | 42.9 to 45.8 (58.5) | 53.7 to 59.3 (68.4) |

Every configuration held its frame cap in every place (16.7 and 8.4 to 8.6 ms), except two Performance places of one LuaJIT launch each (9.2 and 9.4 ms). Lua
5.1's mean is 12 % under LuaJIT's, which the different save can explain; the claim this supports is "no slower", not "faster".

### Where the Lua time goes (profiler, Quality, share of the samples inside Lua)

| | LuaJIT, JIT on | LuaJIT, `-nojit` | Lua 5.1 |
|---|---:|---:|---:|
| Bytecode (interpreter; LuaJIT: plus compiled traces and the JIT compiler) | 12 to 14 % + 7 to 9 % + 0.4 to 1.3 % | 20 to 22 % | 31 to 33 % |
| C functions called from Lua (engine exports, `string.*`, ...) | 46 to 53 % | 44 to 51 % | 37 to 54 % |
| Collector inside allocations | 26 to 31 % | 28 to 36 % | 15 to 32 % |

1,900 to 2,900 samples per place. The function tables are the same on all three (`bind_campfire` `actor_on_update`,
`simulation_objects.get_server_entity`, `anomaly_restrictor_update`, `HUD_fog`, ...; no function above 9 %). Lua 5.1's
collector share is lower in the field (15 against 29 to 32 %) and the same in the Bar; its explicit steps on the second thread
are cheaper too. Not investigated: the larger heap means fewer collections for the same allocation rate.

### Level loads (the Lua part, ms)

| | LuaJIT, JIT on | LuaJIT, `-nojit` | Lua 5.1 |
|---|---:|---:|---:|
| Loading the save: total (compile / run main chunks) | 442 to 548 (91 to 97 / 342 to 408) | 409 to 520 (73 to 98 / 324 to 406) | 463 to 550 (98 to 118 / 358 to 417) |
| Change to the Bar: total | 300 to 400 | 320 to 381 | 382 to 415 |
| The 414 stock scripts compiled standalone (`lua_cache_test --bench`, best of 5) | 60.8 | | 82.6 |
| Their bytecode cache on disk | 5.1 MB | | 8.4 MB |

481 scripts (6.7 MB) per load. Running the main chunks costs the same; compiling costs 20 to 35 % more; the level load itself is 3
to 9 s.

### The VM alone (jit_lab's workloads)

`tools/jit_lab/lua/bench.lua` (15 workloads, the checksums agree on every VM) run by a small driver against each VM, best of three,
two rounds, load average 3 to 4, ms:

| Workload | LuaJIT, JIT on | LuaJIT interpreter | Lua 5.1 (vendored, C++) | Lua 5.1 stock, C |
|---|---:|---:|---:|---:|
| nbody | 16.6 | 174.3 | 413.5 | 367.3 |
| mandelbrot | 14.5 | 74.8 | 77.8 | 79.4 |
| sieve_loops | 47.7 | 182.1 | 239.0 | 236.4 |
| fib_recursion | 1.5 | 9.2 | 14.6 | 14.9 |
| table_sort_stack | 54.9 | 88.4 | 124.1 | 123.7 |
| hash_strings | 11.2 | 16.1 | 58.9 | 61.8 |
| string_ops | 23.1 | 36.9 | 67.4 | 68.3 |
| oop_metatables | 3.1 | 21.0 | 36.8 | 36.8 |
| closures_varargs | 3.8 | 16.1 | 34.7 | 34.1 |
| event_bus | 3.9 | 41.3 | 57.6 | 58.6 |
| ai_update | 2.4 | 23.6 | 31.5 | 32.2 |
| pcall_errors (an error thrown and caught per call) | 383.2 | 383.1 | 414.1 | 54.4 |
| lua_to_c_calls | 51.1 | 40.3 | 51.9 | 54.2 |
| c_calls_lua (C calling Lua through `lua_pcall`) | 13.2 | 14.9 | 31.8 | 34.5 |
| nested_c_lua_c | 4.6 | 4.5 | 10.5 | 10.9 |

Geometric means: Lua 5.1 against LuaJIT's interpreter 1.63 (the "anomaly-like" four), 1.51 (numeric), 2.11 (tables and strings),
1.86 (C interop); against the JIT 5.5 over all but `pcall_errors`. Compiling Lua as C++ and the `ANOMALY` changes cost nothing
(0.99 of stock C 5.1) except errors: a C++ throw is 7.6 times a `longjmp`, the cost LuaJIT's own errors have on this Mac.

## What this means for the plan

- **Risk 2 (Lua speed) is small on native code.** The frame's Lua time on 5.1 is LuaJIT's. The open question moves to wasm: how
  much slower the 5.1 interpreter and its C++ exceptions are in wasm32 than native. Bytecode is a third of the Lua time on 5.1:
  an interpreter twice as slow in wasm would add about 0.4 ms to a 1.2 ms frame; the engine functions the scripts call (half the
  time) slow down with the rest of the engine, which is S1b's question, not Lua's.
- **The coroutine risk is not real for the game**: nothing yields across `pcall` or C, and no script thread runs. The sweep driver
  (test tooling) needs a `copcall` if it is ever run on web.
- **New: saves are per VM.** The plan's "saves export and import" works between web players, not from the Mac or the iPad. Making
  them portable needs `game.CTime` saved as data rather than as a closure (a `marshal` extension that lets `__persist` return a
  table with a constructor name, or a CTime case in `lmarshal.c`); that is a task of its own, together with a version tag in
  `.scoc`.
- **New: the web build must use the vendored Lua, not any other 5.1.** The `for`-step and stack-growth changes are correctness
  fixes the game needs.
- **Memory:** about 12 MB more Lua heap on arm64; less on wasm32, where pointers halve but `TValue` stays 16 bytes. S3 should count
  the Lua heap separately.
- **wasm32 specifics:** `string.format`'s `%d` and `%x` use 64-bit integers (`LUA_USELONGLONG`). `lua_Integer` stays
  `ptrdiff_t`, 32-bit on wasm32; by a grep that is harmless: outside the Lua sources the only users are one `lua_pushinteger` of a
  small index (`object_factory_script.cpp`) and `lmarshal.c`'s reference numbers; luabind converts through `lua_Number`.

Proposed changes to `PLAN.md` (not made here): under Scripts, replace "Scripts that rely on it fail at runtime, so coroutine use
is audited in spike S1a (verify)" with the result above; add "saves are not portable between LuaJIT and Lua 5.1" to Risks and
to the saves item of Files and game data; note the vendored Lua as the only supported 5.1. `docs/ios-port/LUA-EXTENSIONS.md`
item 2 says the stock `.scoc` data holds no functions; it holds the CTime closures, so its LuaJIT 2.0 / 2.1 note applies to
every save.

## How to reproduce

```bash
# builds (see "What was built"); the two smoke tools show the LuaJIT behaviour checks
build/macos-arm64-release-lua51/tools/lua_ext_smoke/lua_ext_smoke
build/macos-arm64-release-lua51/tools/luabind_smoke/luabind_smoke

# compile audit of the stock scripts (unpacked with tools/xrdb_unpack into a scratch folder) and the overlay
build/macos-arm64-release-lua51/tools/lua_cache_test/lua_cache_test --check <unpacked>/scripts gamedata/scripts

# a new game as the base save, then the measurements; ANOMALY_APP picks the build (one game at a time on the Mac)
ANOMALY_APP=$PWD/build/macos-arm64-release-lua51/src/Platform/App/anomaly.app tools/render_harness/run_scene.sh -l 300 <scene>
ANOMALY_APP=$PWD/build/macos-arm64-release-lua51/src/Platform/App/anomaly.app tools/render_harness/lua_measure.sh -t l51
ANOMALY_APP=$PWD/build/macos-arm64-release-lua51/src/Platform/App/anomaly.app tools/render_harness/lua_measure.sh -t l51-prof -P -m quality
tools/render_harness/lua_measure.sh -t lj          # LuaJIT, JIT on (build/macos-arm64-release)
tools/render_harness/lua_measure.sh -t ljoff -j    # LuaJIT, -nojit
```

`<scene>` is the new-game timeline of `tools/render_harness/lua_load.sh` (`Start` at 390,317) followed by
`30 console run_string exec_console_cmd("save h_cordon")`: the repository's `scenes/_newgame.script` clicks at 640,300, which is not
`Start` at the unattended window size, so its new game never starts. Each VM needs its own `build/render-harness/saves/h_cordon.*`.

## Not verified

- Anything in wasm: the cost of the interpreter, of C++ exceptions (`-fwasm-exceptions`), of the allocator.
- The iPad, long play, fights, other levels than the Cordon and the Bar, the late game (a grown A-Life).
- That the larger heap and the different `pairs` order change nothing in play beyond the numbers above: no gameplay check was
  made, only that the scenes run without errors.
- `lua_debug 1` (LuaPanda, `global.lua`), the bytecode cache in a game run on 5.1 (only its tests), the sweep driver on 5.1.
- The Windows build: not built here; the changes outside `ANOMALY_LUA51` code are the `lmarshal.c` load check and comments.
- The LuaJIT build's `make -j8` inside `cmake/deps/LuaJIT.cmake` ran as it always does, above the `-j 4` the brief asks for, for
  the few seconds LuaJIT takes to build.
