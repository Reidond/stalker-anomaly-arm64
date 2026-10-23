#!/bin/bash
# Profile measurements on the Mac (docs/ios-port/SETTINGS.md): the five Cordon scenes of tools/profile_measure.py in both graphics
# modes, once per render size, unattended. A window of the shell log counts only when no other `anomaly` process ran during it
# (two games on one GPU measure each other): a run with contended windows is repeated, up to three times.
#
#   tools/render_harness/measure_profile.sh [-t tag] [-u] [-s "WxH WxH ..."] [-p profile] [-S seconds]
#
#   -t tag      output under build/render-harness/<tag>/ (default: profile-<date>)
#   -u          without the frame cap (`gfx_fps unlimited`, runs with --unlocked-graphics): what the Mac could do
#   -s sizes    render sizes in pixels, one run each (default: the display's native size 3024x1964, then 2/3 and 1/2 of it)
#   -p profile  force a profile (`--graphics-profile`), e.g. ipad-8gb to see the iPad's settings on the Mac
#   -S seconds  length of a scene's measured part (default 34: three 10 s windows)
#   -m modes    graphics modes, one launch each (default "quality performance")
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
tag=profile-$(date +%m%d-%H%M)
unlimited=0
sizes="3024x1964 2016x1310 1512x982"
profile=""
seconds=34
modes="quality performance"
while getopts "t:us:p:S:m:" opt; do
    case $opt in
        t) tag=$OPTARG;; u) unlimited=1;; s) sizes=$OPTARG;; p) profile=$OPTARG;; S) seconds=$OPTARG;; m) modes=$OPTARG;;
        *) exit 2;;
    esac
done
out=$root/build/render-harness/$tag
mkdir -p "$out"
for size in $sizes; do
  for mode in $modes; do
    for attempt in 1 2 3; do
        name=m_${size}_${mode}
        script=$out/$name.script
        gen=(--save h_cordon --seconds "$seconds" --modes "$mode")
        [ $unlimited = 1 ] && gen+=(--unlimited)
        python3 -I "$root/tools/profile_measure.py" script "${gen[@]}" > "$script"
        # wait for the GPU to be free of other games
        waited=0
        while [ "$(pgrep -f 'anomaly.app/Contents/MacOS/anomaly' | wc -l | tr -d ' ')" != 0 ] && [ $waited -lt 2400 ]; do sleep 5; waited=$((waited + 5)); done
        echo "== $size $mode attempt $attempt (waited ${waited}s for a free GPU)"
        sampler_out=$out/$name.contention
        : > "$sampler_out"
        (
            set +e +o pipefail # pgrep exits 1 when nothing matches (before the launch, after the exit): that is a count of 0, not an error
            start=$(date +%s)
            while true; do
                mine=$(pgrep -f "$script" | wc -l | tr -d ' ')
                all=$(pgrep -f 'anomaly.app/Contents/MacOS/anomaly' | wc -l | tr -d ' ')
                echo "$(( $(date +%s) - start )) $(( all - mine ))" >> "$sampler_out"
                sleep 1
            done
        ) &
        sampler=$!
        args=()
        [ $unlimited = 1 ] && args+=(--unlocked-graphics)
        [ -n "$profile" ] && args+=(--graphics-profile "$profile")
        HARNESS_ARGS="${args[*]:-}" "$root/tools/render_harness/run_scene.sh" -t "$tag" -s "$size" -l 1500 "$script" | tail -2
        kill $sampler 2> /dev/null || true
        wait $sampler 2> /dev/null || true
        [ -s "$sampler_out" ] || { echo "   the contention sampler recorded nothing: not counting this run as clean"; exit 1; }
        shell_log=$out/$name/shell.log
        python3 -I "$root/tools/profile_measure.py" summarize --contention "$sampler_out" "$shell_log" > "$out/$name.md"
        # the run must have reached its last scene (the engine sometimes crashes after a teleport: the physics commander, see SETTINGS.md)
        last=$(grep "log MEAS" "$script" | tail -1 | sed 's/^[0-9.]* log //')
        if ! grep -q "$last" "$shell_log" 2> /dev/null; then echo "   incomplete run, repeating"; continue; fi
        if grep -q "contended" "$out/$name.md"; then echo "   contended windows, repeating"; else break; fi
    done
    cat "$out/$name.md"
  done
done
