#!/bin/bash
# The systems check in one command: every plan of gamedata/scripts/sweep_check*.script, one after the other, under the sanitizers when the
# app is the ASan build, then one report. See docs/ios-port/SYSTEMS-CHECK.md for what each plan asserts.
#
#   tools/sweep/check.sh [options] [plan ...]
#
#   options --list            print the plans and exit
#           --soak [minutes]  add the soak (default 60 minutes) at the end
#           --all             also run the older crash-sweep plans (smoke, combat, ..., quit_*)
#           --out <dir>       where the runs and the report go (default build/check-runs/<time>)
#           anything else     goes to tools/sweep/run.sh: --app <anomaly.app> --plain --state <dir> --scale <n> --timeout <s> ...
#   plan    run only these plans (names as in --list)
#
# The first plan, newgame, starts a new game from the menu (the UI path) and saves sweep_base, which the others load. Without plan names the
# whole list runs; with names only those do (newgame first if the save is missing). The exit status is 1 when anything failed.
# Mac runs only: every run is unattended (no window, no sound, no focus), one game at a time.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

CORE_PLANS="newgame chk_new_dolg chk_new_freedom chk_new_ecolog chk_menu chk_hud chk_inventory chk_survival chk_weapons chk_equipment chk_mechanics chk_pda chk_tasks chk_stash chk_saveload chk_alife chk_mutants chk_stalkers chk_anomalies chk_weather chk_sleep chk_sound chk_fasttravel chk_companions chk_travel chk_emission_cover chk_emission_open chk_psistorm_cover"
OLD_PLANS="smoke combat inventory pda trade surge anomalies sleep transition saveload die_reload quit_menu quit_loading quit_level quit_inventory quit_pda quit_dialog quit_combat quit_dead quit_surge"

run_args=()
plans=()
soak=0
soak_min=60
all=0
out=""
state="$repo/build/sweep-state"
while [ $# -gt 0 ]; do
    case "$1" in
        --list) echo "core: $CORE_PLANS"; echo "crash sweep (--all): $OLD_PLANS"; echo "soak (--soak): chk_soak"; exit 0 ;;
        --soak) soak=1; if [ $# -gt 1 ] && [[ "$2" =~ ^[0-9]+$ ]]; then soak_min="$2"; shift; fi; shift ;;
        --all) all=1; shift ;;
        --out) out="$2"; shift 2 ;;
        --state) state="$2"; run_args+=(--state "$2"); shift 2 ;;
        --app|--data-root|--scale|--timeout|--size|--density) run_args+=("$1" "$2"); shift 2 ;;
        --plain) run_args+=(--plain); shift ;;
        -h|--help) sed -n 2,17p "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) plans+=("$1"); shift ;;
    esac
done

if [ ${#plans[@]} -eq 0 ]; then
    plans=($CORE_PLANS)
    [ "$all" = 1 ] && plans+=($OLD_PLANS)
    [ "$soak" = 1 ] && plans+=(chk_soak)
fi
[ -d "$state" ] || mkdir -p "$state"
base="$state/Application Support/Anomaly/appdata/savedgames/sweep_base.scop"
if [ ! -f "$base" ] && [ "${plans[0]}" != newgame ]; then
    plans=(newgame "${plans[@]}")
fi

stamp="$(date +%Y%m%d-%H%M%S)"
[ -n "$out" ] || out="$repo/build/check-runs/$stamp"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
echo "systems check: ${#plans[@]} plans, results in $out"

dirs=()
for plan in "${plans[@]}"; do
    # plan-specific limits (seconds): the emission and psi storm run their full 222 and 254 s, the chain loads six levels
    case "$plan" in
        chk_soak) limit=$(( soak_min * 60 + 900 )) ;;
        chk_travel|chk_emission_*|chk_psistorm_*|smoke) limit=1800 ;;
        newgame|chk_new_*) limit=1500 ;;
        *) limit=1200 ;;
    esac
    extra=()
    printf '%s\n' "${run_args[@]:-}" | grep -q -- '--timeout' || extra=(--timeout "$limit")
    echo "== $plan ($(date +%H:%M:%S))"
    if [ "$plan" = chk_soak ]; then
        SWEEP_MINUTES="$soak_min" nice -n 10 "$here/run.sh" "${run_args[@]}" "${extra[@]}" --out "$out/$plan" "$plan" 2>&1 | grep -a "exit status\|CHECK FAIL\|timeout" | cut -c1-240
    else
        nice -n 10 "$here/run.sh" "${run_args[@]}" "${extra[@]}" --out "$out/$plan" "$plan" 2>&1 | grep -a "exit status\|CHECK FAIL\|timeout" | cut -c1-240
    fi
    dirs+=("$out/$plan")
done

python3 -I "$here/check_report.py" --out "$out" "${dirs[@]}"
status=$?
echo "report: $out/results.md"
exit $status
