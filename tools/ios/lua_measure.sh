#!/bin/zsh
# Lua cost measurements on the iPad (docs/ios-port/LUA-PERF.md), about 40 minutes:
#   - per graphics mode one launch with the engine's Lua timers: the Cordon field, the Rookie village, the Bar (a second level load),
#     after a warm-up in the village (tools/lua_measure.py script),
#   - one sampling-profiler launch in Quality mode (the function tables),
#   - the level load with the bytecode cache off, cold (emptied by `lua_cache clear`) and warm, a few launches each,
#   - optionally (-g) the new game from the main menu with the same three cache states.
#
#   tools/ios/lua_measure.sh [-i] [-s save] [-o outdir] [-k lockfile] [-n loads] [-w seconds] [-g] [-q]
#     -i           install the built app first (tools/ios/ipad.sh build, then install)
#     -s save      the save to load (default ipad_test)
#     -o outdir    where the results go (default ios-lua-<time>)
#     -k lockfile  refuse to start while this file exists (a note another session leaves when it uses the iPad)
#     -n loads     launches per cache state of the load test (default 3)
#     -w seconds   warm-up in the village before the places (default 120; 0 measures cool)
#     -g           also the new-game test (about 90 s a launch)
#     -q           the load test only (no timer or profiler runs)
#
# Needs the iPad connected and unlocked. Never uses the Simulator. Leaves the iPad's user.ltx as it found it (the runs do not change it).
set -euo pipefail
here=${0:A:h}
repo=${0:A:h:h:h}
ipad=$here/ipad.sh
save=ipad_test
install=0
out=ios-lua-$(date +%m%d-%H%M)
lock=""
loads=3
soak=120
newgame=0
quick=0
while getopts "is:o:k:n:w:gq" opt; do
    case $opt in i) install=1;; s) save=$OPTARG;; o) out=$OPTARG;; k) lock=$OPTARG;; n) loads=$OPTARG;; w) soak=$OPTARG;; g) newgame=1;; q) quick=1;; *) exit 2;; esac
done
if [[ -n $lock && -e $lock ]]; then echo "the iPad is in use ($lock exists)" >&2; exit 3; fi
mkdir -p "$out"
appdata="Library/Application Support/Anomaly/appdata"
dev=$(xcrun devicectl list devices --json-output - 2>/dev/null | python3 -I -c '
import json, sys
for d in json.load(sys.stdin)["result"]["devices"]:
    hw = d.get("hardwareProperties", {}); cp = d.get("connectionProperties", {})
    if hw.get("deviceType") == "iPad" and hw.get("reality") == "physical" and cp.get("tunnelState") == "connected":
        print(hw.get("udid") or d["identifier"]); break')
[[ -n $dev ]] || { echo "no connected iPad" >&2; exit 1; }
bundle=com.andriishafar.anomaly
copy_from() { xcrun devicectl device copy from --device "$dev" --domain-type appDataContainer --domain-identifier $bundle --source "$1" --destination "$2" > /dev/null 2>&1 || true; }
running() { xcrun devicectl device info processes --device "$dev" 2>/dev/null | grep -c "anomaly.app" || true; }
run() { # name script [args]: a launch, its console output, the shell log and the engine log
    local name=$1 script=$2; shift 2
    local waited=0
    while [[ $(running) != 0 ]]; do
        (( waited >= 300 )) && { echo "an anomaly run that this script did not start is on the iPad: stopping" >&2; exit 4; }
        sleep 10; waited=$((waited + 10))
    done
    echo "== $name"
    $ipad script "$script" "$@" > "$out/$name.out" 2>&1 || true
    mkdir -p "$out/$name.logs"
    copy_from "$appdata/logs" "$out/$name.logs"
    copy_from "Documents/anomaly-shell.log" "$out/$name.shell.log"
}
engine_log() { find "$out/$1.logs" -name "xray_*.log" -not -name "*.bkp" 2>/dev/null | head -1; }
report=$out/loads.md

(( install )) && { $ipad build; $ipad install; }

if (( ! quick )); then
    for mode in quality performance; do
        python3 -I "$repo/tools/lua_measure.py" script --save $save --mode $mode --soak $soak > "$out/lua_$mode.script"
        run lua_$mode "$out/lua_$mode.script" -dbg -lua_perf
        python3 -I "$repo/tools/lua_measure.py" summarize --engine "$(engine_log lua_$mode)" --shell "$out/lua_$mode.shell.log" > "$out/lua_$mode.md" 2>/dev/null || true
    done
    python3 -I "$repo/tools/lua_measure.py" script --save $save --mode quality --soak $soak --profile > "$out/lua_prof.script"
    run lua_prof "$out/lua_prof.script" -dbg -lua_profile
fi

# the load test: a short script per cache state
cat > "$out/load.script" <<EOF
2 console start server($save/single/alife/load) client(localhost)
3 waitlevel 300 l01_escape
5 key Left Shift
6 key Left Shift
7 key Left Shift
8 key Left Shift
10 quit
EOF
cat > "$out/load_cold.script" <<EOF
1 console lua_cache clear
2 console start server($save/single/alife/load) client(localhost)
3 waitlevel 300 l01_escape
5 key Left Shift
6 key Left Shift
7 key Left Shift
8 key Left Shift
10 quit
EOF
# the iPad's menu (1366x1024 points): the Start button is at (873, 901)
cat > "$out/newgame.script" <<EOF
8 key Return
12 move 873 901
13 click 873 901
14 waitlevel 600
50 key Space
60 key Space
70 key Space
80 key Space
100 quit
EOF
cat > "$out/newgame_cold.script" <<EOF
1 console lua_cache clear
8 key Return
12 move 873 901
13 click 873 901
14 waitlevel 600
50 key Space
60 key Space
70 key Space
80 key Space
100 quit
EOF
{ echo "| What | Cache | Run | Seconds to level ready | Scripts | Lua load ms | file read | compile / cache | run | cache result |"
  echo "|---|---|---:|---:|---:|---:|---:|---:|---:|---|"; } > "$report"
kinds=(load)
(( newgame )) && kinds+=(newgame)
for kind in $kinds; do
    for config in off cold warm; do
        for n in $(seq 1 $loads); do
            case $config in
                off) script=$out/$kind.script; args=(-dbg -lua_perf -no_lua_cache);;
                cold) script=$out/${kind}_cold.script; args=(-dbg -lua_perf);;
                warm) script=$out/$kind.script; args=(-dbg -lua_perf);;
            esac
            # a warm launch needs a filled cache: the first launch after a cold one is the warm-up (its row stays, it is a warm launch)
            run ${kind}_${config}_$n "$script" $args
            python3 -I "$repo/tools/lua_measure.py" loadrow --kind $kind --config $config --run $n --shell "$out/${kind}_${config}_$n.shell.log" --engine "$(engine_log ${kind}_${config}_$n)" >> "$report"
        done
    done
done
cat "$report"
echo "results in $out"
