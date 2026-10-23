# Mod manager

The mod manager inside the Anomaly app: a launcher with the mod list (enable, disable, load order), importers (Files app,
in-app browser, links, FOMOD installers) and the engine side that mounts the enabled mods at start-up, Mod Organizer 2 style:
every mod stays in its own folder and nothing is overwritten. This file has one section per part, each written by the session
that built it.

## Mounting

The engine mounts the enabled mods when it starts, in the order of `modlist.json`. The shell reads the list and writes the mods
into the generated `fsgame.ltx`; `CLocatorAPI` registers each mod's files where they would lie in the game folder, but leaves them
in the mod's folder. Nothing is copied, packed or converted, so a multi-gigabyte texture mod costs a directory scan and nothing
else (measurements below).

### Where things are

| What | Path |
|---|---|
| mods root (`AppleShell::GetPaths().mods_root`) | iPad: `Documents/Mods` (the Files app shows it, excluded from backups); Mac: `~/Library/Application Support/Anomaly/Mods` |
| `ANOMALY_MODS_ROOT=<dir>` | replaces the default on both |
| a Mac run with `ANOMALY_STATE_ROOT=<dir>` | `<dir>/Mods` |
| one mod | `<mods root>/<id>/mod.json` and `<mods root>/<id>/content/{gamedata,db}/...` |
| the list | `<mods root>/modlist.json` = `{"schema":1,"mods":[{"id","enabled"}, ...]}`, lowest priority first |

The shell creates the mods root at start. The engine never writes into a mod folder (see "Writes").

### Mount order

Lowest priority first; a later entry replaces an earlier one with the same path:

1. the stock archives of `db/`;
2. the Monolith overlay;
3. `db/patches`;
4. `db/addons`;
5. the enabled mods in list order. For each mod: the archives under `content/db` (in name order, subfolders too), then the loose
   files of `content/gamedata`;
6. the game folder's own loose `gamedata/` (a manual install, and the files the game itself writes there): wins over everything.

So within one mod the loose files beat its archives, a later mod beats an earlier one, and a manual install beats every mod.
`content/` entries other than `db` and `gamedata` are ignored (matched without regard to case: `GameData` works).

### How it is done

`AppleShell::ResolveMods` (`src/Platform/Apple/Mods.mm`) reads `modlist.json` and checks each enabled entry (`<root>/<id>` exists and
has `content/`), then `WriteFsGameLtx()` puts one line per mod between `$arch_dir_addons$` and `$game_data$`:

```
$mod_<id>$ = true | false | <mods root>/<id>/content
```

`CLocatorAPI::_initialize` treats an id that starts with `$mod_` as a mod and calls `mount_mod(id, content)`:

- `content/db` goes through the normal archive scan (`Recurse`), like `db/addons`: each archive's own header says where its files
  appear (`entry_point`).
- `content/gamedata` is walked once. Every file is registered under `<$fs_root$>\gamedata\<relative path>` with
  `vfs = 0xfffffffe` ("mounted") and `ptr` = an index into a table of the real paths. Opening such an entry
  (`r_open`, `rs_open`) opens the real file through the same readers as a loose file (`CFileReader`, `CVirtualFileReader`'s mapping,
  `CFileStreamReader`), so a texture is mapped from the mod's folder when it is used, and never read before.
- Registering is "last wins", so mounting in list order gives the priority; the scan of the game folder's own `gamedata/` (`$game_data$`)
  comes after the mods and replaces what it has.
- Registry names are lower-case; the real path keeps the capitals the folder has, and `xr_native_path` resolves the rest without regard
  to case, so mixed-case mods work on a case-sensitive volume too. Dot files (`.DS_Store`, AppleDouble `._x`) are not mounted.
  A file over 4 GB is skipped (the registry sizes are 32-bit).

The engine log gets one line per mod, `* mod <id>: <n> archives, <n> files in <n> folders mounted from gamedata (<ms> ms)`; the shell
log gets the effective order (lowest priority first) and one line per mod with its archives and the file count of its `mod.json`, and a
line for every entry that was left out and why.

### When the list is read

`AppleShell::Init` resolves the list and writes `fsgame.ltx`; `AppleShell::WriteFsGameLtx()` does it again, and the app calls it
before it starts the engine after the launcher closes, so what the player set in the launcher is what mounts. The engine reads the
file once, so changes apply at the next engine start; there is no remount in a running engine.

### Flags

| Flag | Effect |
|---|---|
| `--mods-off` | no mod is mounted, whatever `modlist.json` says (also in `commandline.txt`) |
| `--no-launcher` | do not show the launcher; uses `modlist.json` as it is. Accepted by the shell (`AppleShell::SkipLauncher()`) |
| `--script`, `--exit-after` | scripted and timed runs also skip the launcher and use `modlist.json` as it is |

### Bad input

| Case | Result |
|---|---|
| no `modlist.json` | no mods; folders in the mods root count as disabled |
| not JSON, not an object, no `mods` array, `schema` other than 1 | no mods, one line in the shell log (`Mods().error`); the game starts |
| an entry that is not an object, has no usable `id` (empty, a path, starts with a dot, other characters than letters, digits, `.`, `_`, `-`), or repeats an id | that entry is skipped with a note, the others mount |
| `enabled` missing or not a boolean | disabled |
| enabled, but the folder or its `content/` is gone | skipped with a note |
| a mods root path with `|`, `=` or a line break | no mods (`fsgame.ltx` cannot hold the path) |

### What mods can and cannot do

- Mod files appear to the engine and to scripts through the file system (`FS.r_open`, `exist`, `file_list`, `getFS():file_list_open`,
  `ini_file`, `get_script`), exactly like loose files in `gamedata/`.
- `io.open(path, "r")`, `io.lines`, `loadfile` and `dofile` in scripts also find a mod's file: the script engine's read hook returns the mod's real
  path when nothing is at the virtual path itself. Writing never takes this route.
- **Writes.** A write (`FS.w_open`, `io.open(path, "w")`) uses the virtual path, i.e. the game folder's `gamedata/`, never the mod's file,
  and the new file then wins over the mod. Nothing is ever written into a mod folder.
  `$game_config$\axr_options.ltx` (MCM's settings) is such a write: see "Writes" below.

### Caches

- **Shader translations** (`Caches/Anomaly/shaders/xlat`): keyed by the source text, name, macros and options, with the content hash of every include checked when an entry is
  read. A mod that changes a shader or an include gives another key or a failed check, so the translation is made again; no stale entry is served.
- **Lua bytecode** (`Caches/Anomaly/lua`, off unless `-lua_cache`): one entry per script path with the size and a 128-bit hash of the exact text it was compiled from. A mod
  that replaces a script is a stale entry, and the script is compiled again and the entry rewritten. Switching the mod on and off rewrites the entry each time, and
  never serves the wrong text.

### Writes

Mods are read-only to the engine. A write by the virtual name of a mod's file (`FS.w_open`, `io.open(path, "w")`) goes to the game folder's
`gamedata/` and not to the mod; the new file then wins over the mod (it is the game folder's own). `mods_mount_test write_isolation` writes by the
name of a file that mod a provides and compares a snapshot of the whole mods folder before and after: the same files with the same sizes.

What the game writes into `gamedata/` of the data root (found by running a scripted menu session on a scratch data root, and by reading the
stock scripts): `configs/axr_options.ltx`, MCM's settings (`cfg_file` in `utils_data.script` opens it with `io.open(path, "a+")` and rewrites it),
and nothing else in a menu run. The stock debug scripts write a few more files (`debug_print.txt`, `axr_debug_log.txt`, ...) with relative paths,
only with `-dbg`. So with an unchanged build the data root gets `gamedata/configs/axr_options.ltx`: it is the one write into the game folder, it
exists in a data root that earlier runs used, and since it is the game folder's own `gamedata/` it wins over any mod's file of that name. Redirecting
it to appdata is the systems-check session's change and is not part of the mod mounting; with the redirect, nothing here changes.
One consequence to know: `io.open(path, "a+")` of a name that only a mod provides creates an empty file in the game folder (the mod's copy is
not the one that is appended to), which `cfg_file` hits only for a mod that ships its own `axr_options.ltx`.

### Tests

- `ctest -R mods_mount` (`tools/mods_mount_test`, Mac): 13 scenarios, each in its own process because xrCore initializes once. They generate a
  stock-like archive, mods and a manual install in a temporary folder, write the `fsgame.ltx` the shell would, start the real `CLocatorAPI` and read
  what it serves: list order and the later mod winning a file (also reversed), a mod replacing a stock file, a mod's archive and its loose files, the
  game folder's own `gamedata/` winning over every mod, a disabled mod, an unlisted folder and a removed one, `GameData/Configs/CaseTest.LTX` opened as
  `casetest.ltx`, `--mods-off`, `mods_delete` / `mods_clear` (below), a mods folder with a space and a non-ASCII letter, `rs_open` of a mounted file, `file_list` of a mounted folder, the
  engine log line per mod, writes that stay out of the mods folder, and 19 variants of `modlist.json` (empty, truncated, not JSON, array at the top, schema
  2, no `mods`, entries that are not objects, ids with a path or a dot, repeated ids, `enabled` that is not a boolean, ...). It also passes on a
  case-sensitive APFS volume (a disk image; `TMPDIR=<volume> mods_mount_test case`).
- `tools/mods_check/run_mac.sh <scenario>` (`ab`, `ba`, `b_off`, `mods_off`, `bad_json`): an unattended game run (offscreen, silent, own state
  root) with two generated mods; a `--script` timeline in the main menu calls into them. In `ab` the log shows `MODTEST ini value=from_b`
  (mod b's `modtest.ltx` over mod a's), both mods' scripts running, `MODTEST stock file replaced by mod b: true` (a file of `configs.db0`), and
  `MODTEST io.open data of mod b` (a script reading a mod's file with `io.open`). `ba` gives `from_a`; `b_off`, `mods_off` and `bad_json` show the
  mods missing and the game running (`bad_json` also logs the parse error). The probe script is a manual install in the game folder's `gamedata/`
  and works in all of them.
- Caches: with `-lua_cache`, a first run stores 72 entries; a second run with the mods' scripts changed has 70 hits, 2 stale (the two mod scripts),
  and runs the new text. For the shader translations, a mod that replaces `shaders/r3/common.h` (one comment line added) makes 95 of the 137
  translations of the main menu run again (42 from the cache against 137 without the mod), and nothing stale is served.

### Removing mods from a test run

A device has no way to delete a file from outside, so the app does it: `AppleShell::RemoveModFolder(id)` and `ClearModsRoot()` (`Mods.mm`), exposed to
`--script` timelines as `<t> mods_delete <id>` and `<t> mods_clear` (put it last: the engine still has the mounted mods open). The launcher can use the
same functions to delete a mod.

They are safe by construction, not by a path check: they take no path. They act on the mods root the shell resolved (`Paths::mods_root`, handed over by
`Init` through `SetModsRoot`) and refuse unless

- that folder's last component is exactly `Mods`, and
- it has a marker: a regular file `modlist.json`, or it is empty, or everything in it is a mod folder (a real folder holding a `mod.json`).

`RemoveModFolder(id)` also needs `id` to be a plain name (no separator, not `.`/`..`/empty) and `<root>/<id>` to be a real folder (not a link) with a `mod.json`.
Entries go with `remove_all`, which never follows a symbolic link: a link is removed as a link. Each removal is logged (`anomaly: script: removed <path>`).

The tests follow the same rule: `mods_mount_test` makes every folder inside a fresh `mkdtemp` folder that carries a marker file, points
`ANOMALY_MODS_ROOT` at `.../Mods` inside it, never passes or computes a parent path, and only removes a tree whose marker it finds
(`mods_mount_removal_refuses` runs the functions on folders that are not a mods root: another name, no marker, a folder without `mod.json`, lower case, a link;
they refuse and delete nothing). `tools/mods_check/run_mac.sh` only removes an `--out` folder that holds the marker file of an earlier run of itself.

### Cost of a big mod

Mounting reads the directory tree and nothing else; a texture is mapped when the engine opens it. `mods_mount_test perf <files> <mb>` writes a mod
of that many files and megabytes for real and mounts it (Apple M2 Pro, with the machine under heavy load from other builds, so the upper numbers
are pessimistic):

| Mod | `mount_mod` (registry scan) | whole file-system init of the test (xrCore start included) |
|---|---|---|
| 1 file, 1 MB | 0.2 to 0.3 ms | 107 to 176 ms |
| 3000 files, 2048 MB | 28 to 68 ms | 142 to 209 ms |
| 20000 files, 2048 MB | 150 to 285 ms | 257 to 395 ms |

About 10 microseconds per file, independent of the size; a 2 GB texture mod of a few thousand files costs well under 0.1 s at every start. Nothing is
copied, packed or cached, so there is nothing to rebuild when a mod changes.

<!-- iPad -->
