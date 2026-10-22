#!/bin/zsh
# Everything the iPad has to show for the device graphics profile (docs/ios-port/SETTINGS.md), in one go, about 25 minutes:
#   - the menu screenshot,
#   - the measurement table: five Cordon scenes in each graphics mode, one launch per mode (tools/profile_measure.py),
#   - the lock checks: console and script changes refused, a user.ltx full of stale values overridden at the next start.
#
#   tools/ios/measure_profile.sh [-i] [-s save] [-o outdir] [-k lockfile]
#     -i           install the built app first (tools/ios/ipad.sh build, then install)
#     -s save      the save to load (default ipad_test)
#     -o outdir    where the results go (default ios-profile-<time>)
#     -k lockfile  refuse to start while this file exists (a note another session leaves when it uses the iPad)
#
# Needs the iPad connected and unlocked. It restores user.ltx as it found it. Never uses the Simulator.
set -euo pipefail
here=${0:A:h}
repo=${0:A:h:h:h}
ipad=$here/ipad.sh
save=ipad_test
install=0
out=ios-profile-$(date +%m%d-%H%M)
lock=""
while getopts "is:o:k:" opt; do
    case $opt in i) install=1;; s) save=$OPTARG;; o) out=$OPTARG;; k) lock=$OPTARG;; *) exit 2;; esac
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
copy_to() { xcrun devicectl device copy to --device "$dev" --domain-type appDataContainer --domain-identifier $bundle --source "$1" --destination "$2" > /dev/null; }
copy_from() { xcrun devicectl device copy from --device "$dev" --domain-type appDataContainer --domain-identifier $bundle --source "$1" --destination "$2" > /dev/null 2>&1 || true; }
running() { xcrun devicectl device info processes --device "$dev" 2>/dev/null | grep -c "anomaly.app" || true; }
run() { # name script [args]: a launch, its console output and its engine log
    local name=$1 script=$2; shift 2
    # a launch kills a running app (--terminate-existing): never start while one runs that this script did not start
    local waited=0
    while [[ $(running) != 0 ]]; do
        (( waited >= 300 )) && { echo "an anomaly run that this script did not start is on the iPad: stopping" >&2; exit 4; }
        sleep 10; waited=$((waited + 10))
    done
    echo "== $name"
    $ipad script "$script" "$@" > "$out/$name.out" 2>&1 || true
    mkdir -p "$out/$name.logs"; copy_from "$appdata/logs" "$out/$name.logs"
}

(( install )) && { $ipad build; $ipad install; }
copy_from "$appdata/user.ltx" "$out/user.ltx.orig"

run menu "$here/scripts/settings_menu.txt"
mkdir -p "$out/shots"; copy_from "$appdata/screenshots" "$out/shots"

for mode in quality performance; do
    python3 -I "$repo/tools/profile_measure.py" script --save $save --modes $mode > "$out/measure_$mode.script"
    run measure_$mode "$out/measure_$mode.script" -dbg
done
python3 -I "$repo/tools/profile_measure.py" summarize "$out/measure_quality.out" "$out/measure_performance.out" > "$out/measurements.md"
cat "$out/measurements.md"

# locks: refused changes, then the stale file
run lock1 "$here/scripts/lock_test.txt" -dbg
copy_from "$appdata/user.ltx" "$out/user.ltx.after_lock1"
{ cat "$out/user.ltx.after_lock1"
  printf 'r2_sun_quality st_opt_extreme\r\ntexture_lod 0\r\nr4_enable_tessellation on\r\nr3_msaa 4x\r\nr2_ssao st_opt_high\r\nr3_dynamic_wet_surfaces on\r\nr__detail_radius 250\r\nvid_mode 2732x2048\r\nssfx_ssr (1, 1, 1, 1)\r\nr2_mblur_enabled on\r\n'; } > "$out/user.ltx.stale"
copy_to "$out/user.ltx.stale" "$appdata/user.ltx"
run lock2 "$here/scripts/restart_check.txt" -dbg
[[ -s $out/user.ltx.orig ]] && copy_to "$out/user.ltx.orig" "$appdata/user.ltx"
echo "results in $out"
