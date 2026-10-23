# Addons on the iPad

Anomaly addons are folders of game files: scripts, configs, textures, shaders, sounds, models. The iPad build reads them from the same
places the PC game does, so most addons work by being copied into the game folder. This guide says how to install one with the Files
app, in which order the game reads them, what cannot work, how much memory a texture pack may use, and where to look when something is wrong.

> **Status.** Written from a test set of our own addons, [`tools/addons_test`](../../tools/addons_test/README.md), run on a Mac against a
> **case-sensitive volume**, which is what the iPad has (the shell log says `volume: file names are case-sensitive`, and two files `A` and `a` copied into
> the app's `Documents` stay two files). The engine code is the same on both. The same set has not been run on the iPad itself yet: the script
> supports it (`run.sh` without `--target mac`), but the device was not reachable when this was written, so the Files-side steps below are what the
> iPadOS Files app documents, not something we filmed (the test set puts files into the same folders with `devicectl`). No third-party addon has been
> installed yet. Where a statement is about addons in general and not something the test set shows, it says so.

Two routes put an addon into the game, and the manual one keeps working next to the other (a file lying loose in `gamedata/` wins over every mod the manager mounts).
**The mod manager in the app** ([MOD-MANAGER.md](MOD-MANAGER.md)) imports `.zip`, `.7z` and `.rar` files and
FOMOD installers from the Files app or a download, keeps every mod in its own folder under `Documents/Mods`, and lets the player switch mods
on and off and change their order, without ever copying over another mod's files. **By hand**, files are copied into the game folder with
the Files app; this is what the rest of this guide describes (and what a mod that comes as a plain folder or a single `.db0` file needs when the
manager is not used). Both end up in the same file system for the engine: [Order](#order-what-overrides-what).

## The short version (by hand)

1. Open **Files → On My iPad → Anomaly → Anomaly**. This is the game folder (`Documents/Anomaly` of the app): it holds `db/`,
   `gamedata/`, `fsgame.ltx`.
2. An addon that comes as a folder with a `gamedata` folder in it goes into the game's `gamedata` folder, file by file where a folder of the
   same name exists already ([Loose files](#loose-files-the-gamedata-folder)). An addon that comes as a `.db0` / `.db1` ... file goes
   into `db/addons` ([Archives](#archives-one-file-per-addon)).
3. Start the game. There is no list of addons to switch on: whatever is in the folders is in the game.
4. To remove an addon, delete the same files again.
5. A Windows `.dll` / `.exe`, anything for `bin/`, or a change to the graphics settings will not work; see [What cannot work](#what-cannot-work).
6. When something goes wrong, the logs are in the same Files folder; see [Logs](#logs).

## Where the game folder is

| Files shows | Is | Holds |
|---|---|---|
| On My iPad → Anomaly → Anomaly | the app's `Documents/Anomaly` | `db/` (the game's archives), `gamedata/` (loose files: addons, `configs/axr_options.ltx` that the game writes), `fsgame.ltx` (rewritten at every start, do not edit) |
| On My iPad → Anomaly | the app's `Documents` | the game folder, `Mods/` (the mod manager's library: one folder per mod, `modlist.json`), `commandline.txt`, the logs |

Saves, `user.ltx` and the shader cache are in the app's `Library`, which Files does not show. They are not part of addon installs.

## Installing

### Loose files (the `gamedata` folder)

Most addons are a zip with a `gamedata` folder in it (some wrap it in one more folder, some also have `db`, `bin`, a readme, an installer).

1. Unzip: tap the `.zip` in Files; a folder appears next to it. A `.7z` or `.rar` needs an app that can open it (any archive app from the App
   Store that shares into Files; or the mod manager, which reads them itself); do the unpacking there and move the result back.
2. Look inside for the folder called `gamedata` (case does not matter). What is inside it (`scripts`, `configs`, `textures`, `shaders`,
   `meshes`, `sounds`, ...) is what goes into the game's `gamedata`.
3. Put it there. **Files does not merge folders**: when a folder of the same name exists, it offers *Keep Both*, *Replace* or *Skip*,
   and none of them is a merge. Never choose *Replace* for `gamedata`, `configs`, `scripts` or any folder the game or another addon has
   files in: it deletes the other files. So:
   - a folder that does not exist yet in the game's `gamedata` (a new `textures/zz_thing` folder, a whole new `shaders` folder): move it;
   - a folder that exists: open both, and move the addon's *files and subfolders* into the existing one, one level down, until there is
     no clash left.
   If that is many files, or a deep tree, pack the addon into one file instead ([Archives](#archives-one-file-per-addon)).
4. Names: spelling is free. `Scripts` next to `scripts`, `GameData` next to `gamedata`, `Textures/UI/Foo.DDS`: the iPad's file system tells
   them apart (the log says `volume: file names are case-sensitive`), the game does not, and it reads all of them
   ([Names and case](#names-and-case)).

The files are read at the next start of the game. There is nothing to rebuild; shaders an addon changed are translated at run time and the
result is cached, so the first start after adding a shader addon takes a few seconds longer.

### Archives (one file per addon)

A `.db0`, `.db1`, ... file ([or `.xdb0`](#archives-one-file-per-addon)) is an X-Ray archive, the format the game's own data is in. Files
inside it are mounted as if they were loose. Put it straight into `db/addons` (create the folder if there is none) or `db/patches`.
**Not in a sub-folder**: the game scans these two folders for archives without looking into folders inside them, on the PC as well.

Many addons, especially large ones, are not shipped that way, but you can make one with a PC or a Mac, once:

```bash
python3 tools/addons_test/xrdb_pack.py <addon folder>/gamedata  my_addon.db0  --entry-point gamedata
```

(`tools/addons_test/xrdb_pack.py` needs only Python 3. Anomaly's own packer, `compressor/xrCompress.exe`, makes the same format on Windows.)
Then copy the one file with AirDrop, iCloud Drive or a cable into `On My iPad → Anomaly → Anomaly → db → addons`.

What an archive is good for: one file to move, one file to delete; the game reads it with one open file instead of a thousand. What it is not:
a file in an archive is below the same file lying loose in `gamedata`, so an archive cannot override a loose file ([Order](#order-what-overrides-what)).
An addon that *writes* into its own folder (a settings file it creates) writes loose files in `gamedata`, which is fine.

### Mod Organizer, FOMOD, installers

- **FOMOD** (an `fomod/ModuleConfig.xml` in the zip: the installer asks questions) is run by the mod manager's install wizard. By hand it cannot be run:
  do it on a PC against an empty folder, then take the finished `gamedata` folder (or pack it as above) to the iPad.
- **Mod Organizer 2** (a `mods/<name>` folder per addon, a virtual file system) is a Windows program; its mod folders are ordinary addons: each `mods/<name>` folder with a
  `gamedata` inside is one addon here. Install them one at a time, in the order of MO's list (lowest first), or import each into the mod manager and set the same order.
- Addons with an `.exe` installer: the installer does nothing on the iPad (and the mod manager refuses to install a `.exe`). Run it on a PC against an empty folder.

## Order: what overrides what

When two layers have the same file, the later one wins. The game reads these, in this order ([INTEGRATION.md](INTEGRATION.md), "The Monolith overlay"; [MOD-MANAGER.md](MOD-MANAGER.md), "Mount order"):

| # | Layer | Where |
|---|---|---|
| 1 | the game's own archives | `db/**.db*` (`configs`, `levels`, `meshes`, `sounds`, `textures`, `mods`, `files.db0`, `shaders.db0`) |
| 2 | the Monolith engine's files | packed from the app bundle at every start (scripts, configs and shaders the engine build needs: `axr_main.script`, `pda_dynamic_tabs.script`, `mod_system_*.ltx`, ...) |
| 3 | `db/patches` | archives, in the order of their names |
| 4 | `db/addons` | archives, in the order of their names |
| 5 | the mod manager's enabled mods | `Documents/Mods`, in the list's order: a mod's archives (`content/db`), then its loose files (`content/gamedata`) |
| 6 | loose files in `gamedata/` | a manual install and the files the game writes itself (`configs/axr_options.ltx`): win over everything |

- Archives inside one folder are read in the order of their names, compared character by character with capitals before small letters
  (`ZZ_a.db0` comes before `aa.db0`). The later one wins. Name them `01_x.db0`, `02_y.db0` if the order matters.
- **DLTX** works: any file called `mod_<name>_<anything>.ltx` next to `<name>.ltx` (`mod_system_*.ltx` for `system.ltx`) is merged into
  `<name>.ltx`, whichever layer it is in, upper or lower case. `@[section]` adds lines to a stock section, `![section]` replaces one.
- Layer 2 contains the Monolith engine's own versions of some stock scripts (for instance `axr_main.script`, `item_weapon.script`, the
  `options_modded_exes_*` scripts). An addon that ships its own copy of such a file replaces the engine's glue and may break the game's menus. The same conflict exists on a PC with
  the Monolith engine; check an addon for files of those names before installing it.

## Names and case

- The game reads names without regard to case, in archives and on disk: `Textures/UI/Foo.DDS`, `textures\ui\foo.dds` and `TEXTURES/ui/FOO.dds` are one file.
- On the iPad's file system two folders may differ only in case (a Windows zip often has `Scripts` where the game has `scripts`). The game
  lists and opens the files of all spellings of a folder as one folder. (Before 2026-10-23 the game only saw one of them. This was a bug of
  this port and is fixed; an older build has it.)
- Two *files* with the same name in the same folder that differ only in case (`Foo.script` and `foo.script`) are one file to the game; which
  one it reads is not defined. Do not do that.
- Files whose name begins with a dot (`.DS_Store`, the `._file` copies that macOS makes) are ignored.

## What cannot work

- **Windows DLLs and `bin/`.** Nothing in `bin/` is used and nothing is loaded from a DLL. The engine is the one inside the app (a build of the
  Monolith engine: `xrEngine`, `xrGame`, the renderer, the Lua extensions are all part of it). An addon that replaces `xrGame.dll`,
  `xrRender_R4.dll`, `AnomalyDX11.exe` or another executable, or that needs a different engine build, cannot be used. Addons whose Lua
  scripts use functions of a *different* engine mod than the one built in will fail where they call them (the log says which function).
- **Graphics settings.** The graphics profile of the iPad is applied at every start and locked (`docs/ios-port/SETTINGS.md`): an addon
  that sets `r2_*`, `r3_*`, `r4_*`, `r__*`, `texture_lod`, `vid_mode`, ... from a script, a `.ltx` or a `user.ltx` has those lines refused, and
  the engine log says so (`! graphics: 'r2_ssao st_opt_off' refused: locked by the ipad-8gb profile`). Everything else the addon does still
  works. The player's own choices are Quality or Performance, the frame cap, field of view, brightness, motion blur and the HUD.
- **Features the Metal renderer does not have.** No geometry shaders and no tessellation, so the 3D fluid simulation (volumetric fog) and
  tessellated materials are off; a shader addon that needs them is partly dead. The shaders themselves are HLSL and are translated at run time
  (`docs/ios-port/SHADERS.md`); most compile, a broken one fails to compile and is logged, it does not crash the game.
- **Post-processing injectors** (ReShade and the like), the Windows launcher, mod loaders that patch the game's files on disk.
- **Addons that overwrite files the Monolith engine needs** (above). Not impossible, but not safe.

## Memory: texture packs on an 8 GB iPad

iPadOS ends an app that uses more than its memory budget, without a warning screen. With the `increased-memory-limit` entitlement the app's budget
is **7.99 GB** (`device ... memory budget 7.99 GiB` in the shell log), and what counts is the app's footprint: textures, render targets and Metal
buffers included. At the iPad profile ([IPAD.md](IPAD.md)) the Cordon runs at about **3.8 GB**, a level load peaks near 4.3 GB, and the engine itself
counts about 1.45 GB of that as textures. That leaves roughly 3.5 GB; keep at least 1.5 GB of it free for the load peak, level changes and the slow
creep (about 10 MB a minute), so **a pack should add no more than about 2 GB of textures to what one level has loaded.**

What a texture costs on the GPU (with its mip chain, 4/3 of the top level):

| Size | DXT1 / BC1 (0.5 byte per pixel) | DXT5 / BC3 (1 byte) | uncompressed 32 bit (4 bytes) |
|---|---:|---:|---:|
| 1024 x 1024 | 0.67 MiB | 1.33 MiB | 5.3 MiB |
| 2048 x 2048 | 2.7 MiB | 5.3 MiB | 21.3 MiB |
| 4096 x 4096 | 10.7 MiB | 21.3 MiB | 85.3 MiB |

2 GB is about 190 4K DXT1 textures, 95 4K DXT5, 380 2K DXT5, or 24 uncompressed 4K ones. The stock game is mostly 1K and 2K DXT1/DXT5.

- **Pack as DXT1/DXT5 with the mip chain.** Uncompressed textures cost four to eight times as much and are what makes a "4K" pack not fit. A DDS without mips
  is smaller but shimmers.
- **The profile does not shrink textures.** `texture_lod 1` (the iPad profile, locked) loads every texture at full size except the ones named in
  `[reduce_lod_texture_list]` of `system.ltx`, which lose one level. A 4K pack is loaded as 4K.
- **Only what a level uses is loaded.** A pack of 2000 textures costs memory for those the current level and the HUD touch, plus the disk space for
  all of them. A pack for weapons, characters or one kind of surface is cheap; a full retexture at 4K is not.
- **Measured** (`tools/addons_test`, scenario `texpack`, same menu, same start, Mac with the same profile type): replacing the 24 big UI atlases (1K DXT5) by
  4K DXT1 with mips (10.7 MiB each) raised the engine's texture counter from 1336 MB to 1452 MB and the process footprint from 2345 MB to 2456 MB in a new game.
  Twelve of the 24 are loaded in a new game, 12 x (10.7 - 1.33) MiB = 112 MiB: the table above is what the engine pays, and the footprint grows by the
  same amount.
- If a pack is too big the game is killed by the system while loading a level or a few minutes after, and the next start logs
  `previous session ended unexpectedly` ([Logs](#logs)). Remove textures, or turn the 4K ones into 2K, until it stays up.


## Logs

| Log | Where in Files (On My iPad → Anomaly) | What it says |
|---|---|---|
| shell log | `anomaly-shell.log` (`anomaly-shell.prev.log`: the run before) | paths, device, graphics profile (and what it locks), `volume: file names are case-sensitive`, the Monolith overlay, memory and thermal events, `previous session ended unexpectedly` after a crash or a kill |
| crash report of the shell | `anomaly-crash.log`, `anomaly-crash.prev.log` | stack of a crash in the app itself |
| engine log of the **previous** run | `anomaly-engine.prev.log` | everything the engine and the Lua scripts logged: **this is where addon errors are** |
| engine crash report of the previous run | `anomaly-engine-crash.prev.txt` | the engine's report when it crashed (only if there was one) |

The engine writes its current log into `Library/Application Support/Anomaly/appdata/logs/xray_mobile.log`, which Files does not show; the app copies it
to `anomaly-engine.prev.log` when it starts the next time. So: run the game, quit it, start it once more, then open `anomaly-engine.prev.log` (text, Windows-1251,
any text viewer shows it; search for `!`).

What the test set shows an addon problem looks like (these lines are verbatim from the log):

| Problem | Line in the engine log |
|---|---|
| a script with a syntax error | `! [SCRIPT ERROR]: ...gamedata\scripts\zz_broken_script.script:3: unexpected symbol near '='` then `! [ERROR] --- Failed to load script zz_broken_script`. The game goes on without that script; the other scripts load. |
| a runtime error in a script | a `! [LUA] ...` stack in the log; the Monolith scripts also show a "Lua fatal error" message box in the game (Disconnect / Reload save / Continue). *This one was not provoked by the test set.* |
| a setting the profile locks | `! graphics: 'r2_ssao st_opt_off' refused: locked by the ipad-8gb profile. Further attempts on r2_ssao are not logged.` |
| a shader that does not compile | `* ShaderXlat: N shaders requested, M from the translation cache, F failed ...` at the end of the log, and the line of the failed shader above it |
| a texture that is not found | `! Can't find texture 'ui\...'` |

Check an addon quickly: the log lists `* loading script <name>.script` for every script the engine loaded (an addon script that is missing there was not seen:
wrong folder, or in a sub-folder of `db/addons`), and `Init FileSystem` / `FS: N files cached M archives` near the top says how many archives were mounted.


## How this was tested

[`tools/addons_test`](../../tools/addons_test/README.md) holds test addons of our own that follow real addon layouts, a packer, and `run.sh`, which puts a scenario
into a game folder, runs the game offscreen with a `--script` timeline, and has a probe script inside the game check what the addons did
(`ADDON_TEST <id> PASS|FAIL` lines in the engine log).

| Addon | Checked |
|---|---|
| script: a new script, `on_game_start`, two callbacks, a monkey-patched stock function | loaded, started, the patch answers for its own input and passes the rest to the stock function; `actor_on_first_update` and `actor_on_update` fire in a game |
| config: DLTX `mod_system_*.ltx` (lower case, and capitals with `.LTX`), `@[wpn_ak74]`, a plain `.ltx`, a string table | `system_ini()` has the sections and the added line, the stock line is intact, `translate_string` |
| texture: the main menu background replaced by a test card, a new texture | the registry serves the loose file; the screenshot shows the card |
| shader: the stock `distort.ps` (the main menu's image pass) with a tint, through a new include | a screenshot shows the tint; ShaderXlat translates it, and again after the include changes, and again after the shader changes; a run with nothing changed translates nothing; with the addon removed the stock shader comes from the cache |
| MCM page: `on_mcm_load()` in a `*_mcm` script, values in `axr_options.ltx` | page found, default read, value written, **read back by a new process** (the file is `gamedata/configs/axr_options.ltx`); the MCM mod itself is not part of the test (a stand-in host reads the page) |
| archives in `db/patches` (LZO entries, `entry_point = gamedata`) and `db/addons` (`entry_point = $fs_root$\gamedata\`) | mounted; patches over a stock file and over a Monolith overlay file; addons over patches; loose over addons |
| names in Windows case (`Scripts`, `Configs`, `Textures/UI`, `.PS`, `.DB0`, `GameData`, `DB/Addons`) | all of the above with these spellings, next to the lower-case folders the game made |

Result on the case-sensitive volume, the whole sequence (baseline, install, new process, case variants, shader changed twice, removed): **every check passes**
(21 to 28 per run), the shader translation count is 1 in each run that changes the shader or its include and 0 in every other run, and removing the addons brings every file size back to the stock one.
Before the fix of `xr_native_path` / `_findfirst` (xrCore, `PlatformApple.cpp`) the same run failed 7 to 8 checks per run: every file of an addon folder spelled
`Configs` while the game's own `configs` existed, the loose file over an archive's, and the `GameData` and `DB/Addons` roots. The new `xrcore_smoke --case-test <dir>` checks
the path layer alone (12 checks on a case-sensitive volume, 8 of them fail on the old code).

What the test set found out, that is not obvious:

- a **case-variant folder was invisible**: the registry and the file open each looked at one spelling of a folder only (fixed);
- **archives in a sub-folder of `db/addons` or `db/patches` are not mounted** (the same on the PC: the two folders are scanned without recursion);
- the translation cache is **content-addressed**: a shader or an include that changes is translated again, one that was seen before (an addon switched on again) is a hit;
- **scripts cannot delete files**: the engine removes `os.remove`, `os.rename` and `os.execute` from Lua, so an addon that relies on them does not work;
- the engine log, where all of this shows, was not reachable from the Files app; the app now copies the previous run's log next to the shell log ([Logs](#logs)).

To run it yourself: `tools/addons_test/run.sh --target mac --data-root <copy of the Anomaly folder> sequence`, or for the iPad `tools/addons_test/run.sh sequence`.
