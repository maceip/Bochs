cmake_minimum_required(VERSION 3.14)
project(friscy CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# libriscv configuration for Emscripten (wasm32) target
#
# Key insights:
#   - NO MEMORY64: libriscv emulates a 64-bit RISC-V *guest* on a 32-bit
#     Wasm *host*. Guest 64-bit addresses are just uint64_t values in
#     the host's 32-bit address space. The encompassing arena (28-bit =
#     256MB) fits easily in wasm32. MEMORY64 is experimental, breaks JS
#     glue code, and provides zero benefit here.
#
#   - NO TAIL CALLS: The Wasm tail call proposal is separate from
#     libriscv's [[clang::musttail]] dispatch. -mtail-call tells emcc
#     to emit Wasm return_call instructions, but libriscv's tailcall
#     dispatch uses a C++ attribute that doesn't map to Wasm tail calls.
#     The official Wasm build uses threaded dispatch (computed goto)
#     which works fine in Emscripten.
#
#   - NO BINARY TRANSLATION: Requires dlopen() and native code execution.
#     Impossible in Wasm. Must be explicitly disabled.
#
#   - EXCEPTIONS REQUIRED: libriscv uses C++ throw/catch throughout.
#     Emscripten must be told to support them with -fexceptions.
#
# Based on: libriscv/examples/wasm/build.sh (the official working config)
# ============================================================================

# --- Core ISA ---
set(RISCV_32I OFF CACHE BOOL "Disable 32-bit RISC-V (we only need 64-bit)")
set(RISCV_64I ON  CACHE BOOL "Enable 64-bit RISC-V")
set(RISCV_EXT_A ON  CACHE BOOL "Enable Atomics extension")
set(RISCV_EXT_C ON  CACHE BOOL "Enable Compressed instructions")
set(RISCV_EXT_V OFF CACHE BOOL "Disable Vector extension (complex, not needed)")
set(RISCV_EXT_ZICOND OFF CACHE BOOL "")
set(RISCV_EXT_ZMMUL  OFF CACHE BOOL "")
set(RISCV_EXT_ZCMP   OFF CACHE BOOL "")

# --- Dispatch mode ---
# Threaded dispatch (computed goto) works in Emscripten and is fastest.
# Switch-based is safer but ~30% slower. Tail call dispatch does NOT work.
set(RISCV_THREADED ON  CACHE BOOL "Enable computed-goto threaded dispatch")
set(RISCV_TAILCALL_DISPATCH OFF CACHE BOOL "DISABLED: musttail doesn't work in Wasm")

# --- Memory ---
# Encompassing arena allocates the full guest address space upfront.
# 28-bit = 256MB, fits in wasm32's 4GB address space.
set(RISCV_ENCOMPASSING_ARENA ON  CACHE BOOL "Pre-allocate guest address space")
set(RISCV_ENCOMPASSING_ARENA_BITS 28 CACHE STRING "256MB guest address space")
set(RISCV_FLAT_RW_ARENA ON  CACHE BOOL "Fast read-write arena")
set(RISCV_MEMORY_TRAPS OFF CACHE BOOL "Disable page traps (overhead for Wasm)")

# --- Features to DISABLE for Wasm ---
set(RISCV_BINARY_TRANSLATION OFF CACHE BOOL "DISABLED: requires dlopen/native code")
set(RISCV_LIBTCC OFF CACHE BOOL "DISABLED: TCC compiler not available in Wasm")
set(RISCV_EXPERIMENTAL ON  CACHE BOOL "Enable experimental features")

# --- Add libriscv ---
add_subdirectory(src/libriscv/lib)

# --- Main executable ---
add_executable(friscy main.cpp)
target_link_libraries(friscy PUBLIC riscv)

# --- Compile flags ---
target_compile_options(friscy PRIVATE
    -O2
    -fexceptions          # CRITICAL: libriscv uses C++ exceptions everywhere
)

# --- Link flags ---
target_link_options(friscy PRIVATE
    -O2
    -fexceptions
    -sALLOW_TABLE_GROWTH  # Dynamic function table growth
    -sINITIAL_MEMORY=268435456  # 256MB (matches 28-bit arena)
    -sALLOW_MEMORY_GROWTH      # Allow growing beyond initial
    -sMAXIMUM_MEMORY=536870912  # Cap at 512MB
    -sEXPORT_ES6=1
    -sMODULARIZE=1
    -sEXPORTED_RUNTIME_METHODS=['FS','callMain']
    -o friscy.js
)

# --- Optional: WASM SIMD ---
# SIMD provides marginal benefit for interpreter workloads but doesn't hurt.
# Uncomment if targeting modern browsers only:
# target_compile_options(friscy PRIVATE -msimd128)
