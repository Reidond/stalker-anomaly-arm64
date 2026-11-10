#!/usr/bin/env bash
# Builds the browser lab into build/web-lab/site: plain static files (HTML, JS, wasm, test chunks).
#
#   web/lab/setup_emsdk.sh   # once: pinned emsdk into build/emsdk
#   web/lab/build.sh         # SDL3 (first time) + the lab
#   web/lab/serve.py         # http://localhost:8642 with COOP/COEP
#
# Environment: JOBS (default 4), EMSDK_DIR (default build/emsdk), LAB_DEBUG=1 for -O0 -g.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LAB="$ROOT/web/lab"
OUT="$ROOT/build/web-lab"
SITE="$OUT/site"
EMSDK_DIR="${EMSDK_DIR:-$ROOT/build/emsdk}"
JOBS="${JOBS:-4}"

if [ ! -f "$EMSDK_DIR/emsdk_env.sh" ]; then
    echo "No emsdk in $EMSDK_DIR; run web/lab/setup_emsdk.sh first." >&2
    exit 1
fi
# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1

if [ ! -f "$ROOT/external/SDL/CMakeLists.txt" ]; then
    git -C "$ROOT" submodule update --init --depth 1 external/SDL
fi

# SDL3 from the repo's pinned submodule, static, with pthreads.
if [ ! -f "$OUT/sdl3/libSDL3.a" ]; then
    emcmake cmake -S "$ROOT/external/SDL" -B "$OUT/sdl3" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_PTHREADS=ON \
        -DCMAKE_C_FLAGS="-pthread" -DCMAKE_CXX_FLAGS="-pthread"
    cmake --build "$OUT/sdl3" -j "$JOBS"
fi

OPT=(-O2)
if [ "${LAB_DEBUG:-0}" = "1" ]; then OPT=(-O0 -g); fi

mkdir -p "$SITE"
em++ -std=c++20 "${OPT[@]}" -pthread \
    -I "$ROOT/external/SDL/include" -I "$ROOT/src/Platform/GameInput/include" \
    "$LAB/src/lab_main.cpp" "$LAB/src/gpu_lab.cpp" "$LAB/src/input_lab.cpp" "$LAB/src/storage_lab.cpp" "$LAB/src/lab_report.cpp" \
    "$OUT/sdl3/libSDL3.a" \
    --use-port=emdawnwebgpu \
    --js-library "$LAB/src/lab_lib.js" \
    -sPROXY_TO_PTHREAD -sOFFSCREENCANVAS_SUPPORT -sOFFSCREENCANVASES_TO_PTHREAD='#canvas' \
    -sPTHREAD_POOL_SIZE=6 -sDEFAULT_PTHREAD_STACK_SIZE=2MB -sSTACK_SIZE=2MB \
    -sINITIAL_MEMORY=512MB -sALLOW_MEMORY_GROWTH=0 \
    -sENVIRONMENT=web,worker -sASSERTIONS=1 \
    -sEXPORTED_FUNCTIONS=_main,_lab_command,_malloc,_free \
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32,HEAPF32,HEAPF64 \
    -o "$SITE/lab_wasm.js"

cp "$LAB"/page/*.html "$LAB"/page/*.js "$LAB"/page/*.css "$SITE/"
python3 "$LAB/tools/make_chunks.py" "$SITE/chunks" 8

# Item 5 (stretch): OpenAL Soft + SDL3 audio page. Optional: LAB_AUDIO=0 skips it, and a failure
# here leaves the main lab intact.
if [ "${LAB_AUDIO:-1}" = "1" ]; then
    if [ ! -f "$ROOT/external/openal-soft/CMakeLists.txt" ]; then
        git -C "$ROOT" submodule update --init --depth 1 external/openal-soft
    fi
    # A patched copy of the pinned sources: one line in alc.cpp does not compile with this libc++.
    OPENAL_SRC="$OUT/openal-src"
    if [ ! -f "$OPENAL_SRC/.web-lab-patched" ]; then
        mkdir -p "$OPENAL_SRC"
        rsync -a --delete --exclude .git --exclude .web-lab-patched "$ROOT/external/openal-soft/" "$OPENAL_SRC/"
        patch -d "$OPENAL_SRC" -p1 <"$LAB/audio/openal-soft-emscripten.patch"
        touch "$OPENAL_SRC/.web-lab-patched"
    fi
    if emcmake cmake -S "$LAB/audio" -B "$OUT/audio" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL3_DIR="$OUT/sdl3" \
            -DOPENAL_SOURCE_DIR="$OPENAL_SRC" \
            -DCMAKE_C_FLAGS="-pthread" -DCMAKE_CXX_FLAGS="-pthread" >"$OUT/audio-configure.log" 2>&1 &&
        cmake --build "$OUT/audio" -j "$JOBS" >"$OUT/audio-build.log" 2>&1; then
        cp "$OUT/audio/audio_lab.js" "$OUT/audio/audio_lab.wasm" "$SITE/"
    else
        echo "warning: the audio lab did not build; see $OUT/audio-configure.log and $OUT/audio-build.log" >&2
    fi
fi
echo "Built $SITE"
