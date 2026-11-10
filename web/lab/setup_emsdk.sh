#!/usr/bin/env bash
# Installs the pinned Emscripten SDK into build/emsdk (inside the checkout, nothing system-wide).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EMSDK_DIR="$ROOT/build/emsdk"
VERSION="${EMSDK_VERSION:-7.0.0}" # emdawnwebgpu needs 4.0.10 or later

if [ ! -d "$EMSDK_DIR/.git" ]; then
    mkdir -p "$ROOT/build"
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi
cd "$EMSDK_DIR"
./emsdk install "$VERSION"
./emsdk activate "$VERSION"
echo "emsdk $VERSION ready in $EMSDK_DIR"
