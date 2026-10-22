# Sanitizers and the crash sweep

How to build `anomaly` with AddressSanitizer + UndefinedBehaviorSanitizer on the Mac, how to drive the game through a set of
scenarios unattended while it runs under them, and what the first sweep found.

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
| `thread` | only on its own |

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
its tabs, `:run_talk_dialog` and `:start_trade`, `surge_manager.get_surge_manager():start(true)`,
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

### The smoke plan

`tools/sweep/run.sh smoke` (about 5 minutes from `sweep_base`, 8 from the menu): gear; combat with two bandits, a boar and a dog (shooting,
reloading, hits taken, bandage, medkit, kills); the inventory (slots, belt, repair kit, eat, drop); a save with enemies around; death and
the game-over "Load last save"; the PDA (every tab); Sidorovich (teleport, talk dialog); the Cordon to Garbage change; the same fight in
Garbage; a second save; quit. It is the thing to re-run after a change:

```bash
tools/sweep/run.sh newgame && tools/sweep/run.sh smoke      # then read build/sweep-runs/<latest>/summary.txt
```

The pass criterion is `ASan: 0 reports`, no `crash files`, the driver reaching `DONE plan smoke`, and the UBSan list holding only
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

Last run of the whole matrix on the final binary (RelWithDebInfo, ASan+UBSan). Times are wall clock with the machine otherwise busy.

| Plan | Covers | Result |
|---|---|---|
| `newgame` | menu (Return), the Start button, the loading screen's "press any key", the Cordon, save `sweep_base` | clean (also as the first part of the cold-cache `smoke`, below) |
| `combat` | two bandits, a boar and a dog: shooting, reloading, hits taken, bandage, medkit, kills (god mode on between the deliberate hits) | clean, 88 s |
| `inventory` | slots, belt, repair kit, eat, drop; the window opened by key | clean, 65 s |
| `pda` | the PDA and every tab | clean, 71 s |
| `trade` | teleport to Sidorovich, talk dialog, trade window | clean, 64 s |
| `surge` | forced emission: shelter or death, the blowout effects | clean, 110 s |
| `anomalies` | the level's own anomaly zones, artefacts | clean, 75 s |
| `sleep` | forced sleep and waiting (time acceleration) | clean, 63 s |
| `transition` | Cordon to Garbage through the level changer | clean, 116 s |
| `saveload` | save in the middle of a fight, load it back | clean, 122 s |
| `die_reload` | die, the game-over tutorial, "Load last save" | clean, 85 s |
| `smoke` | all of the above in one run (about 8 minutes from the menu) | one ASan report, a script-side use after free (Open items 1); the rest clean, 466 s |
| `smoke` from the menu with a **cold shader cache** (a fresh `ANOMALY_STATE_ROOT`: every shader is translated by ShaderXlat/vkd3d during the run, 464 cache files written) | the whole plan, new game included | **clean**: 0 ASan reports, no crash file, driver reaches `DONE`, 265 s. An earlier cold run that sat on the loading screen's "press any key" for 24 minutes (the runner did not tap keys in the new-game path then; it does now) was clean too |
| `quit_menu` `quit_loading` `quit_inventory` `quit_pda` `quit_dialog` `quit_combat` `quit_dead` `quit_surge` `quit_level` | quit from each state | all exit 0 and shut down by themselves, 20 to 60 s each, no ASan report, no crash file |

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

1. **Script keeps a server entity that `alife():reprocess_spawn()` freed** (ASan heap-use-after-free, `smoke` in the Garbage fight, once per run). The
   Alundaio export (`alife_simulator_script.cpp`, `reprocess_spawn`) writes the entity out, destroys it with `F_entity_Destroy` and returns a *new* one; a stalker's
   action script goes on reading `.id` of the old `CSE_Abstract*` (luabind property getter) a few steps later. Reads freed memory on the PC too (the block usually still holds
   the value, so it passes unnoticed); not an arm64 difference. The script is in the game's archives, not in the repo; the fix is there (use the returned object), or
   the export could keep the old pointer valid until the end of the frame. Not changed here.
2. **The quit hang M2 saw once in ten runs** (a SIGSEGV in `Log`/`std::string` under `IGame_Level::~IGame_Level` to `CResourceManager::_DumpMemoryUsage`, then the crash handler
   spinning for minutes in the game-over save) was **not reproduced**: 12 quits from the Cordon, Garbage and the menu and the 9 `quit_*` plans, all clean. The probable cause
   is finding 23 (log dedupe `erase(end() - 1)` on an empty `LogFile`; it corrupts the vector, and a line logged twice at quit is enough), and finding 24 makes the handler
   write the log first and end the process after 20 s. If it comes back, run with `SWEEP_ASAN_EXTRA=quarantine_size_mb=3072` and read the first report.
3. **Renderer** (not ours, reported to M2): null `ShaderElement` through the `this == 0` idiom (`Shader.cpp:146-161` `ShaderElement::equal`/`Shader::equal`,
   `xr_resource.h:131`, `r__dsgraph_build.cpp:56`, `:914`, `:1134` reference to a null `Fmatrix`): benign while `-fno-delete-null-pointer-checks` is on, reported once per
   run at start and in the dynamic-visual path; the `sigaltstack` replacement in `FrontendVkd3d.cpp` (`struct AltStack`), the same bug as finding 11, fatal under ASan if the thread exits;
   the `alignment` reports (off by default, see Build): `FSkinned.cpp:553` on a misaligned `vertBoned2W`, `resptr_core<SPass>` and `xr_vector<_REG_INFO>` inside packed file data.
4. **NaN into `iFloor`** while a model loads (`FSkinned.cpp:39` `q_N(float)` from `vertHW_1W::set` from `CSkeletonX_ext::_Load_hw`): a NaN normal in some `.ogf`. `iFloor` now gives 0
   (x64's bit trick gave garbage); UBSan reports it (`float-cast-overflow`). Which model has it is not known.
5. **`CTimer::GetElapsed_ticks`** (`xrCore/FTimer.h:81`): the `u64` delta wraps when the ALife update thread reads the counter behind `m_real_ticks`, and the wrapped value converted to `double`
   and back is out of range (`float-cast-overflow`). Harmless in effect (about one tick). Suppressed; the fix is an xrCore header edit that rebuilds everything.
6. **The iPad's intermittent heap corruption** ("pointer being freed was not allocated" in vkd3d `preproc_free_macro`/`preproc_yyparse`, in `CloseLog`): **not caught on the Mac.**
   Whole-engine ASan runs with an empty shader cache (a fresh `ANOMALY_STATE_ROOT`: two cold-cache `smoke` runs, the table above) found nothing in ShaderXlat or vkd3d. What the sweep did
   find are two writes to freed or out-of-range memory in code that runs at the end of a session: finding 23 (`LogFile.erase(end() - 1)` on an empty vector, and the next `push_back` writes
   there) and finding 25 (luabind releasing slots on the stacks of collected coroutines at `lua_close`). Either could corrupt the heap an unrelated allocator later trips over; that they are
   the cause on the iPad is plausible, not shown. If it still happens with this PR, run the iPad build's equivalent under ASan (`-fsanitize=address` is refused on iOS by the option; use
   Malloc debugging in Xcode instead, `MallocScribble`/`MallocGuardEdges`) and keep the first report.
7. **`stop_hud_motion`** in the engine log of every `smoke` is exported and runs; the line `![player_hud::StopScriptAnim()] invalid script_anim_part 255` is the engine's own diagnostic
   when a script stops a hud animation that is not playing (`print_bone_warnings`), same on the PC. M2 reported the export as missing: it is not.
8. **Opening the trade window from a script**: `db.actor:start_trade(trader)` returns without showing it (the Lua hook `CUIActorMenu_OnMode_Trade` or a missing trader state; not investigated).
   The `trade` plan covers the talk dialog; it logs `trade open: false` and goes on.
9. **`--script` has no step that waits for a log line.** The runner taps Left Shift every 15 s so each loading screen's "press any key" gets pressed. A `waitlog <text>` step would make it independent of the clock (app side).

## Reading a report

`stderr.txt` has the sanitizer's own text with symbolised stacks (`-g`). For UBSan, `print_stacktrace=1` adds the call chain. A
dangling pointer into a block that was freed long ago points at a *different* object by the time it is read: rerun with
`SWEEP_ASAN_EXTRA=quarantine_size_mb=3072` and the report names the original allocation and the `free` that preceded it. To get a crash
into lldb attach to the app `open` started (`lldb -p <pid>`; never launch the binary directly: it steals the focus).
