#!/bin/bash
# Level-load and new-game times on the Mac with the script bytecode cache off, cold (empty) and warm (docs/ios-port/LUA-PERF.md).
#
#   tools/render_harness/lua_load.sh [-t tag] [-n runs] [-g] [-a "app args"]
#
#   -t tag    output under build/render-harness/<tag>/ (default: luaload-<date>)
#   -n runs   launches per configuration (default 3)
#   -g        a new game from the main menu as well as the load of the h_cordon save
#   -a args   more app arguments
# Each configuration runs on one state root (build/render-harness/<tag>/state): `off` is -no_lua_cache, `cold` empties the cache directory
# before every launch, `warm` keeps it (one launch before the measured ones fills it). Prints a table: the seconds from the load command to
# `level ready` (shell log) and the engine's `[lua-load]` line of that load.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
tag=luaload-$(date +%m%d-%H%M)
runs=3
newgame=0
extra=""
while getopts "t:n:ga:" opt; do
    case $opt in t) tag=$OPTARG;; n) runs=$OPTARG;; g) newgame=1;; a) extra=$OPTARG;; *) exit 2;; esac
done
out=$root/build/render-harness/$tag
mkdir -p "$out"
export HARNESS_STATE=$out/state
mkdir -p "$HARNESS_STATE"
cache=$HARNESS_STATE/Caches/Anomaly/lua

load_script=$out/load.script
cat > "$load_script" <<'EOF'
# standalone
1 console r_screenshot_mode png
2 console start server(h_cordon/single/alife/load) client(localhost)
3 waitlevel 300 l01_escape
5 key Left Shift
6 key Left Shift
7 key Left Shift
8 key Left Shift
10 quit
EOF
new_script=$out/newgame.script
cat > "$new_script" <<'EOF'
# standalone
2 console r_screenshot_mode png
5 key Return
8 move 640 300
9 click 640 300
10 waitlevel 300 l01_escape
14 key Left Shift
16 key Left Shift
18 key Left Shift
20 key Left Shift
24 quit
EOF

report=$out/summary.md
{ echo "| What | Cache | Run | Seconds to level ready | Scripts | Lua load ms | file read | compile / cache | run | cache result |"
  echo "|---|---|---:|---:|---:|---:|---:|---:|---:|---|"; } > "$report"

one() { # kind script config run
    local kind=$1 script=$2 config=$3 n=$4 name=$1_$3_$4 args=(-lua_perf)
    case $config in
        off) args+=(-no_lua_cache);;
        cold) rm -rf "$cache";;
    esac
    cp "$script" "$out/$name.script"
    local size=()
    [ "$kind" = newgame ] && size=(-s 3024x1964)
    HARNESS_ARGS="${args[*]} $extra" "$root/tools/render_harness/run_scene.sh" -t "$tag" ${size[@]+"${size[@]}"} -l 300 "$out/$name.script" > /dev/null 2>&1 || true
    local run=$out/$name
    python3 -I "$root/tools/lua_measure.py" loadrow --kind "$kind" --config "$config" --run "$n" --shell "$run/shell.log" --engine "$run/engine.log" >> "$report"
}

kinds=(load)
[ $newgame = 1 ] && kinds+=(newgame)
for kind in "${kinds[@]}"; do
    script=$load_script
    [ "$kind" = newgame ] && script=$new_script
    for config in off cold warm; do
        [ "$config" = warm ] && one "$kind" "$script" warm 0 > /dev/null # fills the cache; its row is dropped below
        for n in $(seq 1 "$runs"); do
            echo "== $kind $config $n"
            one "$kind" "$script" "$config" "$n"
        done
    done
done
grep -v "| warm | 0 |" "$report" > "$report.tmp" && mv "$report.tmp" "$report"
cat "$report"
