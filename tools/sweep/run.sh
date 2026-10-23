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
#           die_reload   die in the Cordon, then load a save from the dead state (what "Load" on the game-over screen does)
#           quit_inventory quit_pda quit_dialog quit_combat quit_dead quit_surge
#                        reach the state on sweep_base and quit from it; quit_menu (script quit in the main menu) and
#                        quit_loading (quit while the new game loads) need no save
#   options --app <anomaly.app> the app bundle (default build/macos-arm64-asan/src/Platform/App/anomaly.app)
#           --data-root <dir>   Anomaly game data (default $ANOMALY_DATA_ROOT, else <repo>/game/Anomaly, else the main checkout's)
#           --scale <n>         multiply every time of the menu script (default 1; 2 or 3 on a loaded machine)
#           --timeout <s>       hard limit of the run (default 1500): --exit-after, then the watchdog kills the app
#           --out <dir>         where the run's files go (default build/sweep-runs/<time>-<plan>)
#           --state <dir>       the app's ANOMALY_STATE_ROOT (default build/sweep-state; keeps sweep_base between runs)
#           --plain             no sanitizer environment (for a release build)
#           --script-file <f>   use this --script timeline instead of the generated one (menu path and quit included)
#           SWEEP_ASAN_EXTRA=<options> in the environment adds to ASAN_OPTIONS (e.g. quarantine_size_mb=3072 to keep freed blocks longer)
#           --size WxH          window size in pixels (default 1280x720, the unattended default), --density <n> the display scale (2)
#
# The app is started through LaunchServices (`open -g -j -W -n`), hidden and in the background: a direct run of the bundle's
# binary steals the focus for a moment. The app's own unattended mode (implied by --script/--exit-after) keeps it windowed,
# silent and without a Space. ANOMALY_STATE_ROOT gives the run its own appdata, caches and logs, so a sweep never rewrites
# anyone else's log or save; the first run in a new state dir translates the shaders again (symlink Caches/Anomaly/shaders
# to another state dir's to skip that).
set -u

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

app="$repo/build/macos-arm64-asan/src/Platform/App/anomaly.app"
[ -d "$app" ] || app="$repo/build/macos-asan/src/Platform/App/anomaly.app"
data_root="${ANOMALY_DATA_ROOT:-}"
scale=1
timeout_s=1500
out=""
state=""
plain=0
plan=""
script_file=""
size="1280x720"
density=2

while [ $# -gt 0 ]; do
    case "$1" in
        --app) app="$2"; shift 2 ;;
        --data-root) data_root="$2"; shift 2 ;;
        --scale) scale="$2"; shift 2 ;;
        --timeout) timeout_s="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --state) state="$2"; shift 2 ;;
        --plain) plain=1; shift ;;
        --script-file) script_file="$2"; shift 2 ;;
        --size) size="$2"; shift 2 ;;
        --density) density="$2"; shift 2 ;;
        -h|--help) sed -n 2,27p "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) plan="$1"; shift ;;
    esac
done
[ -n "$plan" ] || { sed -n 2,27p "$0"; exit 2; }
[ -d "$app" ] || { echo "no app at $app (build it: cmake --preset macos-arm64-asan && cmake --build --preset macos-arm64-asan --target anomaly)" >&2; exit 2; }

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

state="${state:-$repo/build/sweep-state}"
mkdir -p "$state"
state="$(cd "$state" && pwd)"
stamp="$(date +%Y%m%d-%H%M%S)"
[ -n "$out" ] || out="$repo/build/sweep-runs/$stamp-$plan"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

appdata="$state/Application Support/Anomaly/appdata"
log_dir="$appdata/logs"
shell_log_dir="$state/Logs/Anomaly"
base_save="$appdata/savedgames/sweep_base.scop"

# ---- the --script timeline ------------------------------------------------------------------------------------------
# t is seconds since the engine is up. The menu path is the one INTEGRATION.md documents; the plan's own waits are inside the
# Lua driver, which follows the game, not the clock.
script="$out/run.script"
start_new_game=0
seed_faction=""
menu_only=0
quit_after=""
case "$plan" in
    quit_menu) quit_after=8; menu_only=1 ;;
    quit_loading) start_new_game=1; quit_after=$(awk -v s="$scale" 'BEGIN{printf "%d", 22*s}') ;; # just after Start: the loading screen
    newgame) start_new_game=1 ;;
    chk_new_*) seed_faction="${plan#chk_new_}" ;; # systems check: a new game as another faction, started from the console
    smoke) [ -f "$base_save" ] || start_new_game=1 ;;
    *) [ -f "$base_save" ] || { echo "no sweep_base save in $appdata/savedgames: run 'tools/sweep/run.sh newgame' first" >&2; exit 2; } ;;
esac
hard_stop=$(( timeout_s - 60 ))
{
    echo "# generated by tools/sweep/run.sh: plan $plan, scale $scale"
    if [ "$start_new_game" = 1 ]; then
        # In a dialog the engine turns the pointer's deltas (points) into UI units (1024x768) by 1024/width and 768/height, and
        # the UI cursor starts at the middle of that space. The Start button is at UI (+111.7,+292.5) from there, which was
        # (+165,+374) points in the 1512x982 window of INTEGRATION.md: 0.10913 of the width and 0.38086 of the height.
        awk -v s="$scale" -v sz="$size" -v d="$density" 'BEGIN{
            split(sz, wh, "x"); w = wh[1] / d; h = wh[2] / d; cx = w / 2; cy = h / 2
            x = cx + 0.10913 * w; y = cy + 0.38086 * h
            printf "%.1f key Return\n",   5*s
            printf "%.1f move %d %d\n", 8*s, x, y
            printf "%.1f click %d %d\n", 9*s, x, y
            printf "%.1f key Space\n",   36*s
            printf "%.1f key Space\n",   48*s
        }'
        # a cold shader cache makes the loading take minutes, so the "press any key" of its end cannot be on a fixed time:
        # keep tapping (Left Shift is the least harmful key once the game runs)
        awk -v s="$scale" -v end="${quit_after:-$hard_stop}" 'BEGIN{ for (t = 60; t < end / s; t += 15) printf "%.1f key Left Shift\n", t*s }'
    elif [ -n "$seed_faction" ]; then
        # the faction dialog only writes [character_creation] of axr_options.ltx and starts the game; do the same from the menu's Lua
        echo "4 console run_string sweep_check_play.seed(\"$seed_faction\")"
        echo "5 console start server(all/single/alife/new) client(localhost)"
        awk -v s="$scale" -v end="$hard_stop" 'BEGIN{ for (t = 20; t < end / s; t += 15) printf "%.1f key Left Shift\n", t*s }'
    elif [ "$plan" = chk_menu ]; then
        # the main menu stops the level (and the driver with it), so this plan is driven from the timeline: load the base save, wait for
        # the level, then open the menu, look at it from Lua (run_string), close it
        echo "4 console start server(sweep_base/single/alife/load) client(localhost)"
        awk -v s="$scale" 'BEGIN{ for (t = 20; t < 80; t += 15) printf "%.1f key Left Shift\n", t*s }'
        echo "90 waitlevel 300 l01_escape"
        echo "100 console main_menu on"
        echo "102 console run_string sweep_check_ui.menu_verify(\"on\")"
        echo "104 console screenshot chk_menu_main"
        echo "108 console main_menu off"
        echo "111 console run_string sweep_check_ui.menu_verify(\"off\")"
    elif [ "$menu_only" = 0 ]; then
        echo "4 console start server(sweep_base/single/alife/load) client(localhost)"
        # every loading screen (also the one of a reload) waits for "press any key" and the game does not run until then; a tap of Left Shift (sprint) is the least harmful key
        awk -v s="$scale" -v end="$hard_stop" 'BEGIN{ for (t = 20; t < end / s; t += 15) printf "%.1f key Left Shift\n", t*s }'
    fi
    echo "${quit_after:-$hard_stop} quit"
} > "$script"
[ -z "$script_file" ] || cp "$script_file" "$script"

# ---- environment ------------------------------------------------------------------------------------------------------
# the overlay is read from the checkout, not from the bundle, so an edit of gamedata/ (the driver) needs no relink
env_args=(--env "ANOMALY_STATE_ROOT=$state" --env "ANOMALY_SWEEP=$plan" --env "ANOMALY_OVERLAY=$repo/gamedata")
# settings of a plan (the soak's length and mode), passed through to the driver's os.getenv
for v in SWEEP_MINUTES SWEEP_SOAK_MODE; do
    [ -z "${!v:-}" ] || env_args+=(--env "$v=${!v}")
done
# SWEEP_ENV="NAME=value NAME=value": more environment for the app (e.g. MallocStackLogging=lite to find who allocates)
for kv in ${SWEEP_ENV:-}; do env_args+=(--env "$kv"); done
if [ "$plain" = 0 ]; then
    # container-overflow: the engine's own vectors are annotated, the system libc++'s are not (false positives);
    # halt_on_error=0: keep going after a report (the app is built with -fsanitize-recover), the reports are the product
    env_args+=(
        --env "MallocNanoZone=0"
        --env "ASAN_OPTIONS=suppressions=$here/asan.supp:detect_stack_use_after_return=1:detect_leaks=0:detect_container_overflow=0:halt_on_error=0:strict_string_checks=1:check_initialization_order=0:malloc_context_size=24:quarantine_size_mb=512:allow_user_segv_handler=0:handle_sigbus=1:abort_on_error=0:print_summary=1${SWEEP_ASAN_EXTRA:+:$SWEEP_ASAN_EXTRA}"
        --env "UBSAN_OPTIONS=print_stacktrace=1:report_error_type=1:suppressions=$here/ubsan.supp"
    )
fi

rm -f "$appdata/sweep_state.txt" "$appdata"/chk_soak.txt "$appdata"/chk_snapshot.txt "$appdata"/chk_companion.txt # a plan starts at its first phase
rm -f "$log_dir"/xray_*.log "$shell_log_dir"/anomaly-crash.log "$log_dir"/*.crash.txt 2>/dev/null

echo "plan $plan, out $out"
cd "$out" || exit 2
open -g -j -W -n "${env_args[@]}" --stdout "$out/stdout.txt" --stderr "$out/stderr.txt" "$app" \
    --args --data-root "$data_root" --script "$script" --exit-after "$hard_stop" --size "$size" -dbg &
opid=$!
# the watchdog: `open -W` does not end by itself if the app hangs; find the app by its unique --script argument
stop_app() { pkill -TERM -f -- "--script $script" 2>/dev/null; sleep 5; pkill -KILL -f -- "--script $script" 2>/dev/null; }
trap 'stop_app; kill "$opid" 2>/dev/null; exit 130' INT TERM HUP
t0=$(date +%s)
killed=""
while kill -0 "$opid" 2>/dev/null; do
    sleep 2
    if [ $(( $(date +%s) - t0 )) -ge "$timeout_s" ]; then
        echo "timeout after ${timeout_s}s, stopping the app"
        killed=timeout
        stop_app
        break
    fi
done
wait "$opid" 2>/dev/null
status=$?
trap - INT TERM HUP
pgrep -f -- "--script $script" >/dev/null 2>&1 && { stop_app; echo "the app was still up, killed"; }
dur=$(( $(date +%s) - t0 ))

cp "$log_dir"/xray_*.log "$out/engine.log" 2>/dev/null
cp "$shell_log_dir/anomaly-shell.log" "$out/shell.log" 2>/dev/null
cp "$shell_log_dir"/anomaly-crash.log "$log_dir"/*.crash.txt "$out/" 2>/dev/null

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
