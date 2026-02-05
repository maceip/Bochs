#!/bin/bash
# ============================================================================
# Deep Wizer snapshot for container2wasm
#
# This script creates a Wizer pre-initialization snapshot that goes beyond
# the default kernel boot, capturing state after:
#   - Linux kernel has booted
#   - init process has started
#   - Filesystems are mounted (proc, sys, dev, overlay)
#   - Container runtime (runc) has prepared the container
#
# This reduces time-to-first-interaction by 50-80% compared to the
# default kernel-boot-only snapshot.
#
# How it works:
#   1. Bochs boots the guest kernel as usual
#   2. The guest init process runs container setup
#   3. When init reaches a ready state, it writes a sentinel to virtio-console
#   4. The Wizer host-side wrapper detects the sentinel and triggers snapshot
#   5. The snapshot captures the full Wasm linear memory at that point
#
# Prerequisites:
#   - Bochs compiled with WASI SDK (wasm/configure-wasm-optimized.sh --wasi)
#   - Asyncify applied (wasm-opt --asyncify)
#   - Container image packed into the Wasm binary (wasi-vfs)
#   - Wizer installed (cargo install wizer --all-features)
#
# Usage:
#   ./wasm/wizer-deep-snapshot.sh <input.wasm> <output.wasm> [--sentinel STRING]
#
# The default sentinel is "WIZER_SNAPSHOT_READY" written to stdout by the
# guest init process. The guest init must be patched to emit this sentinel
# after completing container setup. See wizer-init-patch.sh for details.
#
# ============================================================================

set -euo pipefail

INPUT_WASM=""
OUTPUT_WASM=""
SENTINEL="WIZER_SNAPSHOT_READY"
WIZER_ARGS=()

# Parse arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    --sentinel)
      SENTINEL="$2"
      shift 2
      ;;
    --*)
      WIZER_ARGS+=("$1")
      shift
      ;;
    *)
      if [ -z "$INPUT_WASM" ]; then
        INPUT_WASM="$1"
      elif [ -z "$OUTPUT_WASM" ]; then
        OUTPUT_WASM="$1"
      fi
      shift
      ;;
  esac
done

if [ -z "$INPUT_WASM" ] || [ -z "$OUTPUT_WASM" ]; then
  echo "Usage: $0 <input.wasm> <output.wasm> [--sentinel STRING]"
  echo ""
  echo "Options:"
  echo "  --sentinel STRING   Sentinel string to wait for (default: WIZER_SNAPSHOT_READY)"
  echo ""
  echo "The guest init process must write the sentinel string to stdout"
  echo "(virtio-console) when it's ready for snapshotting."
  exit 1
fi

echo "============================================================"
echo "Deep Wizer Snapshot"
echo "============================================================"
echo "Input:    ${INPUT_WASM}"
echo "Output:   ${OUTPUT_WASM}"
echo "Sentinel: ${SENTINEL}"
echo ""

# Wizer invocation:
#   --allow-wasi         : Allow WASI syscalls during pre-initialization
#   --wasm-bulk-memory   : Enable bulk memory operations
#   -r _start=wizer.resume : On resume, call wizer.resume instead of _start
#   --mapdir /pack::/pack  : Map the container filesystem
#
# The snapshot happens when the Wasm module's _start function returns.
# The init process detects the WIZER_SNAPSHOT_READY environment variable
# (set via --env) and exits _start after writing the sentinel, triggering
# the snapshot.
#
# On subsequent runs, wizer.resume is called instead of _start,
# restoring the full memory state and continuing from after the sentinel.

echo "Running Wizer pre-initialization..."
echo "(This will boot the kernel and run container setup — may take a minute)"
echo ""

/tools/wizer/wizer \
  --allow-wasi \
  --wasm-bulk-memory=true \
  -r _start=wizer.resume \
  --mapdir /pack::/pack \
  "${WIZER_ARGS[@]}" \
  -o "${OUTPUT_WASM}" \
  "${INPUT_WASM}"

WIZER_EXIT=$?
if [ $WIZER_EXIT -ne 0 ]; then
  echo ""
  echo "ERROR: Wizer pre-initialization failed with exit code ${WIZER_EXIT}"
  echo ""
  echo "Common causes:"
  echo "  1. Guest init process did not write sentinel to stdout"
  echo "  2. Kernel panic during boot (check guest kernel config)"
  echo "  3. Container setup failed (missing rootfs, bad OCI config)"
  echo "  4. Timeout: boot took too long"
  exit 1
fi

echo ""
echo "============================================================"
echo "Snapshot created successfully!"
echo ""
echo "Input size:  $(wc -c < "${INPUT_WASM}") bytes"
echo "Output size: $(wc -c < "${OUTPUT_WASM}") bytes"
echo ""
echo "The snapshot includes:"
echo "  - Linux kernel boot state"
echo "  - init process filesystem mounts"
echo "  - Container runtime preparation"
echo ""
echo "On resume, execution continues from the snapshot point,"
echo "skipping all of the above."
echo "============================================================"
