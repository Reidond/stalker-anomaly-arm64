#!/bin/zsh
# Installs jit_lab on the iPad and runs it, optionally under lldb with the TXM broker.
#
#   tools/jit_lab/run_ipad.sh MODE [jit_lab arguments...]
#
# MODE:
#   plain     no debugger: the JIT must fall back to the interpreter
#   lldb      launch stopped, attach lldb with txm_jit_lldb.py, stay attached
#   detach    like lldb, but really detach once the region is authorized
#   stikdebug install and launch only; start the app's JIT from StikDebug yourself
#             (pass --wait-ms=60000 so the app waits for the debugger)
#
# Environment: JITLAB_UDID (device), JITLAB_BUILD (build dir, default build/ios-arm64),
# JITLAB_LOG (where the console output goes).
set -eu

here=${0:A:h}
repo=${here:h:h}
# Default: the first connected physical iPad, via devicectl.
udid=${JITLAB_UDID:-$(xcrun devicectl list devices 2>/dev/null | awk '/iPad/ && /physical/ { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9A-F]{8}-[0-9A-F]{16}$/) { print $i; exit } }')}
[[ -n $udid ]] || { print -u2 "run_ipad.sh: no connected iPad found; set JITLAB_UDID"; exit 1 }
build=${JITLAB_BUILD:-$repo/build/ios-arm64}
bundle=com.andriishafar.anomaly.jitlab
log=${JITLAB_LOG:-${TMPDIR:-/tmp}/jit_lab_console.log}

mode=${1:-lldb}
shift || true
args=("$@")

app=$(find "$build" -name jit_lab.app -path '*Debug-iphoneos*' -type d | head -1)
[[ -n $app ]] || { echo "jit_lab.app not found under $build; build the jit_lab scheme first" >&2; exit 1; }

echo "installing $app"
xcrun devicectl device install app --device "$udid" "$app" >/dev/null

launch=(xcrun devicectl device process launch --device "$udid" --terminate-existing --console)

case $mode in
  plain|stikdebug)
    "${launch[@]}" "$bundle" -- "${args[@]}" 2>&1 | tee "$log"
    ;;
  lldb|detach)
    honor=0; [[ $mode == detach ]] && honor=1
    : > "$log"
    "${launch[@]}" --start-stopped "$bundle" -- "${args[@]}" > "$log" 2>&1 &
    launcher=$!
    pid=""
    for _ in {1..60}; do
      pid=$(xcrun devicectl device info processes --device "$udid" 2>/dev/null \
        | awk '/jit_lab\.app\/jit_lab/ { print $1; exit }')
      [[ -n $pid ]] && break
      sleep 1
    done
    [[ -n $pid ]] || { echo "could not find the jit_lab process" >&2; kill $launcher 2>/dev/null || true; exit 1; }
    echo "attaching lldb to pid $pid"
    TXM_HONOR_DETACH=$honor xcrun lldb -b \
      -o "command script import $here/txm_jit_lldb.py" \
      -o "device select $udid" \
      -o "device process attach -p $pid" \
      -o "process handle SIGBUS SIGSEGV SIGILL SIGTRAP -p true -s false -n false" \
      -o "txm_wait" &
    lldbpid=$!
    tail -f "$log" &
    tailpid=$!
    wait $lldbpid || true
    # After a real detach, lldb returns while the app keeps running: follow it to the end.
    wait $launcher || true
    kill $tailpid 2>/dev/null || true
    ;;
  *)
    echo "unknown mode '$mode' (plain | lldb | detach | stikdebug)" >&2
    exit 2
    ;;
esac
