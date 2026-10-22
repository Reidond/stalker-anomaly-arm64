# xrGame in shards

`src/xrGame` (1,906 files, 1,240 translation units) plus the 58 `.cpp` of `src/xrServerEntities` that the Visual Studio project
compiles into it. The Apple build splits them into ten shards. Each shard has an exact file list, its own CMake target and
a clean starting point: the headers every shard includes were fixed once, in the triage step, so the errors left in a shard
are in that shard's own files.

- [Status](#status) · [Quick start](#quick-start) · [The shards](#the-shards) · [Rules](#rules) · [Tools](#tools)
- [Multiplayer exclusion](#multiplayer-exclusion) · [Fix patterns](#fix-patterns) · [Dialect](#dialect) · [Dependencies](#dependencies) · [Not covered](#not-covered)

## Status

`-fsyntax-only` over all 1,241 xrGame translation units, the same flags the CMake target uses
(`tools/xrgame_probe.py`, multiplayer sources included, no PCH, Apple clang 21, C++17):

|  | before triage | after triage |
|---|---:|---:|
| errors in xrGame / xrServerEntities files | 69,092 | **147** |
| unique error sites (file, line, message) | 4,386 | **141** |
| sites in a header that more than one shard includes (or in another shard's file) | 576 | **0** |
| translation units without an error of their own | 217 | **1,191** |
| translation units without any error | 217 | **1,186** |

The five translation units that show an error but none of their own include `3rd party/cximage` (multiplayer screenshots, mp-stub) or
`xrPhysics/PHCharacter.h:23` (`enum EHitType;`: the physics session's branch replaces it with an include of `alife_space.h`, whose definition now carries `: int`).
On the tree before the netserver session merged, 668 more were blocked by `REFIID` in `xrNetServer/net_shared.h`.

Per shard (the ownership lists are the final ones, applied to both runs):

| shard | TUs | TUs without xrGame errors | errors before | sites before | errors after | sites after |
|---|---:|---:|---:|---:|---:|---:|
| ai-monsters-a | 83 | 83 | 4,596 | 1,268 | 0 | 0 |
| ai-monsters-b | 146 | 143 | 32,877 | 3,421 | 3 | 3 |
| ai-core | 99 | 94 | 8,295 | 821 | 15 | 15 |
| nav-alife | 68 | 68 | 1,404 | 200 | 0 | 0 |
| script-entities | 118 | 111 | 3,860 | 394 | 38 | 32 |
| ui | 141 | 134 | 3,431 | 457 | 9 | 9 |
| objects-items | 146 | 138 | 4,085 | 225 | 14 | 14 |
| objects-world | 112 | 107 | 4,070 | 258 | 12 | 12 |
| core-physics | 161 | 157 | 2,871 | 188 | 10 | 10 |
| mp-stub | 167 | 156 | 3,603 | 116 | 46 | 46 |
| **total** | 1,241 | 1,191 | 69,092 | 4,386 | 147 | 141 |

A shard's "before" numbers are what its own session would have seen: the same shared-header errors were repeated in every
shard. The ai-monsters-b "before" figure counts the state-machine templates (`ai/monsters/states`) that moved to
ai-monsters-a when the shard boundary was drawn. Ownership is defined in [`cmake/targets/xrgame_shards/`](../../cmake/targets/xrgame_shards).

What is left is a per-file long tail, partly Windows-only code: `mp-stub` has `RegistryFuncs.cpp` (the Windows registry, 30 sites; pinned to mp-stub because only
multiplayer code reads it); `script-entities` the mailslot debugger and `script_render_device_script.cpp`; `ui` and `core-physics` the cursor/window calls
(`ClipCursor`, `GetClientRect`, `SetCursorPos`: they wait for engine-input's cursor seam); `objects-*` a few `?:` with a `void`
arm, `goto`/`switch` jumps over initialisers, `shared_str` through `...` and non-const references bound to temporaries.

### Wave 3: script-entities, ui, objects-items, objects-world, core-physics, mp-stub, lua_extensions

The xrgame-rest session took the six shards above to zero errors (`-fsyntax-only` probe with multiplayer on and PCH off, and the real
compile with PCH on and multiplayer off, `macos-arm64` and `ios-arm64`). The real compile found nothing the probe had not: its first
run printed 83 errors for the five single-player shards, the probe's 38 + 9 + 14 + 12 + 10 sites.

| shard | TUs | errors before | sites before | errors after | files moved |
|---|---:|---:|---:|---:|---|
| script-entities | 118 | 38 | 32 | 0 | |
| ui | 139 | 9 | 9 | 0 | `UIGameAHunt`, `UIGameMP` to mp-stub |
| objects-items | 144 | 14 | 14 | 0 | `cta_game_artefact*` to mp-stub |
| objects-world | 112 | 12 | 12 | 0 | |
| core-physics | 161 | 10 | 10 | 0 | |
| mp-stub (multiplayer on, 171 TUs) | 171 | 46 | 46 | 0 | |
| mp-stub (multiplayer off) | 25 | | | 0 | |

With multiplayer off the shard builds `apple_mp_stubs.cpp` and 24 leaf sources, and `tools/xrgame_mp_symbols.py` reports nothing
(see [Multiplayer exclusion](#multiplayer-exclusion)). `lua_extensions` builds for both platforms ([LUA-EXTENSIONS.md](LUA-EXTENSIONS.md)).
What was left from the triage list turned out to be, in this order: non-const references bound to temporaries (`auto&` on a `find()`, a
by-value `GetCurBoosterInfluences()`), `?:` with a `void` arm, `goto`/`switch` over initialisers, `shared_str` through `...`, luabind
member pointers (through a base class: `def_readonly(D T::*)` does not deduce, `CRenderDevice` needs a conversion helper; through an
overloaded or template `bind`; a C-style cast that dropped `const` or the `const float` return type), the Win32 pieces (`SM_*`,
`GetProcessAffinityMask`, the SEH stack-overflow guard in `CScriptStorage::load_buffer`, the Lua Studio and mailslot debuggers, the
registry) and cximage's own copies of `DWORD`/`RECT`/`ntohs`.

## Quick start

```bash
git submodule update --init --depth 1 external/SDL external/openal-soft external/LuaJIT external/ogg external/vorbis \
    external/theora external/SPIRV-Cross external/vkd3d external/sse2neon

# Build directory for real compiles: PCH on, multiplayer sources off (the Apple configuration).
cmake -S . -B build/xrgame -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
      -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DANOMALY_BUILD_XRGAME=ON
cmake --build build/xrgame --target luajit_build         # once: the Lua headers live in the LuaJIT build copy

ninja -C build/xrgame -k 0 xrGame_ui                     # compile one shard, keep going after errors
ninja -C build/xrgame -k 0 xrGame_shards                 # every shard, no archive
ninja -C build/xrgame xrGame                             # the full libxrGame.a (needs every shard to compile)
```

The shard session's **verification command** is the second line of this block for its target, plus the probe for the
per-file picture. The probe wants its own build directory (no PCH, multiplayer on) so that every translation unit exists
and a shared-header error is reported where it is:

```bash
cmake -S . -B build/probe -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
      -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DANOMALY_BUILD_XRGAME=ON \
      -DANOMALY_XRGAME_PCH=OFF -DANOMALY_XRGAME_MULTIPLAYER=ON
cmake --build build/probe --target luajit_build
tools/xrgame_probe.py build/probe --shard ui              # errors of one shard, per file, nothing is written
tools/xrgame_probe.py build/probe                         # the table above
```

A shard is done when `ninja -k 0 xrGame_<shard>` reports no error that points into the shard's own files. Errors that point
into another module's header (`net_shared.h` today) are that module's.

## The shards

Target `xrGame_<shard>` with `-` written as `_`. The list is `cmake/targets/xrgame_shards/<shard>.txt` (every file, `.cpp` and
headers, paths relative to `src/`); `shared.txt` holds the files the triage session owns.

| shard | target | `.cpp` | files | lines | what it is |
|---|---|---:|---:|---:|---|
| ai-monsters-a | `xrGame_ai_monsters_a` | 83 | 341 | 43.8K | `AI\AMonsters\common` (base monster, components, control manager), the state-machine templates every mutant instantiates (`ai/monsters/states`, `group_states`), Bloodsucker, Controller |
| ai-monsters-b | `xrGame_ai_monsters_b` | 147 | 322 | 47.7K | `AI\AMonsters\humans` (stalker, trader: decision maker, movement manager, smart covers) and the other mutants, rats, crow, phantom |
| ai-core | `xrGame_ai_core` | 99 | 284 | 35.0K | `AI\{AComponents,AGroup,ACover,AEval,ADebug,ASpace,ASound}`: action planner, memory manager, agent manager, ... |
| nav-alife | `xrGame_nav_alife` | 68 | 189 | 24.4K | `AI\{ANavigation,ALife}`: level/game graph, path managers, A*, the ALife simulator |
| script-entities | `xrGame_script_entities` | 121 | 269 | 51.0K | `AI\AScript` (script exports) and **all of `src/xrServerEntities`** (server entities, object factory, script engine, property list types) |
| ui | `xrGame_ui` | 142 | 269 | 53.8K | `UI\*` except the multiplayer screens |
| objects-items | `xrGame_objects_items` | 146 | 266 | 49.4K | `Core\Client\Objects\{items & weapons, inventory}` |
| objects-world | `xrGame_objects_world` | 112 | 202 | 48.9K | the other `Core\Client\Objects`: actor, monsters' game objects, anomalies, vehicles, doors |
| core-physics | `xrGame_core_physics` | 159 | 301 | 63.3K | `Core\{Client other, Server SP, Common}`, `Physics\*`: Level, game types for single-player, console, save/load |
| mp-stub | `xrGame_mp_stub` | 167 | 325 | 53.5K | multiplayer game types and UI, GameSpy, accounts, anti-cheat, MP screenshots, file transfer. **Not compiled** for the Apple port, see below |
| *shared* | (none) | 0 | 119 | 19.5K | owned by the triage session |

Derivation. The shard of a file is, in order: a pin in `pins.txt` → `shared.txt` → `overrides.txt` (headers that only one shard
includes belong to that shard) → the file's folder in `xrGame.vcxproj.filters` (the audit's table, M1-AUDIT 7.2) → for files
the filters do not list, the folder of their neighbours. Two deviations from the audit, both from the include graph:
the 128 state-machine template headers `ai/monsters/{states,group_states}` are instantiated by every mutant, so they sit with
the framework in ai-monsters-a (together with Bloodsucker and Controller, which also evens out the a/b sizes: 28K/64K became
44K/48K); and `src/xrServerEntities/**` is one block in script-entities, not spread over the shards by project folder.

`tools/xrgame_shards.py` regenerates the lists (`--check` for CI, `--stats` for the table above, `--deps build/probe --report`
to recompute `overrides.txt` and the dependency tables). Only the triage session runs it.

## Rules

1. **Own files only.** A shard session edits the files in its list. Everything else goes to the triage session.
2. **Headers.** The list says who owns a header. Edit your own freely. A header owned by another shard or by `shared` is not
   yours even when your error points into it: send the exact change (file, line, edit) to the xrgame-triage session, which applies it or
   forwards it. The point of the triage step was that this does not happen often: no site is reported by more than one shard
   any more. A new error in a `shared` header, or the same header failing in two shards, is a triage job.
3. **New sources.** Not in the Visual Studio project, not in any list: a new `.cpp` for the Apple port is added to the
   shard's list by the triage session (`pins.txt`); a file that exists in the `.vcxproj` but is in no list gets a CMake
   configure warning and falls into `core-physics`.
4. **CMake and generated files** (`cmake/targets/xrGame.cmake`, `xrgame_shards/*.txt` except `mp-stub-compiled.txt`,
   `stdafx.h`, `pch_script.h`, `build_config_defines.h`, `src/Include/**`) belong to the triage session, `src/Include/**` to engine-core.
5. **Windows stays working.** Platform code behind `XR_PLATFORM_*`, nothing deleted, no reformatting, no mass case fixes.
   Case-mismatched includes work on the default (case-insensitive) macOS volume; fix one only when it blocks you.
6. **OpenXRay is the answer key**, read-only: `reference/xray-16/src/xrGame`, `xrAICore`, `xrScriptEngine`. Copy portability fixes, not gameplay.
7. **Hand-over.** Leave notes in the shared notes folder, never in the repository.

## Tools

| tool | use |
|---|---|
| `tools/xrgame_probe.py <build> [--shard S] [--file TEXT] [--sample N] [--markdown] [--json F]` | `-fsyntax-only` over the shard's translation units; errors per shard and file, the most frequent sites, sites in a file the shard does not own. `--from-raw` re-reads a `--raw` file. |
| `tools/xrgame_fixit.py <build> --owner <shard\|shared\|any> [--shard S] [--write]` | applies the mechanical fixes below to the files of one owner |
| `tools/xrgame_shards.py` | the lists (triage only) |
| `tools/xrgame_mp_symbols.py <off-build> --mp-build <on-build>` | the link check of the multiplayer exclusion (below) |

`xrgame_fixit.py` reads clang's own fix-its (`typename`, `template`, `template<>`, `Class::` before a member address) and adds what
clang names but does not fix: `this->` for a member of a dependent base class (`explicit qualification required to use member`), `typename` for
`xr_vector<T>::const_iterator I = ...` (clang reports `expected ';'`). `--qualify a,b,c` also adds `this->` to those identifiers where they are
undeclared, `--qualify-in path,path` to every lower-case undeclared name in those files. A run is one pass over all translation units; fixing the
first layer exposes the next (a template body is only instantiated once the code before it parses), so run it until it reports 0 hints.
The triage run changed about 2,150 sites over eight passes (1,741 in the first). Read the diff.

## Multiplayer exclusion

The mp-stub shard is the multiplayer game types, GameSpy, accounts, anti-cheat, MP screenshots, award system, file transfer
and secure messages (171 sources). The mechanism:

- `ANOMALY_XRGAME_MULTIPLAYER` (default **OFF**) in `cmake/targets/xrGame.cmake`. OFF: the `xrGame_mp_stub` target compiles only the files in
  `cmake/targets/xrgame_shards/mp-stub-compiled.txt`; every other file of `mp-stub.txt` is not built and not part of `libxrGame.a`, and the
  shard targets get `XRGAME_NO_MULTIPLAYER`. ON: it compiles them all (what the probe build uses, so the sources stay checkable).
- Headers are never excluded, so single-player code that names a multiplayer class still parses. Three kinds of link-time reference
  remain, each with its own answer:
  1. **Guarded.** Single-player code that only *mentions* a multiplayer class has `#ifndef XRGAME_NO_MULTIPLAYER` around it: the object factory
     entries of the MP game types and the MP actor (`object_factory_register.cpp`), the GameSpy script exports
     (`script_engine_export.cpp`), the MP console commands (`console_commands_mp.cpp`; `register_mp_console_commands()` is commented out in
     `console_commands.cpp` anyway), the reward-event calls in `GameObject.cpp`/`WeaponFire.cpp`/`Level_Bullet_Manager.cpp`, `game_sv_GameState::CheckNewPlayer`,
     and `CLevel::net_start1`, which **refuses every game type but "single"** (log, `net_start_result_total = FALSE`).
     `smart_cast<game_cl_mp*>`/`<game_sv_mp*>` in single-player files is `mp_cl_game_cast`/`mp_sv_game_cast` (`game_cl_mp.h`, `game_sv_mp.h`): the
     RTTI of those classes lives in excluded sources, so the cast is a constant `nullptr` there. Windows expands to the old `smart_cast`.
  2. **Compiled leaf sources** (`mp-stub-compiled.txt`): the MP sources the single-player objects reference and that need nothing else from the
     multiplayer code: `file_transfer` (xrServer's file transfers), `secure_messaging`, `player_account`, the weapon/ammo/shooting dump helpers,
     the two script-export leaves (`script_monster_*_info`) and the small MP windows the XML loader and the message window create
     (`ServerList`, `UIMapList`, `UICDkey`, `UIChatWnd`, `UIGameLog`, `UIMapInfo`, ...).
  3. **Stubs** in `src/xrGame/apple_mp_stubs.cpp`: the five console variables `xrServer::GetServerInfo` prints, `game_sv_mp::IsPlayerBanned`,
     the MP screenshot file names `screenshot_server.cpp` asks `game_cl_mp` for, and the two CTA getters of the main HUD. Each logs and refuses
     (`! Multiplayer is not available in this build (...)`); none can be reached in a single-player game.
- Files the include graph had put into single-player shards but that are multiplayer only moved to mp-stub (`pins.txt`): `UIGameAHunt`, `UIGameMP`,
  `cta_game_artefact`, `cta_game_artefact_activation`.
- `GameSpy`, `xr_dsa_*` and the stats/screenshot signing are not built: the GameSpy script classes are not exported to Lua, the MP game
  types are not in the object factory, so a script that names them gets `nil`.

How to find the next missing symbol (after a merge or a new file): build every shard with multiplayer off (the default) and `xrGame_mp_stub`
once with `ANOMALY_XRGAME_MULTIPLAYER=ON` (only to learn what the excluded sources define), then
`tools/xrgame_mp_symbols.py build/xrgame --mp-build build/mp` lists, per excluded source, the symbols that the OFF objects reference and
only it defines; exit status 0 means libxrGame.a has no unresolved multiplayer reference (0 on `main` after this wave). For each new symbol choose,
cheapest first: guard the single-player reference with `XRGAME_NO_MULTIPLAYER`, add a leaf source to `mp-stub-compiled.txt` (check with the
script that its own references stay inside the compiled set), or stub it. A class with a vtable cannot be stubbed cheaply (every
virtual of the class has to exist): guard the use instead. The check does not see symbols that nobody in xrGame defines (engine, shim,
GameSpy SDK): those are the integration link's.

## Fix patterns

What the 69,092 errors were, and the fix used (so a shard can apply the same one by hand):

| clang says | cause (MSVC accepted it) | fix |
|---|---|---|
| `ISO C++ forbids forward references to 'enum' types` | `enum E;` | `enum E : int;` or `: u32` at the declaration **and** the definition. The type MSVC chose: `int`, `unsigned` when an enumerator is `u32(-1)`/`0x8...ui32`. |
| `declaration of 'X' shadows template parameter` | `typedef P P;` in a class template over `P`, nested template-template parameter lists reusing outer names | rename the parameter (`T_` prefix) and keep the member name; drop the unused names of nested template-template parameters |
| `explicit qualification required to use member 'f' from dependent base class`, `use of undeclared identifier` in a template body | unqualified member of a dependent base | `this->f` (fixit tool); a type: `typename inherited::T` or a local typedef |
| `missing 'typename' prior to dependent type name`, `expected ';' after expression` on `X<T>::iterator I` | missing `typename` | `typename ` (fixit tool) |
| `reference to overloaded function could not be resolved`, `use 'template' keyword` | `X<T>::f<a>(...)` | `X<T>::template f<a>(...)` (fixit tool) |
| `template specialization requires 'template<>'` | `class C<int> ...` | `template<> ` (fixit tool) |
| `must explicitly qualify name of member function when taking its address` | `&f` for a member | `&Class::f` (fixit tool) |
| `definition of variable with array type needs an explicit size` | `xr_token t[];` in a header | `extern xr_token t[];` |
| `friend class X` finds nothing | MSVC resolves a friend to a class in the global scope | forward declare `class X;` at global scope, `friend class ::X;` |
| `pasting formed '"a""b"'` | `#a ## b` on string literals | `#a b` (adjacent literals concatenate) |
| `unknown type name 'boolean'`, `WM_USER`, `HKEY_*`, `ClipCursor`, `GetClientRect`, `ultoa` | Win32 names the shim deliberately lacks | a local definition under `#ifndef`, or `XR_PLATFORM_APPLE` code; the window system belongs to engine-core/engine-input |
| `non-const lvalue reference ... cannot bind to a temporary` | MSVC extension | a named local |
| `cannot jump from switch statement to this case label` | jump past an initialiser | braces around the case |
| `cannot pass object of non-trivial type through variadic function` | `shared_str` to `...` | `.c_str()` |
| `right operand to ? is void` | `cond ? void_call() : value` | `if`/`else` |

## Dialect

The shards compile with `ANOMALY_MS_COMPAT=OFF`: ISO C++17 plus `-fms-extensions -fdelayed-template-parsing -fno-operator-names` (XRCORE.md).
Measured on 30 translation units per shard after the header fixes: 59 errors without `-fms-compatibility`, 54 with it, while
the errors in other modules' headers go from 4-27 per shard to 180-520 (the shim and luabind do not survive Microsoft mode). It buys
nothing once the shared headers are fixed, so it stays off. `-fdelayed-template-parsing` stays: it is what lets MSVC-style template bodies parse,
and it hides nothing the shard sessions need to see; it is also why a shared template can look clean until the first instantiation.

`xrGame/stdafx.h` is the precompiled header (`ANOMALY_XRGAME_PCH`, default ON for the shard targets): CPU time of `xrGame_ui` 159 s → 98 s.
It matches `/Yu stdafx.h`: everything before the `#include "stdafx.h"` of a `.cpp` is ignored by MSVC and by the PCH alike.

## Dependencies

Every shard includes the same core: `xrCore` (69 headers), `xrEngine` (44-85), `Include` (15-27), `Common` (the shim), xrCDB,
xrNetServer, xrSound, xrXMLParser, luabind, LuaJIT, ODE and the 119 `shared` headers. Header counts per shard and module
(headers reached by the shard's translation units, `tools/xrgame_shards.py --deps build/probe --report`):

| shard | xrEngine | xrCore | Include | Common | xrPhysics | xrCDB | xrNetServer | xrSound | xrXMLParser | Layers | 3rd party |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ai-monsters-a | 60 | 69 | 20 | 8 | 9 | 2 | 9 | 1 | 2 | 1 | 80 |
| ai-monsters-b | 59 | 69 | 21 | 8 | 2 | 2 | 9 | 1 | 2 | 2 | 80 |
| ai-core | 59 | 69 | 21 | 8 | 0 | 2 | 9 | 1 | 2 | 1 | 80 |
| nav-alife | 44 | 69 | 15 | 8 | 0 | 2 | 9 | 1 | 2 | 1 | 81 |
| script-entities | 67 | 69 | 20 | 10 | 0 | 2 | 9 | 2 | 2 | 2 | 84 |
| ui | 73 | 69 | 23 | 11 | 0 | 2 | 9 | 1 | 2 | 1 | 88 |
| objects-items | 66 | 68 | 21 | 10 | 0 | 2 | 9 | 1 | 2 | 2 | 88 |
| objects-world | 70 | 68 | 22 | 10 | 1 | 2 | 9 | 1 | 2 | 2 | 88 |
| core-physics | 85 | 72 | 27 | 10 | 2 | 2 | 10 | 2 | 2 | 1 | 88 |
| mp-stub | 64 | 70 | 19 | 10 | 0 | 2 | 9 | 1 | 2 | 1 | 90 |

xrPhysics headers matter to ai-monsters-a (9, the monster physics components), core-physics, ai-monsters-b and objects-world; the `Layers` headers are
the render interface in `Include/xrRender` and the R4 headers some UI and debug code names. No shard includes xrParticles.

Headers of other shards that a shard's translation units include (row: shard, column: owner; the parenthesised number is the shard's own):

| shard \ owner | mon-a | mon-b | ai-core | nav | script | ui | items | world | core | mp | shared |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ai-monsters-a | (203) | 6 | 83 | 62 | 74 | 28 | 55 | 57 | 59 | 8 | 94 |
| ai-monsters-b | 193 | (166) | 144 | 60 | 74 | 32 | 77 | 63 | 64 | 9 | 108 |
| ai-core | 42 | 24 | (179) | 68 | 52 | 24 | 56 | 59 | 78 | 9 | 102 |
| nav-alife | 2 | 0 | 11 | (112) | 56 | 22 | 14 | 16 | 30 | 6 | 65 |
| script-entities | 93 | 49 | 99 | 93 | (140) | 88 | 166 | 94 | 79 | 58 | 109 |
| ui | 44 | 1 | 28 | 76 | 55 | (164) | 98 | 74 | 61 | 61 | 83 |
| objects-items | 41 | 21 | 66 | 72 | 50 | 46 | (170) | 62 | 72 | 25 | 88 |
| objects-world | 42 | 2 | 35 | 72 | 76 | 69 | 92 | (120) | 107 | 23 | 88 |
| core-physics | 44 | 24 | 64 | 60 | 71 | 64 | 78 | 70 | (162) | 44 | 61 |
| mp-stub | 5 | 0 | 13 | 30 | 48 | 80 | 62 | 59 | 51 | (163) | 38 |

Everything depends on everything through the object headers (`Actor.h`, `Level.h`, `inventory_item.h` ...), so a header a shard
needs changed is usually in `shared`, core-physics or objects-items. nav-alife is the most self-contained shard (112 of its own
headers, 65 shared, almost nothing else); script-entities and mp-stub reach into every other shard (script exports and multiplayer
screens name game objects). ai-monsters-b depends on ai-monsters-a (193 headers): the monster shards go in that order.

Blockers outside xrGame that shard sessions will meet (recorded in the shared notes `shim-requests.md`): `NONE` defined by the shim (`game_sv_artefacthunt.h` works around it with an `#undef`), `HKEY_*`/window-system calls (engine-core, engine-input),
`CLONE_MTL_SOUND_DO` pasting `mtl_pair->` (xrCore/xrEngine macro, physics).

## Not covered

- **Only parsing.** `-fsyntax-only` and the real compile (`xrGame_shards`, 2.5 minutes for all shards on an M-series Mac) agree on the errors, but nothing links: the
  link of `libxrGame.a` into the game is the integration step, and the `nm`-based tools above are how the multiplayer stand-ins are found.
- **`#ifdef DEBUG` code** is not compiled (the build defines `NDEBUG`; `-DANOMALY_ENGINE_DEBUG=ON` turns it on). It will have the same
  patterns (`action_planner_inline.h` debug printing was patched for `EVALUATORS`/`OPERATOR_VECTOR`); `xrgame_fixit.py` finds them once the build is switched on.
- **Warnings are not errors yet.** The shard targets silence what only restates an MSVC-ism (`-Wcomment`, `-Wexplicit-specialization-storage-class`,
  ...). Left visible on purpose, because they are undefined behaviour or 64-bit truncation that MSVC compiled as written:
  `-Wnonnull`, `-Wundefined-bool-conversion` (`if (this)`, `if (&ref)`), `-Wreturn-type`, `-Wpointer-to-int-cast`, `-Wformat-security`.
  A shard fixes the ones in its files as part of its job.
- **Includes.** 2,000+ include spellings differ in case from the file on disk and 4 carried a trailing blank in the quotes; only the latter were fixed.
