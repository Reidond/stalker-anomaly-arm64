#!/bin/zsh
# The app lifecycle on the USB-connected iPad, driven from the Mac (nobody touches the iPad): the game runs `scripts/lifecycle.txt`, then
#   1. Settings is launched over it (the game goes to the background like a Home press),
#   2. the game is launched again (back to the foreground),
#   3. a memory warning is sent to its process,
#   4. Settings again, and the game process is killed while it is in the background (what jetsam does),
#   5. the game is started on `scripts/lifecycle_reload.txt`, which loads the save the app made on the way out.
# Lock and unlock, Control Center, a notification banner and a real call or alarm need a hand: the iPad's own button or swipe.
#   tools/ios/lifecycle_check.sh [out-dir]       logs and the per-step notes land in out-dir (default build/lifecycle-run)
set -euo pipefail
repo=${0:A:h:h:h}
cd $repo
out=${1:-build/lifecycle-run}; mkdir -p $out
bundle=com.andriishafar.anomaly
other=${LIFECYCLE_OTHER_APP:-com.apple.Preferences}
ipad=tools/ios/ipad.sh
device=${ANOMALY_IPAD_UDID:-}
[[ -n $device ]] || device=$(xcrun devicectl list devices --json-output - 2>/dev/null | python3 -I -c '
import json, sys
for dev in json.load(sys.stdin)["result"]["devices"]:
    hw = dev.get("hardwareProperties", {}); cp = dev.get("connectionProperties", {})
    if hw.get("deviceType") == "iPad" and hw.get("reality") == "physical" and cp.get("tunnelState") == "connected":
        print(hw.get("udid") or dev["identifier"]); break')
[[ -n $device ]] || { echo "no connected iPad" >&2; exit 1; }
dc() { xcrun devicectl "$@"; }
note() { print -r -- "$(date +%H:%M:%S) $*" | tee -a $out/steps.txt; }
pid_of() { dc device info processes --device $device 2>/dev/null | awk '/anomaly\.app/ {print $1; exit}'; }

note "start lifecycle.txt"
dc device copy to --device $device --domain-type appDataContainer --domain-identifier $bundle --source tools/ios/scripts/lifecycle.txt --destination Documents/lifecycle.txt >/dev/null
dc device copy to --device $device --domain-type appDataContainer --domain-identifier $bundle --source tools/ios/scripts/lifecycle_reload.txt --destination Documents/lifecycle_reload.txt >/dev/null
dc device process launch --device $device --terminate-existing $bundle --script lifecycle.txt >/dev/null
sleep 100                                            # load, "press any key", teleport: the script logs LIFE ready at about 60 s
note "pid $(pid_of)"

note "1 background: launch $other"
dc device process launch --device $device $other >/dev/null; sleep 15
note "2 foreground: launch the game"
dc device process launch --device $device $bundle >/dev/null; sleep 20
note "3 memory warning to pid $(pid_of)"
dc device process sendMemoryWarning --device $device --pid $(pid_of) >/dev/null || note "sendMemoryWarning failed"; sleep 15
note "4 background again, then kill it there"
dc device process launch --device $device $other >/dev/null; sleep 12
pid=$(pid_of); note "pid in the background: $pid"
[[ -n $pid ]] && dc device process terminate --device $device --pid $pid >/dev/null; sleep 3
$ipad logs $out/after-kill >/dev/null 2>&1 || true
note "5 restart on lifecycle_reload.txt"
dc device process launch --device $device $bundle --script lifecycle_reload.txt >/dev/null; sleep 70
$ipad logs $out/after-reload >/dev/null 2>&1 || true
dc device process terminate --device $device --pid $(pid_of) >/dev/null 2>&1 || true
note "done: $out"
