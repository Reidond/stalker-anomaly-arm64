#!/bin/bash
# The locks across a restart (docs/ios-port/SETTINGS.md), unattended on the Mac:
#   1. a run changes locked settings from the console and from scripts (refused) and the player's own ones (accepted), then saves;
#   2. the saved settings, plus stale values for locked settings written by hand, are the next start's user.ltx;
#   3. the second run prints what the engine ended up with: the profile's values for the locked ones, the player's for the others.
# Output: build/render-harness/lockrestart/. The engine logs of both runs are kept.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$root/build/render-harness/lockrestart
rm -rf "$out"; mkdir -p "$out"
export HARNESS_STATE=$out/state
"$root/tools/render_harness/run_scene.sh" -t lockrestart 20_locks | tail -2
appdata="$HARNESS_STATE/Application Support/Anomaly/appdata"
{ cat "$appdata/unattended.ltx"
  # stale values for locked settings, as an old build, a preset or a hand edit would leave them
  printf 'r2_sun_quality st_opt_extreme\r\ntexture_lod 0\r\nr4_enable_tessellation on\r\nr3_msaa 4x\r\nr2_ssao st_opt_high\r\nr3_dynamic_wet_surfaces on\r\nr__detail_radius 250\r\nssfx_ssr (1, 1, 1, 1)\r\n'
  printf 'r2_mblur_enabled on\r\n'; } > "$appdata/user.ltx"
"$root/tools/render_harness/run_scene.sh" -t lockrestart 25_locks_restart | tail -2
for f in "$out"/2*/engine.log; do
    echo "== $f"
    iconv -f cp1251 -t utf-8 "$f" | grep -a "graphics\|^- [a-z0-9_]* " | cut -c1-260
done
