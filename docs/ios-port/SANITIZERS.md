# Sanitizers and the crash sweep

How to build `anomaly` with AddressSanitizer + UndefinedBehaviorSanitizer (or ThreadSanitizer) on the Mac, how to drive the game through a set of
scenarios unattended while it runs under them, and what the sweeps found.

## Build

```bash
cmake --preset macos-arm64-asan                       # build/macos-arm64-asan, RelWithDebInfo, every module + the app
cmake --build --preset macos-arm64-asan --target anomaly
```

The preset is `macos-arm64-release` plus `ANOMALY_SANITIZE="address;undefined"` and `ANOMALY_BUILD_ALL_MODULES=ON`. The option
(`cmake/AnomalyPlatform.cmake`) is a `;`-separated list for any macOS build, default empty, and is refused on iOS:

| Value | Effect |
|---|---|
| `address` | `-fsanitize=address -fsanitize-recover=address`; stack-use-after-scope is on, stack-use-after-return is a runtime option (below) |
| `undefined` | `-fsanitize=undefined,float-cast-overflow -fno-sanitize=vptr,function -fsanitize-recover=undefined`, and `-fno-sanitize=alignment` unless `alignment` is also listed |
| `thread` | `-fsanitize=thread`; only on its own (`cmake --preset macos-arm64-tsan`, below) |

It reaches every target defined after the file is included: all engine modules, the third-party libraries built from `external/`,
the shell, the app. LuaJIT (built by its own Makefile, `cmake/deps/LuaJIT.cmake`) gets the same flags on its target code
(`TARGET_CFLAGS`, the host tools `minilua` and `buildvm` stay plain) and is built with `LUAJIT_USE_SYSMALLOC`, so Lua's heap is
malloc'ed and ASan sees it; the assembly interpreter (`lj_vm.S`) cannot be instrumented. A different flag set builds LuaJIT again.

Why the checks are what they are:

- `float-cast-overflow` is not part of `undefined`, but it is the check for the x86-64 to arm64 change that matters most here: an
  out-of-range double or float converted to an unsigned type wraps on MSVC's x64 code (`cvttsd2si`: -1.0 becomes 0xFFFFFFFF) and
  saturates on arm64 (`fcvtzu`: -1.0 becomes 0). Scripts pass -1 for "no id" all the time.
- `vptr` is off: the engine casts through unrelated bases on purpose (the `CObject`/`CGameObject` mixins, luabind) and has no use
  for the RTTI check. `function` is off: luabind and the script exports call through casted function pointers.
- `alignment` is off: about 90% of the first run's UBSan reports (1,100 in two minutes), all of the same shape. The engine reads packed
  file data (`GameGraph`, `.ogf`, the spawn registry) through typed pointers and nests structs inside `#pragma pack` records; arm64
  loads and stores do not care and nothing uses atomics or NEON loads on those objects. `-DANOMALY_SANITIZE="address;undefined;alignment"`
  turns it back on to look for a case that does.

A full build takes 35 to 45 minutes on 12 cores while other builds run (the binary is 280 MB); the iOS build and the plain macOS presets are unchanged.

### ThreadSanitizer

```bash
cmake --preset macos-arm64-tsan                       # build/macos-arm64-tsan, RelWithDebInfo, every module + the app
cmake --build --preset macos-arm64-tsan --target anomaly            # or --target xrcore_smoke: the log race test, a few minutes
tools/sweep/run.sh --tsan --scale 3 --timeout 3000 smoke            # the same plans as under ASan; stderr.txt holds the reports
```

`--tsan` selects that build and `TSAN_OPTIONS` (`tools/sweep/tsan.supp`, `halt_on_error=0`, `report_thread_leaks=0`: the engine's detached threads are
never joined by design; `history_size=5`), and `triage.py` groups the reports by kind and the two accesses' top frames in the engine's code. The game
runs 5 to 15 times slower, so use `--scale 3` or more. Two things differ from the plain build, on purpose:

- **LuaJIT is built with `LUAJIT_UNWIND_INTERNAL` and without the JIT** (the iPad's configuration). The JIT's machine code is not instrumented, so TSan could
  not see what it does, and the stock Mac build unwinds Lua errors through the C frames with the system unwinder.
- **The link uses `-Wl,-no_compact_unwind`.** ThreadSanitizer adds an unwind cleanup (the `__tsan_func_exit` call) to every function that calls something
  that may throw, and clang gives those the C personality routine; with the C++ one, the Objective-C one (SDL, the DualSense code) and LuaJIT's interpreter's
  that is four, one more than compact unwind can encode (`ld: Too many personality routines for compact unwind to encode`). With external unwinding and
  this flag the first Lua error that crosses a C frame ended the process with `exit(1)` in `lj_err_throw` and no report at all (found with
  `SWEEP_LLDB=1 tools/sweep/run.sh`, which attaches lldb in batch mode and prints every thread's stack when the app calls `exit`, `_exit` or `abort`).
  A C++ exception that is really thrown cannot be unwound in this build.

`xrcore_smoke --log-race [seconds]` is the quick check of the log and the file system: threads that log (with repeated lines), the freeze watchdog flushing
the log, the console reading the last lines, `clear_log`, and then `Core._destroy()` while the watchdog still runs, as the engine's shutdown does.
Reports show even when the run does not crash. `-DXRCORE_SMOKE_OLD_LOG_API=ON` builds it against the log code as it was before log_lines.h (for a before run).

## Running under the sanitizers

```bash
tools/sweep/run.sh newgame       # once: new game from the menu, saves sweep_base in the Cordon
tools/sweep/run.sh smoke         # about 5 minutes (see below)
tools/sweep/run.sh combat        # one scenario each from sweep_base: combat inventory pda trade surge anomalies sleep transition saveload die_reload
tools/sweep/run.sh quit_dialog   # quit from a state: quit_menu quit_loading quit_level quit_inventory quit_pda quit_dialog quit_combat quit_dead quit_surge
```

`run.sh` writes `build/sweep-runs/<time>-<plan>/` with `stderr.txt` (every sanitizer report, with stacks), `engine.log`, `shell.log`,
the `--script` it generated and `summary.txt`: exit status, the driver's `[sweep]` lines, the ASan reports grouped by kind and
top frames, the UBSan reports grouped by place. `tools/sweep/triage.py <stderr.txt>` does the grouping alone.

What the runner sets up, none of it needed by hand:

- **No interruption of the person at the Mac.** The app is started through LaunchServices, hidden and in the background (`open -g -j
  -W -n --env ... --stdout f --stderr f anomaly.app --args ...`). `--script` and `--exit-after` make the app run unattended by
  themselves ([INTEGRATION.md](INTEGRATION.md#unattended-runs): offscreen, silent, never frontmost, the cursor untouched).
  `--exit-after` is the hard limit; the runner's watchdog kills the app after `--timeout` (default 1500 s) and when the script is interrupted.
- **Its own state.** `ANOMALY_STATE_ROOT=build/sweep-state` (override: `--state`): saves, logs, `user.ltx`, shader and overlay
  caches are separate from any other run of the app, so a sweep never rewrites someone else's log or save. The first run in a new
  state translates every shader again (a *cold* shader cache: `rm -rf <state>/Caches` before a run does that on purpose).
- **The overlay from the checkout** (`ANOMALY_OVERLAY=<repo>/gamedata`), so the driver script needs no relink.
- **`-dbg`**, which registers the debug commands (`jump_to_level`, `g_god`, `g_unlimitedammo`, `run_string`, `run_script`; without
  it the build, compiled as `MASTER_GOLD`, has none of them).
- `ASAN_OPTIONS`: `detect_stack_use_after_return=1` (the M1 bug class of pointers to dead locals), `halt_on_error=0` (the app is
  built with `-fsanitize-recover`; a report is printed and the run goes on), `detect_container_overflow=0` (the engine's vectors are
  annotated, the system libc++'s are not: false positives), `detect_leaks=0` (the engine leaks by design at exit),
  `allow_user_segv_handler=0` (ASan, not the engine's crash handler, reports a SIGSEGV, with the object it touched),
  `quarantine_size_mb=512` (`SWEEP_ASAN_EXTRA=quarantine_size_mb=3072` in the environment keeps freed blocks longer, to see who freed
  what a dangling pointer points at), `suppressions=tools/sweep/asan.supp`. `UBSAN_OPTIONS`: `print_stacktrace=1`,
  `suppressions=tools/sweep/ubsan.supp`.

### The driver

`gamedata/scripts/sweep_driver.script` is inert unless the environment has `ANOMALY_SWEEP=<plan>`. `axr_main` calls its
`on_game_start()`; it then runs the plan as a coroutine, one step per frame from `level.add_call` (which, unlike `actor_on_update`,
also runs while the actor is dead). It follows the game, not the clock, so the `--script` timeline is only the menu path
(Return, the Start button, "press any key") and a last-resort `quit`. Each step is under `pcall` and logs `[sweep] [t=<ms> f=<frame>]
ok|FAIL <step>` (and flushes the engine log, which is buffered). Lua errors of the driver are `FAIL`s; an engine crash or a
sanitizer report is a finding. A plan that crosses a reload or a level change (the Lua state is rebuilt by both) keeps its progress
in `<appdata>/sweep_state.txt`.

What it does, with the engine's own Lua exports: `alife():create` / `alife_create_item` to spawn enemies and items, `level.press_action`
for fire, reload and the inventory key, `db.actor:hit`, `:eat`, `:move_to_slot`, `:drop_item`, `ActorMenu.get_pda_menu()` for the PDA and
its tabs, `:run_talk_dialog`, `trader:start_trade(db.actor)` (the trader is the object, the actor the argument) and `:start_upgrade`, `surge_manager.get_surge_manager():start(true)`,
`actor_status_sleep.force_sleep()`, `jump_to_level` (needs `-dbg`), the console `save` / `load`. Dialogs are tracked from the `GUI_on_show` /
`GUI_on_hide` callbacks (`CUIActorMenu:IsShown()` is false for the inventory even while it shows). God mode (`g_god`, `-dbg`) is on
during fights and off for the deliberate hits and the death.

Learned the hard way, so the next person does not have to:

- A loading screen (every one, also of a reload) waits for "press any key" and the game does not run until then; the runner taps Left Shift every 15 s.
- Escape with no dialog open is the pause menu, which stops the level and with it the driver's tick. It is only sent to a talk window.
- The game-over screen is a tutorial whose stop callback switches the main menu on; "Load last save" is `main_menu off` + `load_last_save`
  and the driver does exactly that (`die_reload`). A `load` while the tutorial still runs is followed by its `main_menu on`.
- A level change rebuilds the Lua state; the plan continues in the next phase at the first update of the new level.
- `alife():create` of an anomaly zone makes a zone without a shape (`CCustomZone` asserts): the `anomalies` plan uses the level's own zones (`db.anomaly_by_name`).
- Anomaly's trade, loot and repair/upgrade windows are **not the engine's `CUIActorMenu`**. `actor_menu_inventory.CUIActorMenu_OnMode_Trade` (and `_DeadBodySearch`, `_Upgrade`)
  hand the mode to `ui_inventory` (a `CUIScriptWnd`, `ui_inventory.GUI`, mode `"trade"`, `"loot"`, `"repair"`) and return true, so the engine opens nothing and
  `ActorMenu.get_menu_mode()` stays 0. The driver looks at `ui_inventory.GUI:IsShown()` and `.mode`, and uses the window's own handlers for what a click does
  (`Action_Move`, `TMode_Buy`, `TMode_Sell`, `RMode_InitItem`, `RMode_RepairYes`).
- `start_trade(obj)` wants the actor as the *argument* (`CScriptGameObject::StartTrade` casts `obj` to the actor and takes `this` as the partner): `trader:start_trade(db.actor)`,
  the way the game's own `ui_debug_launcher` and `axr_companions` call it. `db.actor:start_trade(trader)` returns without a window by design. The `trade` plan used the second form,
  which is all that "`trade open: false`" was (Open items 8).
- A character with `inf_money` in its profile (Sidorovich) never loses money: `CInventoryOwner::set_money` keeps the larger value. A sale to him pays the actor and leaves his purse as it was.
- The key a window listens for is not always what opens it: in the talk dialog `kSPRINT_TOGGLE` is the trade button, but only for a character whose trade is enabled (the Barkeep's is off until
  the story switches it on), and a mechanic's window comes from his dialog lines, not from that key. `mechanic:start_upgrade(db.actor)` and `barkeep:start_trade(db.actor)` always work.
- God mode (`g_god`) is not part of a save. The mid-combat save `smoke` reloads has enemies around the actor, and a dead actor can neither be teleported (`ForceTransform` returns) nor get
  its first update (`actor_binder:update` returns), so the trade steps found the actor 39 m from Sidorovich and the Garbage level never started the next phase (the run ran into
  `--exit-after`, 24 minutes, with 0 ASan reports). The driver turns god mode on again after the reload and checks `db.actor:alive()` before the trade.
- Checks: `check(name, cond, detail)` in the driver logs `ok   check <name>` or `FAIL check <name> -- <detail>`; `run.sh` counts the `FAIL` lines (`driver FAILs: n`, with the first twenty), and a plan passes with 0.

### The smoke plan

`tools/sweep/run.sh smoke` (about 5 minutes from `sweep_base`, 8 from the menu): gear; combat with two bandits, a boar and a dog (shooting,
reloading, hits taken, bandage, medkit, kills); the inventory (slots, belt, repair kit, eat, drop); a save with enemies around; death and
the game-over "Load last save"; the PDA (every tab); `alife():register` on six unregistered items; Sidorovich (the talk dialog's trade key and `start_trade`, each with a buy and
a sell); the Cordon to Garbage change; the same fight in Garbage; a second save; quit. It is the thing to re-run after a change:

```bash
tools/sweep/run.sh newgame && tools/sweep/run.sh smoke      # then read build/sweep-runs/<latest>/summary.txt
```

The pass criterion is `ASan: 0 reports`, no `crash files`, `driver FAILs: 0`, the driver reaching `DONE plan smoke`, and the UBSan list holding only
what [Open items](#open-items) names.

### Tools

- `tools/sweep/triage.py stderr.txt`: the sanitizer reports grouped (ASan by kind and top frames in the engine's code, UBSan by place).
- `tools/sweep/argorder.py -p build/macos-arm64-asan`: a libclang scan (`pip install libclang` in a venv) for calls where one argument
  modifies what another argument of the same call reads. The order arguments are evaluated in is unspecified; MSVC's x64 code did them right
  to left, clang on arm64 goes left to right (`f(height, RayPick(..., height))` with `RayPick(float& range)` rained through roofs). It
  finds the known hit in about four minutes of parsing; the engine modules had that one only.
- `-Wunsequenced` is clang's default and the engine builds without a single such warning.

### Audits that do not need a run

A sanitizer only reports what executes. Two classes were also searched for statically, in xrCore, xrEngine, xrGame, xrPhysics,
xrServerEntities, xrSound, xrCDB, xrParticles and Common:

- **Argument evaluation order**: `argorder.py` (above) and the build's `-Wunsequenced`.
- **Bit and float-to-int tricks** (the `iFloor`/`iCeil` class: shifts by an out-of-range count, which x86 masks and arm64 clang at -O2 folds to poison
  for constant arguments, `color_argb_f(1,1,1,1)` returned 0x0000ffff). The sanitized build is RelWithDebInfo (-O2), UBSan's `shift`
  checks (`shift-exponent`, `shift-base`) are part of `undefined` and suppressed nowhere but LuaJIT and libvorbis, and the check is emitted
  by the front end, so a call with constant arguments is still reported when it runs. Every shift report of the 98 sweep runs came
  from `iFloor`/`iCeil` or from those two libraries. Greps for mismatched-size punning (`*(long*)&f`), shifts of bytes and shorts into the
  int sign bit, float-to-int magic constants (`0x4B400000`, `6755399441055744`), `_mm_cvt*`, bit-scan intrinsics and clamp macros on unsigned found:
  `apx_InvSqrt` (reads a `long`, 8 bytes on Apple, through a float; unused, fixed), `CColor::get_windows` (`BYTE << 24`; unused, fixed), and nothing
  else. The compile-time constants `1 << 31` in enums are evaluated by the compiler. The build has no `-Wshift-*` warning.

## Scenarios and the state of each

Run on the ASan+UBSan build, from `sweep_base` unless noted. "Clean" is: no ASan report, no crash, the driver reaches `DONE`.

Last run of the whole matrix on the final binary (RelWithDebInfo, ASan+UBSan); `smoke`, `trade` and `reprocess` were re-run after the 27th finding and the trade checks. Times are wall clock with the machine otherwise busy.

| Plan | Covers | Result |
|---|---|---|
| `newgame` | menu (Return), the Start button, the loading screen's "press any key", the Cordon, save `sweep_base` | clean (also as the first part of the cold-cache `smoke`, below); three runs on the final binary: 0 ASan reports, 157, 102 and 99 s |
| `combat` | two bandits, a boar and a dog: shooting, reloading, hits taken, bandage, medkit, kills (god mode on between the deliberate hits) | clean, 88 s |
| `inventory` | slots, belt, repair kit, eat, drop; the window opened by key | clean, 65 s |
| `pda` | the PDA and every tab | clean, 71 s |
| `trade` | Sidorovich by the talk dialog's key and by `start_trade`, each with a buy and a sell (items and money both ways); a body and a stash looted; then the Bar: the Barkeep's trade and the mechanic's repair | clean, 40 checks, 0 driver FAILs, 216 s |
| `reprocess` | `alife():register` on six unregistered items and `alife():release` on six more, the old objects read at once, 1.5 s and 15 s later | clean, 0 ASan reports, 43 checks, 137 s (with the entities freed at once: 3 ASan reports, 2 use after free and a SEGV that ended the run, for `register` as well as for `release`) |
| `surge` | forced emission: shelter or death, the blowout effects | clean, 110 s |
| `anomalies` | the level's own anomaly zones, artefacts | clean, 75 s |
| `sleep` | forced sleep and waiting (time acceleration) | clean, 63 s |
| `transition` | Cordon to Garbage through the level changer | clean, 116 s |
| `saveload` | save in the middle of a fight, load it back | clean, 122 s |
| `die_reload` | die, the game-over tutorial, "Load last save" | clean, 85 s |
| `smoke` | all of the above in one run (about 8 minutes from the menu) | **clean**: four complete runs on the final binary, 0 ASan reports, 0 driver FAILs, no crash file, `DONE plan smoke`, 589 to 655 s with the machine at a load average of 100 to 400 (before the fix one use after free per run, Open items 1). Two earlier runs of the same binary did not reach `DONE` (0 ASan reports): the actor was most likely dead, see "Learned the hard way" |
| `smoke` from the menu with a **cold shader cache** (a fresh `ANOMALY_STATE_ROOT`: every shader is translated by ShaderXlat/vkd3d during the run, 464 cache files written) | the whole plan, new game included | **clean**: 0 ASan reports, no crash file, driver reaches `DONE`, 265 s. An earlier cold run that sat on the loading screen's "press any key" for 24 minutes (the runner did not tap keys in the new-game path then; it does now) was clean too |
| `quit_menu` `quit_loading` `quit_inventory` `quit_pda` `quit_dialog` `quit_combat` `quit_dead` `quit_surge` `quit_level` | quit from each state | all exit 0 and shut down by themselves, 20 to 60 s each, no ASan report, no crash file |

### ThreadSanitizer and the quit runs

Run on `macos-arm64-tsan` (`tools/sweep/run.sh --tsan --scale 3`, `tools/sweep/quit_runs.sh --tsan`), with `tools/sweep/tsan.supp`:

| Run | State | Result |
|---|---|---|
| `xrcore_smoke --log-race 3`, the log code before this work | | 8 reports: `m_files.clear()` (`CLocatorAPI::_destroy`) against `Register` and `m_files.insert` from `FlushLog`'s `FS.w_close`, `xr_delete(xr_FS)` against `FlushLog`, the console's reads of `LogFile` (size, strings being destroyed) against the push_backs and clears |
| `xrcore_smoke --log-race 3`, the new log code | | first run 21 reports, all `Memory.stat_calls++` in `mem_alloc`/`mem_free` (finding 35); then 0 |
| `smoke` | fresh state, **cold shader cache**, from `sweep_base` | `DONE plan smoke`, exit 0, 730 s, **0 reports** |
| `quit_menu` `quit_loading` `quit_level` `quit_combat` `quit_inventory` `quit_pda` `quit_dialog` `quit_dead` `quit_surge` | the smoke run's state | all nine exit 0 by themselves, **0 reports** (65 to 300 s each under TSan; `quit_loading` takes 27.6 s to quit: the load step it interrupts must finish) |

Without `tsan.supp` the same `smoke` run reports 477 races in 66 places. 301 are `CRandom::randI` (the one global generator, advanced by every thread);
the rest are the by-design overlap of the secondary thread with the render phase (below). None is a container or a lifetime: those were the ones
fixed in findings 29 to 37 (the first TSan runs, before the fixes, also had them: `CModelPool::Delete` and `CBlender_Compile` against the
prefetcher's thread, `s_bones_array_const`, and `FS.dwOpenCounter` from the texture loader's GCD workers).

`tools/sweep/tsan.supp` names every function it hides and why. The by-design class: the secondary thread (`mt_Thread`: `Device.seqParallel`,
`seqFrameMT`, the ALife update, the network processor, the sound update, the HOM and detail-manager work) runs while the primary thread renders; the
engine hands data over with `mt_csEnter`/`mt_csLeave` and with per-frame counters (`Device.dwFrame`, `m_frame_calc`, ...), not with locks around the data, so
TSan sees scalars and flags that one side writes and the other reads a frame earlier or later. A stale value costs a frame of wrong occlusion or a
fade; `CDetailManager` is the one place where it could be more (open item 11).

Thirty plain quits on the RelWithDebInfo build (`tools/sweep/quit_runs.sh -n 30 --plain`, a state with `sweep_base`; the time is from the request to the
shell's `quit (ok)` line, for `quit_level` from the engine's last frame, after the level is disconnected):

| Quit from | Runs | Hangs, crashes | Quit time, median (min to max) |
|---|---|---|---|
| the main menu (`quit_menu`) | 10 | 0 | 0.08 s (0.06 to 0.12) |
| a level (`quit_level`) | 10 | 0 | 0.29 s (0.23 to 0.34) |
| the new game's loading screen (`quit_loading`) | 10 | 0 | 0.21 s (0.15 to 0.25) |

Commands that need the debug build (`-dbg`; the runner passes it): `g_god` (all fights), `jump_to_level` (`transition`, `smoke`, `quit_level`). The rest is Lua exports and keys.

Before the fixes in the table below these runs showed use-after-free reports in the main menu, the space restriction holder and the crow list, a
stack-use-after-scope in `spawn_supplies`, a null `UI()` in the cursor, and 18 reports each in `surge` and `sleep` from luabind releasing registry slots on collected
coroutines at `lua_close`. The quit hang that M2 saw about once in ten runs was not reproduced (Open items 2).

UBSan on the final matrix: only the renderer's reports remain (Open items 3, 4); everything else is fixed or suppressed with a reason (below).

## Findings

The sweep's fixes, in the order they were made. Module is the commit prefix.

| # | Module | Class | Root cause and fix |
|---|---|---|---|
| 1 | Common | heap-buffer-overflow (read) | `strncpy_s` read `src[count]` one past the count: `IReader::r_string` at the end of a buffer. Stops at `count`, terminates there. |
| 2 | xrCore | signed overflow, shift out of range | `iFloor`/`iCeil` bit tricks shift by an out-of-range count and overflow on -0.0f (and give 0 for NaN differently from x64). `floorf`/`ceilf` on Apple. |
| 3 | xrEngine | enum value out of range | `ECamEffectorType`: scripts pass ids beyond `cefNext`; fixed underlying type `int`, so the out-of-range id is a defined value (clang gives an unfixed enum only the range of its enumerators). |
| 4 | xrEngine | enum value out of range | `EEffectorPPType`: same, beyond `ppeNext`. |
| 5 | xrGame | stack-use-after-scope | `CALifeObject::spawn_supplies`: the ammo section `string128` was declared in the loop body and its pointer kept past it. Hoisted. |
| 6 | xrGame | out-of-bounds read | `UILines`: the default-colour check compared a range that ends past a string literal. Bounded `compare`. |
| 7 | xrGame | null dereference | `GE_MONEY` for a receiver that is gone (the level is being stopped): `pTa->m_dwMoney = ...` without the null check. |
| 8 | 3rd party/luabind | float-cast-overflow (the x64 to arm64 class) | A negative Lua number passed for an unsigned parameter: MSVC x64 `cvttsd2si` wraps (-1 is 0xFFFFFFFF), arm64 `fcvtzu` saturates to 0. Scripts pass -1 for "no id" everywhere. `lua_number_to_unsigned` in `policy.hpp`. |
| 9 | 3rd party/lua-extensions | out-of-bounds read | `trim` functions of the script library read `s[-1]` on an empty string. |
| 10 | 3rd party/ODE | stack out-of-bounds read | `dInternalStepIsland_x2`: `ofs[j1] < ofs[j2]` ran before the `j == -1` (inactive joint) check and read `ofs[-1]`. Order swapped. |
| 11 | xrCore | crash of the runtime (ASan CHECK) | `install_alt_stack` replaced the alternate signal stack the sanitizer runtime had installed on the thread, which the runtime then unmapped at thread exit. Installs only when the thread has none. |
| 12 | xrPhysics | pointer overflow | `SURFACE()` of the tri-collider did `size_t` arithmetic that wraps below zero and back. `ptrdiff_t`. |
| 13 | xrGame | iterator past the end | `CCarLights::findLight` dereferenced `end()` for a bone without a light. |
| 14 | xrGame | heap-use-after-free | `CSpaceRestrictionHolder::clear()` deleted the bridges while the compositions still held references to the restrictions they own. The compositions release first. |
| 15 | xrGame | float-cast-overflow | `CScriptActionCondition`: life time -1 ("never") was a negative double to `u64`, 0 on arm64: the condition expired at once. |
| 16 | xrGame | uninitialised read | `CPathManager<game vertex>`: `m_start_is_accessible` read before the call that sets it. |
| 17 | xrGame | heap-use-after-free, null dereference | Main menu `restart_all()`/`DestroyInternal` deleted the start dialog while it was shown (the holder's lists dangled); `Activate(false)` then dereferenced the null. Hides the dialog first, guards the null. |
| 18 | xrGame | null dereference | `CUICursor` reached a static member through `UI()`, which is still null while the main menu starts. |
| 19 | xrGame | assertion (died after a load) | `CMapLocation::Update` twice in one device frame returned a different result. Returns the first. |
| 20 | xrGame | heap-use-after-free | The crow AI's list of dead NPCs was never cleared and kept pointers to removed objects. |
| 21 | xrCore | use after shutdown | `FlushLog` from the freeze watchdog thread ran after the file system was gone. |
| 22 | xrEngine | argument evaluation order | `CEffect_Rain::Born`: `RenewItem(dest, height, RayPick(..., height))`: `RayPick` shortens `height`, `RenewItem` reads it. MSVC x64 evaluates right to left, clang left to right: rain fell through roofs. Hoisted in MSVC's order. The only hit of the whole engine (see Tools). |
| 23 | xrCore | memory corruption (probably the quit hang and the iPad abort) | Log dedupe did `LogFile.erase(LogFile.end() - 1)` on an empty vector (after `CloseLog`/`clear_log`): `end()` below `begin()`, the next `push_back` wrote there. A line logged twice at quit crashed the process. |
| 24 | xrCore | fatal handler | The SIGSEGV handler saved the game (and could spin for minutes) before it wrote the log. Writes the log first, flushes, and a 20 s `SIGALRM` watchdog ends the process. |
| 25 | 3rd party/luabind, xrServerEntities | heap-use-after-free at `lua_close` | `weak_ref` and `lua_reference` keep the `lua_State*` they were made on, almost always a script coroutine, collected long before the reference is released (when a C++ object holding a functor dies, or when the VM closes). The release then pushes onto a freed stack. `luabind::open` makes a thread of its own, anchored in the registry, and every release goes through it (never the main thread: while it runs a coroutine its stack must not be touched). |
| 26 | xrCore | out-of-bounds read, shift into the sign bit | `apx_InvSqrt` (unused) did `*(long*)&float` and kept the result in a `long`, which is 64 bits on Apple (four bytes past the float); `CColor::get_windows` (unused) shifted a `BYTE` promoted to `int` by 24. Same-size types and `u32` shifts. |
| 27 | xrGame | heap-use-after-free (script) | `alife():register(se_obj)` (Alundaio's `reprocess_spawn`) writes the entity into a packet, **destroys it** and returns a new one made from the packet; the scripts that call it (`death_manager.spawn_with_condition`, `item_parts`) go on reading the object they passed (`se_outfit.id` on the next line). Reading freed memory worked on the PC by luck. Nothing can tell when the last Lua reference to the old entity is gone (luabind holds a raw pointer), so `CALifeSimulatorBase::defer_destroy` parks it (same id as the new one; nothing registers it) for as long as the simulator lives: `unload()` and the destructor free it, before the registries and the script engine go (a script-derived entity releases Lua references), and the Lua state that could hold it ends with the simulator. At most 32768 are parked, the oldest is freed beyond that (outside the lock). Guarded by a lock (finding 28 parks from the ALife update thread too). |
| 28 | xrGame | heap-use-after-free (script reads what the ALife update freed) | `axr_companions` keeps squads in a table (`companion_squads[squad.id] = squad`) and a stalker's binder script calls `squad:commander_id()` on them in a later frame, after the switch step of the ALife update (`CALifeSimulatorBase::release` to `xrServer::entity_Destroy`) freed the squad. It looked like a race between threads and is not: the ALife update runs on the second device thread while the main thread renders (`mt_csEnter`/`mt_csLeave` hand the frame over, `CRenderDevice::on_idle`), the script runs in the next `FrameMove`; the report's "freed by thread T9" is the earlier cycle that freed it. It is the stale-pointer class of finding 27, for every server entity that goes: `xrServer::entity_Destroy` parks the entity in `defer_destroy` when the simulator exists instead of freeing it (online destroys by `GE_DESTROY` and offline `release` alike), until `unload()`. Memory cost: see Open items 10. |
| 29 | xrEngine, xrGame | heap use after free at quit (probably the quit hang) | The spawn prefetcher (`SPAWN_ANTIFREEZE`, thread "Pre-Spawn Prefetcher Thread": loads the models of spawn events ahead of time) was stopped by `~CLevel` *after* it deleted the queues the thread works on, by a flag that is a member of the level being destroyed, and nothing waited for the thread: it went on loading models and textures while `~IGame_Level` walked the texture map in `_DumpMemoryUsage`, which is the SIGSEGV in `Log`/`std::string` the quit hang started with. `~CLevel` now stops the thread first (own flags, the event set until the thread reports it finished, 30 s limit) and only then frees the queues; `_DumpMemoryUsage` holds the creation lock. |
| 30 | xrRender | data race, heap corruption | The same thread calls `Render->models_PrefetchOne` (`CModelPool::Create`/`Delete`) while the primary thread creates and releases the models of the objects it spawns: `Models`, `Registry` and `Pool` are plain containers with no lock. One recursive lock for the pool. |
| 31 | xrCore | data race, heap corruption | `CLocatorAPI::m_files` (a `std::set`, every file the game can open) was written by `Register()` from every thread that closes a file writer (the prefetcher compiling shaders writes each to the shader cache; the log flush of the freeze watchdog did, every 5 s while the primary thread was busy) and read by every thread that opens one. One recursive lock around the registry functions. |
| 32 | xrCore | data race, use after free at quit | `FlushLog` (called from the freeze watchdog thread, which never ended) tested `xr_FS` before taking the log lock, registered the log file in `m_files` through `FS.w_close`, and ran against `CloseLog`'s unlocked `LogFile.clear()` and `Core._destroy`'s `m_files.clear()` / `xr_delete(xr_FS)`: eight TSan reports in `xrcore_smoke --log-race`. The lock is taken first, `CloseLog` closes the log under it, the flush no longer registers the file. |
| 33 | xrEngine | data race, use after free | The console's log window read `LogFile` while other threads pushed to it (a reallocation frees the buffer being read), `clear_log` cleared it unlocked: `ClearLog`, `LogLineCount`, `LogCopyLines` (`log_lines.h`) copy under the log lock. |
| 34 | xrEngine | thread lifetime | The freeze watchdog is stopped in `CRenderDevice::RunEnd` (3 s limit) before the core and the log are torn down; what it reads from the primary thread (the time of its last frame, whether a level is loading) is atomic, not a `CTimer` and the live `g_loading_events` list. |
| 35 | xrCore, xrEngine, xrGame, Metal shim | data race (formal) | Plain integers that several threads change: `shared_str`'s reference count (Apple: `__atomic` add and sub), `mt_bMustExit` and `mt_Thread_marker` (the secondary thread's hand-shake), `Memory.stat_calls` (every allocation of every thread: all of the first 21 TSan reports), `CPU::qpc_counter`, `FS.dwOpenCounter` (the disk icon), and the Metal shim's poll of `Query::done` (set by the completion handler's thread) outside the lock that guards it. |
| 36 | ShaderXlat | guard | The translation hook holds a mutex: vkd3d-shader and SPIRV-Cross are reentrant and the resource manager already serialises shader creation (`creationGuard`), so this is insurance against a second caller. |
| 37 | xrCore | infinite recursion in the crash handler | After the static destructors have run, `Msg` locks a destroyed `std::recursive_mutex`, which throws `std::system_error`; nothing catches it, `std::terminate` calls the engine's `terminate_handler`, which reports with `Msg`, which throws: for ever, deeper and deeper (`sample` of a process that aborted at exit showed `terminate_handler` > `Msg` > `AddOne` > `recursive_mutex::lock` > `__cxa_throw` > `terminate` > ... 896 samples of the same cycle). This is the crash handler "spinning for minutes". `terminate_handler` reports once and `_exit`s the second time; `AddOne` and `FlushLog` write to stderr / return once the log's statics are gone (an object defined after them sets a flag as the first of them to be destroyed). |

### Suppressions

`tools/sweep/ubsan.supp` (runtime, `<check>:<file glob>`; every line has its reason in the file):

| Check | Where | Why |
|---|---|---|
| `shift-base`, `null` | `*lj_*` (LuaJIT) | Signed shifts by design (ULEB128 reader, arm64 assembler immediates, upvalue flags); a NULL-based member offset in the parser. Upstream behaviour, built this way everywhere LuaJIT runs. |
| `shift-base` | `*vorbis/lib/*` | The codebook decoder shifts a signed int into the sign bit. |
| `pointer-overflow` | `*rt_lzo*` | `ip - off` style pointer arithmetic with unsigned offsets. |
| `null` | `*wrap_iter.h` | `&*v.begin()` / `&v[0]` on an empty `xr_vector`: the engine's "data pointer or null", never dereferenced. Real null dereferences are reported at their own line. |
| `float-cast-overflow` | `*FTimer.h` | The known `CTimer::GetElapsed_ticks` wrap (see Open items). |
| ASan `interceptor_via_lib` | `CoreGraphics` | The SwiftUI launcher decodes a PDF asset; CoreGraphics' lexer calls `strndup` with a count past the source (the real one stops at the NUL, ASan's interceptor checks the whole count). Apple code. |

Compiled out instead of suppressed: `vptr`, `function` and `alignment` (see Build).

### Open items

1. ~~Script keeps a server entity that `alife():register()` freed~~ **Fixed**, finding 27. The `reprocess` plan reproduces it at once: on the pre-fix build 3 ASan reports (two
   use after free, one SEGV through the dangling pointer that ended the run), on the fixed one 0 with all six items registered and the old objects read at once, 1.5 s and 15 s later.
   The old entities are kept until the level (the simulator and its Lua state) goes; a script that kept one across a level change would have lost it anyway.
2. **The quit hang M2 saw once in ten runs** (a SIGSEGV in `Log`/`std::string` under `IGame_Level::~IGame_Level` to `CResourceManager::_DumpMemoryUsage`, then the crash handler
   spinning for minutes): **two causes, both fixed.** The SIGSEGV: the spawn prefetcher thread was still loading models and textures while the level was destroyed
   (finding 29; the same thread shared the model pool and the file registry with the primary thread without a lock, findings 30 and 31). The spin: `terminate_handler` recursing
   through `Msg` once the log's mutex was gone (finding 37), which a fatal signal during `exit()` reaches; it reproduced on demand (a TSan run that reported anything aborts at exit)
   and is gone. The earlier suspects, findings 23 and 24, stay fixed. Not reproduced before this on the Mac (about 100 quits), so the evidence for the first is the code and the
   thread's TSan reports, not a crash; after: 30 plain quits and the 9 `quit_*` plans under TSan, none hung. If it comes back: `SWEEP_LLDB=1 tools/sweep/run.sh` and
   `sample <pid>` on the stuck process show where it is.
3. **Renderer** (not ours, reported to M2): null `ShaderElement` through the `this == 0` idiom (`Shader.cpp:146-161` `ShaderElement::equal`/`Shader::equal`,
   `xr_resource.h:131`, `r__dsgraph_build.cpp:56`, `:914`, `:1134` reference to a null `Fmatrix`): benign while `-fno-delete-null-pointer-checks` is on, reported once per
   run at start and in the dynamic-visual path; the `sigaltstack` replacement in `FrontendVkd3d.cpp` (`struct AltStack`), the same bug as finding 11, fatal under ASan if the thread exits;
   the `alignment` reports (off by default, see Build): `FSkinned.cpp:553` on a misaligned `vertBoned2W`, `resptr_core<SPass>` and `xr_vector<_REG_INFO>` inside packed file data.
4. **NaN into `iFloor`** while a model loads (`FSkinned.cpp:39` `q_N(float)` from `vertHW_1W::set` from `CSkeletonX_ext::_Load_hw`): a NaN normal in some `.ogf`. `iFloor` now gives 0
   (x64's bit trick gave garbage); UBSan reports it (`float-cast-overflow`). Which model has it is not known.
5. **`CTimer::GetElapsed_ticks`** (`xrCore/FTimer.h:81`): the `u64` delta wraps when the ALife update thread reads the counter behind `m_real_ticks`, and the wrapped value converted to `double`
   and back is out of range (`float-cast-overflow`). Harmless in effect (about one tick). Suppressed; the fix is an xrCore header edit that rebuilds everything.
6. **The iPad's intermittent heap corruption** ("pointer being freed was not allocated" in vkd3d `preproc_free_macro`/`preproc_yyparse`, in `CloseLog`, once in a system
   framework): **not caught on the Mac by ASan** (two cold-cache `smoke` runs found nothing in ShaderXlat or vkd3d), and **not by TSan either in the code that was suspected**:
   ShaderXlat/vkd3d has one caller at a time (the resource manager's `creationGuard`, and now a mutex in the hook), no shared state of its own, and no TSan report. What TSan and
   a reading of the threads did find are the writers that share the heap with it: the spawn prefetcher (findings 29 to 31: unlocked `CModelPool` and `m_files`, containers
   resized and erased by two threads), the freeze watchdog's log flush against the file system's teardown (32), unlocked `LogFile` (32, 33). Any of them corrupts the heap; vkd3d's
   macro table and the log's strings, which are freed in bulk, are where libmalloc notices. On the iPad (cold shader cache, a slower primary thread, so more spawn events in
   flight and longer stale-timer intervals) they have the room to; on the Mac's warm cache they rarely do. The baseline did not reproduce it either: see IPAD.md for the runs
   before and after. If it still happens: `lua_parallel_gc 0` first (item 12), then `MallocStackLogging` with `MallocErrorAbort` and read the allocation stack of the block.
7. **`stop_hud_motion`** in the engine log of every `smoke` is exported and runs; the line `![player_hud::StopScriptAnim()] invalid script_anim_part 255` is the engine's own diagnostic
   when a script stops a hud animation that is not playing (`print_bone_warnings`), same on the PC. M2 reported the export as missing: it is not.
8. ~~Opening the trade window from a script~~ **Not an engine bug.** `db.actor:start_trade(trader)` has the roles reversed (`trader:start_trade(db.actor)` is the call), and Anomaly's
   trade window is the Lua `ui_inventory`, which the driver did not look at (see "Learned the hard way"). The `trade` plan now opens it by the talk dialog's key and by `start_trade`
   with Sidorovich and the Barkeep, buys and sells (items and money checked), loots a body and a stash, and repairs a gun at the mechanic. Barkeep's trade and a mechanic's window by
   their dialog lines are not driven: the dialog's phrase list has no export.
9. **`--script` has no step that waits for a log line.** The runner taps Left Shift every 15 s so each loading screen's "press any key" gets pressed. A `waitlog <text>` step would make it independent of the clock (app side).
10. ~~A race between the ALife update thread and the scripts~~ **Fixed**, finding 28 (it was a stale pointer, not a race between threads). Seen once in the first `newgame` on the
    final binary, at a load average of 25. The fix parks every server entity the simulator drops until `unload()`; what that costs, from the `* ALife: N entities ... were kept until now`
    line of the engine log at each unload: 50 to 220 per level in a `smoke`, 860 to 980 in the Cordon of a `newgame` (the game's start spawns and releases a lot), 55 in a `reprocess`;
    an entity is 0.5 to 2 KB, so about 1 MB per level, and the cap (32768) is not reached. If a level stays loaded for hours it grows with the number of objects destroyed.

11. **The grass (`CDetailManager`) hand-off between the secondary and the primary thread** (TSan: `hw_Render_dump` against `UpdateVisibleM`/`MT_CALC`, about 40 reports in a `smoke`, hidden
    in `tsan.supp`). `MT_CALC` on the secondary thread fills next frame's visible-slot vectors once `m_frame_rendered + 1 == Device.dwFrame`; the render phase dumps this frame's from the same
    vectors; if any pass dumps after `m_frame_rendered` is set (the ssfx grass shadows render the dump again for the lights; not checked) the two overlap. The counters are `volatile`, not acquire/release, and `Device.dwFrame` changes in
    `End()` while the secondary thread is still running. A vector that grows (`push_back`) while the render phase iterates it is a read of freed memory (a crash, not a heap corruption). Not
    changed: it needs the renderer's owner; a lock held by `Render()` for the dump would close it.
12. **`lua_parallel_gc`** (default on, `device.cpp`): the secondary thread runs `lua_gc` steps on the script VM while the primary thread renders. TSan cannot see into the interpreter's
    assembly (`lj_vm.S`) and the script paths it saw were quiet, but the VM is not thread safe; if the render phase ever runs a script callback, the two collide. `lua_parallel_gc 0` is the
    switch; not changed.

## Reading a report

`stderr.txt` has the sanitizer's own text with symbolised stacks (`-g`). For UBSan, `print_stacktrace=1` adds the call chain. A
dangling pointer into a block that was freed long ago points at a *different* object by the time it is read: rerun with
`SWEEP_ASAN_EXTRA=quarantine_size_mb=3072` and the report names the original allocation and the `free` that preceded it. To get a crash
into lldb attach to the app `open` started (`lldb -p <pid>`; never launch the binary directly: it steals the focus).
