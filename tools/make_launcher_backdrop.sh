#!/bin/bash
# Makes the launcher's backdrop: the Pripyat scene of the game's own main-menu art, cropped and written as a JPEG that the launcher
# blurs, desaturates and darkens behind its glass.
#
#   tools/make_launcher_backdrop.sh <out-dir> [game-dir]
#
# Reads textures/ui/ui_actor_main_menu_one.dds from <game-dir>/db/textures/textures_ui.db0 with a host build of tools/xrdb_unpack
# (compiled here, a few seconds), decodes the DDS with `sips` and writes <out-dir>/launcher-backdrop.jpg. The picture is GSC artwork:
# it is generated into the build tree (gitignored) and bundled from there, never committed. Without the game data the script exits
# 1 and the launcher draws a plain gradient instead. Run by cmake/AnomalyLauncherBackdrop.cmake at configure time; also usable by hand.
set -euo pipefail

out=${1:?usage: make_launcher_backdrop.sh <out-dir> [game-dir]}
game_dir=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
archive="$game_dir/db/textures/textures_ui.db0"
[[ -n $game_dir && -f $archive ]] || { echo "make_launcher_backdrop: no $archive" >&2; exit 1; }

work="$out/.work"
rm -rf "$out"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# the unpacker refuses to write below a folder named "game": the work folder is in the build tree
/usr/bin/c++ -std=c++17 -O1 -I"$repo/tools/xrdb_unpack" "$repo"/tools/xrdb_unpack/{main,xrdb,lzhuf,lzo1x}.cpp -o "$work/xrdb_unpack" 2>"$work/cc.log" ||
    { cat "$work/cc.log" >&2; exit 1; }
"$work/xrdb_unpack" extract "$archive" -o "$work/x" ui_actor_main_menu_one >/dev/null
dds=$(find "$work/x" -name 'ui_actor_main_menu_one.dds' | head -1)
[[ -n $dds ]] || { echo "make_launcher_backdrop: the archive has no ui_actor_main_menu_one.dds" >&2; exit 1; }
sips -s format png "$dds" --out "$work/menu.png" >/dev/null
# the scene is the top left of the 2048x2048 atlas (the rest is black and a second copy); keep the part inside the vignette
sips --cropToHeightWidth 600 1170 --cropOffset 40 60 "$work/menu.png" --out "$work/crop.png" >/dev/null
sips -s format jpeg -s formatOptions 72 "$work/crop.png" --out "$out/launcher-backdrop.jpg" >/dev/null
echo "$out/launcher-backdrop.jpg"
