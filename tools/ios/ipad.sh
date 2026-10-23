#!/bin/zsh
# Build-free helpers for the signed `anomaly` app on a USB-connected iPad (never the Simulator).
#
#   tools/ios/ipad.sh build [config]          xcodebuild the anomaly scheme (default config: RelWithDebInfo), signed; XCODEBUILD_JOBS=n caps the jobs
#   tools/ios/ipad.sh install                 install the built app over com.andriishafar.anomaly (Documents stay)
#   tools/ios/ipad.sh install-app <anomaly.app>   install an app bundle built earlier (a saved copy of a build)
#   tools/ios/ipad.sh run [app args...]       launch with --console (Ctrl-C detaches; the app keeps running)
#   tools/ios/ipad.sh start [app args...]     launch without attaching the console
#   tools/ios/ipad.sh script <file> [args]    copy a --script timeline to Documents/ and launch with it
#   tools/ios/ipad.sh logs [dir]              pull the shell log, crash log and the engine log into <dir> (default: ipad-logs)
#   tools/ios/ipad.sh put <local> <Documents-relative path>
#   tools/ios/ipad.sh get <path under the container> <local>
#   tools/ios/ipad.sh shot <file.png>         screenshot of the iPad's screen (devicectl device capture)
#   tools/ios/ipad.sh pid                     the app's process id on the device (empty when it is not running)
#   tools/ios/ipad.sh signal [SIGTERM]        send a signal to the app
#   tools/ios/ipad.sh front <bundle id>       launch another app in front of the game (com.apple.Preferences): the game is backgrounded
#   tools/ios/ipad.sh kill                    terminate the app
#
# Options (before the subcommand): --device <udid|name>  (default $ANOMALY_IPAD_UDID, else the first connected iPad)
#                                  --build-dir <dir>     (default build/ios-arm64)
set -euo pipefail

repo=${0:A:h:h:h}
bundle=com.andriishafar.anomaly
device=${ANOMALY_IPAD_UDID:-}
build_dir=$repo/build/ios-arm64

while [[ ${1:-} == --* ]]; do
    case $1 in
        --device) device=$2; shift 2 ;;
        --build-dir) build_dir=$2; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
cmd=${1:-}; [[ -n $cmd ]] || { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
shift

die() { echo "error: $*" >&2; exit 1; }

if [[ -z $device ]]; then
    device=$(xcrun devicectl list devices --json-output - 2>/dev/null | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
for dev in d["result"]["devices"]:
    hw = dev.get("hardwareProperties", {})
    cp = dev.get("connectionProperties", {})
    if hw.get("deviceType") == "iPad" and hw.get("reality") == "physical" and cp.get("tunnelState") == "connected":
        print(hw.get("udid") or dev["identifier"]); break
')
    [[ -n $device ]] || die "no connected iPad found (connect it, unlock it, or pass --device)"
fi

dc() { xcrun devicectl "$@"; }
# ANOMALY_LAUNCH_ENV='{"MTL_HUD_ENABLED":"1"}' passes environment variables to the launched app (the Metal HUD shows GPU time)
launch_env=(); [[ -n ${ANOMALY_LAUNCH_ENV:-} ]] && launch_env=(--environment-variables "$ANOMALY_LAUNCH_ENV")
app_for() { echo "$build_dir/src/Platform/App/$1-iphoneos/anomaly.app"; }

case $cmd in
    build)
        config=${1:-RelWithDebInfo}
        # XCODEBUILD_JOBS=<n> caps the parallel jobs (the default is one per core)
        jobs_args=(); [[ -n ${XCODEBUILD_JOBS:-} ]] && jobs_args=(-jobs $XCODEBUILD_JOBS)
        xcodebuild -project "$build_dir/AnomalyPort.xcodeproj" -scheme anomaly -configuration "$config" \
            -destination 'generic/platform=iOS' -allowProvisioningUpdates "${jobs_args[@]}" build
        ;;
    install)
        app=$(app_for ${ANOMALY_CONFIG:-RelWithDebInfo})
        [[ -d $app ]] || die "no $app (run: tools/ios/ipad.sh build)"
        dc device install app --device "$device" "$app"
        ;;
    install-app)
        dc device install app --device "$device" "${1:?path to an anomaly.app}"
        ;;
    run)
        dc device process launch --device "$device" --terminate-existing --console "${launch_env[@]}" "$bundle" "$@"
        ;;
    start)
        dc device process launch --device "$device" --terminate-existing "${launch_env[@]}" "$bundle" "$@"
        ;;
    script)
        file=${1:?script file}; shift
        dc device copy to --device "$device" --domain-type appDataContainer --domain-identifier "$bundle" \
            --source "$file" --destination "Documents/${file:t}"
        dc device process launch --device "$device" --terminate-existing --console "${launch_env[@]}" "$bundle" \
            --script "${file:t}" "$@"
        ;;
    logs)
        out=${1:-ipad-logs}; mkdir -p "$out"
        for f in anomaly-shell.log anomaly-shell.prev.log anomaly-crash.log anomaly-crash.prev.log; do
            dc device copy from --device "$device" --domain-type appDataContainer --domain-identifier "$bundle" \
                --source "Documents/$f" --destination "$out/$f" >/dev/null 2>&1 || true
        done
        dc device copy from --device "$device" --domain-type appDataContainer --domain-identifier "$bundle" \
            --source "Library/Application Support/Anomaly/appdata/logs" --destination "$out/engine-logs" >/dev/null 2>&1 || true
        ls -l "$out" "$out/engine-logs" 2>/dev/null
        ;;
    put)
        dc device copy to --device "$device" --domain-type appDataContainer --domain-identifier "$bundle" \
            --source "$1" --destination "Documents/${2:?destination under Documents/}"
        ;;
    get)
        dc device copy from --device "$device" --domain-type appDataContainer --domain-identifier "$bundle" \
            --source "$1" --destination "$2"
        ;;
    shot)
        dc device capture screenshot --device "$device" --destination "${1:?file.png}"
        ;;
    pid)
        dc device info processes --device "$device" 2>/dev/null | awk '/anomaly\.app/ {print $1; exit}'
        ;;
    signal)
        sig=${1:-SIGTERM}
        pid=$(dc device info processes --device "$device" 2>/dev/null | awk '/anomaly\.app/ {print $1; exit}')
        [[ -n $pid ]] && dc device process signal --device "$device" --pid "$pid" --signal "$sig" || echo "not running"
        ;;
    front)
        # bring another app to the front (its bundle id, e.g. com.apple.Preferences): the game goes to the background
        dc device process launch --device "$device" "${1:?bundle id}"
        ;;
    kill)
        pid=$(dc device info processes --device "$device" 2>/dev/null | awk '/anomaly\.app/ {print $1; exit}')
        [[ -n $pid ]] && dc device process terminate --device "$device" --pid "$pid" || echo "not running"
        ;;
    *) die "unknown command $cmd" ;;
esac
