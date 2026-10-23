# Mod manager

A mod manager inside the Anomaly app for the iPad and the Mac: a SwiftUI launcher is shown first, mods are stored
apart from the game and mounted at engine start in load order, Mod Organizer 2 style, and nothing in the game folder is
overwritten. Four sessions built it: engine mounting, import and FOMOD, downloads and links, and the launcher. This file has
one section per part, each written by the session that built it.

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
