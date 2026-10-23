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

## Import and FOMOD

`ModManagerCore` (`src/Platform/ModManagerCore`) is a Swift static library with no engine code. It reads
`<mods root>/modlist.json` and `<id>/mod.json`, turns archives and folders into installed mods, runs FOMOD installers, and
shows the FOMOD wizard. It has no other UI. Host tests: `tools/modmanager_test` (ctest name `modmanager_core`).

```swift
let library = try ModLibrary(root: modsRoot)            // @Published mods, in load order, lowest priority first
let importer = ModImporter(library: library)
let plan = try await importer.inspect(fileURL)          // reads the table of contents, finds the mod root and any installer
if let installer = plan.fomod {                         // a FOMOD, or the synthetic one for option folders
    FomodWizardView(installer: installer,
                    onFinish: { choices in Task { _ = try await importer.install(plan, choices: choices) { _ in } } },
                    onCancel: { })
} else {
    let mod = try await importer.install(plan, choices: nil) { fraction in }
}
```

Beyond the contract, all additive: `InstalledMod` also carries `author`, `summary`, `origin`, `installedAt` and `usedInstaller`;
`ModLibrary.rename(_:to:)` and `downloadsURL`; `ImportPlan` also carries `version`, `author`, `details`, `fileCount` and
`sizeBytes`; `inspect(_:origin:)` takes a `ModOrigin` (kind `file`, `url` or `browser`, plus the URL) for what goes into
`mod.json`'s `source`; `ModImporter.environment` answers a FOMOD's file questions; `FomodWizardView` has a second `init`
with that environment. `ImportPlan(displayName:source:fomod:warnings:)` is public for previews and cannot be installed.

### Threads, progress, cancel

- `inspect` and `install` do their blocking work on a background queue. Cancelling the Task stops them: the extraction loop
  checks a flag for every block it writes, the staging folder is removed and the library is untouched
  (`CancellationError`).
- The `progress` closure is called on the main queue, at most about ten times a second, from 0 to 1. The last call (`1`)
  comes after the mod is in the library. About 3% at the end is the SHA-256 of the archive.
- `ModLibrary` is not tied to an actor: a lock keeps it consistent, and `@Published mods` is published from the thread
  that made the change. The importer adds the mod on the main actor; call the library from the main thread in views.

### Archives

`.zip`, `.7z` (LZMA, LZMA2, PPMd, BZip2, Deflate, Copy, BCJ filters) and `.rar` (RAR 4 and RAR 5) through libarchive, and plain
folders. Built from source, like the other dependencies, for macOS and iOS (`cmake/deps/LibArchive.cmake`):

| Library | Where | Notes |
|---|---|---|
| libarchive 3.8.10 (BSD-2-Clause) | `external/libarchive` | only the zip, 7-zip and RAR readers, plus the zip and 7-zip writers for the tests; fixed `cmake/deps/libarchive/config.h`, no probing; no iconv, ACLs, external programs (`fork` is not on iOS), or disk reader and writer |
| liblzma from xz 5.8.4 (0BSD) | `external/xz` | its own CMake, tools and tests off; 5.6.0 and 5.6.1 are the backdoored releases and are not used |
| zlib, bzip2 | SDK | |

Credits for both are in the README.

- **Streaming.** Nothing is held in memory as a whole: libarchive hands over decoded blocks and they go straight to the file.
  Memory stays at a few MB for a multi-GB mod. `inspect` reads only headers, except in solid 7z and RAR where libarchive has to
  decode to skip, so listing a solid archive costs one decode of it.
- **Paths.** Every name is normalized (`\` to `/`, `.` and empty parts dropped) and then refused, for the whole archive, if it
  is absolute, has a drive letter, a control character or a `..`. Entries are written with `open(O_NOFOLLOW)` below a staging
  folder that starts empty.
- **Links and special files** (symlinks, hard links, devices, FIFOs): the archive is refused.
- **Junk** (`__MACOSX`, `._*`, `.DS_Store`, `Thumbs.db`, `desktop.ini`) is left out.
- **Windows programs** (`.exe`, `.dll`, `.msi`, `.bat`, `.cmd`, `.com`, `.scr`, `.ps1`, `.vbs`) are never installed; the plan has
  a warning with the count and names. A warning also says when `bin/` files are present (this port does not use `bin/`) and when
  files lie outside `gamedata/` and `db/` (they are stored but the engine never mounts them).
- **Checksums.** A damaged archive fails with `ModImportError.corruptArchive` (zip and 7z checks, RAR hashes), at `inspect` or
  while extracting; the staging folder is removed.
- **Disk space.** Before anything is written the bytes to install, plus the larger of 32 MB and 2%, are compared with the
  volume's available capacity (`volumeAvailableCapacityForImportantUsage`); running out anyway gives `diskFull`.
- **Not handled:** password protected archives (`encrypted`), multi-volume RAR (`.part1.rar`), nested archives, and Deflate64.
  An archive of more than 400,000 entries is refused.
- A file from the Files app is read under its security scope, which the plan keeps open until it is released.

### Where the mod root is

`inspect` finds it from the table of contents alone:

1. A `fomod/ModuleConfig.xml` (shallowest, at most four folders deep) means an installer; its folder is the installer's root.
   If the config cannot be read, a warning says so and the archive installs as if it had no FOMOD.
2. Otherwise the shallowest folders that have a child `gamedata` or `db` are the roots (case-insensitive, at most four deep,
   never inside another root or a game folder). `ModName/gamedata`, `ModName v1.2/ModName/gamedata` and `db/mods/x.db0` all work.
3. Without those, folders that hold `scripts`, `configs`, `textures`, `meshes`, `sounds`, `shaders`, `anims`, `levels` or
   `spawns` are the roots and their content goes into `gamedata/` (only those folders, not a readme beside them).
4. One root installs without a question. Several roots (`00 Main`, `01 Optional`, `Main` and `Options/Alt`) become options in a
   synthetic installer: one step, one group "Options" that needs at least one pick, an option named like main, core, base,
   required, default or standard (or the first when numbered) preselected, and later folders win a shared file. It
   is a real `FomodInstaller` (`isSynthetic`), so the wizard and the stored choices work the same way.
5. Nothing found: `ModImportError.noGameContent`.

A readme or picture directly in a root is not installed; files beside the root are left out, with a warning when they look like
game files.

### FOMOD

Parsed from `fomod/ModuleConfig.xml` and `fomod/info.xml`, schema 4.0 and 5.0, tolerant like the installers are in practice:
UTF-8 and UTF-16 (BOM or none, whatever the declaration says), any case of tag and attribute names, namespace prefixes, bare `&`.
Source paths may use `\` and any case.

| Feature | Behavior |
|---|---|
| `installSteps`, `optionalFileGroups`, `plugins` `order` | `Explicit` keeps document order, `Ascending` (the default) and `Descending` sort by name |
| group `SelectExactlyOne` / `SelectAtMostOne` | radio buttons; the first needs a pick, the second can be cleared |
| group `SelectAtLeastOne` | checkboxes; Next and Install wait for a pick; the first usable option starts on |
| group `SelectAll` / `SelectAny` | all on and locked / free choice |
| plugin type `Required` / `Recommended` | on and locked / preselected |
| plugin type `NotUsable` / `CouldBeUsable` | off and locked / selectable, with a note |
| `typeDescriptor` with `dependencyType` | default type and patterns, first match wins, evaluated on the flags of the earlier steps |
| step `visible` | dependencies on flags and files; a hidden step is skipped (and its choices are not stored) |
| `conditionFlags` | set when the plugin is on; a later plugin overwrites an earlier flag |
| dependencies | `And` / `Or`, nested, `flagDependency` (case-insensitive value), `fileDependency` (`Active`, `Inactive`, `Missing`), game and extender versions count as met |
| `requiredInstallFiles`, plugin `files` | `file` and `folder`; `alwaysInstall` installs without the plugin, `installIfUsable` unless it is `NotUsable` |
| `conditionalFileInstalls` | each pattern whose dependencies hold on the final flags |
| `priority` | per file or folder, default 0; files are applied by priority, then by order, and the later one wins a destination |
| `destination` | omitted: same path as the source; empty: a file goes to the root by its name, a folder merges into the root; ending in `/`: a file goes into that folder; otherwise the new path |

- `fileDependency` asks `ModImporter.environment`, which by default looks for the path in the enabled mods' `content/`.
- An installer written for a `Data` folder (everything under `scripts/`, `configs/`, ...) lands in `gamedata/`.
- A source the archive does not have is skipped, not an error. Choices that no longer fit (a renamed plugin) fall back to the defaults.
- The choices made are stored in `mod.json` as `fomod_choices`: `[{"step", "group", "plugins": [...]}]`, by name, for the steps
  that were shown.
- Images and descriptions: the config, `info.xml` and image files (png, jpg, bmp, gif, webp, up to 4 MB each, 96 MB in all) are
  copied out of the archive during `inspect` into a temporary folder that is removed with the installer; the wizard decodes
  them at screen size.

### FomodWizardView

One page per step, built from stock SwiftUI so it looks like the rest of iPadOS 27 and macOS 27: an inset-grouped `List` on iPadOS
and a grouped `Form` on the Mac, one section per group, Dynamic Type text styles and semantic colours. It is meant to sit in a
host that has a title and a Cancel button, as the launcher's import sheet does (`showsCancel: true` adds a Cancel button for a
bare window).

| Element | Control |
|---|---|
| Page title | the first section's header: module name, "Step n: name", and the module picture on the first step |
| Group | a `Section`; the footer says the rule ("Choose one.", "Choose at least one.") and turns into a red message when it is broken |
| Choose any / at least one / all | `Toggle` with the switch style, disabled (and so dimmed) for locked options |
| Choose one / one or none | rows with a trailing checkmark, like an inline picker |
| Option | name (headline), description (secondary, up to three lines), a mark (Recommended with a star in the accent, Required or Included with a lock, Not usable with a warning), and a 62 x 44 thumbnail when the installer has a picture |
| Back, Next / Install | `.glass` and `.glassProminent` button styles on iOS 26 / macOS 26 and later (in a `GlassEffectContainer`), `.bordered` and `.borderedProminent` before; the label morphs from Next to Install |

The accent is the launcher's amber and comes from the environment; the host may also give a backdrop (the list's own background
is then hidden). Nothing else is custom-drawn:

```swift
FomodWizardView(installer: installer, onFinish: finish, onCancel: cancel)
    .fomodTheme(FomodTheme(accent: Theme.accent, backdrop: nil))     // nil: the system's grouped background
```

**Motion.** A step change is a spring (0.3 s, no bounce): the new page slides in 28 pt from the side the player is heading to and
fades in, the old one fades out, softer than the entrance; the title and the Next / Install label interpolate. The checkmark of a
checkmark row pops in with scale 0.25 to 1, opacity and a 4 pt blur; switches and press feedback are the system's. Moving the cursor
is not animated: a D-pad is used constantly and the ring must be where the finger is. With Reduce Motion every one of these is a
short fade. Reduce Transparency and the glass fallbacks are the system's own.

**Input.**
- Touch and mouse: tap a row, flip a switch, press a button.
- Keyboard: the arrow keys move the cursor over the options of the page and then the footer buttons; Return or Space presses it;
  Escape is Back, and Cancel on the first step. The page scrolls to the cursor.
- Controller: the view reads no controller. `FomodSession` (public, `ObservableObject`) holds the steps, selections, flags,
  validity and the cursor, and takes the same commands the keyboard produces: `session.send(.up / .down / .left / .right /
  .confirm / .back / .next)`. Whoever reads the pad (the launcher) creates the session, shows
  `FomodWizardView(session:onFinish:onCancel:)` and forwards D-pad to the four directions, Cross to `.confirm`, Circle to `.back`
  and R1 or Options to `.next` (press the primary button if the rules allow).
- The cursor is drawn the way the launcher draws its pad focus: a 3 pt accent ring (radius 12, 4 pt outside the row) and an accent
  tint at 16% behind the row; on a footer button the ring sits in its own margin. It shows only while a pad or the keyboard was the
  last input (`session.showsCursor`); a touch or a click hides it.
- Locked options can hold the cursor, so the reason for the lock can be read, but not be changed.
- VoiceOver: every row has a label, a selected value and, for locked ones, a hint.

**What was checked.** The stock controls cannot be drawn by `ImageRenderer`, so the wizard is hosted in a window that is never shown
and drawn into a bitmap, as the launcher's snapshot tool does (real macOS controls); the test writes PNGs when
`MODMANAGER_TEST_ARTIFACTS` is set: light and dark, page 2 with the pad cursor on a locked option, the cursor on the primary
button, with Cancel, Reduce Motion, and a custom accent and backdrop. Limits: those are the macOS controls (the iPad list code is
compiled for iOS and mirrors the launcher's, but has not been drawn); a window that is never shown is not key, so switches show
their position but not the accent track; and a view with Liquid Glass buttons draws as an empty bitmap in such a window (as it
does in `ImageRenderer`), so the pictures use the bordered styles. The glass buttons, the motion and the pad on an iPad 27 have
not been seen.

### Metadata and ids

Name, version and author come from `info.xml` (then the config's `moduleName`); otherwise from the archive name with the
version stripped: `Weapon_Pack-v1.4.2.7z` is "Weapon Pack" 1.4.2, a Nexus name `Cool Mod-1234-1-5-1678123456.zip` is
"Cool Mod" 1.5, `Mod (1).zip` is "Mod". The id is the name as a lowercase ASCII slug of at most 48 characters (Cyrillic and
accents are transliterated), with `-2`, `-3` for a taken one. `mod.json` also gets `source` (kind, URL, file name, SHA-256
of the archive), `installed_at`, `file_count` and `size_bytes` of what was written.

A new mod is added last in the load order (the highest priority) and is on. A finished download in
`<mods root>/.downloads/` is deleted after a successful install; files from anywhere else are never touched.

### Library

- `modlist.json` and `mod.json` are written with a temporary file and a rename. An unreadable `modlist.json` is copied to
  `modlist.json.corrupt` and the folders are used instead.
- Installs go through `<mods root>/.staging/<uuid>/` (same volume, so the final move is a rename); removing a mod moves it
  there first. Leftovers are deleted when the library opens.
- `move(fromOffsets:toOffset:)` has the semantics of SwiftUI's `onMove`.
- `conflicts()` walks each enabled mod's `content/` and compares paths lower-cased with `/`, as the engine resolves them; it
  lists `gamedata/` loose files and the names of `db/` archives (not what is inside an archive). The winner is the last enabled
  mod. The file lists are cached per mod until its `mod.json` changes.

### Build

`cmake/targets/ModManagerCore.cmake`, opt-in with `-DANOMALY_BUILD_MODMANAGERCORE=ON` or `ANOMALY_BUILD_ALL_MODULES`. It
enables Swift, pulls in `cmake/deps/LibArchive.cmake` (liblzma and libarchive) and builds the static library `ModManagerCore`
(Swift 5 language mode, macOS 14 / iOS 17 targets). Configuring costs one pass of xz's feature probes, a minute or two on a busy
machine. The Windows build does not use CMake and does not see any of this.

```bash
cmake --preset macos-arm64 -DANOMALY_BUILD_MODMANAGERCORE=ON
cmake --build build/macos-arm64 --target modmanager_core_test -j 6
ctest --test-dir build/macos-arm64 -R modmanager_core --output-on-failure
```

### Tests

`modmanager_core` (a Swift executable with its own small runner; `MODMANAGER_TEST_FILTER=<text>` runs some of them,
`MODMANAGER_TEST_ARTIFACTS=<dir>` keeps the wizard renders as PNG):

- Fixture archives are made at run time with libarchive's zip and 7-zip writers, so nothing binary is committed: nested
  roots, bare folders, `db`, option folders, programs and `bin/`, a FOMOD with every group type, a dependent type, hidden steps,
  flags, a conditional install and priorities (also as UTF-16 and as 7z), a broken FOMOD, a Data-style FOMOD.
- Hostile input: `..`, absolute and backslash paths, symlinks (zip and 7z, and a folder), truncated, garbled, empty and
  bit-flipped archives, nothing left in staging.
- Cancel in the middle of an extraction, free-space check, download cleanup, id collisions.
- RAR 4 and RAR 5 come from libarchive's own test files (`external/libarchive/libarchive/test/*.rar.uu`, decoded at run time), since
  RAR cannot be made without non-free tools: listed and extracted with sizes checked, and an encrypted one is refused.
- The wizard is hosted in a never-shown window and drawn to a bitmap (real macOS controls): light and dark, page 2 with the pad
  cursor, with Cancel, Reduce Motion, custom accent and backdrop; `FomodSession` rules and pad commands are tested without the view.
- The library: order, unknown ids, appended folders, toggle, move, rename, remove, corrupt list, stale staging, conflicts.

If the test executable's link step fails on a rebuild with `cannotResolveTempPath(modmanager_core_test-1.swiftmodule)` (a CMake plus
swiftc quirk after the library was rebuilt twice in a row), delete `build/<dir>/CMakeFiles/modmanager_core_test.dir/Debug` and run
cmake again; a fresh build is not affected.

The iOS build of the library is checked with `ios-arm64-local` and `-DANOMALY_BUILD_MODMANAGERCORE=ON`
(`cmake --build build/ios-arm64 --target ModManagerCore`).

## Launcher

The launcher is the first screen of an ordinary start, on the iPad and on the Mac: native SwiftUI, shown before the engine exists,
with **Play**, **Mods** and **Add Mod**. Play writes `modlist.json` and continues into the engine in the same process. Code:
`src/Platform/Launcher`.

```
SDL_AppInit ─ SDL_Init, AppleShell::Init ─ launcher? ─ yes ─▶ SDL window (no renderer) + SwiftUI hosted in it
                                                    └─ no ──▶ xr_engine::Init   (--script, --exit-after, --unattended, --no-launcher)
SDL_AppIterate: AppleShell::Update, poll anomaly_launcher_result
    Play ─▶ anomaly_launcher_close (views leave the window) ─▶ SDL_DestroyWindow ─▶ StartEngine (the engine makes its own window)
    Quit ─▶ SDL_APP_SUCCESS
```

### Look

Everything from app start until the game launches is Liquid Glass over the Zone, in the game's colors, on both platforms.

- **Backdrop** (`Backdrop.swift`): the Pripyat scene of the game's own main-menu art (`textures/ui/ui_actor_main_menu_one.dds`), blurred
  (26 pt), desaturated to 35 % and darkened, with a rust glow from the top left and a faint radiation green at the bottom. It is made at
  configure time by `tools/make_launcher_backdrop.sh` (a host build of `xrdb_unpack` reads it from `textures_ui.db0`, `sips` decodes the
  DDS; `cmake/AnomalyLauncherBackdrop.cmake` runs it into the build tree and `anomaly` copies it into the bundle as
  `launcher-backdrop.jpg`). It is GSC artwork: never committed. Without the game data at configure time the launcher draws the same
  glows on a plain dark gradient.
- **Glass** (`Glass.swift`): `glassSurface` is the one place that decides how a surface is drawn. iPadOS / macOS 26 and later: `glassEffect`
  (regular glass, tinted amber or rust where it speaks, `interactive` for controls); the buttons are `.glass` and `.glassProminent`; glass
  that sits together is in a `GlassGroup` (a `GlassEffectContainer`) so that it blends, and Play morphs into "Starting" with
  `glassEffectID`. Before that, or with Reduce Transparency on: a material (opaque dark for Reduce Transparency), a 1 px hairline and a soft shadow,
  the same shapes and sizes. Sheets are system sheets, which are glass on iPadOS / macOS 26.
- **Colors** (`Theme.swift`): amber (`#F5AB33`) is the accent: focus, the primary button, links. Rust (`#C75424`) is the warm glow, conflicts,
  warnings and destructive actions. Radiation yellow-green (`#B8D93D`) is used sparingly: a switch that is on, a success. Text is white at
  96 / 68 / 46 %.
- **Type and size**: system fonts, at the iPad's distance (row titles 20 pt semibold, titles 34 to 44 pt, one line below at 15 pt), tabular figures for
  counts and sizes, a soft shadow behind large type. Rows and buttons are at least 56 pt high (a 44 pt icon tile with 14 pt around it makes a row 72).
- **Shapes**: radii are concentric: a row is 22 with 14 of padding around an 8 pt icon tile; a card is 28 with 16 of padding around fields and
  buttons of 12; a sheet's buttons and warnings share 14. The logo has the 1 px white-at-10 % outline an image gets.
- **Motion** (`LauncherMotion`): purposeful and quick. A screen change is a fade with a 10 pt settle on a spring of 0.3 s with no bounce; Home's
  pieces enter once per launch, staggered 50 ms, from 12 pt below; the focus ring changes in 0.1 s ease-out (the stick repeats every 90 ms, so
  it must not lag); pressed rows give way to 0.97 where the system's glass does not do it itself. No animation repeats and nothing moves for its own
  sake. Reduce Motion keeps the fades and drops the movement and the lift; Reduce Transparency draws opaque surfaces.
- **Focus on glass**: an amber ring (white on the amber prominent buttons, where amber would vanish) with a glow and a 2 % lift, drawn on the row
  itself; it only shows while a pad or a keyboard is in use.
- **Logo**: the generated app icon (`tools/make_app_icon.sh`: the logo of `AnomalyLauncher.exe` when the game data was there at configure time,
  the repository's fallback artwork otherwise) as `launcher-logo.png`; a plain mark is drawn without it.

The game's own UI after Play is unchanged. In the offscreen snapshot tool the glass is drawn as the material fallback (an offscreen window has
no compositor), and the prominent button as an amber capsule; the real glass is on the device.

### Screens

| Screen | What it does |
|---|---|
| Home | The logo, **Play** (disabled until the game data is valid) with the number of mods that are on, **Mods** (n of m on), **Add Mod**, Quit (Mac). When the game data is missing or incomplete it shows the shell's check (`ValidateGameData`) with the how-to-copy text and a Rescan, and looks again by itself every 2 s, so a copy in progress shows up live. This replaces the old debug-text launcher, which stays in the shell for `shell_lab` and as the fallback when the SwiftUI one cannot be hosted. A previous crash is shown as a notice. |
| Mods | The load order, first (lowest priority) to last (wins a file conflict): number, name, version, size and file count, a conflict count, an info button and a switch. Drag to reorder (Edit on the iPad, direct on the Mac), swipe or context menu to remove, tap the row for the details. |
| Mod details | About, version, author, size, files, load position, source (file, link or browser), date added, and the file conflicts from `ModLibrary.conflicts()`: which files it overrides in which mod and which are overridden by which, with the paths. Switch on/off, load later / earlier, remove (asks first). |
| Add Mod | Files (`.fileImporter`: zip, 7z, rar, or a folder), Browse (`DownloadsView`: ModDB, GitHub) and a link field with Paste and Download (`LinkDownloader`). |
| Import sheet | For all three sources: *reading* (`inspect`), the warnings the importer found ("Contains Windows DLLs", ...) with *Add Anyway* / *Cancel*, the FOMOD wizard (`FomodWizardView`) when the mod has one, *installing* with progress, then *added* (switch on, open Mods) or *failed* with the importer's message. A download that ends up here is deleted afterwards (the mod is stored). |

Everything the launcher changes goes through `ModLibrary` at once (`setEnabled`, `move`, `remove` replace `modlist.json` atomically);
Play writes the file once more so that it exists and lists every installed mod, also on a first run. Changes apply at the next engine
start; there is no in-process engine restart (iOS cannot relaunch an app).

### Controls

Touch, and the mouse and keyboard on the Mac, work on the controls as on any app. On top of that the whole launcher can be
driven from a pad: a layer that knows the reachable rows of the screen (the model builds them: `LauncherModel.focusItems`), moves a
focus ring along them and runs the row's action. It is data-driven, so it does not depend on what the views draw, and the cards that
sit on top (the import sheet, the remove confirmation) take the focus until they are gone.

| Input | Pad (DualSense, Xbox) | Keyboard (Mac; hardware keyboard on the iPad) | Touch / mouse |
|---|---|---|---|
| Move focus | D-pad up/down, left stick | arrow keys | tap / click |
| Select, switch a mod on/off | Cross | Return, Space | tap the row's control |
| Back | Circle | Esc | the back button |
| Details of the focused mod | Triangle | I | tap the row |
| Reorder | hold Square, then D-pad up/down | Option + arrow up/down | Edit, drag the handle |
| Play | Options (home, mods) | P | the Play button |
| Remove | Remove in the details | Delete | swipe, context menu |

The focus ring and the hint bar at the bottom (pad glyphs or key caps, for what the buttons do on this screen) appear when a pad or the
keyboard is used and go away at the next touch or click, so the touch and mouse UI stays the plain system one. The list scrolls to the
focused row, going back returns to the row that was open, and the stick repeats like the D-pad (after 0.35 s, then every 0.09 s). The system
file picker and the in-app browser are other views: touch there, not the pad. The FOMOD wizard is driven through its `FomodSession`: the launcher owns the
session while a FOMOD is being answered, and D-pad / stick move the wizard's cursor, Cross picks the option or presses the footer button, Circle goes back
(Cancel on the first step), R1 or Options go on (Next, Install on the last step).

### Handover to the engine

The launcher is hosted in an SDL window that has no renderer and no Metal view, so SwiftUI draws and SDL only supplies the window and
the events: a child view controller of the window's root view controller on iPadOS, a subview of the content view on macOS. The shell
maps SDL gamepad events (and, on the iPad, key events) onto the launcher's buttons; the Mac keyboard is read by the launcher itself
(`NSEvent` local monitor, so a key is never handled twice).

When Play is pressed the launcher only sets its result. `SDL_AppIterate` sees it, calls `anomaly_launcher_close` (the hosting view
controller or view is removed and the model released: timers and tasks stopped), destroys the window, and calls `StartEngine`, which
creates the engine's own window exactly as in a start without the launcher. Before that the shell's `WriteFsGameLtx()` runs once more: it
re-reads `modlist.json` and writes the enabled mods into the generated `fsgame.ltx`, which is what the engine mounts (see Mounting). This happens from the SDL callback, not inside a SwiftUI
action, so the engine's start never runs inside a UIKit or AppKit event handler. Gamepads the launcher opened are closed first (and the
background-events hint it set is reset); the game's input layer enumerates the connected pads itself.

The C interface (`src/Platform/Launcher/AnomalyLauncher.h`, implemented with `@_cdecl` in `LauncherEntry.swift`):

| Function | |
|---|---|
| `anomaly_launcher_open(native_window, config_json, data_status, user)` | hosts the launcher in a `UIWindow*` / `NSWindow*` (SDL's window properties); `config_json` carries the mods root, platform, notices, logo path; `data_status` is the callback for the game-data check |
| `anomaly_launcher_result(l)` | 0 running, 1 Play, 2 Quit |
| `anomaly_launcher_button(l, button, down, keyboard)`, `anomaly_launcher_axis(l, axis, value)` | pad and key input |
| `anomaly_launcher_snapshot(l, path)` | PNG of the launcher's view |
| `anomaly_launcher_close(l)` | removes it from the window and releases it |

### When it is shown

| Start | Launcher |
|---|---|
| ordinary start (the icon, `open`, no flags) | shown |
| `--script`, `--exit-after`, `--unattended`, `--no-launcher`, `ANOMALY_NO_LAUNCHER=1` | skipped: straight to the engine, mods as `modlist.json` has them (`--mods-off` mounts none); the sweep, harness and iPad scripts are unchanged |
| `--show-launcher` | shown, whatever else is given |
| `--launcher-script <file>` | shown, also on an unattended run (its window is then hidden), driven by the script |
| game data missing | an ordinary start shows the launcher (Home explains it); a run that skipped it (`--script`, `--exit-after`, `--unattended`, `--no-launcher`) has nobody to read it and fails with a message, as before |

`--launcher-script` takes the pad commands of the engine script (`padon`, `pad <button> [ms]`, `paddown`, `padup`, `padaxis`, `padoff`),
plus `snapshot <name>` (a PNG of the launcher into the shell directory), `log` and `quit`; the time is seconds since the launcher came up.
The virtual pad is an SDL virtual joystick, so the launcher sees it as a DualSense. `ANOMALY_LAUNCHER_AUTOPLAY_MS=<n>` presses Play after
n ms.

```
# drive the launcher: Mods, switch the second mod, reorder it, details, back, Play
1.5 padon
2.5 snapshot 01-home
3.0 pad down
3.5 pad cross
4.5 snapshot 02-mods
5.0 pad down
5.5 pad cross
6.5 paddown square
7.0 pad down
7.5 padup square
8.5 pad triangle
10.0 pad circle
10.5 pad circle
11.5 pad options
```

### Where things are

| | |
|---|---|
| `src/Platform/Launcher/*.swift` | `LauncherModel` (state, and the rows the pad can reach), `Focus` (pad layer), `HomeView`, `ModsView`, `DetailsView`, `AddModView`, `ImportOverlay` (the sheets) + `ImportCoordinator` (the flow), `ScreenChrome`, `Glass` (surfaces, groups, morph), `Backdrop`, `Components`, `Theme` (palette, radii, motion), `LauncherEntry` (C entry, hosting) |
| `src/Platform/Launcher/Stubs.swift` | stand-in for `ModDownloads` (`DownloadsView`, `LinkDownloader`) while it is not on `main`; compiles out when `canImport` finds the real module, deleted when it lands |
| `src/Platform/App/main.cpp` | opening, input mapping, script and handover (`OpenLauncher`, `LauncherEvent`, `RunLauncherScript`, `SDL_AppIterate`) |
| `cmake/targets/launcher.cmake`, `src/Platform/Launcher/CMakeLists.txt` | `enable_language(Swift)`, the `anomaly_launcher` static library |
| `tools/make_launcher_backdrop.sh`, `cmake/AnomalyLauncherBackdrop.cmake` | the backdrop picture, made at configure time from the game data |
| `tools/launcher_snapshot` | renders every screen offscreen; `--selftest` drives the pad logic; `--fixtures` makes a mods root of invented mods |

Build notes: the library is Swift 5 mode, built for the deployment target (CMake does not pass it to `swiftc`), and `anomaly` links with the
C++ driver (`LINKER_LANGUAGE CXX`) because CMake would pick `swiftc` otherwise, which does not take `-fsanitize` and the other C++ link
flags; the Swift runtime's search paths come with the library's link options.

### Testing

```bash
cmake --build --preset macos-arm64-release --target launcher_snapshot
T=build/macos-arm64-release/tools/launcher_snapshot/launcher_snapshot
$T build/launcher-shots --logo build/macos-arm64-release/app_icon/icon-1024.png   # the screens as the Mac draws them
$T --selftest build/launcher-selftest                                              # pad focus, reordering, sheets, what Play writes
```

The first hosts the real views in a window that is never shown and draws them into a bitmap (the Mac's controls; the iPad's look
comes from the device, below). The second presses the pad's buttons the way the shell does and checks the focus, the reordering, the
confirmation sheet, the import warning and what Play writes.

On the Mac the whole path, launcher, pad script, Play, engine, exit, runs without a window:

```bash
T=build/macos-arm64-release/tools/launcher_snapshot/launcher_snapshot; $T --fixtures /tmp/state/Mods
open -g -j -W -n --env ANOMALY_STATE_ROOT=/tmp/state build/macos-arm64-release/src/Platform/App/anomaly.app \
  --args --launcher-script launcher.txt --exit-after 10 -nosound
```

(`--launcher-script` on an unattended run keeps the launcher's window hidden; `snapshot` still draws it.) On the iPad, `tools/ios/ipad.sh
put launcher.txt launcher.txt` and `tools/ios/ipad.sh start --launcher-script launcher.txt` do the same on the device, and the snapshots come
back with `tools/ios/ipad.sh logs`.
