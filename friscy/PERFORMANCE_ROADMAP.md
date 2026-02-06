# friscy: Beating WebVM/CheerpX Performance Roadmap

## Executive Summary

**Goal**: Create the fastest Docker-in-browser runtime using libriscv + Emscripten.

**Why libriscv can win**:
| Factor | WebVM (x86 JIT) | friscy (RISC-V interp) | Advantage |
|--------|-----------------|------------------------|-----------|
| ISA complexity | x86: 1000+ opcodes, complex decode | RISC-V: ~50 base opcodes, fixed-width | friscy |
| Wasm codegen | Custom x86→Wasm JIT at runtime | Ahead-of-time Emscripten compilation | friscy |
| Memory model | Complex x86 segments, alignment | Simple flat memory, natural alignment | friscy |
| Startup time | JIT warmup required | Instant (Wizer snapshots possible) | friscy |
| Code size | Large JIT compiler in Wasm | Small interpreter (~100KB) | friscy |
| Native perf | ~15-30% native (estimated) | ~30-50% native (threaded dispatch) | friscy |

**Key insight**: CheerpX's x86 JIT must dynamically generate Wasm code at runtime, which is slow and complex. libriscv's threaded interpreter is pre-compiled to optimized Wasm by Emscripten's LLVM backend.

## Performance Tiers

### Tier 1: Core Execution Speed (DONE)
- [x] Threaded dispatch (computed goto) - fastest interpreter mode
- [x] Encompassing arena (28-bit) - O(1) memory access
- [x] Flat RW arena - no page table overhead
- [x] -O2 + LLVM optimizations via Emscripten

### Tier 2: Startup Optimization (CRITICAL)
- [ ] Wizer pre-initialization snapshots
- [ ] Lazy rootfs loading (don't parse full tar at startup)
- [ ] Streaming ELF loading
- [ ] WASM module caching (Cache-Control, IndexedDB)

### Tier 3: Syscall Fast Paths
- [ ] Inline critical syscalls (write, read, brk)
- [ ] Batch I/O operations
- [ ] Memory-mapped file I/O (mmap pages directly)

### Tier 4: Dynamic Linking (REQUIRED for most containers)
- [ ] Load ld-musl-riscv64.so.1 as entry point
- [ ] Implement dl_* syscalls
- [ ] Lazy .so loading from VFS

### Tier 5: Advanced Features
- [ ] WebWorker threading (for multi-core guests)
- [ ] Network stack (socket → WebSocket bridge)
- [ ] GPU passthrough (WebGPU for compute)
- [ ] Persistent storage (IndexedDB/OPFS)

---

## Implementation Plan

### Phase 1: Wizer Snapshots (2-5x startup improvement)

```
┌─────────────────────────────────────────────────────┐
│                  Build Time                          │
│  ┌─────────┐    ┌─────────┐    ┌─────────────────┐  │
│  │ friscy  │───▶│  Wizer  │───▶│ friscy-snapshot │  │
│  │  .wasm  │    │         │    │     .wasm       │  │
│  └─────────┘    └─────────┘    └─────────────────┘  │
│                      │                               │
│              Run initialization:                     │
│              - Parse rootfs tar                      │
│              - Build VFS tree                        │
│              - Load ELF headers                      │
│              - Setup initial memory layout           │
└─────────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│                  Runtime                             │
│  User loads friscy-snapshot.wasm                    │
│  → Instantly ready to execute guest code            │
│  → No tar parsing, no ELF loading                   │
│  → Memory already initialized                        │
└─────────────────────────────────────────────────────┘
```

**Implementation**:
```c++
// In main.cpp, add Wizer initialization point
#ifdef WIZER_INIT
extern "C" void wizer_init() {
    // Load rootfs, build VFS, setup machine
    // This runs once at build time, state is snapshotted
}
#endif
```

### Phase 2: Dynamic Linker Support

Most Docker containers use dynamically linked binaries. Instead of requiring `--static`, we emulate the dynamic linker:

```
┌─────────────────────────────────────────────────────┐
│  Container: alpine/python:3.11                       │
│                                                      │
│  /usr/bin/python3 (dynamically linked)              │
│      │                                               │
│      ├── INTERP: /lib/ld-musl-riscv64.so.1         │
│      │                                               │
│      ├── NEEDED: libc.musl-riscv64.so.1            │
│      ├── NEEDED: libpython3.11.so.1.0              │
│      └── NEEDED: libm.so.6                          │
└─────────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────┐
│  friscy Dynamic Loader                               │
│                                                      │
│  1. Load /lib/ld-musl-riscv64.so.1 as entry        │
│  2. Set up aux vector (AT_PHDR, AT_ENTRY, etc.)    │
│  3. Let musl's dynamic linker do the work          │
│  4. Intercept dlopen/dlsym for VFS .so loading     │
└─────────────────────────────────────────────────────┘
```

**Key syscalls needed**:
- `openat` with library search paths (/lib, /usr/lib)
- `mmap` with PROT_EXEC for code pages
- `mprotect` for relocation

### Phase 3: Optimized Emscripten Configuration

```cmake
# Maximum performance flags
target_compile_options(friscy PRIVATE
    -O3                      # Full optimization
    -flto                    # Link-time optimization
    -fno-rtti                # Remove RTTI overhead
    -fno-exceptions          # If we can remove throws (perf gain)
    -msimd128                # WASM SIMD
    -mbulk-memory            # Fast memcpy/memset
    -mnontrapping-fptoint    # Faster FP conversions
)

target_link_options(friscy PRIVATE
    -O3
    -flto
    --closure 1              # Minify JS glue
    -sWASM_BIGINT            # Native i64 support
    -sENVIRONMENT=web,worker # Strip Node.js code
    -sSINGLE_FILE            # Embed wasm in JS
    -sSTACK_SIZE=1048576     # 1MB stack
    -sINITIAL_MEMORY=268435456
    -sALLOW_MEMORY_GROWTH
    -sMAXIMUM_MEMORY=2147483648  # 2GB max
)
```

### Phase 4: Lazy Rootfs Loading

Instead of loading entire tar upfront, stream files on-demand:

```
┌─────────────────────────────────────────────────────┐
│  Lazy VFS Architecture                               │
│                                                      │
│  rootfs.tar (stored as-is, not parsed)              │
│       │                                              │
│       ▼                                              │
│  ┌─────────────┐                                    │
│  │ Index File  │  ← Small JSON: {path: offset, len} │
│  └─────────────┘                                    │
│       │                                              │
│       ▼                                              │
│  On file access:                                     │
│  1. Lookup path in index                            │
│  2. Seek to offset in tar                           │
│  3. Read only that file's bytes                     │
│  4. Cache in memory                                 │
└─────────────────────────────────────────────────────┘
```

**Benefits**:
- Instant startup (no tar parsing)
- Lower memory (only load accessed files)
- Works with large containers (100MB+ rootfs)

---

## Benchmark Targets

| Metric | WebVM | friscy Target | Method |
|--------|-------|---------------|--------|
| Cold start | 3-5s | <500ms | Wizer snapshots |
| Boot to shell | 5-10s | <1s | No kernel, lazy VFS |
| CoreMark | ~15% native | >40% native | Threaded dispatch |
| Memory overhead | ~100MB | <50MB | Lazy loading |
| Wasm size | ~5MB | <500KB | Minimal interpreter |

---

## Competitive Analysis

### WebVM/CheerpX (x86)
**Pros**: Runs unmodified x86 Linux binaries
**Cons**: Complex JIT, large runtime, slow startup, closed source

### container2wasm/Bochs (x86)
**Pros**: Full system emulation, boots real kernel
**Cons**: Very slow (~1% native), huge memory, long boot

### v86 (x86)
**Pros**: Open source, reasonable speed
**Cons**: Still requires kernel boot, x86 complexity

### friscy (RISC-V) - OUR APPROACH
**Pros**:
- Simple ISA = efficient interpreter
- No kernel boot (userland only)
- Pre-compiled Wasm (no runtime JIT)
- Open source
- Instant start with Wizer

**Cons**:
- Requires RISC-V containers (docker buildx handles this)
- Dynamic linking needs work

---

## Next Steps (Priority Order)

1. **Dynamic linker** - Most containers need this
2. **Wizer snapshots** - Instant startup
3. **Performance tuning** - O3, LTO, SIMD
4. **Lazy VFS** - Handle large containers
5. **Benchmarks** - Prove we're faster

The combination of libriscv's efficient threaded interpreter + Emscripten's optimizing LLVM backend + Wizer snapshots should deliver significantly better performance than WebVM's runtime x86→Wasm JIT approach.
