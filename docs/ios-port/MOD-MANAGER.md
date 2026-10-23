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

One page per step: the groups with their rule, the options, and a card with the image and description of the option that is
hovered, highlighted or, failing that, selected. The footer has Cancel, Back and Next, which is Install on the last step.
Wide layouts (640 pt and more) put the card beside the options, narrow ones below.

**Look.** Liquid Glass over a dark backdrop, in the launcher's colours: amber for selection, the cursor and the primary button,
rust for hairlines and cautions, radiation yellow-green only for the "Recommended" mark.

| Where | Material |
|---|---|
| iOS 26 / macOS 26 and later | `glassEffect`: header, option rows (tinted amber when selected, interactive), detail card, footer buttons (the primary one tinted amber); rows and buttons sit in a `GlassEffectContainer`, with `glassEffectID`s so shapes morph (rows by position, Next into Install) |
| iOS 17 to 25, macOS 14 and 15 | ultra-thin material, a tint layer and a 1 px hairline |
| Reduce Transparency | solid panels (`FomodTheme.solid` / `solidRaised`) with the hairline |

No glass sits on glass (the group headings and the cursor ring are plain), and radii are concentric: header 24, card 28 with a
10 pt picture radius inside 18 pt of padding, rows 20. Pictures get the 1 px white 10% outline.

**Theme.** Colours and the backdrop come from the environment, so the launcher can supply its own:

```swift
FomodWizardView(installer: installer, onFinish: finish, onCancel: cancel)
    .fomodTheme(FomodTheme(accent: ..., rust: ..., signal: ..., text: ..., dimText: ..., danger: ...,
                           solid: ..., solidRaised: ..., line: ..., backdrop: nil))   // nil: what is behind shows through
```

`FomodTheme.anomaly` is the default (amber `#E8A838`, rust, signal green, a near-black backdrop with a warm and a rust glow;
`FomodBackdrop` is public). The wizard always runs in the dark colour scheme. The backdrop matters on glass: it is what the glass
samples, so a launcher that has its own backdrop behind the wizard passes `backdrop: nil`.

**Motion.** Fast and purposeful: a step change is a spring (0.3 s, no bounce) with the new page sliding in 28 pt from the side
the player is heading to, the old one fading out, and its groups rising 10 pt one after another, 60 ms apart; the first page is
not animated. The radio dot and the checkmark pop in with scale 0.25 to 1, opacity and a 4 pt blur; the primary button morphs from
Next to Install; presses scale to 0.96 (real glass brings its own press response); the detail card cross-fades in 0.12 s.
Moving the cursor is **not** animated: a D-pad is used constantly and the ring must be where the finger is. With Reduce Motion
every one of these becomes a short fade, with no offsets, scaling, blur or stagger.

**Cursor.** The highlight is an accent ring with a white line inside it and a glow, so it reads on glass, on the amber tint of a
selected row and on the solid fallback alike. It is on one element at a time.

**Input.**
- Touch and mouse: tap an option or a footer button.
- Keyboard: the arrow keys move the cursor over the options of the page and then the footer buttons; Return or Space presses it;
  Escape is Back, and Cancel on the first step. The first option is highlighted when a page opens, and the page scrolls to it.
- Controller: the view reads no controller. `FomodSession` (public, `ObservableObject`) holds the steps, selections, flags,
  validity and the cursor, and takes the same commands the keyboard produces: `session.send(.up / .down / .left / .right /
  .confirm / .back / .next)`. Whoever reads the pad (the launcher) creates the session, shows
  `FomodWizardView(session:onFinish:onCancel:)` and forwards D-pad to the four directions, Cross to `.confirm`, Circle to `.back`
  and R1 or Options to `.next` (press the primary button if the rules allow). `FomodWizardView(installer:onFinish:onCancel:)`
  creates its own session and is what the touch and keyboard path needs.
- Locked options (required, not usable, SelectAll) can be highlighted, so the reason for the lock can be read, but not changed.
- VoiceOver: every option and button has a label, a selected value and, for locked ones, a hint. Selection is shown by shape
  (filled dot, checkmark) as well as colour.

**What was checked.** The layout, the fallback materials, the cursor ring, a custom theme and the solid and reduced-motion
variants are rendered offscreen with `ImageRenderer` (no window) and written as PNGs when `MODMANAGER_TEST_ARTIFACTS` is set. Two
limits: `ImageRenderer` cannot draw Liquid Glass at all (it draws the glass layers and their content as empty; a hosted view
captured without a window does the same), so the pictures show the iOS 17 to 25 material path, with the glass path switched off
by an internal test override; and nothing animated can be seen in a still. The glass path is compiled for iOS and macOS and the
session logic under it is tested, but how it looks and moves on an iPad 27 has not been watched yet.

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
- The wizard renders offscreen with `ImageRenderer` (no window), in the material fallback, solid, reduced-motion and custom-theme
  variants and on a second page with the pad cursor; `FomodSession` rules and pad commands are tested without the view.
- The library: order, unknown ids, appended folders, toggle, move, rename, remove, corrupt list, stale staging, conflicts.

If the test executable's link step fails on a rebuild with `cannotResolveTempPath(modmanager_core_test-1.swiftmodule)` (a CMake plus
swiftc quirk after the library was rebuilt twice in a row), delete `build/<dir>/CMakeFiles/modmanager_core_test.dir/Debug` and run
cmake again; a fresh build is not affected.

The iOS build of the library is checked with `ios-arm64-local` and `-DANOMALY_BUILD_MODMANAGERCORE=ON`
(`cmake --build build/ios-arm64 --target ModManagerCore`).
