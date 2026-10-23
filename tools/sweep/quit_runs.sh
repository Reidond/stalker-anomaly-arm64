#!/bin/bash
# Repeats the quit plans of tools/sweep/run.sh and measures how long each quit takes and whether any hangs or crashes.
#
#   tools/sweep/quit_runs.sh [-n runs] [-o outdir] [run.sh options...] [plan ...]
#
#   plans  default: quit_menu quit_level quit_loading (a quit from the main menu, from a level, and while the new game loads),
#          run round-robin for `runs` runs (default 30). quit_level needs the save sweep_base in the state (run.sh newgame).
#   -o     where the runs go (default build/quit-runs/<time>); each run is <outdir>/run-NN-<plan>/ as run.sh writes it
#   other options (--tsan, --plain, --app, --scale, --timeout, --state, ...) are handed to run.sh as they are
#
# One line per run in <outdir>/results.txt:  run plan result wall-seconds quit-seconds
#   result  ok      the shell logged "anomaly: quit (ok)" and no crash file was written
#           crash   a crash file or a sanitizer report of the fatal kind
#           hang    the runner's watchdog had to stop the app (--timeout), or the log has no quit line
#   quit-seconds is the time between "script asked to quit" and the shell's "quit (...)" line: the engine's whole shutdown
set -u
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
runs=30
out=""
opts=()
plans=()
while [ $# -gt 0 ]; do
    case "$1" in
        -n) runs="$2"; shift 2 ;;
        -o) out="$2"; shift 2 ;;
        --app|--data-root|--scale|--timeout|--state|--size|--density) opts+=("$1" "$2"); shift 2 ;;
        --plain|--tsan) opts+=("$1"); shift ;;
        -h|--help) sed -n 2,16p "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) plans+=("$1"); shift ;;
    esac
done
[ ${#plans[@]} -gt 0 ] || plans=(quit_menu quit_level quit_loading)
[ -n "$out" ] || out="$repo/build/quit-runs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

total=0
bad=0
for ((i = 1; i <= runs; i++)); do
    plan="${plans[$(( (i - 1) % ${#plans[@]} ))]}"
    dir="$out/run-$(printf %02d "$i")-$plan"
    t0=$(date +%s)
    "$here/run.sh" "${opts[@]}" --out "$dir" "$plan" > "$dir.console.txt" 2>&1
    wall=$(( $(date +%s) - t0 ))
    result=ok
    shell_log="$dir/shell.log"
    quit_a=$(grep -a "script asked to quit" "$shell_log" 2>/dev/null | tail -1 | sed -E 's/^\[ *([0-9.]+)\].*/\1/')
    quit_b=$(grep -a "anomaly: quit (" "$shell_log" 2>/dev/null | tail -1 | sed -E 's/^\[ *([0-9.]+)\].*/\1/')
    if grep -a -q "timeout after\|the app was still up" "$dir.console.txt"; then
        result=hang
    elif ls "$dir"/*crash* >/dev/null 2>&1 || grep -a -q "^==[0-9]*==ERROR\|ThreadSanitizer: .*\(heap-use-after-free\|SEGV\)" "$dir/stderr.txt" 2>/dev/null; then
        result=crash
    elif ! grep -a -q "anomaly: quit (ok)" "$shell_log" 2>/dev/null; then
        result=hang
    fi
    qt="-"
    [ -n "$quit_a" ] && [ -n "$quit_b" ] && qt=$(awk -v a="$quit_a" -v b="$quit_b" 'BEGIN{printf "%.2f", b - a}')
    echo "$i $plan $result ${wall}s $qt" | tee -a "$out/results.txt"
    total=$((total + 1))
    [ "$result" = ok ] || bad=$((bad + 1))
done
echo "== $total runs, $bad not ok" | tee -a "$out/results.txt"
