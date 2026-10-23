# Addon test set

Test addons that mirror real Anomaly addon layouts, a script that puts them into the game folder the way the Files app does, and a
check that each one took effect, in the right layer. The player guide is [docs/ios-port/ADDONS.md](../../docs/ios-port/ADDONS.md).

Nothing third-party is in this folder: the addons are written for the test, the textures are generated, and the shader is the stock
`r3\distort.ps` read out of your own `shaders.db0` with one line changed.

## The addons

| Addon (`addons/<name>`) | What it is | How it is checked |
|---|---|---|
| `script` | `Scripts/ZZ_Addon_Script.script`: a new script with `on_game_start`, two callbacks, and a monkey-patch of the stock `trim()` | module loaded, `on_game_start` ran, `trim("ZZ_x")` is the patched answer and `trim("  a ")` still the stock one; in a game `actor_on_first_update` and `actor_on_update` fire |
| `config` | DLTX `mod_system_*.ltx` (one in capitals, `.LTX` too), a safe override (`@[wpn_ak74]`) of a stock section, a plain `.ltx`, a string table (`Configs/Text/Eng/*.XML`) | `system_ini()` sees the sections and the added line, the stock line is still there, `game.translate_string` |
| texture (generated) | `Textures/UI/UI_Actor_Main_Menu2.DDS` replaces the main menu background (a 1024x1024 DXT1 test card), `ui_zz_addon_new.dds` is new | registry entry is loose and has the generated size |
| shader (generated) | `Shaders/R3/Distort.PS` is the stock `distort.ps` (the main menu's image pass, translated at start-up) tinted through a new include `ZZ_Addon_Tint.H` | registry entry; ShaderXlat translates it (and again when the include or the shader changes) |
| `mcm` | `zz_addon_mcm.script`, an option page in the layout of MCM pages (`on_mcm_load()` returns it), its strings | page found by the host, default read, value written to `gamedata\configs\axr_options.ltx` and read back by a new process |
| `archive_patches` | packed into `db/patches/ZZ_Addon_Patches.DB0` (LZO entries, `entry_point = gamedata`) | its files are served; it overrides a stock file and a Monolith overlay file |
| `archive_addons` | packed into `db/addons/ZZ_Addon_Pack.db0` (`entry_point = $fs_root$\gamedata\`) | its files are served; it wins over `db/patches` |
| `loose_priority` | a loose file that is also in both archives | the loose one wins |
| `case_variants` (+ `case_variants_pack`) | `GameData/Scripts/ZZ_Case_Script.script` and an archive in `DB/Addons/`: folder spellings that are separate folders next to `gamedata/` and `db/addons/` on a case-sensitive volume | the script is loaded, the archive is mounted |
| `broken` | a script with a syntax error (scenario `broken`, not part of the sequence) | `run.sh check broken game` prints what the engine log says about it |
| texture pack (generated, scenario `texpack`, not part of the sequence) | 24 big UI atlases replaced by 4096x4096 DXT1 with a mip chain (11.2 MB each) | `verify.py` prints the texture bytes and the footprint the shell logged; compare with the baseline run |
| `probe` | the harness: `zzz_addon_probe.script` runs the checks and logs `ADDON_TEST <id> PASS|FAIL`; `zz_addon_mcm_host.script` stands in for the MCM mod; `zz_addon_expect.script` (generated) says what the scenario should find | |

File and folder names are spelled the way a Windows zip has them (`Scripts`, `Configs`, `Textures/UI`, `.PS`, `.DB0`), on purpose.

## Scenarios

`addons.py scenarios` lists them; `run.sh sequence` runs them in this order:

1. `removed` (boot): the baseline. Only the probe is installed. Stock sizes everywhere, stock shaders translated and cached.
2. `installed` (game): every addon. One shader translated (the changed `distort.ps`). MCM value written.
3. `installed_again` (boot): a new process. All addons still there, **no** shader translated, MCM value read back from the file.
4. `shader_v2` / `shader_v3` (boot): the include / the shader itself changed: translated again.
5. `removed` (boot): the addons are gone. Every size is the stock one again, the original `distort` translation comes from the cache.

Not in the sequence: `broken` (game) and `texpack` (boot), run them with `run.sh check <scenario> <mode>`.

*boot* stops in the main menu and runs the probe from the console; *game* starts a new game so the addon scripts' `on_game_start` and
callbacks run (axr_main calls them only when a game starts).

## Running it

```bash
tools/addons_test/run.sh --target mac --data-root <a copy of the Anomaly folder, db/ and gamedata/> sequence     # the Mac, offscreen
tools/addons_test/run.sh sequence                                                                              # the iPad over USB
tools/addons_test/run.sh push installed game      # just put a scenario into the game folder
tools/addons_test/run.sh cleanup                  # take everything this tool pushed out again
```

**Removing files on the iPad.** `devicectl` cannot delete a file, and its `copy to --remove-existing-content` once emptied a whole app container
(game data and saves) when it was given an empty source folder. Never use it. `run.sh` therefore has the game delete what an earlier push left:
it generates `zz_addon_remove.script` (the list of files, with their case), pushes it, runs `timelines/remove.txt` (a boot that calls it) and the
script removes the files, the folders that are then empty, and itself, with `os.remove` under `$HOME/Documents/Anomaly`.

On the Mac `--data-root` is written to (the addons go into its `gamedata/` and `db/`), so use a copy: `db/` can be a folder of symlinks to the
real archives. A volume that is **case-sensitive** (`hdiutil create -fs "Case-sensitive APFS" ...`) shows the same behaviour as an iPad whose
volume is, and is where the folder-spelling bugs show.

The game folder `--game-dir` (default `game/Anomaly` of the checkout or of the main checkout) and a built `xrdb_unpack` are needed to
make the shader; both are found by themselves when they are where the build puts them.

## Other tools

- `xrdb_pack.py <folder> <out.db0> [--entry-point gamedata] [--compress]` packs a folder into an archive the engine mounts. The player guide
  uses it to turn an addon into one file. `--compress` stores LZO streams (literals only) so the compressed read path is used too.
- `verify.py` judges a run's engine log (`run.sh` calls it).
- `timelines/boot.txt`, `timelines/game.txt` are the `--script` timelines (INTEGRATION.md: `--script`).
