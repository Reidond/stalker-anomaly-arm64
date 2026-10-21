#!/bin/zsh
# Copies Anomaly 1.5.3 game data into the app's Documents/Anomaly on a connected iPad.
#
#   tools/ios/push_gamedata.sh [options]
#
# Steps:
#   1. Reads the iPad's free storage (launches shell_lab with --print-report; it must be installed).
#   2. Copies a small subset of real archives and measures throughput.
#   3. Copies everything else, skipping the Windows-only bin/ and tools/ (and *.exe), but only if
#      the iPad has more than --min-free-gb free. Files already on the device are skipped, so an
#      interrupted run can simply be started again.
#   4. Launches shell_lab again to validate the data on the device and prints the timings.
#
# Options:
#   --device <udid|name>   target device (default: $ANOMALY_IPAD_UDID, else the first connected iPad)
#   --source <dir>         Anomaly folder to copy (default: <repo>/game/Anomaly, or the main checkout's)
#   --bundle <id>          app whose Documents receives the data (default: com.andriishafar.anomaly)
#   --min-free-gb <n>      refuse the full copy below this much free space, in GiB (default: 40)
#   --subset-only          stop after the throughput measurement
#   --check-only           only report the iPad's free storage and the data status, copy nothing
#   --dry-run              print what would be copied
#   -h, --help             this text
set -euo pipefail
zmodload zsh/datetime

repo=${0:A:h:h:h}
main=${$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir):h}

device=${ANOMALY_IPAD_UDID:-}
src=$repo/game/Anomaly
bundle=com.andriishafar.anomaly
min_free_gb=40
subset_only=0 check_only=0 dry_run=0
dest_root=Documents/Anomaly

# The subset: tiny config archives, the file/shader lists and two mid-size archives (~265 MB in all).
# They are real game files, so the full copy later skips them.
subset=(
    db/configs/ai.db0 db/configs/configs.db0 db/configs/scripts.db0 db/configs/spawns.db0
    db/files.db0 db/shaders.db0
    db/meshes/meshes.db0 db/sounds/sounds_ambient.db0
)

usage() { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; }

while (( $# )); do
    case $1 in
        --device) device=$2; shift 2 ;;
        --source) src=$2; shift 2 ;;
        --bundle) bundle=$2; shift 2 ;;
        --min-free-gb) min_free_gb=$2; shift 2 ;;
        --subset-only) subset_only=1; shift ;;
        --check-only) check_only=1; shift ;;
        --dry-run) dry_run=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# game/ is gitignored, so a linked worktree has none: fall back to the main checkout's copy.
[[ -d $src/db ]] || src=$main/game/Anomaly
[[ -d $src/db ]] || { echo "error: no Anomaly data at $src (pass --source)" >&2; exit 1; }

die() { echo "error: $*" >&2; exit 1; }
gb() { printf '%.2f GiB' "$(( $1 / 1073741824.0 ))"; }
mb() { printf '%.0f MiB' "$(( $1 / 1048576.0 ))"; }
size_of() { stat -f%z "$1"; }

# ---- device ----------------------------------------------------------------------------------
if [[ -z $device ]]; then
    device=$(xcrun devicectl list devices --json-output - 2>/dev/null | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
for dev in d["result"]["devices"]:
    hw = dev.get("hardwareProperties", {})
    cp = dev.get("connectionProperties", {})
    if hw.get("deviceType") == "iPad" and cp.get("tunnelState") == "connected":
        print(hw.get("udid") or dev["identifier"]); break
')
    [[ -n $device ]] || die "no connected iPad found (connect it, unlock it, or pass --device)"
fi
echo "device: $device"

# Launches shell_lab headless and returns its ANOMALY_REPORT line.
report() {
    local out
    out=$(xcrun devicectl device process launch --device "$device" --terminate-existing --console "$bundle" \
        --print-report 2>&1) || true
    local line
    line=$(print -r -- "$out" | grep -m1 '^ANOMALY_REPORT' || true)
    [[ -n $line ]] || { print -r -- "$out" | tail -15 >&2
        die "could not read the report from $bundle: is shell_lab installed, and is the iPad unlocked?" }
    print -r -- "$line"
}
field() { print -r -- "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }

# ---- 1. free storage -------------------------------------------------------------------------
echo "\n[1/4] iPad storage"
rep=$(report)
free=$(field "$rep" storage_free); total=$(field "$rep" storage_total); important=$(field "$rep" storage_free_important)
echo "  model: $(field "$rep" model), $(field "$rep" os)"
echo "  free: $(gb $free) of $(gb $total)  ($(gb $important) including purgeable space)"
echo "  data status on the device: $(field "$rep" data) ($(field "$rep" files_ok)/$(field "$rep" files_expected) archives, $(gb $(field "$rep" bytes_present)))"
if (( check_only )); then exit 0; fi

# What is left to copy, in bytes (files that already match are skipped by devicectl).
need=0
while IFS= read -r -d '' f; do need=$(( need + $(size_of "$f") )); done < <(
    find "$src" -type f ! -path "$src/bin/*" ! -path "$src/tools/*" ! -name '*.exe' ! -name '.*' -print0)
echo "  to copy (everything except bin/, tools/, *.exe): $(gb $need)"

# ---- copy helpers ----------------------------------------------------------------------------
# What is already on the device: relative path -> size in bytes. Files with the right size are not
# copied again, which makes a re-run after an interruption fast.
typeset -A have
load_have() {
    have=()
    local size rel
    while IFS=$'\t' read -r size rel; do have[$rel]=$size; done < <(
        xcrun devicectl device info files --device "$device" --domain-type appDataContainer --domain-identifier "$bundle" \
            --subdirectory "$dest_root" -j - 2>/dev/null | python3 -I -c '
import json, sys
try:
    files = json.load(sys.stdin)["result"]["files"]
except Exception:
    files = []
for f in files:
    if not f["resources"].get("isDirectory"):
        print("%d\t%s" % (f["metadata"]["size"], f["relativePath"]))
')
}

# copy_file <path relative to the source>: copies one file, keeping its folder structure.
# Sets copied=1 if it was transferred, 0 if the device already had a file of the same size.
copy_file() {
    local rel=$1
    copied=0
    [[ ${have[$rel]-} == $(size_of "$src/$rel") ]] && return 0
    if (( dry_run )); then echo "  would copy $rel"; return 0; fi
    xcrun devicectl device copy to --device "$device" --domain-type appDataContainer --domain-identifier "$bundle" \
        --source "$src/$rel" --destination "$dest_root/$rel" --quiet >/dev/null
    copied=1
}

# ---- 2. throughput subset --------------------------------------------------------------------
echo "\n[2/4] Throughput subset"
load_have
subset_bytes=0 subset_copied=0 t0=$EPOCHREALTIME
for rel in $subset; do
    [[ -f $src/$rel ]] || die "subset file missing in the source: $rel"
    copy_file "$rel"
    if (( copied )); then subset_bytes=$(( subset_bytes + $(size_of "$src/$rel") )); subset_copied=$(( subset_copied + 1 )); fi
done
subset_secs=$(( EPOCHREALTIME - t0 ))
subset_rate=0
if (( subset_copied )); then
    subset_rate=$(( subset_secs > 0 ? subset_bytes / 1048576.0 / subset_secs : 0 ))
    printf '  %d files, %s in %.1f s = %.1f MiB/s (including per-file overhead)\n' $subset_copied "$(mb $subset_bytes)" $subset_secs $subset_rate
    # Only count what is still missing, so a resumed run gives a sensible estimate.
    remaining=$(( need - subset_bytes ))
    printf '  estimated time for the rest: about %.0f min\n' $(( remaining / 1048576.0 / subset_rate / 60 ))
else
    echo "  all ${#subset} subset files are already on the device, nothing to time (re-run on a fresh container to measure)"
fi
(( subset_only )) && { echo "\n--subset-only: done."; exit 0; }

# ---- 3. full copy ----------------------------------------------------------------------------
echo "\n[3/4] Full copy"
min_free=$(( min_free_gb * 1073741824 ))
if (( free <= min_free )); then
    echo "  skipped: only $(gb $free) free on the iPad, the full copy needs more than ${min_free_gb} GB."
    echo "  Free up space on the iPad and run this script again; the subset above is already in place."
    exit 3
fi

total_secs=0 total_bytes=0
# Top-level entries in a fixed order: the small ones first so problems show up early.
entries=(fsgame.ltx default-44100.mhr default-48000.mhr gamedata db/configs db/files.db0 db/shaders.db0 db/meshes db/sounds db/textures db/levels)
for e in $entries; do
    [[ -e $src/$e ]] || continue
    t0=$EPOCHREALTIME
    bytes=0 skipped=0 nfiles=0
    files=("${(@f)$(cd "$src" && find "$e" -type f ! -name '.*' | LC_ALL=C sort)}")
    for rel in $files; do
        [[ -n $rel ]] || continue
        nfiles=$(( nfiles + 1 ))
        copy_file "$rel"
        if (( copied )); then bytes=$(( bytes + $(size_of "$src/$rel") )); else skipped=$(( skipped + 1 )); fi
    done
    secs=$(( EPOCHREALTIME - t0 ))
    total_secs=$(( total_secs + secs )); total_bytes=$(( total_bytes + bytes ))
    if (( bytes > 0 )); then
        printf '  %-20s %3d files, %3d copied %10s in %7.1f s = %.1f MiB/s\n' "$e" $nfiles $(( nfiles - skipped )) "$(mb $bytes)" $secs $(( bytes / 1048576.0 / secs ))
    else
        printf '  %-20s %3d files, already on the device\n' "$e" $nfiles
    fi
done

# ---- 4. verify -------------------------------------------------------------------------------
echo "\n[4/4] Verify on the device"
rep=$(report)
echo "  data status: $(field "$rep" data) ($(field "$rep" files_ok)/$(field "$rep" files_expected) archives, $(gb $(field "$rep" bytes_present)))"
echo "  iPad free storage now: $(gb $(field "$rep" storage_free))"
printf '\nTimings: subset %s; full pass copied %s in %.0f s (%.1f MiB/s).\n' \
    "$( (( subset_copied )) && printf '%.1f MiB/s' $subset_rate || echo 'not measured')" "$(gb $total_bytes)" $total_secs $(( total_secs > 0 ? total_bytes / 1048576.0 / total_secs : 0 ))
[[ $(field "$rep" data) == ok ]] || { echo "data is not complete yet: run the script again."; exit 4; }
