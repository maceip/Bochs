#!/bin/bash
# ============================================================================
# friscy — libriscv compiled to WebAssembly via Emscripten
#
# This harness builds libriscv (RISC-V 64-bit emulator) as a Wasm module
# that can run RISC-V Linux ELF binaries in a browser or Node.js.
#
# Prerequisites: Docker (or see setup_native_harness.sh for local build)
#
# What this does NOT use (and why):
#   -sMEMORY64=1  — libriscv emulates 64-bit RISC-V on a 32-bit Wasm host.
#                   Guest addresses are just uint64_t values. The 256MB
#                   encompassing arena fits fine in wasm32. MEMORY64 is
#                   experimental and breaks Emscripten's JS glue.
#   -mtail-call   — Wasm tail calls are a separate proposal from libriscv's
#                   [[clang::musttail]] dispatch. The flag doesn't help and
#                   limits engine compatibility.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== friscy: Building libriscv for WebAssembly ==="

# 1. Clone libriscv (upstream, not a stale fork)
mkdir -p src
if [ ! -d "src/libriscv" ]; then
    echo "Cloning libriscv (upstream)..."
    git clone --depth=1 https://github.com/libriscv/libriscv.git src/libriscv
else
    echo "libriscv already cloned, updating..."
    (cd src/libriscv && git pull --ff-only 2>/dev/null || true)
fi

# 2. Build with Emscripten via Docker
echo ""
echo "Building with Emscripten (Docker)..."
echo "CMake configuration:"
echo "  - RISCV_64I=ON, RISCV_EXT_A=ON, RISCV_EXT_C=ON"
echo "  - RISCV_THREADED=ON (computed goto dispatch)"
echo "  - RISCV_BINARY_TRANSLATION=OFF (no JIT in Wasm)"
echo "  - RISCV_ENCOMPASSING_ARENA=ON (28-bit = 256MB)"
echo "  - -fexceptions (required: libriscv uses C++ throw/catch)"
echo ""

mkdir -p build

docker run --rm \
    -v "${SCRIPT_DIR}:/src" \
    -w /src/build \
    -u "$(id -u):$(id -g)" \
    emscripten/emsdk:3.1.50 \
    bash -c '
        emcmake cmake .. \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_FLAGS="-fexceptions" \
            -DCMAKE_C_FLAGS="-fexceptions" \
        && emmake make -j$(nproc) VERBOSE=1
    '

# 3. Verify output
if [ -f "build/friscy.js" ] && [ -f "build/friscy.wasm" ]; then
    echo ""
    echo "=== Build successful ==="
    ls -lh build/friscy.js build/friscy.wasm
    echo ""
    echo "Usage:"
    echo "  # Cross-compile a guest binary:"
    echo "  riscv64-linux-gnu-gcc -static -O2 -o guest guest.c"
    echo ""
    echo "  # Run via Node.js:"
    echo "  node --experimental-wasm-modules test_node.js guest"
    echo ""
    echo "  # Or embed in a web page (see test_node.js for API)"
else
    echo ""
    echo "ERROR: Build failed — friscy.js/friscy.wasm not found"
    echo "Check build output above for errors."
    exit 1
fi
