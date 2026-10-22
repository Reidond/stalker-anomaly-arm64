#!/bin/bash
# Runs one crash-sweep plan of gamedata/scripts/sweep_driver.script against the `anomaly` app, normally the ASan+UBSan
# build (cmake --preset macos-arm64-asan), and summarises the sanitizer reports. See docs/ios-port/SANITIZERS.md.
#
#   tools/sweep/run.sh [options] <plan>
#
#   plans   newgame      new game from the menu, wait in the Cordon, save sweep_base (the start of every other plan)
#           smoke        ~5 min: combat, inventory, mid-combat save, death + reload, PDA, trade, Cordon -> Garbage, save
#                        (starts a new game from the menu unless sweep_base exists: then it loads that)
#           combat inventory pda trade surge anomalies sleep transition saveload
#                        one scenario each, from the save sweep_base
#           quit_inventory quit_pda quit_dialog quit_combat quit_dead quit_surge
#                        reach the state on sweep_base and quit from it; quit_menu (script quit in the main menu) and
#                        quit_loading (quit while the new game loads) need no save
#   options --bin <path>        the anomaly executable (default build/macos-arm64-asan/.../anomaly)
#           --data-root <dir>   Anomaly game data (default $ANOMALY_DATA_ROOT, else <repo>/game/Anomaly, else the main checkout's)
#           --scale <n>         multiply every time of the menu script (default 1; 2 or 3 on a loaded machine)
#           --timeout <s>       kill the run after this long (default 1500)
#           --out <dir>         where the run's files go (default build/sweep-runs/<time>-<plan>)
#           --plain             no sanitizer environment (for a release build)
#           --vid-mode WxH      render size of the window (default 1260x818), --density <n> the display scale (default 2)
#           --quit-at <s>       end the script this many seconds after the engine is up even if the plan has not (default timeout-60)
#
# The app never takes the focus or a Space: SDL_MAC_BACKGROUND_APP, no activation when shown/raised, a null OpenAL device
# (silent), a small window (rs_screenmode windowed, vid_mode 1260x818 = 630x409 points on a 2x display, written to the
# run's user.ltx) and a watchdog that kills the app after --timeout and when this script is interrupted.
#
# The app runs with its own HOME (CFFIXED_USER_HOME=build/sweep-home), so saves, logs, shader and overlay caches are not
# shared with anyone else's runs of the app; the first run there compiles the shaders again. Set SWEEP_HOME to move it.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

bin="$repo/build/macos-arm64-asan/src/Platform/App/anomaly.app/Contents/MacOS/anomaly"
[ -x "$bin" ] || bin="$repo/build/macos-asan/src/Platform/App/anomaly.app/Contents/MacOS/anomaly"
data_root="${ANOMALY_DATA_ROOT:-}"
scale=1
timeout_s=1500
out=""
plain=0
quit_at=""
plan=""
vid_mode="1260x818"   # render pixels; the window is vid_mode / density points (the aspect of the 1512x982 display of the M1 runs)
density=2

while [ $# -gt 0 ]; do
    case "$1" in
        --bin) bin="$2"; shift 2 ;;
        --data-root) data_root="$2"; shift 2 ;;
        --scale) scale="$2"; shift 2 ;;
        --timeout) timeout_s="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --plain) plain=1; shift ;;
        --quit-at) quit_at="$2"; shift 2 ;;
        --vid-mode) vid_mode="$2"; shift 2 ;;
        --density) density="$2"; shift 2 ;;
        -h|--help) sed -n 2,28p "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) plan="$1"; shift ;;
    esac
done
[ -n "$plan" ] || { sed -n 2,22p "$0"; exit 2; }
[ -x "$bin" ] || { echo "no executable at $bin (build it: cmake --preset macos-arm64-asan && cmake --build --preset macos-arm64-asan --target anomaly)" >&2; exit 2; }

if [ -z "$data_root" ]; then
    if [ -d "$repo/game/Anomaly" ]; then
        data_root="$repo/game/Anomaly"
    else
        # a git worktree: the game data sits in the main checkout
        main="$(git -C "$repo" worktree list --porcelain | awk '/^worktree /{print $2; exit}')"
        data_root="$main/game/Anomaly"
    fi
fi
[ -d "$data_root/db" ] || { echo "no game data at $data_root (--data-root)" >&2; exit 2; }

home="${SWEEP_HOME:-$repo/build/sweep-home}"
mkdir -p "$home"
stamp="$(date +%Y%m%d-%H%M%S)"
[ -n "$out" ] || out="$repo/build/sweep-runs/$stamp-$plan"
mkdir -p "$out"

appdata="$home/Library/Application Support/Anomaly/appdata"
base_save="$appdata/savedgames/sweep_base.scop"

# ---- the --script timeline ------------------------------------------------------------------------------------------
# t is seconds since the engine is up. The menu path is the one INTEGRATION.md documents (1512x982 window); the plan's own
# waits are inside the Lua driver, which follows the game, not the clock.
script="$out/run.script"
start_new_game=0
menu_only=0
quit_after=""
case "$plan" in
    quit_menu) quit_after=8; menu_only=1 ;;
    quit_loading) start_new_game=1; quit_after=$(awk -v s="$scale" 'BEGIN{printf "%d", 22*s}') ;; # just after Start was clicked: the loading screen
    newgame) start_new_game=1 ;;
    smoke) [ -f "$base_save" ] || start_new_game=1 ;;
    *) [ -f "$base_save" ] || { echo "no sweep_base save in $appdata/savedgames: run 'tools/sweep/run.sh newgame' first" >&2; exit 2; } ;;
esac
{
    echo "# generated by tools/sweep/run.sh: plan $plan, scale $scale"
    if [ "$start_new_game" = 1 ]; then
        # the UI cursor starts at the middle of the UI space and moves by the pointer's deltas, so the Start button is
        # (+165,+374) points from the window centre whatever the window size (921,865 in the 1512x982 window of INTEGRATION.md)
        awk -v s="$scale" -v vm="$vid_mode" -v d="$density" 'BEGIN{
            split(vm, wh, "x"); cx = wh[1] / d / 2; cy = wh[2] / d / 2
            printf "%.1f key Return\n",   5*s
            printf "%.1f move %d %d\n", 8*s, cx + 165, cy + 374
            printf "%.1f click %d %d\n", 9*s, cx + 165, cy + 374
            printf "%.1f key Space\n",   36*s
            printf "%.1f key Space\n",   48*s
        }'
    elif [ "$menu_only" = 0 ]; then
        echo "4 console start server(sweep_base/single/alife/load) client(localhost)"
    fi
    echo "$(( ${quit_at:-${quit_after:-$((timeout_s - 60))}} )) quit"
} > "$script"

# ---- environment ------------------------------------------------------------------------------------------------------
# the overlay is read from the checkout, not from the bundle, so an edit of gamedata/ (the driver) needs no relink
env_vars=(SDL_MAC_BACKGROUND_APP=1 SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_WINDOW_ACTIVATE_WHEN_RAISED=0 ALSOFT_DRIVERS=null
    CFFIXED_USER_HOME="$home" HOME="$home" ANOMALY_SWEEP="$plan" ANOMALY_DATA_ROOT="$data_root" ANOMALY_OVERLAY="$repo/gamedata")
if [ "$plain" = 0 ]; then
    env_vars+=(
        MallocNanoZone=0
        # container-overflow: the engine's own vectors are annotated, the system libc++'s are not (false positives);
        # halt_on_error=0: keep going after a report (the app is built with -fsanitize-recover), the reports are the product
        ASAN_OPTIONS="suppressions=$here/asan.supp:detect_stack_use_after_return=1:detect_leaks=0:detect_container_overflow=0:halt_on_error=0:strict_string_checks=1:check_initialization_order=0:malloc_context_size=24:quarantine_size_mb=128:abort_on_error=0:print_summary=1"
        UBSAN_OPTIONS="print_stacktrace=1:report_error_type=1:suppressions=$here/ubsan.supp"
        LSAN_OPTIONS="detect_leaks=0"
    )
fi

# windowed and small: user.ltx runs after the engine's defaults; the lines replace any earlier ones of ours
mkdir -p "$appdata"
touch "$appdata/user.ltx"
grep -av '^rs_screenmode \|^vid_mode ' "$appdata/user.ltx" > "$appdata/user.ltx.new"
printf 'rs_screenmode windowed\nvid_mode %s\n' "$vid_mode" >> "$appdata/user.ltx.new"
mv "$appdata/user.ltx.new" "$appdata/user.ltx"

log="$home/Library/Application Support/Anomaly/appdata/logs/xray_$(id -un).log"
shell_log="$home/Library/Logs/Anomaly/anomaly-shell.log"
rm -f "$log"

echo "plan $plan, out $out"
echo "$bin --data-root $data_root --script $script -dbg"
cd "$out" || exit 2
env "${env_vars[@]}" "$bin" --data-root "$data_root" --script "$script" -dbg > "$out/stderr.txt" 2>&1 &
pid=$!
trap 'kill -TERM "$pid" 2>/dev/null; sleep 3; kill -KILL "$pid" 2>/dev/null; exit 130' INT TERM HUP
t0=$(date +%s)
killed=""
while kill -0 "$pid" 2>/dev/null; do
    sleep 2
    if [ $(( $(date +%s) - t0 )) -ge "$timeout_s" ]; then
        echo "timeout after ${timeout_s}s, stopping the app"
        killed=timeout
        kill -TERM "$pid" 2>/dev/null; sleep 5; kill -KILL "$pid" 2>/dev/null
        break
    fi
done
wait "$pid" 2>/dev/null
status=$?
trap - INT TERM HUP
kill -0 "$pid" 2>/dev/null && { kill -KILL "$pid" 2>/dev/null; echo "the app was still up, killed"; }
dur=$(( $(date +%s) - t0 ))

cp "$log" "$out/engine.log" 2>/dev/null
cp "$shell_log" "$out/shell.log" 2>/dev/null
ls "$home/Library/Logs/Anomaly/" 2>/dev/null | grep -i crash | while read -r f; do cp "$home/Library/Logs/Anomaly/$f" "$out/"; done
cp "$appdata/logs/"*.crash.txt "$out/" 2>/dev/null

# ---- summary ------------------------------------------------------------------------------------------------------------
sum="$out/summary.txt"
{
    echo "plan $plan, exit status $status, ${dur}s ${killed:+($killed)}"
    echo
    echo "driver:"
    grep -a "\[sweep\]" "$out/engine.log" 2>/dev/null | sed 's/^.*\[sweep\] /  /' | tail -80
    echo
    python3 -I "$here/triage.py" "$out/stderr.txt" --ubsan-top 40
    echo
    echo "crash files: $(ls "$out"/*crash* 2>/dev/null | wc -l | tr -d ' ')"
    grep -a "^FATAL\|^\! \[LUA\]\|LUA error\|Expression\|Description" "$out/engine.log" 2>/dev/null | head -10
} > "$sum"
cat "$sum"
