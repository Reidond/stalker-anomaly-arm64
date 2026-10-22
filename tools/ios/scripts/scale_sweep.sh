#!/bin/zsh
# fps, CPU ms, GPU ms and footprint per render scale and upscaler, at the native output size, in the same Cordon spot as res_sweep.sh:
#   tools/ios/scripts/scale_sweep.sh [scale:upscaler[:WxH] ...] > out.txt     (default: every scale with metalfx, then bilinear)
# The optional third field is vid_mode (default 2732x2048, the panel): `1.0:bilinear:1366x1024` is the old preset, the whole frame at 1366x1024.
# One launch per entry: load ipad_test, teleport to the field, 25 s of settling, then four 10 s lines are averaged, per thermal state
# (the iPad throttles at Serious: the same scene needs about 40% more GPU time then). The preset is applied first
# (--device-preset reset) so the settings are the documented ones, and vid_mode is the panel. SWEEP_COOLDOWN seconds (default 120) pass
# between launches, SWEEP_SHARP (default 0) is r__upscale_sharpness. Needs the app installed, the iPad unlocked, and nothing else launching on it. The player's settings change
# (user.ltx is saved at exit, with the last render scale): back it up first.
here=${0:A:h}
repo=${here:h:h:h}
entries=("$@")
if (( $#entries == 0 )); then
    for s in 0.5 0.6 0.67 0.75 0.85 1.0; do entries+=("$s:metalfx"); done
    for s in 0.5 0.6 0.67 0.75 0.85; do entries+=("$s:bilinear"); done
fi
tmp=$(mktemp -d)
out=${SWEEP_OUT:-$tmp/logs}
mkdir -p $out
echo "| scale | upscaler | output | thermal | samples | fps | CPU ms | GPU ms | footprint MB |"
echo "|---|---|---|---|---:|---:|---:|---:|---:|"
first=1
for e in $entries; do
    (( first )) || sleep ${SWEEP_COOLDOWN:-120}
    first=0
    parts=(${(s.:.)e})
    s=$parts[1]; up=$parts[2]; vm=${parts[3]:-2732x2048}
    tag=${s}_${up}_${vm}
    cat > $tmp/scale_${tag}.txt <<EOT
5 console vid_mode $vm
6 console r__upscaler $up
6 console r__upscale_sharpness ${SWEEP_SHARP:-0}
6 console r__render_scale $s
8 console start server(ipad_test/single/alife/load) client(localhost)
45 key Space
50 console run_string ipad_debug.tp(54.9, 3.5, 157.1)
60 log SCALE $s $up
100 quit
EOT
    $here/../ipad.sh script $tmp/scale_${tag}.txt --device-preset reset -dbg > /dev/null 2>&1
    $here/../ipad.sh logs $out/${tag} > /dev/null 2>&1
    python3 -I - "$out/${tag}/anomaly-shell.log" "$s" "$up" "$vm" <<'PY'
import re, sys
log, scale, up, vm = sys.argv[1:5]
by_state = {}
for line in open(log, errors="replace"):
    m = re.match(r"\[\s*([\d.]+)\] anomaly: ([\d.]+) fps \(\d+ frames in [\d.]+ s\), frame cpu ([\d.]+) ms avg [\d.]+ max, gpu ([\d.]+) ms, footprint (\d+) MB \(peak (\d+)\).*thermal (\w+)", line)
    if m and 60 <= float(m.group(1)) <= 100:
        by_state.setdefault(m.group(7), []).append([float(m.group(i)) for i in (2, 3, 4, 6)])
if not by_state:
    print(f"| {scale} | {up} | {vm} | no samples | - | - | - | - | - |")
for state, rows in by_state.items():
    n = len(rows)
    avg = lambda i: sum(r[i] for r in rows) / n
    print(f"| {scale} | {up} | {vm} | {state} | {n} | {avg(0):.1f} | {avg(1):.1f} | {avg(2):.1f} | {max(r[3] for r in rows):.0f} |")
PY
done
