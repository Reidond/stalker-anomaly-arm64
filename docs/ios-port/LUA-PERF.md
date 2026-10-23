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
| `-no_lua_cache`, `-lua_prefetch`, `lua_cache info\|on\|off\|clear\|warm` | the bytecode cache, and its opt-in read-ahead |

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
- **Read-ahead (opt-in, `-lua_prefetch`).** Opening and reading ~475 small files is most of the time of a hit (53 of 60 ms on the iPad, 59 to 91 ms on the Mac), more than decoding
  the bytecode (6 ms on the iPad) and as much as parsing the text (65 ms). With the switch, `set_directory` starts a thread that reads the directory's entries into memory; a load takes
  its entry from there (once) and reads the file only when the thread has not got to it. The engine calls `set_directory` again at every new Lua state, which refills the read-ahead.
  An entry read ahead is checked like any other, so one that was replaced or damaged afterwards is a stale or corrupt entry, never a wrong script. **It is off by default**: the iPad runs below are of the default, the read-ahead is verified on the Mac only, and it adds a thread next to the engine's allocator and
  collector threads. Two of the four ASan smoke sweeps run with it stalled after the Cordon to Garbage level change (the game kept running at 5 to 15 fps, the sweep driver never
  advanced, nothing in the log). That is probably not the read-ahead: the same stall happened with it off (one of three runs with the cache on, none of one with the cache off) and in 5 of
  22 smoke runs of other sessions on builds without any of these changes (`render-fixes`, `xrgame-triage`: 1500 to 2350 s, no `DONE plan smoke`). It is a flake of the sweep or the
  game, open and not looked at here.
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

The h_cordon save loads 476 scripts (6.58 MB of text) at the level load: from the archives (`file read`), compiled (`compile / cache`) and run (main chunks). Three or four launches
per cache state, the quietest sets (load 12 to 15, two other games); the load time is from the `start` command to `level ready`:

| Cache | seconds to level ready | Lua part of the load, ms | file read | compile / cache | run main chunks |
|---|---:|---:|---:|---:|---:|
| off (`-no_lua_cache`) | 5.8 to 8.3 | 462 to 568 | 10 to 18 | 91 to 107 | 353 to 449 |
| cold (empty: compile and store 407 entries) | 5.8 to 7.4 | 588 to 698 | 9 to 19 | 235 to 301 | 344 to 405 |
| warm, one file per script read on demand | 5.2 to 7.9 | 419 to 559 | 9 to 18 | 74 to 111 | 336 to 440 |
| warm, with the read-ahead (`-lua_prefetch`) | 5.9 to 8.7 | 407 to 432 | 9 to 17 | 16 to 21 | 379 to 395 |

- **Without the read-ahead the cache did not shorten the load**: loading the bytecode is 10 to 15 ms, but 474 `open`+`read` pairs cost 59 to 91 ms (an endpoint-security agent on this
  Mac makes every `open` slow, 130 to 190 µs a file), so a warm load was 74 to 111 ms against 91 to 107 from the text. **With it a warm load is 16 to 21 ms**: 70 to 90 ms saved
  of a 5 to 8 s level load (473 of 474 hits came from memory), but see the stall above. Standalone (`lua_cache_test --bench`, the same scripts, files in the page cache): 37 to 50 ms against 103 to 136.
- The level load is 5 to 8 s and the scripts are 0.4 to 0.57 s of it. **Running the main chunks (340 to 450 ms) is the Lua cost of a load, not compiling.**
- A cold start costs 0.15 to 0.2 s more once (the entries are written); `lua_cache warm` does that in one go.
- The seconds to level ready vary by more than the cache can move them (load and thermal state of the Mac).

New game from the main menu (two loads, as the Cordon save): the Lua part is the same 340 to 520 ms. The shell's `level ready` fires after the first stage, so the new-game time
is read from the frame rate: the first 10 s window at 60 fps ends 27 to 29 s after the click on Start on the iPad.

## Measurements: the iPad (Air 13-inch, M3, 8 GB, 60 Hz), LuaJIT interpreter

`tools/ios/lua_measure.sh`, `ipad_test` save, the signed `RelWithDebInfo` build, a two-minute warm-up in the village first, then 34 s per place. Quality is render scale 0.6 at the even
30 fps cap, Performance is 0.42 at 60 (SETTINGS.md); the frame cap is set by hand in the script (a cap a previous run saved in `user.ltx` stays on when the mode changes).
Thermal state was Nominal throughout. Another session also launched the app on the iPad in this time, which made some launches fail (error 10002) and be repeated; every run listed
reached its `quit` and is a complete one.

| | Quality field | Quality village | Quality Bar | Performance field | Performance village | Performance Bar |
|---|---:|---:|---:|---:|---:|---:|
| Frame rate (windows of 10 s) | 29.9 | 29.9 | 30.0 | 60.0 | 59.8 (59.4) | 60.0 |
| Frames on the beat (33 ms / 17 ms) | 98.5 % | 99.3 % | 100 % | 99.6 % | 98.5 % | 99.8 % |
| Frame p95 / longest ms | 37.4 / 97 | 37.4 / 83 | 36.6 / 34 | 18.8 / 20 | 19.9 / 61 | 19.2 / 22 |
| **Lua per frame, ms (CPU)** | **2.64** | **3.58** | **2.85** | **0.88** | **1.96** | **1.11** |
| Lua p50 / p95 / max ms | 2.3 / 5.0 / 18 | 3.2 / 6.1 / 25 | 2.4 / 5.5 / 20 | 0.8 / 1.9 / 5 | 1.7 / 4.0 / 44 | 0.7 / 3.1 / 12 |
| Lua share of the frame budget (33.3 / 16.7 ms) | 8 % | 11 % | 8 % | 5 % | 12 % | 7 % |
| binders / other / callbacks, ms | 1.70 / 0.84 / 0.08 | 2.02 / 1.42 / 0.10 | 1.16 / 1.56 / 0.09 | 0.63 / 0.23 / 0.01 | 1.21 / 0.66 / 0.09 | 0.71 / 0.38 / 0.02 |
| calls into Lua per frame | 149 | 216 | 190 | 69 | 120 | 63 |
| collector steps on the second thread, ms | 3.8 | 3.3 | 3.6 | 4.1 | 2.8 | 3.4 |
| `actor_on_update` / `npc_on_hear_callback`, ms | 0.53 / 0.06 | 0.51 / 0.22 | 0.32 / 0.64 (77 calls) | 0.24 / 0.01 | 0.41 / 0.07 | 0.30 / 0.05 |

- **The budget holds in both modes.** Quality keeps its 30 fps with 98.5 to 100 % of the frames on the 33 ms beat and Lua at 8 to 11 % of the budget; Performance keeps its 60 with
  98.5 to 99.8 % of the frames on the 17 ms beat and Lua at 5 to 12 % of the budget (the village is the busiest and the one with the longest Lua frames: p95 4.0 ms, one frame of 44 ms
  that shows as the 61 ms longest interval). The iPad's interpreter is as fast as the Mac's in Lua time per frame (compare the Mac table); the cost is engine calls and the collector,
  not bytecode.
- Lua per frame is higher in Quality because a Quality frame covers two Performance frames of game time: per second of play the cost is 79 to 107 ms in Quality and 53 to 118 ms in
  Performance, 5 to 12 % of one core.
- **Where it goes** (profile, Quality, 1316 to 2720 samples of 1 ms): bytecode in the interpreter 22 to 24 %, C functions called from scripts 48 to 54 %, the collector inside allocations
  24 to 29 %. The profile is as flat as the Mac's: `simulation_objects.get_server_entity` 2.5 to 7.3 % (one `alife_object` call), `anomaly_restrictor_update` 3.8 to 5.2 %, `bind_campfire` `actor_on_update`
  4.9 to 5.0 %, `HUD_fog` 2.4 to 3.2 %, `hspairs` 1.8 to 4.6 %, `get_object_squad` 1.8 to 3.5 %, `pick_section_from_condlist` 1.9 to 2.8 %, `time_global` 1.8 to 2.3 %, the `npc_on_hear_callback` chain 8 %
  in the Bar (18 % with the binder's `hear_callback` around it). Cumulatively `SendScriptCallback` is 30 to 34 % and `generic_object_binder:update` 10 %.

### Level load and new game on the iPad

The `ipad_test` save, three launches per cache state (warm = the entries of an earlier launch; the cache directory is `Library/Caches/Anomaly/lua`, 476 entries, about 5 MB):

| Cache | seconds to level ready | Lua part of the load, ms | file read | compile / cache | run main chunks |
|---|---:|---:|---:|---:|---:|
| off | 5.9 to 7.2 | 327 to 343 | 13 | 65 to 66 | 249 to 264 |
| cold | 5.9 to 6.4 | 450 to 464 | 11 to 12 | 184 to 191 (compile 67, store 106) | 248 to 266 |
| warm, one file per script read on demand (the build measured) | 5.7 to 6.1 | 327 to 348 | 14 | 63 to 67 (file I/O 53 to 57, decode 6, hash 1) | 250 to 268 |

- A level load is about 6 s and the scripts are 0.33 to 0.35 s of it: **there is nothing to win in compiling** (65 ms of parsing, the iPad parses 100 MB/s). The bytecode loads in 6 ms, but opening
  and reading 474 small files takes 53 to 57 ms (115 µs a file in the app's sandbox), so a warm load costs what the parse costs: **the cache as shipped (read-ahead off) gains nothing on the iPad**
  and costs a cold start of 0.12 s once. The read-ahead (Mac result above) would move the file I/O off the load; **it has not been measured on the iPad**: the one run for it was cut off by
  another session's launches. Expected from the split: 63 ms becomes about 8.
- New game from the menu (3 launches per state): the Lua part is 342 to 344 ms off, 460 cold, 349 to 352 warm; a steady 60 fps window ends 27 to 29 s after the click on Start in every state.
  (IPAD.md's first measurement of a new game was about 45 s.)

## Step 3, the hot paths: not needed

The plan's condition for porting script functions to C++ was a Lua share that makes Quality miss its 30 fps cap or Performance its 60 in the Cordon or a busy base. It does not (tables
above): on the iPad's interpreter Quality holds 29.9 to 30.0 fps with Lua at 8 to 11 % of the 33.3 ms budget, Performance holds 59.4 to 60.0 fps with Lua at 5 to 12 % of 16.7 ms, and
98.5 % or more of the frames land on their beat in every place. Nothing was ported.

What a port could win is small anyway. The profile is flat (no function above 7 % of the Lua time, which is 0.2 ms of a Quality frame), half of the Lua time is already inside C functions
and a quarter in the collector, and only a fifth is interpreted bytecode, so the JIT's 3x on bytecode ([IPAD-JIT.md](IPAD-JIT.md)) would also give back well under a millisecond a frame.
If a future scene needs it, in the order of what the profile says: the collector (25 % of the Lua time, inline in allocations: `setpause` / `setstepmul`, or more of its steps on the second
thread), the `get_server_entity` and `anomaly_restrictor_update` loops, the `npc_on_hear_callback` chain (0.64 ms and 77 calls a frame in the Bar).

Limits of these numbers: the places are quiet ones (nobody fights; the player is kept safe by `xr_harness`), so a firefight with many NPCs costs more Lua than the 3.6 ms of the village
(its p95 frame is 6.1 ms in Quality and 4.0 in Performance, its longest 25 and 44); the save is a fresh new game, so the A-Life has not grown. Run `lua_measure.sh` on a later save to see.

## Files

| | |
|---|---|
| `src/xrServerEntities/script_perf.{h,cpp}` | timers, profiler, `lua_perf`, `lua_profile`, `lua_cache` |
| `src/xrServerEntities/script_bytecode_cache.{h,cpp}` | the cache, the read-ahead |
| `tools/lua_cache_test` | its tests (ctest `lua_cache_test`) and `--bench` |
| `tools/lua_measure.py` | script generator (`script`), log summaries (`summarize`, `loadrow`) |
| `tools/render_harness/lua_measure.sh`, `lua_load.sh` | the Mac runs: places per mode, level load and new game per cache state |
| `tools/ios/lua_measure.sh` | the iPad runs. Engine flags go through `Documents/commandline.txt`; the app is launched without `--console` (a console launch failed with error 10002 after the first one) and polled until it exits; it leaves `user.ltx` at Quality / Auto |

## Checks

- `ctest -R lua_cache_test`: 153 checks (stale source, addon override and its removal, corrupt and foreign entries, error messages, unusable directory, warm and purge, hash, two threads, the read-ahead).
- Lua behaviour unchanged: `tools/sweep/run.sh smoke` on the ASan+UBSan build, four completed runs of the shipped configuration or its predecessors (cache on, read-ahead off; 263 to 286 s), each
  `DONE plan smoke` with 0 ASan reports and the UBSan reports of the renderer that [SANITIZERS.md](SANITIZERS.md) lists (null `ShaderElement` and `Fmatrix` references, NaN into `iFloor`), no crash
  files. One run in eight hit the `reprocess_spawn` use-after-free of SANITIZERS.md open item 1. Three of the nine runs stalled after the level change to Garbage (see above); the sweep does
  that on other branches too.
- The scripts that were in the archives and the overlay run from the cache as from the text: a level load with the cache warm loads 473 of 476 files from it, and the sweep plays through.
- Windows: nothing of this is compiled there; the hooks in shared files are no-ops (`script_perf.h` stubs) or generic (`luabind::set_call_hooks`).
