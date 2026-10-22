#!/bin/bash
# Prints a markdown table of the Metal frame statistics a set of harness runs logged (the shell log has one "metal:" line per
# 10 s: GPU and CPU ms per frame, draws, passes, texture/buffer/allocated MB), averaged over the samples taken after the level
# had loaded (the last two). A footprint.txt next to the log adds the process's physical footprint.
#
#   tools/render_harness/perf_report.sh <tag> [<tag> ...]     (build/render-harness/<tag>/<scene>/shell.log)
root=$(cd "$(dirname "$0")/../.." && pwd)
echo "| run | scene | GPU ms | CPU ms | draws | passes | textures MB | render targets MB | buffers MB | Metal MB | footprint MB |"
echo "|---|---|---|---|---|---|---|---|---|---|---|"
for tag in "$@"; do
    for dir in "$root"/build/render-harness/$tag/*/; do
        scene=$(basename "$dir")
        log=$dir/shell.log
        [ -f "$log" ] || continue
        row=$(grep -a "anomaly: metal:" "$log" | tail -2 | perl -ne 'if (/gpu ([\d.]+) ms\/frame, cpu ([\d.]+) ms\/frame \(blocked [\d.]+\), (\d+) draws, ([\d.]+) passes\/frame; textures (\d+) MB(?: \(render targets (\d+) MB\))?, buffers (\d+) MB, allocated (\d+) MB/) { $n++; $s[0]+=$1; $s[1]+=$2; $s[2]+=$3; $s[3]+=$4; $s[4]+=$5; $s[5]+=($6 // 0); $s[6]+=$7; $s[7]+=$8; } END { printf "%.2f | %.2f | %.0f | %.1f | %.0f | %.0f | %.0f | %.0f", map { $_ / $n } @s if $n }')
        [ -n "$row" ] || continue
        fp=""
        [ -f "$dir/footprint.txt" ] && fp=$(grep -a -m1 "Footprint:" "$dir/footprint.txt" | sed -E 's/.*Footprint: ([0-9.]+) ([KMG]B).*/\1 \2/' | awk '{ v = $1; if ($2 == "KB") v /= 1024; if ($2 == "GB") v *= 1024; printf "%.0f", v }')
        echo "| $tag | $scene | $row | ${fp:--} |"
    done
done
