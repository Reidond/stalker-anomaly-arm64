#!/bin/bash
# Prints a markdown table of the Metal frame statistics a set of harness runs logged (the shell log has one "metal:" line per
# 10 s: GPU and CPU ms per frame, draws, passes, texture/buffer/allocated MB), averaged over the samples taken after the level
# had loaded (the last two). A footprint.txt next to the log adds the process's physical footprint.
#
#   tools/render_harness/perf_report.sh <tag> [<tag> ...]     (build/render-harness/<tag>/<scene>/shell.log)
root=$(cd "$(dirname "$0")/../.." && pwd)
echo "| run | scene | GPU ms | CPU ms | draws | passes | textures MB | buffers MB | Metal MB | footprint MB |"
echo "|---|---|---|---|---|---|---|---|---|---|"
for tag in "$@"; do
    for dir in "$root"/build/render-harness/$tag/*/; do
        scene=$(basename "$dir")
        log=$dir/shell.log
        [ -f "$log" ] || continue
        row=$(grep -a "anomaly: metal:" "$log" | tail -2 | sed -E 's/.*gpu ([0-9.]+) ms\/frame, cpu ([0-9.]+) ms\/frame \(blocked [0-9.]+\), ([0-9]+) draws, ([0-9.]+) passes\/frame; textures ([0-9]+) MB, buffers ([0-9]+) MB, allocated ([0-9]+) MB.*/\1 \2 \3 \4 \5 \6 \7/' |
            awk '{ for (i = 1; i <= 7; i++) s[i] += $i; n++ } END { if (n) printf "%.2f | %.2f | %.0f | %.1f | %.0f | %.0f | %.0f", s[1]/n, s[2]/n, s[3]/n, s[4]/n, s[5]/n, s[6]/n, s[7]/n }')
        [ -n "$row" ] || continue
        fp=""
        [ -f "$dir/footprint.txt" ] && fp=$(grep -a -m1 "Footprint:" "$dir/footprint.txt" | sed -E 's/.*Footprint: ([0-9.]+) ([KMG]B).*/\1 \2/' | awk '{ v = $1; if ($2 == "KB") v /= 1024; if ($2 == "GB") v *= 1024; printf "%.0f", v }')
        echo "| $tag | $scene | $row | ${fp:--} |"
    done
done
