#!/bin/bash
# ============================================================================
# Optimized Bochs configure for Wasm targets (WASI and Emscripten)
#
# This script replaces the container2wasm default configure with a
# stripped-down configuration that:
#   - Removes unused CPU features (AVX, VMX, SVM, 3DNow)
#   - Uses a simpler CPU model (Penryn vs Haswell)
#   - Enables trace linking via the Wasm-safe iterative implementation
#   - Disables all unnecessary subsystems
#
# Expected impact:
#   - 10-30% smaller Wasm binary (fewer instruction handlers)
#   - Better Wasm JIT code cache utilization
#   - Fewer functions instrumented by Asyncify
#   - Trace linking enabled (5-10% CPU throughput gain)
#
# Usage:
#   For WASI SDK:
#     CC="${WASI_SDK_PATH}/bin/clang" \
#     CXX="${WASI_SDK_PATH}/bin/clang++" \
#     ./wasm/configure-wasm-optimized.sh --wasi
#
#   For Emscripten:
#     emconfigure ./wasm/configure-wasm-optimized.sh --emscripten
#
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOCHS_DIR="$(dirname "$SCRIPT_DIR")"

TARGET=""
EXTRA_CFLAGS=""
EXTRA_LDFLAGS=""
LOGGING_FLAG="--disable-logging"
HOST_FLAG=""

# Parse arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    --wasi)
      TARGET="wasi"
      HOST_FLAG="--host wasm32-unknown-wasi"
      shift
      ;;
    --emscripten)
      TARGET="emscripten"
      # emconfigure handles host detection
      shift
      ;;
    --enable-logging)
      LOGGING_FLAG="--enable-logging"
      shift
      ;;
    *)
      echo "Unknown option: $1"
      echo "Usage: $0 [--wasi|--emscripten] [--enable-logging]"
      exit 1
      ;;
  esac
done

if [ -z "$TARGET" ]; then
  echo "Error: must specify --wasi or --emscripten"
  exit 1
fi

cd "$BOCHS_DIR"

# ============================================================================
# Core configuration: minimal CPU feature set for container workloads
# ============================================================================
#
# What we KEEP:
#   --enable-x86-64          : Required for x86_64 guest
#   --enable-repeat-speedups : REP string operation fast path
#   --enable-fast-function-calls : Register-based calling convention
#   --enable-handlers-chaining : Direct handler-to-handler dispatch
#   --enable-trace-linking   : NEW: enabled via iterative Wasm-safe impl
#   --enable-usb             : Required for rootfs CDROM device
#   --enable-usb-ehci        : Required for rootfs CDROM device
#
# What we REMOVE vs current container2wasm build:
#   --disable-avx            : Saves ~2000 instruction handlers
#   --disable-evex           : AVX-512 not needed
#   --disable-vmx            : No nested virtualization in containers
#   --disable-svm            : No AMD-V in containers
#   --disable-3dnow          : Legacy AMD, never used
#   --disable-smp            : Single CPU (already implicit)
#   --disable-debugger       : No interactive debugging
#   --disable-disasm         : No runtime disassembly
#   --disable-plugins        : Static build only
#   --disable-docbook        : No documentation generation
#
# ============================================================================

CONFIGURE_FLAGS=(
    # Target architecture
    --enable-x86-64
    --enable-cpu-level=6
    --with-nogui

    # CPU features: minimal set covering SSE through SSE4.1
    # This covers virtually all binaries compiled with -march=x86-64
    --disable-avx
    --disable-evex
    --disable-vmx
    --disable-svm
    --disable-3dnow

    # Performance optimizations
    --enable-repeat-speedups
    --enable-fast-function-calls
    --enable-handlers-chaining
    --enable-trace-linking           # NOW SAFE: iterative Wasm implementation

    # USB for rootfs
    --enable-usb
    --enable-usb-ehci

    # Disable everything unnecessary
    --disable-large-ramfile
    --disable-show-ips
    --disable-stats
    --disable-debugger
    --disable-disasm
    --disable-plugins
    --disable-docbook
    --disable-readline
    ${LOGGING_FLAG}

    ${HOST_FLAG}
)

echo "============================================================"
echo "Configuring Bochs for Wasm (${TARGET}) with optimized flags"
echo "============================================================"
echo ""
echo "Configure flags:"
printf '  %s\n' "${CONFIGURE_FLAGS[@]}"
echo ""

./configure "${CONFIGURE_FLAGS[@]}" "$@"

# ============================================================================
# Post-configure: enable Wasm-specific optimizations in config.h
# ============================================================================
# The configure script sets BX_ENABLE_TRACE_LINKING=1 but we also need
# to enable the Wasm-safe iterative implementation and direct RAM fast path.
if grep -q "BX_WASM_ITERATIVE_TRACE_LINKING" config.h; then
    sed -i 's/#define BX_WASM_ITERATIVE_TRACE_LINKING 0/#define BX_WASM_ITERATIVE_TRACE_LINKING 1/' config.h
    echo ""
    echo ">>> Enabled BX_WASM_ITERATIVE_TRACE_LINKING in config.h"
fi

if grep -q "BX_WASM_DIRECT_RAM_FASTPATH" config.h; then
    sed -i 's/#define BX_WASM_DIRECT_RAM_FASTPATH 0/#define BX_WASM_DIRECT_RAM_FASTPATH 1/' config.h
    echo ">>> Enabled BX_WASM_DIRECT_RAM_FASTPATH in config.h"
fi

echo ""
echo "============================================================"
echo "Configuration complete. Next steps:"
echo ""
if [ "$TARGET" = "wasi" ]; then
    echo "  1. make -j\$(nproc) bochs"
    echo "  2. wasm-opt bochs --asyncify -O2 -o bochs.async \\"
    echo "       --pass-arg=asyncify-onlylist@wasm/asyncify-onlylist.txt"
    echo "  3. (optional) wizer --allow-wasi ... -o bochs.wizer bochs.async"
else
    echo "  1. emmake make -j\$(nproc) bochs"
fi
echo "============================================================"
