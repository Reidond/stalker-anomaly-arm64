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
