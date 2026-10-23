# Addons on the iPad

Anomaly addons are folders of game files: scripts, configs, textures, shaders, sounds, models. The iPad build reads them from the same
places the PC game does, so most addons work by being copied into the game folder. This guide says how to install one with the Files
app, in which order the game reads them, what cannot work, how much memory a texture pack may use, and where to look when something is wrong.

> **Status.** Written from a test set of our own addons, run on the iPad (iPad Air 13-inch M3, iPadOS 27.0.1) and on a Mac with a
> case-sensitive volume: [`tools/addons_test`](../../tools/addons_test/README.md), [How this was tested](#how-this-was-tested). No third-party
> addon has been installed yet, and the Files app itself was not driven (the test set copies files into the same folders with `devicectl`),
> so the Files steps below are the ones the iPadOS Files app documents, not something we filmed. Where a statement is about addons in general
> and not something the test set shows, it says so.

## The short version

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
| On My iPad → Anomaly | the app's `Documents` | the game folder, `commandline.txt`, the logs |

Saves, `user.ltx` and the shader cache are in the app's `Library`, which Files does not show. They are not part of addon installs.

## Installing

### Loose files (the `gamedata` folder)

Most addons are a zip with a `gamedata` folder in it (some wrap it in one more folder, some also have `db`, `bin`, a readme, an installer).

1. Unzip: tap the `.zip` in Files; a folder appears next to it. A `.7z` or `.rar` needs an app that can open it (any archive app from the App
   Store that shares into Files); do the unpacking there and move the result back.
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

- **FOMOD** (an `fomod/ModuleConfig.xml` in the zip, the installer asks questions) and **Mod Organizer 2** (a `mods/<name>` folder per addon
  and a virtual file system) are Windows programs. Run them on a PC against an empty folder, then take the finished `gamedata` folder (or
  pack it as above) to the iPad.
- Addons with an `.exe` installer: the installer does nothing here. Run it on a PC the same way, pointing it at an empty folder.

## Order: what overrides what

When two layers have the same file, the later one wins. The game reads these, in this order (`docs/ios-port/INTEGRATION.md`, "The Monolith overlay"):

| # | Layer | Where |
|---|---|---|
| 1 | the game's own archives | `db/**.db*` (`configs`, `levels`, `meshes`, `sounds`, `textures`, `mods`, `files.db0`, `shaders.db0`) |
| 2 | the Monolith engine's files | packed from the app bundle at every start (scripts, configs and shaders the engine build needs: `axr_main.script`, `pda_dynamic_tabs.script`, `mod_system_*.ltx`, ...) |
| 3 | `db/patches` | archives, in the order of their names |
| 4 | `db/addons` | archives, in the order of their names |
| 5 | loose files in `gamedata/` | always win |

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

TO BE FILLED IN FROM THE MEASUREMENTS.

## Logs

TO BE FILLED IN FROM THE MEASUREMENTS.

## How this was tested

TO BE FILLED IN.
