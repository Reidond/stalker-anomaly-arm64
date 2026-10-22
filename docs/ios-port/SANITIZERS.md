# Sanitizers and the crash sweep

How to build `anomaly` with AddressSanitizer + UndefinedBehaviorSanitizer on the Mac, and how to drive the game through a set of
scenarios unattended while it runs under them. The first sweep's findings are in [Findings](#findings).

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
| `undefined` | `-fsanitize=undefined -fno-sanitize=vptr,function -fsanitize-recover=undefined`, and `-fno-sanitize=alignment` unless `alignment` is also listed |
| `thread` | only on its own |

It reaches every target defined after the file is included: all engine modules, the third-party libraries built from `external/`,
the shell, the app. LuaJIT (built by its own Makefile, `cmake/deps/LuaJIT.cmake`) gets the same flags on its target code
(`TARGET_CFLAGS`, the host tools `minilua` and `buildvm` stay plain) and is built with `LUAJIT_USE_SYSMALLOC`, so Lua's heap is
malloc'ed and ASan sees it; the assembly interpreter (`lj_vm.S`) cannot be instrumented.

Why `vptr`, `function` and `alignment` are off:

- `vptr`: the engine casts through unrelated bases on purpose (the `CObject`/`CGameObject` mixins, luabind) and has no use for the RTTI check.
- `function`: luabind and the script exports call through casted function pointers.
- `alignment`: about 90% of the first run's UBSan reports (1,100 in two minutes), all of the same shape. The engine reads packed file
  data (`GameGraph`, `.ogf`, spawn registry) through typed pointers and nests structs inside `#pragma pack` records. arm64 loads
  and stores do not care; nothing uses atomics or NEON loads on those objects. Turn it back on with `-DANOMALY_SANITIZE="address;undefined;alignment"`
  to look for a case that does.

A full build takes about 25 min on 12 cores (the ASan RelWithDebInfo binary is 280 MB); the iOS build and the plain macOS presets are unchanged.

## Running under the sanitizers

```bash
tools/sweep/run.sh newgame       # once: new game from the menu, saves sweep_base in the Cordon
tools/sweep/run.sh smoke         # about 5 minutes (see below)
tools/sweep/run.sh combat        # one scenario each from sweep_base: combat inventory pda trade surge anomalies sleep transition saveload
tools/sweep/run.sh quit_dialog   # quit from a state: quit_menu quit_loading quit_inventory quit_pda quit_dialog quit_combat quit_dead quit_surge
```

`run.sh` writes `build/sweep-runs/<time>-<plan>/` with `stderr.txt` (every sanitizer report, with stacks), `engine.log`, `shell.log`,
the `--script` it generated and `summary.txt`: exit status, the driver's `[sweep]` lines, the ASan reports grouped by kind and
top frames, the UBSan reports grouped by place. `tools/sweep/triage.py <stderr.txt>` does the grouping alone.

What the runner sets up, none of it needed by hand:

- **No interruption of the person at the Mac.** `SDL_MAC_BACKGROUND_APP=1 SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0
  SDL_WINDOW_ACTIVATE_WHEN_RAISED=0 ALSOFT_DRIVERS=null`: no Dock icon, no focus, no Space, silent. The window is windowed and
  small (`rs_screenmode windowed`, `vid_mode 1260x818` = 630x409 points on a 2x display, written to the run's `user.ltx`).
  The app is killed after `--timeout` (default 1500 s) and when the script is interrupted.
- **Its own HOME.** `CFFIXED_USER_HOME=build/sweep-home` (override: `SWEEP_HOME`): saves, logs, `user.ltx`, shader and overlay
  caches are separate from any other run of the app, so a sweep never rewrites someone else's log or save. The first run there
  translates the shaders again. Core Foundation honours `CFFIXED_USER_HOME`; `HOME` alone does not move `NSSearchPathForDirectoriesInDomains`.
- **The overlay from the checkout** (`ANOMALY_OVERLAY=<repo>/gamedata`), so the driver script needs no relink.
- **`-dbg`**, which registers the debug commands (`jump_to_level`, `g_god`, `g_unlimitedammo`, `run_string`, `run_script`; without
  it the build, compiled as `MASTER_GOLD`, has none of them).
- `ASAN_OPTIONS`: `detect_stack_use_after_return=1` (the M1 bug class of pointers to dead locals), `halt_on_error=0` (the app is
  built with `-fsanitize-recover`; a report is printed and the run goes on), `detect_container_overflow=0` (the engine's vectors are
  annotated, the system libc++'s are not: false positives), `detect_leaks=0` (the engine leaks by design at exit),
  `suppressions=tools/sweep/asan.supp`. `UBSAN_OPTIONS`: `print_stacktrace=1`, `suppressions=tools/sweep/ubsan.supp`.

### The driver

`gamedata/scripts/sweep_driver.script` is inert unless the environment has `ANOMALY_SWEEP=<plan>`. `axr_main` calls its
`on_game_start()`; it then runs the plan as a coroutine from `actor_on_update`, so it follows the game, not the clock, and the
`--script` timeline is only the menu path (Return, the Start button, "press any key") and a last-resort `quit`. Each step is
under `pcall` and logs `[sweep] ok|FAIL <step>`. Lua errors of the driver are `FAIL`s; an engine crash or a sanitizer report is a finding.
A plan that reloads a save keeps its progress in `<appdata>/sweep_state.txt`.

It uses the Lua exports of the engine rather than synthetic input where that is faster: `alife():create` and `alife_create_item`
to spawn enemies and items, `level.press_action` for fire/reload/inventory/PDA, `ActorMenu.get_pda_menu():SetActiveSubdialog` for
the PDA tabs, `db.actor:hit`, `:eat`, `:move_to_slot`, `:start_trade`, `surge_manager.get_surge_manager():start(true)`,
`actor_status_sleep.force_sleep()`, `jump_to_level` (needs `-dbg`) and the console `save`/`load`.

### The smoke plan

`tools/sweep/run.sh smoke`: new game (menu path) in the Cordon, gear, combat with two bandits, a boar and a dog (shooting,
reloading, taking hits, bandage and medkit, kills), the inventory (slots, belt, repair kit, eat, drop), a mid-combat save, death,
reload of that save, PDA (every tab), Sidorovich (talk and trade), the Cordon -> Garbage change, combat there, a second save,
quit. Pass `--scale 2` or `--scale 3` on a loaded machine (it stretches the menu part).

## Reading a report

`stderr.txt` has the sanitizer's own text with symbolised stacks (`-g`). For UBSan, `print_stacktrace=1` adds the call chain. To
get a crash into lldb, run the same command line under `lldb --` with the same environment (`env` lines are printed at the top of `summary.txt`).

## Findings

(See the pull request; the table is kept there while the sweep goes on.)
