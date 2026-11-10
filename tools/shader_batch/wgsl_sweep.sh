#!/usr/bin/env bash
# The WGSL sweep of docs/web-port/SHADERS-WGSL.md in one command:
#
#   cmake --preset macos-arm64-release -B build/wgsl -DANOMALY_SHADERXLAT_WGSL=ON -DANOMALY_BUILD_XRRENDER_R4=ON
#   tools/shader_batch/wgsl_sweep.sh [build dir] [game dir]
#
# build dir defaults to build/wgsl, game dir to $ANOMALY_GAME_DIR or the main checkout's game/Anomaly. The game data is
# only read. Everything is written below <build dir>/wgsl-sweep:
#   shaders-extract/      shaders.db0 unpacked
#   runtime-requests.tsv  the 334 requests r4_shader_warmup makes at default settings, with their macros
#   all-profiles/         low, medium, high_hbao, ultra_hdao
#   extra-profiles/       msaa4, ssao4_hdao
#   runtime/              the runtime set
# Each result directory has summary.md (the "WGSL (Tint)" section), results.jsonl, failures.log, the WGSL of failing
# jobs in wgsl/ and their SPIR-V in wgsl_failed_spirv/. JOBS (default 4) sets build and sweep parallelism.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${1:-$repo/build/wgsl}"
game="${2:-${ANOMALY_GAME_DIR:-}}"
if [[ -z "$game" ]]; then
    for candidate in "$repo/game/Anomaly" "$repo/../../../game/Anomaly"; do # <repo>/.claude/worktrees/<name>
        if [[ -f "$candidate/db/shaders.db0" ]]; then game="$(cd "$candidate" && pwd)"; break; fi
    done
fi
if [[ -z "$game" || ! -f "$game/db/shaders.db0" ]]; then
    echo "no game data: pass the Anomaly directory (with db/shaders.db0) or set ANOMALY_GAME_DIR" >&2
    exit 2
fi
jobs="${JOBS:-4}"

cmake --build "$build" -j "$jobs" --target shader_batch shader_wgsl r4_shader_warmup xrdb_unpack

out="$build/wgsl-sweep"
mkdir -p "$out/warmup-run"
"$build/tools/xrdb_unpack/xrdb_unpack" extract "$game/db/shaders.db0" -o "$out/shaders-extract" | tail -1
"$build/tools/r4_shader_warmup/r4_shader_warmup" --game-dir "$game" --run-dir "$out/warmup-run" \
    --export-requests "$out/runtime-requests.tsv" | tail -2

batch=(
    "$build/tools/shader_batch/shader_batch"
    --root "$repo/gamedata/shaders" --root "$out/shaders-extract/shaders" # loose overlay first: it shadows the archive
    --engine-src "$repo/src/Layers/xrRender" --engine-src "$repo/src/Layers/xrRenderDX10"
    --engine-src "$repo/src/Layers/xrRenderPC_R4"
    --target wgsl --uav-format 0=r16float --jobs "$jobs"
)
"${batch[@]}" --out "$out/all-profiles" > "$out/all-profiles.log" 2>&1
"${batch[@]}" --out "$out/extra-profiles" --profile msaa4 --profile ssao4_hdao > "$out/extra-profiles.log" 2>&1
"${batch[@]}" --out "$out/runtime" --requests "$out/runtime-requests.tsv" > "$out/runtime.log" 2>&1

for run in all-profiles extra-profiles runtime; do
    echo "== $run"
    sed -n '/^- \*\*valid WGSL/p' "$out/$run/summary.md"
done
echo "details: $out/{all-profiles,extra-profiles,runtime}/summary.md"
