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
#
# Environment: HARNESS_COLD_SHADERS=1 (empty shader cache), HARNESS_ENV="VAR=value ..." (passed to the app), HARNESS_ARGS="..." (more
# app arguments; `-p preset` already adds `--graphics-profile none`),
# HARNESS_STATE=<dir> (use that state root instead of a fresh one), HARNESS_FOOTPRINT=1 (writes footprint.txt: `footprint` of the app at about 40 s; tools/render_harness/perf_report.sh reads it).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
app=${ANOMALY_APP:-$root/build/macos-arm64-release/src/Platform/App/anomaly.app}
data=${ANOMALY_DATA_ROOT:-$root/game/Anomaly}
if [ ! -d "$data/db" ]; then # a worktree has no game/: the main checkout's one
    data=$(dirname "$(git -C "$root" rev-parse --git-common-dir)")/game/Anomaly
fi
[ -d "$data/db" ] || { echo "game data not found (set ANOMALY_DATA_ROOT)" >&2; exit 1; }
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
    # a path to a script file works too (tools/profile_measure.py script)
    if [ -f "$scene" ]; then src=$(cd "$(dirname "$scene")" && pwd)/$(basename "$scene"); scene=$(basename "${scene%.*}"); fi
    [ -f "$src" ] || { echo "no scene $src" >&2; exit 2; }
    run=$out/$tag/$scene
    rm -rf "$run"
    mkdir -p "$run"
    state=${HARNESS_STATE:-$run/state} # HARNESS_STATE=<dir>: a state root that outlives the run (settings that must survive a restart)
    appdata=$state/Application\ Support/Anomaly/appdata
    mkdir -p "$appdata/savedgames" "$state/Caches/Anomaly" "$state/Logs/Anomaly"
    # the shader cache is shared with the player's by default (translating the whole shader set takes minutes);
    # HARNESS_COLD_SHADERS=1 runs with an empty one
    [ "${HARNESS_COLD_SHADERS:-0}" = 1 ] || ln -sfn "$HOME/Library/Caches/Anomaly/shaders" "$state/Caches/Anomaly/shaders"
    if [ $scene != _newgame ]; then
        ls "$saves"/h_cordon.* >/dev/null 2>&1 || { echo "no base save: run $0 --newgame first" >&2; exit 1; }
        cp "$saves"/h_cordon.* "$appdata/savedgames/"
    fi
    [ -n "$preset" ] && cp "$root/tools/render_harness/presets/$preset.ltx" "$appdata/user.ltx"

    script=$run/run.script
    if head -1 "$src" | grep -q '^# standalone' || [ $scene = _newgame ]; then
        cp "$src" "$script"
    else
        # the scene's times count from the level being ready: shift them past the load script's waitlevel step
        { cat "$root/tools/render_harness/scenes/_load.script"
          awk '/^[ \t]*#/ || NF == 0 { print; next } { $1 = $1 + 16; print }' "$src"; } > "$script"
    fi

    args=(--data-root "$data" --script "$script" --exit-after "$limit" -dbg)
    [ -n "$size" ] && args+=(--size "$size")
    # the Mac's graphics profile (docs/ios-port/SETTINGS.md) overrides user.ltx: a `-p preset` run is about the preset's own values, so it
    # runs without a profile. HARNESS_ARGS adds anything else, e.g. "--graphics-profile ipad" or "--unlocked-graphics".
    [ -n "$preset" ] && args+=(--graphics-profile none)
    # shellcheck disable=SC2206
    [ -n "${HARNESS_ARGS:-}" ] && args+=(${HARNESS_ARGS})
    echo "== $scene (tag $tag${preset:+, preset $preset})"
    start=$(date +%s)
    envargs=()
    for kv in ${HARNESS_ENV:-}; do envargs+=(--env "$kv"); done # extra environment of the app, e.g. "MallocScribble=1"
    open -g -j -n --env ANOMALY_STATE_ROOT="$state" ${envargs[@]+"${envargs[@]}"} --stdout "$run/stdout.txt" --stderr "$run/stderr.txt" "$app" --args "${args[@]}" || true
    # wait for the run to end; the engine sometimes hangs in its crash handler on the way out, so after the script asked to quit
    # it gets 40 s, and the run as a whole `limit` + 90 s, before it is killed
    shell_log=$state/Logs/Anomaly/anomaly-shell.log
    done_at=0
    fp_done=0
    while pgrep -f "$script" > /dev/null; do
        sleep 1
        now=$(date +%s)
        # HARNESS_FOOTPRINT=1: the process's physical footprint, once the level has run for a while
        if [ "${HARNESS_FOOTPRINT:-0}" = 1 ] && [ $fp_done = 0 ] && [ $(( now - start )) -gt 40 ]; then
            fp_done=1
            footprint "$(pgrep -f "$script" | head -1)" > "$run/footprint.txt" 2>&1 || true
        fi
        if [ $done_at = 0 ] && grep -aq "script asked to quit\|Anomaly shell shutting down\|--exit-after .* reached" "$shell_log" 2>/dev/null; then done_at=$now; fi
        if [ $done_at != 0 ] && [ $(( now - done_at )) -gt 40 ]; then echo "   ! process did not exit after shutdown, killing" ; pkill -9 -f "$script" || true; fi
        if [ $(( now - start )) -gt $(( limit + 90 )) ]; then echo "   ! run took too long, killing"; pkill -9 -f "$script" || true; fi
    done
    echo "   $(( $(date +%s) - start )) s"

    mkdir -p "$run/shots"
    mv "$appdata"/screenshots/* "$run/shots/" 2>/dev/null || true
    cp "$appdata"/logs/xray_*.log "$run/engine.log" 2>/dev/null || true
    cp "$state/Logs/Anomaly/anomaly-shell.log" "$run/shell.log" 2>/dev/null || true
    [ $scene = _newgame ] && cp "$appdata"/savedgames/h_cordon.* "$saves/" 2>/dev/null || true
    echo "   shots: $(ls "$run/shots" | tr '\n' ' ')"
    grep -a "SCRIPT ERROR\|! screenshot\|Fatal Error\|FATAL" "$run/engine.log" 2>/dev/null | head -5 | sed 's/^/   ! /' || true
done
