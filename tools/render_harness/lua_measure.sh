#!/bin/bash
# Lua cost measurements on the Mac (docs/ios-port/LUA-PERF.md), unattended like the other harness runs: per graphics mode one launch that
# loads the Cordon save and plays the field, the Rookie village and the Bar (tools/lua_measure.py), with the engine's Lua timers on.
#
#   tools/render_harness/lua_measure.sh [-t tag] [-m "quality performance"] [-p places] [-S seconds] [-P] [-n] [-a "app args"]
#
#   -t tag      output under build/render-harness/<tag>/ (default: lua-<date>)
#   -m modes    graphics modes, one launch each (default "quality performance")
#   -p places   comma list of field,village,bar (default all three)
#   -S seconds  length of a place's measured part (default 34)
#   -P          a sampling-profiler run (lua_profile) instead of a timer run: the function table
#   -n          without the bytecode cache (-no_lua_cache)
#   -j          LuaJIT's JIT off (-nojit): the interpreter, which is what the iPad runs
#   -a args     more app arguments (e.g. "--graphics-profile ipad-8gb")
# The state root of a run is its own (a cold bytecode cache, a shared shader cache); HARNESS_STATE=<dir> keeps one (a warm cache).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
tag=lua-$(date +%m%d-%H%M)
modes="quality performance"
places="field,village,bar"
seconds=34
profile=0
nocache=0
nojit=0
extra=""
while getopts "t:m:p:S:Pnja:" opt; do
    case $opt in
        t) tag=$OPTARG;; m) modes=$OPTARG;; p) places=$OPTARG;; S) seconds=$OPTARG;; P) profile=1;; n) nocache=1;; j) nojit=1;; a) extra=$OPTARG;;
        *) exit 2;;
    esac
done
out=$root/build/render-harness/$tag
mkdir -p "$out"
for mode in $modes; do
    name=lua_${mode}
    [ $profile = 1 ] && name=${name}_prof
    script=$out/$name.script
    gen=(--save h_cordon --mode "$mode" --places "$places" --seconds "$seconds")
    [ $profile = 1 ] && gen+=(--profile)
    python3 -I "$root/tools/lua_measure.py" script "${gen[@]}" > "$script"
    # one game at a time on the GPU
    waited=0
    while [ "$(pgrep -f "$script" | wc -l | tr -d ' ')" != 0 ] && [ $waited -lt 600 ]; do sleep 5; waited=$((waited + 5)); done
    args=(-lua_perf)
    [ $profile = 1 ] && args=(-lua_profile)
    [ $nocache = 1 ] && args+=(-no_lua_cache)
    [ $nojit = 1 ] && args+=(-nojit)
    echo "== $mode (${args[*]} $extra)"
    HARNESS_ARGS="${args[*]} $extra" "$root/tools/render_harness/run_scene.sh" -t "$tag" -l 1500 "$script" | tail -3
    run=$out/$name
    python3 -I "$root/tools/lua_measure.py" summarize --engine "$run/engine.log" --shell "$run/shell.log" > "$out/$name.md" || true
    [ $profile = 1 ] || python3 -I "$root/tools/profile_measure.py" summarize "$run/shell.log" >> "$out/$name.md" || true
    cat "$out/$name.md"
done
