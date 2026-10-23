#!/bin/zsh
# Pushes the test addons into the game folder the way the Files app would, runs the game and checks what the addons did.
#
#   tools/addons_test/run.sh [options] <command> [args]
#
# Commands
#   stage <scenario> [mode]     stage the scenario's files (addons.py) under <out>/stage/<scenario>; mode boot (default) or game
#   push <scenario> [mode]       stage, then copy into the game folder; files an earlier push put there and this scenario no
#                                longer has are removed (what deleting an addon in the Files app does)
#   run <scenario> <boot|game>   launch the game with timelines/<mode>.txt, wait for it, collect the logs into <out>/<scenario>-<mode>/
#   check <scenario> <boot|game> push + run + verify.py
#   sequence                     the whole test (below)
#   cleanup                      remove everything this tool pushed
#
# Scenarios (addons.py): installed, installed_again, case_variants, shader_v2, shader_v3, removed. `sequence` runs
#   removed(boot)          the baseline: stock behaviour, the stock shaders are translated and cached
#   installed(game)        every addon at once; the changed UI font shader is translated; MCM value written (42)
#   installed_again(boot)  a new process: everything still there, no shader translated, MCM value read back (42)
#   case_variants(boot)    plus files under GameData/ and DB/Addons/ next to gamedata/ and db/addons/ (two folders on a case-sensitive volume)
#   shader_v2(boot)        the shader's include changed: translated again
#   shader_v3(boot)        the shader itself changed: translated again
#   removed(boot)          the addons are gone: stock sizes everywhere, the stock shader comes from the cache again
#
# Options (before the command)
#   --target ipad|mac    ipad (default): the iPad over devicectl; mac: a data root on this Mac (--data-root)
#   --data-root <dir>    mac: the Anomaly folder to push into (its db/ and gamedata/); it is written to
#   --game-dir <dir>     the Anomaly folder with the stock archives, read by addons.py (default: game/Anomaly of the checkout)
#   --out <dir>          where staging, logs and results go (default: build/addons-test)
#   --device <udid|name> ipad: as for tools/ios/ipad.sh
#   --app <anomaly.app>  mac: the app to run (default: build/macos-arm64-release of the checkout)
#   --state <dir>        mac: state root of the runs (appdata, shader cache, logs); kept between runs (default <out>/mac-state)
set -euo pipefail
setopt null_glob

here=${0:A:h}
repo=${here:h:h}
target=ipad
out=$repo/build/addons-test
data_root=
game_dir=
device_args=()
app=$repo/build/macos-arm64-release/src/Platform/App/anomaly.app
state=

while [[ ${1:-} == --* ]]; do
    case $1 in
        --target) target=$2; shift 2 ;;
        --data-root) data_root=${2:A}; shift 2 ;;
        --game-dir) game_dir=${2:A}; shift 2 ;;
        --out) out=${2:A}; shift 2 ;;
        --device) device_args=(--device "$2"); shift 2 ;;
        --app) app=${2:A}; shift 2 ;;
        --state) state=${2:A}; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
cmd=${1:-}; [[ -n $cmd ]] || { sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
shift

die() { echo "error: $*" >&2; exit 1; }
[[ -n $state ]] || state=$out/mac-state
mkdir -p "$out"
stage_args=()
[[ -n $game_dir ]] && stage_args=(--game-dir "$game_dir")
pushed_list=$out/pushed-$target.txt

stage() {
    python3 -I "$here/addons.py" stage "$1" "$out/stage/$1" --mode "${2:-boot}" "${stage_args[@]}"
}

# ---- pushing ------------------------------------------------------------------------------------------------------------
# paths are relative to the Anomaly folder: gamedata/..., db/patches/..., db/addons/...
push() {
    local scenario=$1 mode=${2:-boot}
    stage "$scenario" "$mode"
    local manifest=$out/stage/$scenario/manifest.txt
    local -a old=() new=()
    [[ -f $pushed_list ]] && old=("${(@f)$(<$pushed_list)}")
    new=("${(@f)$(<$manifest)}")
    local -a gone=(${old:|new})
    case $target in
        mac)
            [[ -d $data_root/db ]] || die "--data-root $data_root has no db/"
            local f
            # manifest lines are `<tree>/<path in the game folder>`
            for f in $gone; do rm -f "$data_root/${f#*/}"; done
            # emptied folders of ours (case spelled as pushed) go too, but never gamedata/db themselves
            for f in $gone; do
                local d=${${f#*/}:h}
                while [[ $d != . && ${d:l} != gamedata && ${d:l} != db && ${d:l} != db/patches && ${d:l} != db/addons ]]; do
                    rmdir "$data_root/$d" 2>/dev/null || break
                    d=${d:h}
                done
            done
            for f in $new; do
                mkdir -p "$data_root/${${f#*/}:h}"
                cp -f "$out/stage/$scenario/$f" "$data_root/${f#*/}"
            done
            ;;
        ipad) ipad_push "$scenario" "$manifest" "${(@)gone}" ;;
    esac
    printf '%s\n' $new > "$pushed_list"
    echo "pushed $scenario: ${#new} files, ${#gone} removed ($target)"
}

# ---- iPad ---------------------------------------------------------------------------------------------------------------
bundle=com.andriishafar.anomaly
ipad_dc() { xcrun devicectl "$@"; }
ipad_device() {
    if [[ ${#device_args} -gt 0 ]]; then echo ${device_args[2]}; return; fi
    echo ${ANOMALY_IPAD_UDID:-}
}
ipad_sh() { "$repo/tools/ios/ipad.sh" "${device_args[@]}" "$@"; }

# copies the scenario's files one by one (devicectl creates the folders); what the scenario no longer has is removed after
ipad_push() {
    local scenario=$1 manifest=$2; shift 2
    local f
    for f in "${(@f)$(<$manifest)}"; do
        ipad_sh put "$out/stage/$scenario/$f" "Anomaly/${f#*/}" > /dev/null
    done
    (( $# )) && ipad_remove "$@"
    return 0
}

# Deletes files an earlier push put into the game folder. devicectl has no delete, and its `--remove-existing-content` wiped a whole app
# container once (never use it), so the game removes them itself: a generated script lists the files (paths as pushed, case included),
# deletes them with os.remove under $HOME/Documents/Anomaly, then the folders left empty (os.remove refuses a folder with anything in
# it, so only what is ours can go), and finally itself.
ipad_remove() {
    local lua=$out/remove/gamedata/Scripts/zz_addon_remove.script
    local -a files=() dirs=()
    local p d
    for p in "$@"; do
        p=${p#*/}                    # manifest line `<tree>/<path in the game folder>`
        files+=("$p")
        d=${p:h}
        while [[ $d != . && ${d:l} != gamedata && ${d:l} != db ]]; do dirs+=("$d"); d=${d:h}; done
    done
    dirs=("${(@u)dirs}")
    dirs=("${(@f)$(for d in $dirs; do print -r -- "${#d} $d"; done | sort -rn | sed 's/^[0-9]* //')}")   # longest first: a folder before its parent
    mkdir -p "${lua:h}"
    {
        echo '-- generated by tools/addons_test/run.sh: deletes what an earlier push put into the game folder, then itself'
        echo 'local root = os.getenv("HOME") .. "/Documents/Anomaly/"'
        echo 'local files = {'; for p in $files; do echo "  \"$p\","; done; echo '}'
        echo 'local dirs = {'; for d in $dirs; do echo "  \"$d\","; done; echo '}'
        cat <<'LUA'
function run()
	for _, p in ipairs(files) do
		local ok = os.remove(root .. p)
		printf("%s", "ADDON_REMOVE " .. p .. " " .. tostring(ok))
	end
	for _, d in ipairs(dirs) do
		os.remove(root .. d)
	end
	os.remove(root .. "gamedata/Scripts/zz_addon_remove.script")
	os.remove(root .. "gamedata/Scripts")
end
LUA
    } > "$lua"
    ipad_sh put "$lua" "Anomaly/gamedata/Scripts/zz_addon_remove.script" > /dev/null
    ipad_sh script "$here/timelines/remove.txt" -dbg > "$out/remove-console.txt" 2>&1 || true
}

# ---- running ------------------------------------------------------------------------------------------------------------
run() {
    local scenario=$1 mode=$2
    local timeline=$here/timelines/$mode.txt
    [[ -f $timeline ]] || die "no timeline $timeline"
    local res=$out/$scenario-$mode
    rm -rf "$res"; mkdir -p "$res"
    case $target in
        mac)
            [[ -d $app ]] || die "no app at $app"
            mkdir -p "$state/Application Support/Anomaly/appdata" "$state/Caches/Anomaly" "$state/Logs/Anomaly"
            # the shader cache starts as a copy of the player's (translating the whole set takes minutes) and then stays here
            if [[ ! -d $state/Caches/Anomaly/shaders && -d $HOME/Library/Caches/Anomaly/shaders ]]; then
                cp -R "$HOME/Library/Caches/Anomaly/shaders" "$state/Caches/Anomaly/shaders"
            fi
            rm -f "$state/Application Support/Anomaly/appdata/logs/"xray_*.log "$state/Logs/Anomaly/anomaly-shell.log"
            rm -rf "$state/Application Support/Anomaly/appdata/screenshots"
            local script=$res/run.script
            cp "$timeline" "$script"
            open -g -j -n --env ANOMALY_STATE_ROOT="$state" --stdout "$res/stdout.txt" --stderr "$res/stderr.txt" "$app" --args \
                --data-root "$data_root" --script "$script" --exit-after 300 -dbg "$@[3,-1]" || true
            local start=$SECONDS
            while pgrep -f "$script" >/dev/null; do
                sleep 1
                if (( SECONDS - start > 330 )); then echo "run took too long, killing"; pkill -9 -f "$script" || true; fi
            done
            cp "$state/Application Support/Anomaly/appdata/logs/"xray_*.log "$res/engine.log" 2>/dev/null || true
            cp "$state/Logs/Anomaly/anomaly-shell.log" "$res/shell.log" 2>/dev/null || true
            cp -R "$state/Application Support/Anomaly/appdata/screenshots" "$res/shots" 2>/dev/null || true
            ;;
        ipad)
            local copy=$res/ipad-logs
            ipad_sh script "$timeline" -dbg "$@[3,-1]" > "$res/console.txt" 2>&1 || true
            ipad_sh logs "$copy" > /dev/null 2>&1 || true
            cp "$copy"/engine-logs/logs/xray_*.log "$res/engine.log" 2>/dev/null || cp "$copy"/engine-logs/xray_*.log "$res/engine.log" 2>/dev/null || true
            cp "$copy/anomaly-shell.log" "$res/shell.log" 2>/dev/null || true
            ipad_sh get "Library/Application Support/Anomaly/appdata/screenshots" "$res/shots" > /dev/null 2>&1 || true
            ;;
    esac
    [[ -f $res/engine.log ]] || die "no engine log collected into $res"
    echo "logs: $res"
}

verify() {
    local scenario=$1 mode=$2; shift 2
    local res=$out/$scenario-$mode
    local -a flags=("$@")
    [[ $mode == game ]] && flags+=(--game)
    python3 -I "$here/verify.py" "$res/engine.log" "$scenario" --shell-log "$res/shell.log" "${flags[@]}"
}

check() {
    local scenario=$1 mode=$2; shift 2
    push "$scenario" "$mode"
    run "$scenario" "$mode"
    verify "$scenario" "$mode" "$@"
}

sequence() {
    local rc=0
    export ADDONS_TEST_NONCE=$(date +%s)   # new shader text for every sequence: the translation cache keeps what it has seen
    check removed boot || rc=1
    check installed game --translated some || rc=1
    check installed_again boot --translated none || rc=1
    check case_variants boot --translated none || rc=1
    check shader_v2 boot --translated some || rc=1
    check shader_v3 boot --translated some || rc=1
    check removed boot --translated none || rc=1
    return $rc
}

cleanup() {
    [[ -f $pushed_list ]] || { echo "nothing pushed"; return; }
    local -a left=("${(@f)$(<$pushed_list)}")
    (( ${#left} )) || { echo "nothing pushed"; return; }
    case $target in
        mac)
            local f d
            for f in $left; do
                rm -f "$data_root/${f#*/}"
                d=${${f#*/}:h}
                while [[ $d != . && ${d:l} != gamedata && ${d:l} != db ]]; do rmdir "$data_root/$d" 2>/dev/null || break; d=${d:h}; done
            done
            ;;
        ipad) ipad_remove "${(@)left}" ;;
    esac
    : > "$pushed_list"
    echo "cleaned: ${#left} files ($target)"
}

case $cmd in
    stage) stage "$@" ;;
    push) push "$@" ;;
    run) run "$@" ;;
    check) check "$@" ;;
    sequence) sequence ;;
    cleanup) cleanup ;;
    *) die "unknown command $cmd" ;;
esac
