# Lua performance: measurement and the bytecode cache

M5 of [PLAN.md](PLAN.md) for the iPad, where LuaJIT runs as an interpreter ([IPAD-JIT.md](IPAD-JIT.md)). Three parts, in the order the plan gives:
what the scripts cost per frame and in loads (the timers and the sampling profiler), a bytecode cache for the script files, and the hot paths.
Everything here is Apple only (`XR_PLATFORM_APPLE`); the Windows build does not compile it.

| Part | Where |
|---|---|
| Timers of the calls into Lua, the sampling profiler, the `lua_perf`, `lua_profile` and `lua_cache` commands | `src/xrServerEntities/script_perf.{h,cpp}` |
| Hooks: `luabind::set_call_hooks` (every luabind call), script threads, `CScriptBinder`, the A-Life update, `CScriptCallbackEx`, the explicit GC steps, `CLevel::OnFrame` | `src/3rd party/luabind/luabind/src/pcall.cpp`, `script_thread.cpp`, `script_binder.cpp`, `alife_update_manager.cpp`, `script_callback_ex.h`, `Level.cpp` |
| The bytecode cache (no engine dependency) and its glue (`CScriptStorage::load_buffer`, `do_file`, `warm_bytecode_cache`) | `src/xrServerEntities/script_bytecode_cache.{h,cpp}`, `script_storage.cpp` |
| Tests of the cache | `tools/lua_cache_test` (ctest `lua_cache_test`) |
| Measurement scripts | `tools/lua_measure.py`, `tools/render_harness/lua_measure.sh` (Mac), `tools/ios/` (iPad, below) |

## What is measured

**Timers** (`-lua_perf` on the command line, or `lua_perf on`; off otherwise, one branch per call). The wall time of every call from C++ into Lua:
luabind calls (`luabind::detail::pcall`: functors, `call_member`, callbacks), the script threads (`lua_resume` in `CScriptThread::update`), and the
explicit garbage-collector steps (`lua_gc(LUA_GCSTEP)` in `CLevel::script_gc` and the parallel `CLevel::LuaGC`). Only the outermost call is timed: Lua
calling the engine calling Lua again counts once, as a call "nested". The time includes the engine functions that Lua calls (`level.object_by_id`,
`obj:position()`, ...), which is the cost of the scripts as the frame sees it.

A call is booked to a **section**, set by the engine code that makes it:

| Section | Set in | What runs there |
|---|---|---|
| scheduler | `CScriptThread::update` | the script threads (`CScriptProcess::update` resumes one per frame) |
| binders | `CScriptBinder::*` | `bind_stalker`, `bind_monster`, ... `update`, `net_spawn`, `net_destroy`, save and load |
| A-Life | `CALifeUpdateManager::update*` | the simulator's switch and scheduled update: `se_*` scripts, smart terrains, squads |
| callbacks | `CScriptCallbackEx::operator()` | `set_callback` functions of game objects (hit, death, use, ...) |
| other | (default) | the UI, the console, level callbacks |
| GC | `CLevel::script_gc` | explicit collector steps |

`lua_perf mark <label>` ends the current period (a report line) and starts a new one; with no mark a period is 30 s. A period's line has the
frame time (mean, p95, max), the Lua time per frame (mean, median, p95, max), its share of the frame and the sections, in ms per frame:

```
* [lua-perf] label=quality_village why=mark secs=34.0 frames=1980 frame_ms=17.2 frame_p95=19.8 frame_max=41.0 lua_ms=3.100 lua_p50=2.800 ... share=18.0 sched=0.950 binder=1.200 ...
```

`Lua per frame` is the time on the main thread. Calls that other threads make (A-Life when `mtALife` runs it on a worker, the parallel collector)
are counted apart as `off_main`; they are CPU time but not on the frame's critical path.

`lua_perf watch <path> [arg]` wraps a Lua function with a timer; with `arg` it is keyed by the first argument. `SendScriptCallback` is watched
by name from the start (`actor_on_update`, `npc_on_update`, `monster_on_update`, ...): the second line of a period lists the heaviest, in ms per frame
and calls per frame.

**Loads** (`[lua-load]` lines, once after each level load): the scripts that `do_file` loaded, their size, the time reading them from the archives
(`file read`), compiling them (`compile / cache`: the parser, or the bytecode cache) and running their main chunks (`run`, which includes the
scripts they pull in), and the cache's counters.

**Profiler** (`-lua_profile` or `lua_profile on [ms]`): LuaJIT's `luaJIT_profile_start`, which samples the interpreter too (the same engine as
`-jp`). The callback walks the Lua stack of the running coroutine and counts per function (`file:line`) the *self* samples, split by VM state
(interpreter, C function called from it, garbage collector), and the *inclusive* samples (the function is somewhere in the stack), plus an
inclusive share per script file. `lua_profile report [top]` writes `[lua-prof]` lines, with the text of each function's first line.
The samples come from a process CPU timer (`SIGPROF`), so the share of "native" samples is the CPU time of every thread outside Lua and means
little; the other states are relative to the samples that landed in Lua. A profiler run is separate from a timer run: the hook slows the
interpreter a little.

## Running it

```bash
# Mac: both graphics modes, the Cordon field, the Rookie village, the Bar (a second level load); results in build/render-harness/<tag>/
tools/render_harness/lua_measure.sh -t lua-mac
tools/render_harness/lua_measure.sh -t lua-mac-prof -P            # the profiler's function tables
tools/render_harness/lua_measure.sh -t lua-mac-nocache -n         # without the bytecode cache
```

The runs are unattended like the other harness runs (offscreen, silent, their own state root). The first run needs the base save
(`tools/render_harness/run_scene.sh --newgame`). `tools/lua_measure.py summarize --engine engine.log --shell shell.log` makes the tables from
any run's logs (on the iPad `tools/ios/ipad.sh logs`).

Console and command-line summary:

| | |
|---|---|
| `-lua_perf`, `lua_perf on\|off\|mark <label>\|report\|reset\|interval <s>\|watch <path> [arg]\|unwatch` | the timers |
| `-lua_profile [-lua_profile_ms <n>]`, `lua_profile on [ms]\|off\|report [top]\|reset [label]` | the profiler (2 ms samples) |
| `-no_lua_cache`, `lua_cache info\|on\|off\|clear\|warm` | the bytecode cache |

## Bytecode cache

`CScriptStorage::do_file` reads a script, wraps it in the namespace header and compiles the result with `luaL_loadbuffer`. For the script files
(not for one-line chunks) the compile goes through `script_cache::load`:

- **Where**: `Library/Caches/Anomaly/lua` (`$lua_cache$` of the app shell's fsgame.ltx; the OS may delete it, which only costs a cold start). One
  file per script, named by a hash of the chunk name (the script's path).
- **What an entry holds**: a header with the cache format, an id of the VM's bytecode format (the LuaJIT version string and the first bytes of a
  dump of a tiny chunk, which carry the bytecode version and the FR2 / GC64, big-endian and FFI flags), the chunk name, the size and a 128-bit hash
  of the exact text that was compiled (header and all), the size and a 128-bit hash of the bytecode; then the bytecode, with its debug information
  (line numbers and names), so error messages and stack traces are those of the source.
- **A hit** needs all of it to match. The text is hashed on every load (about 8 ms for the 6.5 MB of stock scripts), so a **stale** entry is a
  miss: an edited script, an addon that replaces a stock script (the entry is rewritten with the addon's bytecode, and again when the addon goes), a
  script that changed by one byte. A cache written by another LuaJIT build or bytecode format is **incompatible**, a truncated or damaged file
  **corrupt**; the VM's own bytecode loader has the last word (`luaL_loadbuffer` of the bytecode fails: corrupt).
- **Any miss compiles from the text** and stores the entry again (a temporary file, then `rename`: two launches never see half an entry). Any
  failure to read, write or create the directory gives the same result as no cache. The result of `load` is always that of `luaL_loadbuffer(text)`.
- `lua_cache warm` runs every `*.script` of `$game_scripts$` (the stock archives, the Monolith overlay, loose addon files) through the same
  `do_file` path without running it, so the first level load is already warm. `-no_lua_cache` turns it off.

`tools/lua_cache_test` checks, in fresh Lua states and directories: miss, store and hit; a stale source; the addon override and its removal; corrupt
entries (empty, garbage, truncated at several places, extra bytes, a flipped byte, a bad magic, absurd sizes, bytecode the VM refuses); an entry of
another VM or format (including bytecode with another FR2 or endianness flag); error messages and tracebacks the same from bytecode; syntax errors;
unusable directories; warm and purge; the hash; two threads storing and loading one chunk; a large script.
`lua_cache_test --bench <dir of scripts>` times the compile of a folder of scripts from the text and through a warm cache.

## Measurements: the Mac (MacBook Pro 14", M2 Pro)

Unattended runs of `tools/render_harness/lua_measure.sh` on the `h_cordon` save (`RelWithDebInfo`): 34 s per place after 10 s of settling, five sets with the JIT
on and four with it off (`-nojit`: the interpreter, which is what the iPad runs). **The Mac was not quiet** for most of them: the port's other sessions were building
and running games on it (load average 12 to 500, one to four other `anomaly` processes). The frame times and rates of a set belong to the load of that moment (a
Quality window ran at 15 fps once), not to the machine; the Lua time of a frame is less affected, and the tables give the thread's CPU time, not the wall time. The
last set (`lua-mac5`, load 17 to 54, two other games) is the cleanest and is the one shown in full; the ranges are over all sets. The iPad is a quiet device: its
numbers follow below.

Cleanest set, JIT on (Quality capped at 60 fps, Performance at 120 on this display):

| Place | frame ms | Lua ms (CPU) | Lua p50 / p95 / max | share of the frame | binders | other | calls into Lua / frame |
|---|---:|---:|---|---:|---:|---:|---:|
| Quality, Cordon field | 17.3 | 2.09 | 1.9 / 4.1 / 69 | 12 % | 1.49 | 0.60 | 90 |
| Quality, Rookie village | 17.4 | 2.16 | 1.9 / 4.1 / 26 | 12 % | 1.43 | 0.69 | 100 |
| Quality, Bar | 16.7 | 1.72 | 1.4 / 4.1 / 17 | 10 % | 0.86 | 0.80 | 108 |
| Performance, Cordon field | 12.3 | 1.79 | 1.5 / 3.8 / 43 | 15 % | 1.30 | 0.50 | 69 |
| Performance, Rookie village | 9.3 | 1.44 | 1.1 / 3.5 / 25 | 16 % | 0.96 | 0.45 | 73 |
| Performance, Bar | 9.3 | 1.21 | 0.8 / 3.4 / 17 | 13 % | 0.64 | 0.55 | 71 |

Lua per frame on the main thread, CPU ms, range over the sets of runs:

| | Quality field | Quality village | Quality Bar | Performance field | Performance village | Performance Bar |
|---|---:|---:|---:|---:|---:|---:|
| JIT on | 2.1 to 3.2 | 2.0 to 3.7 | 1.5 to 1.7 | 1.8 to 2.6 | 1.4 to 2.9 | 1.2 to 2.2 |
| JIT off (interpreter) | 2.1 to 3.1 | 2.0 to 2.3 | 1.5 to 2.0 | 2.2 to 3.1 | 1.6 to 2.1 | 0.8 to 1.6 |

- **Lua is 0.8 to 3.7 ms of a frame, the same with or without the JIT.** The interpreter is not slower here because the time is not in interpreted bytecode: the
  profile below puts about half of it in the engine functions that the scripts call, a quarter in the garbage collector's work inside allocations and a fifth in
  bytecode.
- The binders (`bind_stalker`, `bind_monster`, `generic_object_binder`, ... `update`) are about two thirds of it, 0.6 to 2.3 ms. The per-object and per-frame callbacks
  (`actor_on_update`, `npc_on_update`, `squad_on_update`, `npc_on_hear_callback`, ... through `SendScriptCallback`) run inside them; the heaviest is `actor_on_update`
  at 0.4 to 0.6 ms, then `npc_on_hear_callback` (0.25 ms and 37 calls per frame in the Bar), `squad_on_update` (0.07 to 0.12) and `npc_on_update` (0.05).
- The script threads (`CScriptProcess`) are empty in Anomaly 1.5.3 (0.000 ms). The `set_callback` object callbacks (hit, death, ...) cost 0.01 to 0.09 ms between events.
  Lua that runs on other threads (the A-Life update runs on a worker, `mtALife`) is 0.2 to 0.6 ms per frame.
- **The collector runs on a second thread**: `mtLUA_GC` does its steps there, in parallel with the renderer (`Device.seqParallel`), 1.4 to 7.6 ms per frame, and nothing
  waits for it. The collector's work inside allocations stays on the main thread and is part of the Lua time above.
- Frames with a long Lua part exist: p95 is 3.4 to 7.4 ms and the maximum 15 to 108 ms (a frame that loads or spawns many objects).

### Where the Lua time goes (the interpreter profile)

`lua_measure.sh -P -j -S 60`, Quality, JIT off, 2762 to 4427 samples (1 ms) per place:

| VM state | Cordon field | Village | Bar |
|---|---:|---:|---:|
| bytecode in the interpreter | 20.7 % | 20.2 % | 21.8 % |
| C functions called from scripts (engine exports, `string.*`, `table.*`) | 52.2 % | 53.5 % | 52.0 % |
| garbage collector (steps inside allocations) | 27.0 % | 26.3 % | 26.1 % |

The profile is flat. The heaviest functions (self time as a share of the Lua time; engine calls included): `bind_campfire` `actor_on_update` 6 to 7 %,
`simulation_objects.get_server_entity` 3 to 6.5 % (one C call, `alife_object`), `anomaly_restrictor_update` 4.6 to 5.7 %, `actor_effects.HUD_fog` 2.9 to 4.5 %,
`xr_motivator:hear_callback` 3.6 to 5.9 %, `time_global` 1.5 to 3 %, `_g.script` `// called from engine!` wrappers 2.7 %; the cumulative `SendScriptCallback` is 31 to 34 %,
`actor_binder:update` 22 to 25 %, `motivator_binder:update` 22 %, `generic_object_binder:update` 9 to 10 %, `state_manager:update` 11 to 13 %, `sim_squad_scripted:update` 6 to 7 %.
No function is above 7 %, and half of the time is already inside C. (An earlier profile with LuaJIT's own SIGPROF clock was wrong: macOS ticks it every 10 ms of
the whole process's CPU time, and a tick that lands while the engine runs C++ is booked to the next Lua function. The sampler now runs on a thread that signals only
while the main thread is inside a call into Lua.)

### Level loads and the bytecode cache on the Mac

The h_cordon save loads 476 scripts (6.58 MB of text) at the level load: from the archives (`file read`), compiled (`compile / cache`) and run (main chunks). Four launches
per cache state, the quietest set (load 12 to 15, two other games); the load time is from the `start` command to `level ready`:

| Cache | seconds to level ready | Lua part of the load ms | file read | compile / cache | run main chunks |
|---|---:|---:|---:|---:|---:|
| off (`-no_lua_cache`) | 5.8 to 8.3 | 462 to 568 | 10 to 14 | 94 to 107 | 353 to 449 |
| cold (empty: compile and store 407 entries) | 6.4 to 7.4 | 640 to 698 | 12 to 19 | 249 to 301 | 371 to 405 |
| warm | 5.2 to 7.9 | 419 to 559 | 9 to 18 | 74 to 111 | 336 to 440 |

- **The cache does not shorten the load on the Mac.** Compiling 6.58 MB takes LuaJIT about 100 ms (66 MB/s); loading the bytecode is 10 to 15 ms of that, but reading 474 small
  files costs 59 to 91 ms there (an endpoint-security agent on this Mac makes every `open` slow: 130 to 190 µs a file), so a warm load is 74 to 111 ms against 94 to 107 from
  the text. Standalone (`lua_cache_test --bench`, the same scripts, files in the page cache) the same work is 36 to 50 ms against 100 to 135. The level load itself is 5 to 8 s
  and the scripts are 0.45 to 0.57 s of it; **running the main chunks (350 to 450 ms) is the Lua cost of a load, not compiling.**
- A cold start costs 0.15 to 0.2 s more once (the entries are written); `lua_cache warm` does that in one go.
