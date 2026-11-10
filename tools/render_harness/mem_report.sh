#!/bin/bash
# The memory table of docs/web-port/MEMORY.md with one command: how much a 32-bit WebAssembly heap (4 GB cap) would have to hold,
# measured on the Mac build, unattended. Runs the scenarios below under the iPad profile (ipad-8gb: texture_lod 1, render scale 0.6,
# 2732x2048) and the Mac's own default, then the texture_lod and no_ram_textures sweeps, and prints one table per run
# (tools/render_harness/mem_report.py explains the columns). About 45 minutes.
#
#   tools/render_harness/mem_report.sh [-t tag]
#
#   MEM_PROFILES="ipad mac"                         which profiles
#   MEM_SCENES="newgame menu cordon levels"         which scenarios (newgame also makes the base save the others load)
#   MEM_SWEEPS=1                                    0 skips the texture_lod / no_ram_textures sweeps
#   MEM_REPORT_ONLY=1                               do not run anything, only print the tables of the runs under <tag>
#
# Needs the release app (cmake --build build/macos-arm64-release --target anomaly) and the game data (see run_scene.sh).
# Waits while another `anomaly` process runs, so a timing or memory number is not taken on a loaded machine.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
tag=mem-$(date +%Y%m%d-%H%M)
if [ "${1:-}" = "-t" ]; then tag=$2; shift 2; fi
profiles=${MEM_PROFILES:-"ipad mac"}
scenes=${MEM_SCENES:-"newgame menu cordon levels"}
sweeps=${MEM_SWEEPS:-1}
run=$root/tools/render_harness/run_scene.sh
out=$root/build/render-harness/$tag
app=${ANOMALY_APP:-$root/build/macos-arm64-release/src/Platform/App/anomaly.app}

wait_idle() {
    while pgrep -x anomaly > /dev/null; do
        echo "   another anomaly process is running; waiting"
        sleep 20
    done
}

# one run: wait_run <run tag> <scene> [run_scene.sh options...]; the sampler and the snapshots are on
wait_run() {
    local rtag=$1 scene=$2
    shift 2
    wait_idle
    HARNESS_MEMSTAT=1 "$run" -t "$rtag" "$@" "$scene" 2>&1 | grep -v "^   shots:" | sed "s|^|[$rtag] |"
}

if [ "${MEM_REPORT_ONLY:-0}" != 1 ]; then
    for profile in $profiles; do
        case $profile in
            ipad) export HARNESS_ARGS="--graphics-profile ipad"; size=2732x2048;;
            mac) unset HARNESS_ARGS; size=1280x720;;
            *) echo "unknown profile $profile (ipad or mac)" >&2; exit 2;;
        esac
        for s in $scenes; do
            case $s in
                newgame) wait_run "$tag/$profile" _newgame -l 150 -s 3024x1964;; # its clicks are laid out for that size
                menu) wait_run "$tag/$profile" mem_menu -l 120 -s $size;;
                cordon) HARNESS_MEMSNAP="200 400" wait_run "$tag/$profile" mem_cordon -l 600 -s $size;;
                levels) wait_run "$tag/$profile" mem_levels -l 700 -s $size;;
                *) echo "unknown scenario $s" >&2; exit 2;;
            esac
        done
    done
    unset HARNESS_ARGS
    if [ "$sweeps" = 1 ]; then
        # the engine defaults (`_preset Default`, no graphics profile) with one setting changed; 1280x720
        for lod in 0 1 2 3; do
            wait_run "$tag/sweep-lod$lod" mem_cordon_short -l 300 -s 1280x720 -p default -x "texture_lod $lod"
        done
        wait_run "$tag/sweep-ramtex" mem_cordon_short -l 300 -s 1280x720 -p default -x "texture_lod 2" -x "r__no_ram_textures off"
    fi
fi

# the program's static data (the Mach-O __DATA segments), a good stand-in for what a wasm binary's data segment would take
static_mb=$(size -m "$app/Contents/MacOS/anomaly" 2>/dev/null | awk '/^Segment __DATA/ { s += $3 } END { printf "%.0f", s / 1048576 }')
{
    echo "<!-- tag $tag; static data $static_mb MB; stacks 64 MB (estimate) -->"
    py="python3 -I $root/tools/render_harness/mem_report.py --static-mb ${static_mb:-0}"
    for pdir in "$out"/ipad "$out"/mac; do
        [ -d "$pdir" ] || continue
        echo "## Summary: $(basename "$pdir") profile (MB; steady = medians of the last 10 s, peak = the largest sample with its parts)"
        echo
        $py --summary "$pdir"
        echo
    done
    if ls -d "$out"/sweep-* > /dev/null 2>&1; then
        echo "## Sweeps: the Cordon for 90 s, engine defaults (MB)"
        echo
        $py --sweeps "$out"/sweep-*
        echo
    fi
    echo "## Every run"
    echo
    for dir in "$out"/*/*/; do
        [ -f "$dir/mem.csv" ] && $py "$dir"
    done
} | tee "$out/mem_report.md"
echo "written: $out/mem_report.md"
