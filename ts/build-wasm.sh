#!/bin/bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-wasm"

if ! command -v emcmake >/dev/null 2>&1; then
    echo "Error: emcmake not found. Install and activate Emscripten first." >&2
    exit 1
fi

rm -rf "${BUILD_DIR}"

emcmake cmake \
    -S "${ROOT_DIR}/ts/embind" \
    -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "${BUILD_DIR}"

echo "WASM build complete. Outputs are in ts/fastdb4ts/src/wasm/"
