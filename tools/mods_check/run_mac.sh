#!/bin/bash
# An unattended Mac game run that shows mods taking effect: two generated mods (a script, a config, an override of a file that two mods
# both provide) are mounted from <state>/Mods, the engine runs a scripted timeline in the main menu, and the engine log is checked for
# what the mods' script and config printed. See docs/ios-port/MOD-MANAGER.md, "Mounting".
#
#   tools/mods_check/run_mac.sh [options] [scenario]
#
#   scenarios  ab (default)   mods a, b enabled in that order: b's modtest.ltx wins, both scripts run
#              ba             the order swapped: a's modtest.ltx wins
#              b_off          b disabled
#              mods_off       both enabled, but --mods-off
#              bad_json       an invalid modlist.json: the game starts with no mods
#   options    --app <anomaly.app>   default build/macos-arm64-release/src/Platform/App/anomaly.app
#              --data-root <dir>     the Anomaly game data (default: <main checkout>/game/Anomaly); it is never written to: the run
#                                    uses a scratch data root of symlinks, because the game writes MCM's axr_options.ltx into gamedata/
#              --out <dir>           where the run's files go (default build/mods-check/<scenario>)
#              --lua-cache           run with -lua_cache (the bytecode cache must follow the mods)
#              --keep                keep the state of an earlier run in --out (its caches), only the mods and the timeline are made again;
#                                    MODTEST_VERSION=<text> in the environment is appended to what mod a's script logs (a changed script)
#
# The run is unattended (--script implies it): no window, no focus, no sound; its own ANOMALY_STATE_ROOT, so nothing of the player's is
# touched. One run at a time.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
app="$repo/build/macos-arm64-release/src/Platform/App/anomaly.app"
data_root=""
out=""
scenario="ab"
lua_cache=0
keep=0
while [ $# -gt 0 ]; do
    case "$1" in
        --app) app="$2"; shift 2 ;;
        --data-root) data_root="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --lua-cache) lua_cache=1; shift ;;
        --keep) keep=1; shift ;;
        -h|--help) sed -n 2,22p "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) scenario="$1"; shift ;;
    esac
done
[ -d "$app" ] || { echo "no app at $app" >&2; exit 2; }
if [ -z "$data_root" ]; then
    main="$(git -C "$repo" worktree list --porcelain | awk '/^worktree /{print $2; exit}')"
    data_root="$main/game/Anomaly"
fi
[ -d "$data_root/db" ] || { echo "no game data at $data_root (--data-root)" >&2; exit 2; }
[ -n "$out" ] || out="$repo/build/mods-check/$scenario"
if [ "$keep" = 1 ]; then rm -rf "$out/game" "$out/state/Mods" "$out/stdout.txt" "$out/stderr.txt"; else rm -rf "$out"; fi
mkdir -p "$out"
out="$(cd "$out" && pwd)"

# the scratch game folder: the archives are the real ones, gamedata/ is empty and writable
scratch="$out/game"
mkdir -p "$scratch/gamedata"
for entry in "$data_root"/*; do
    name="$(basename "$entry")"
    [ "$name" = gamedata ] && continue
    ln -s "$entry" "$scratch/$name"
done

# a probe script in the game folder's own gamedata (a manual install, which has to keep working): it reads what the mods provide, and
# says so when they provide nothing
mkdir -p "$scratch/gamedata/scripts"
cat > "$scratch/gamedata/scripts/modtest_probe.script" <<LUA
function ini_value()
    if getFS():exist("\$game_config\$", "modtest.ltx") then
        local i = ini_file("modtest.ltx")
        log("MODTEST ini value=" .. i:r_string("test", "value") .. " who=" .. i:r_string("test", "who"))
    else
        log("MODTEST ini absent")
    end
end

function stock_override()
    log("MODTEST stock file replaced by mod b: " .. tostring(ini_file("default_custom_data.ltx"):section_exist("mod_b_override")))
end
LUA

state="$out/state"
mods="$state/Mods"
mkdir -p "$mods"

# ---- the mods ------------------------------------------------------------------------------------------------------------
make_mod() { # id value
    local id="$1" value="$2" dir="$mods/$1/content/gamedata"
    mkdir -p "$dir/scripts" "$dir/configs"
    printf '[test]\nvalue = %s\nwho = mod_%s\n' "$value" "$id" > "$dir/configs/modtest.ltx"
    cat > "$dir/scripts/modtest_$id.script" <<LUA
function run()
    log("MODTEST script of mod $id ran${MODTEST_VERSION:-}")
end

-- reads a data file of the mod through io.open, the way a script reads its own file in gamedata/configs
function read_data()
    local f = io.open(getFS():update_path("\$game_config\$", "") .. "modtest_data.txt", "r")
    log("MODTEST io.open " .. (f and f:read("*l") or "nothing"))
    if f then f:close() end
end

-- the same with forward slashes in the path
function read_data_slash()
    local f = io.open((getFS():update_path("\$game_config\$", "") .. "modtest_data.txt"):gsub("\\\\", "/"), "r")
    log("MODTEST io.open slash " .. (f and f:read("*l") or "nothing"))
    if f then f:close() end
end
LUA
    printf '{"schema":1,"id":"%s","name":"Mod %s","version":"1","file_count":2}\n' "$id" "$id" > "$mods/$id/mod.json"
}
make_mod a from_a
make_mod b from_b
# a data file the mod's script reads with io.open, and a file of the game itself (configs.db0) that mod b replaces
printf 'data of mod b\n' > "$mods/b/content/gamedata/configs/modtest_data.txt"
printf '[mod_b_override]\nvalue = 1\n' > "$mods/b/content/gamedata/configs/default_custom_data.ltx"

# MODTEST_SHADER_INCLUDE=<file>: mod b also provides shaders/r3/common.h with that content (an include of the renderer's shaders, which the
# translation cache has to notice has changed)
if [ -n "${MODTEST_SHADER_INCLUDE:-}" ]; then
    mkdir -p "$mods/b/content/gamedata/shaders/r3"
    cp "$MODTEST_SHADER_INCLUDE" "$mods/b/content/gamedata/shaders/r3/common.h"
fi

mods_off=""
case "$scenario" in
    ab) echo '{"schema":1,"mods":[{"id":"a","enabled":true},{"id":"b","enabled":true}]}' > "$mods/modlist.json" ;;
    ba) echo '{"schema":1,"mods":[{"id":"b","enabled":true},{"id":"a","enabled":true}]}' > "$mods/modlist.json" ;;
    b_off) echo '{"schema":1,"mods":[{"id":"a","enabled":true},{"id":"b","enabled":false}]}' > "$mods/modlist.json" ;;
    mods_off) echo '{"schema":1,"mods":[{"id":"a","enabled":true},{"id":"b","enabled":true}]}' > "$mods/modlist.json"; mods_off="--mods-off" ;;
    bad_json) echo '{"schema":1,"mods":[{"id":"a","enabled":tr' > "$mods/modlist.json" ;;
    *) echo "unknown scenario $scenario" >&2; exit 2 ;;
esac

# ---- the timeline -------------------------------------------------------------------------------------------------------
# run_string executes Lua in the script engine (needs -dbg); the console lower-cases what it is given, so the mod's own script does the work
# that needs capitals. The menu has loaded the scripts by then
cat > "$out/run.script" <<'EOF'
# generated by tools/mods_check/run_mac.sh
6 console run_string modtest_probe.ini_value()
6.5 console run_string modtest_probe.stock_override()
7 console run_string if modtest_a then modtest_a.run() else log("MODTEST script of mod a missing") end
8 console run_string if modtest_b then modtest_b.run() else log("MODTEST script of mod b missing") end
9 console run_string if modtest_b then modtest_b.read_data() else log("MODTEST io.open nothing: mod b missing") end
9.5 console lua_cache info
9.2 console run_string if modtest_b then modtest_b.read_data_slash() else log("MODTEST io.open slash nothing: mod b missing") end
10 quit
EOF

# ---- the run ------------------------------------------------------------------------------------------------------------
extra=()
[ "$lua_cache" = 1 ] && extra+=(-lua_cache)
open -g -j -W -n --env "ANOMALY_STATE_ROOT=$state" --stdout "$out/stdout.txt" --stderr "$out/stderr.txt" "$app" --args \
    --data-root "$scratch" --script "$out/run.script" --exit-after 120 -dbg $mods_off "${extra[@]+"${extra[@]}"}"
echo "exit status $?"

shell_log="$state/Logs/Anomaly/anomaly-shell.log"
engine_log="$(ls -t "$state/Application Support/Anomaly/appdata/logs"/xray_*.log 2>/dev/null | head -1)"
echo "--- shell log (mods)"
grep -a "mods\|mod [0-9a-z]*/\|fsgame" "$shell_log" 2>/dev/null
echo "--- engine log (mods and MODTEST)"
grep -a -i "\* ShaderXlat: [0-9]\|\* mod \|MODTEST\|using fs-ltx\|lua cache\|lua_cache\|script cache" "$engine_log" 2>/dev/null
echo "--- files the game wrote into the scratch game folder"
(cd "$scratch" && find gamedata -type f 2>/dev/null | grep -v modtest_probe.script)
echo "--- files in the mods folder that are not the generated ones"
(cd "$mods" && find . -type f | sort | grep -v -e 'modtest\(_[ab]\)\?\.\(ltx\|script\)' -e mod.json -e modlist.json -e modtest_data.txt -e default_custom_data.ltx -e common.h)
echo "run files: $out"
