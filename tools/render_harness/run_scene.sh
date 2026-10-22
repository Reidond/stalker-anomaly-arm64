#!/bin/bash
# Runs --script scenes unattended and collects screenshots and logs under build/render-harness/<tag>/<scene>/.
#
#   tools/render_harness/run_scene.sh [-t tag] [-p preset] [-s WxH] [-l seconds] scene [scene ...]
#   tools/render_harness/run_scene.sh --newgame      # once: starts a new game and saves it as h_cordon (the base of the scenes)
#
# A scene is tools/render_harness/scenes/<scene>.script; unless it starts with `# standalone` the runner puts
# scenes/_load.script (load h_cordon, wait for the level) in front of it. `-p preset` copies presets/<preset>.ltx over
# the user.ltx of the run (no user.ltx at all otherwise: engine defaults). Every run has its own state root
# (ANOMALY_STATE_ROOT), so it neither reads nor changes the player's settings and saves. The app runs unattended
# (offscreen, background); see docs/ios-port/INTEGRATION.md.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
app=${ANOMALY_APP:-$root/build/macos-arm64-release/src/Platform/App/anomaly.app}
data=${ANOMALY_DATA_ROOT:-$root/game/Anomaly}
tag=$(date +%Y%m%d-%H%M%S)
preset=""
size=""
limit=420
newgame=0
scenes=()
while [ $# -gt 0 ]; do
    case $1 in
        -t) tag=$2; shift 2;;
        -p) preset=$2; shift 2;;
        -s) size=$2; shift 2;;
        -l) limit=$2; shift 2;;
        --newgame) newgame=1; shift;;
        -*) echo "unknown option $1" >&2; exit 2;;
        *) scenes+=("$1"); shift;;
    esac
done
[ $newgame = 1 ] && { scenes=(_newgame); size=${size:-3024x1964}; }
[ ${#scenes[@]} -gt 0 ] || { echo "usage: $0 [-t tag] [-p preset] [-s WxH] [-l seconds] scene... | --newgame" >&2; exit 2; }

out=$root/build/render-harness
saves=$out/saves
mkdir -p "$saves"

for scene in "${scenes[@]}"; do
    src=$root/tools/render_harness/scenes/$scene.script
    [ -f "$src" ] || { echo "no scene $src" >&2; exit 2; }
    run=$out/$tag/$scene
    rm -rf "$run"
    state=$run/state
    appdata=$state/Application\ Support/Anomaly/appdata
    mkdir -p "$appdata/savedgames" "$state/Caches/Anomaly" "$state/Logs/Anomaly"
    ln -sfn "$HOME/Library/Caches/Anomaly/shaders" "$state/Caches/Anomaly/shaders"
    if [ $scene != _newgame ]; then
        ls "$saves"/h_cordon.* >/dev/null 2>&1 || { echo "no base save: run $0 --newgame first" >&2; exit 1; }
        cp "$saves"/h_cordon.* "$appdata/savedgames/"
    fi
    [ -n "$preset" ] && cp "$root/tools/render_harness/presets/$preset.ltx" "$appdata/user.ltx"

    script=$run/run.script
    if head -1 "$src" | grep -q '^# standalone' || [ $scene = _newgame ]; then
        cp "$src" "$script"
    else
        cat "$root/tools/render_harness/scenes/_load.script" "$src" > "$script"
    fi

    args=(--data-root "$data" --script "$script" --exit-after "$limit" -dbg)
    [ -n "$size" ] && args+=(--size "$size")
    echo "== $scene (tag $tag${preset:+, preset $preset})"
    start=$(date +%s)
    open -g -j -W -n --env ANOMALY_STATE_ROOT="$state" --stdout "$run/stdout.txt" --stderr "$run/stderr.txt" "$app" --args "${args[@]}" || true
    echo "   $(( $(date +%s) - start )) s"

    mkdir -p "$run/shots"
    mv "$appdata"/screenshots/* "$run/shots/" 2>/dev/null || true
    cp "$appdata"/logs/xray_*.log "$run/engine.log" 2>/dev/null || true
    cp "$state/Logs/Anomaly/anomaly-shell.log" "$run/shell.log" 2>/dev/null || true
    [ $scene = _newgame ] && cp "$appdata"/savedgames/h_cordon.* "$saves/" 2>/dev/null || true
    echo "   shots: $(ls "$run/shots" | tr '\n' ' ')"
    grep -a "SCRIPT ERROR\|! screenshot\|fatal\|FATAL" "$run/engine.log" 2>/dev/null | head -5 | sed 's/^/   ! /' || true
done
