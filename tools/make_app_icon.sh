#!/bin/bash
# Generates the app icon set for the `anomaly` bundle: an iOS asset catalog and a macOS .icns.
#
#   tools/make_app_icon.sh <out-dir> [game-dir]
#
# With game-dir (the Anomaly install holding AnomalyLauncher.exe) the icon is the launcher's logo on a dark background;
# without it, or when the logo cannot be extracted, it is platform/icon/fallback.png, the repository's own artwork.
# The Anomaly logo is GSC artwork: it is generated into <out-dir> (the gitignored build tree) and is never committed.
# Run by cmake/AnomalyAppIcon.cmake at configure time; also usable by hand.
#
# Writes into <out-dir>:
#   Assets.xcassets/AppIcon.appiconset   single-size 1024 px iOS icon (iOS 17+)
#   AppIcon.icns                         macOS icon (needs `swift` for the rounded body; skipped with a warning without it)
#   icon-1024.png, icon-mac-1024.png     the two masters, for a look at the result
#   source.txt                           "game" or "fallback"
set -euo pipefail

out=${1:?usage: make_app_icon.sh <out-dir> [game-dir]}
game_dir=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)

rm -rf "$out"
mkdir -p "$out"
master="$out/icon-1024.png"
source=fallback

if [[ -n $game_dir && -f $game_dir/AnomalyLauncher.exe ]]; then
    if /usr/bin/python3 -I "$here/app_icon/extract_launcher_icon.py" "$game_dir/AnomalyLauncher.exe" "$out/launcher-logo.png" >/dev/null &&
       swift "$here/app_icon/compose_icon.swift" logo "$out/launcher-logo.png" "$master"; then
        source=game
    else
        echo "make_app_icon: could not build the icon from $game_dir/AnomalyLauncher.exe, using the fallback" >&2
    fi
    rm -f "$out/launcher-logo.png"
elif [[ -n $game_dir ]]; then
    echo "make_app_icon: $game_dir/AnomalyLauncher.exe not found, using the fallback" >&2
fi
if [[ $source == fallback ]]; then
    cp "$repo/platform/icon/fallback.png" "$master"
fi
echo "$source" > "$out/source.txt"

# iOS: one 1024 px image, opaque (iOS rounds the corners itself).
set_dir="$out/Assets.xcassets/AppIcon.appiconset"
mkdir -p "$set_dir"
cp "$master" "$set_dir/icon-1024.png"
cat > "$out/Assets.xcassets/Contents.json" <<'JSON'
{ "info": { "author": "xcode", "version": 1 } }
JSON
cat > "$set_dir/Contents.json" <<'JSON'
{
  "images": [
    { "filename": "icon-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024" }
  ],
  "info": { "author": "xcode", "version": 1 }
}
JSON

# macOS: rounded body with a shadow, at every size iconutil wants.
mac="$out/icon-mac-1024.png"
if swift "$here/app_icon/compose_icon.swift" mac "$master" "$mac"; then
    iconset="$out/AppIcon.iconset"
    mkdir -p "$iconset"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$mac" --out "$iconset/icon_${size}x${size}.png" >/dev/null
        sips -z $((size * 2)) $((size * 2)) "$mac" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$iconset" -o "$out/AppIcon.icns"
    rm -rf "$iconset"
else
    echo "make_app_icon: swift is not available, the macOS bundle gets no icon" >&2
fi
echo "make_app_icon: $source icon written to $out"
