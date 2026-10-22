#!/bin/bash
# Runs scenes under each quality preset (tools/render_harness/presets/*.ltx) and prints the Metal statistics table.
#
#   tools/render_harness/sweep.sh -t tag "preset1 preset2 ..." "scene1 scene2 ..."
#
# HARNESS_SIZE=WxH sets the back buffer size (default 1280x720; the iPad Air's screen is 2732x2048).
# Every run is tagged <tag>-<preset> with the process footprint sampled (HARNESS_FOOTPRINT); the table is the output of
# perf_report.sh for those tags. A run that did not produce its screenshot is reported as a failure.
set -u
root=$(cd "$(dirname "$0")/../.." && pwd)
tag=sweep
if [ "${1:-}" = "-t" ]; then tag=$2; shift 2; fi
presets=$1
scenes=$2
tags=()
for p in $presets; do
    tags+=("$tag-$p")
    HARNESS_FOOTPRINT=1 "$root/tools/render_harness/run_scene.sh" -t "$tag-$p" -p "$p" ${HARNESS_SIZE:+-s $HARNESS_SIZE} $scenes 2>&1 | grep -v "^   [0-9]* s$" | sed "s/^/[$p] /"
done
echo
"$root/tools/render_harness/perf_report.sh" "${tags[@]}"
