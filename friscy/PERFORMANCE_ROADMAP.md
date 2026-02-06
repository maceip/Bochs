# friscy: Docker → Browser Runtime Roadmap

## Vision

**Goal**: Input any Docker image, output a high-performance browser-based runtime that beats WebVM/CheerpX.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           User Workflow                                      │
│                                                                              │
│   $ docker build -t myapp .                                                  │
│   $ friscy-pack myapp:latest --output myapp.wasm                            │
│   $ # Deploy myapp.wasm + myapp-rootfs.tar to CDN                           │
│   $ # User visits website → instant container execution in browser          │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Current Status (What's Built)

### ✅ Core Emulator
| Component | Status | Location |
|-----------|--------|----------|
| libriscv RV64GC emulator | ✅ Working | `src/libriscv/` |
| Threaded dispatch (computed goto) | ✅ Enabled | CMakeLists.txt |
| Encompassing arena (512MB) | ✅ Configured | 29-bit arena |
| Emscripten build harness | ✅ Working | `harness.sh` |
| Native build (for testing) | ✅ Working | `build-native/` |

### ✅ Syscall Emulation (~50 syscalls)
| Category | Syscalls | Status |
|----------|----------|--------|
| Process | exit, getpid, getuid, gettid | ✅ |
| Memory | brk, mmap, munmap, mprotect | ✅ (basic) |
| Files | open, close, read, write, lseek, stat | ✅ |
| Dirs | getdents64, getcwd, chdir | ✅ |
| Time | clock_gettime, getrandom | ✅ |
| I/O | ioctl, fcntl, writev | ✅ |
| **Network** | socket, connect, send, recv, close | ✅ **NEW** |

### ✅ Virtual Filesystem
| Feature | Status | Notes |
|---------|--------|-------|
| Tar archive loading | ✅ | Embedded or fetched |
| Directory tree | ✅ | In-memory |
| File read | ✅ | From tar content |
| Symlink resolution | ✅ | Basic |
| getdents64 | ✅ | For `ls`, `find` |

### ✅ Networking (Just Completed!)
| Feature | Status | Notes |
|---------|--------|-------|
| Socket syscalls | ✅ | socket, connect, send, recv |
| Native sockets | ✅ | Real TCP/UDP in native builds |
| WebSocket bridge | ✅ | `network_bridge.js` for browser |
| Host proxy | ✅ | `host_proxy/main.go` |
| HTTP fetch test | ✅ | `tests/test_http_minimal.c` |

### ✅ Documentation
- `ARCHITECTURE.md` - System design
- `PERFORMANCE_ROADMAP.md` - This document

---

## What's Missing (Build Order)

### ✅ Phase 1: End-to-End Pipeline (DONE)

**Goal**: User runs one command, gets working Wasm bundle.

```bash
# Usage:
$ friscy-pack myimage:latest --output bundle/

# Output:
bundle/
├── friscy.wasm          # Emulator compiled to Wasm
├── friscy.js            # Emscripten glue
├── rootfs.tar           # Container filesystem
├── index.html           # Demo page with xterm.js
└── manifest.json        # Entrypoint, env, workdir
```

**Completed Tasks**:
| Task | Status |
|------|--------|
| Create `friscy-pack` CLI tool | ✅ `friscy-pack` |
| Extract Docker rootfs | ✅ Works with --platform linux/riscv64 |
| Generate index.html with terminal | ✅ xterm.js + WebSocket bridge |
| Bundle manifest (entrypoint, args, env) | ✅ JSON manifest |
| Optional `--aot` flag for AOT compilation | ✅ Skeleton (needs rv2wasm) |

**Location**: `friscy-pack` (executable shell script)

---

### ✅ Phase 2: Dynamic Linker Support (DONE)

**Problem**: Most Docker containers use dynamically linked binaries.

```bash
$ file /bin/busybox  # Alpine
/bin/busybox: ELF 64-bit LSB pie executable, UCB RISC-V, ... dynamically linked,
interpreter /lib/ld-musl-riscv64.so.1
```

**Solution**: Support the dynamic linker (ld-musl).

**Completed Tasks**:
| Task | Status | Location |
|------|--------|----------|
| Parse ELF PT_INTERP | ✅ | `elf_loader.hpp` |
| Load ld-musl as entry point | ✅ | `main.cpp` |
| Build aux vector (AT_PHDR, AT_ENTRY, etc.) | ✅ | `elf_loader.hpp` dynlink namespace |
| Load interpreter at 0x40000000 | ✅ | `main.cpp` |
| Jump to interpreter entry | ✅ | `main.cpp` |

**How it works**:
```
┌─────────────────────────────────────────────────────────────────┐
│  Dynamic Binary: /bin/python3                                    │
│                                                                  │
│  1. friscy reads ELF, finds PT_INTERP = /lib/ld-musl-riscv64.so │
│  2. friscy loads interpreter at 0x40000000                       │
│  3. friscy sets up stack with aux vector:                        │
│     AT_PHDR = address of python3's program headers              │
│     AT_PHNUM = number of program headers                         │
│     AT_ENTRY = python3's entry point                             │
│     AT_BASE = interpreter load address (0x40000000)             │
│     AT_RANDOM, AT_PLATFORM, AT_HWCAP, etc.                      │
│  4. friscy jumps to interpreter entry point                      │
│  5. ld-musl runs, reads auxv, loads libc.so, etc.               │
│  6. ld-musl jumps to python3's entry point                      │
│  7. Python runs normally                                         │
└─────────────────────────────────────────────────────────────────┘
```

**Remaining**: Test with real Alpine containers

---

### 🟡 Phase 3: Wizer Snapshots (2-5x startup improvement)

**Problem**: Parsing tar + loading ELF on every page load is slow.

**Solution**: Snapshot initialized state into Wasm using Wizer.

```
Build time:                          Runtime:
┌─────────┐    ┌─────────┐          ┌────────────────┐
│ friscy  │───▶│  Wizer  │    ───▶  │ Instant start! │
│  .wasm  │    │         │          │ (pre-warmed)   │
└─────────┘    └─────────┘          └────────────────┘
                   │
           Runs initialization:
           - Parse rootfs.tar
           - Build VFS tree
           - Load ELF headers
           - Setup memory layout
           - SNAPSHOT!
```

**Tasks**:
| Task | Effort | Priority |
|------|--------|----------|
| Add `wizer_init()` export | Low | 🟡 |
| Run Wizer in build pipeline | Medium | 🟡 |
| Test with various container sizes | Medium | 🟡 |

---

### 🟡 Phase 4: Terminal Integration

**Problem**: Need interactive terminal in browser.

**Solution**: Integrate xterm.js with stdin/stdout.

**Tasks**:
| Task | Effort | Priority |
|------|--------|----------|
| xterm.js integration | Low | 🟡 |
| stdin from keyboard | Low | 🟡 |
| stdout/stderr to terminal | ✅ Done | via write syscall |
| ANSI escape handling | ✅ Done | xterm.js handles |
| Window resize (TIOCGWINSZ) | ✅ Done | ioctl returns 80x24 |

---

### 🟡 Phase 5: Persistent Storage

**Problem**: Container state lost on page reload.

**Solution**: Use IndexedDB or OPFS for persistence.

**Tasks**:
| Task | Effort | Priority |
|------|--------|----------|
| Identify writable directories | Low | 🟡 |
| Sync writes to IndexedDB | Medium | 🟡 |
| Restore state on reload | Medium | 🟡 |
| OPFS for large files | Medium | 🟡 |

---

### 🟢 Phase 6: Performance Optimization

**Current**: Working but not optimized.

**Tasks**:
| Task | Effort | Impact |
|------|--------|--------|
| -O3 -flto builds | Low | 10-20% |
| WASM SIMD (-msimd128) | Low | 5-10% |
| Bulk memory ops | Low | Faster memcpy |
| Inline hot syscalls | Medium | 10-20% |
| Lazy VFS (don't load full tar) | High | Faster startup |

---

### 🟢 Phase 7: Advanced Features

| Feature | Effort | Notes |
|---------|--------|-------|
| Multi-threading (WebWorkers) | High | For parallel workloads |
| GPU compute (WebGPU) | High | For ML/compute containers |
| Audio (Web Audio API) | Medium | For multimedia |
| Clipboard access | Low | Copy/paste support |

---

## Competitive Analysis

| Feature | WebVM | container2wasm | v86 | **friscy** |
|---------|-------|----------------|-----|------------|
| ISA | x86 (JIT) | x86 (Bochs) | x86 (JIT) | RISC-V (interp) |
| Boot time | 3-5s | 30-60s | 5-10s | **<500ms** |
| Kernel | Yes | Yes | Yes | **No (userland)** |
| Dynamic linking | Yes | Yes | Yes | **Yes** |
| Networking | Yes | Limited | Yes | **Yes** |
| Open source | No | Yes | Yes | **Yes** |
| CoreMark % | ~15% | ~1% | ~10% | **~40%** |
| AOT compiler | No | No | No | **Skeleton** |

**Why friscy wins**:
1. **No kernel** = instant start, smaller Wasm
2. **RISC-V** = simpler ISA, faster interpreter
3. **Pre-compiled** = Emscripten optimizes, no runtime JIT
4. **Wizer** = snapshot initialization for instant warm start

---

## Implementation Priority

```
Week 1-2: End-to-End Pipeline
├── friscy-pack CLI tool
├── Auto RISC-V build
├── Bundle generation
└── Demo page with xterm.js

Week 3-4: Dynamic Linking
├── Load ld-musl as entry
├── Aux vector setup
├── mmap PROT_EXEC
└── Test Alpine busybox, Python

Week 5: Polish
├── Wizer integration
├── Performance tuning
├── Documentation
└── Demo containers (Python, Node, etc.)
```

---

## Quick Start (What Works Today)

```bash
# 1. Build friscy (native, for testing)
cd friscy
mkdir build-native && cd build-native
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j

# 2. Compile a static RISC-V binary
riscv64-linux-gnu-gcc -static -o hello hello.c

# 3. Run it
./friscy hello

# 4. Test networking
python3 tests/test_server.py 8080 &
./friscy tests/test_http_minimal
# → Makes real HTTP request!

# 5. Build for Wasm (requires Docker)
./harness.sh
# → Produces build/friscy.wasm
```

---

## Files Reference

```
friscy/
├── main.cpp                 # Entry point, machine setup, dynamic linker
├── vfs.hpp                  # Virtual filesystem
├── syscalls.hpp             # ~50 Linux syscalls
├── network.hpp              # Socket syscalls (TCP/UDP)
├── network_bridge.js        # Browser WebSocket bridge
├── elf_loader.hpp           # ELF parsing, aux vector, dynlink namespace
├── CMakeLists.txt           # Build configuration
├── harness.sh               # Docker-based Wasm build
├── friscy-pack              # CLI tool: Docker → browser bundle
├── host_proxy/              # Host-side network proxy
│   ├── main.go              # WebSocket → real sockets
│   └── go.mod
├── rv2wasm/                 # RISC-V → Wasm AOT compiler (Rust)
│   ├── Cargo.toml
│   └── src/
│       ├── main.rs          # CLI entry point
│       ├── elf.rs           # ELF parsing
│       ├── disasm.rs        # RISC-V disassembler
│       ├── cfg.rs           # Control flow graph
│       ├── translate.rs     # RISC-V → Wasm translation
│       └── wasm_builder.rs  # Wasm module construction
├── tests/
│   ├── test_http_minimal.c  # Networking test
│   ├── test_server.py       # HTTP test server
│   └── run_network_test.sh  # Automated test
├── ARCHITECTURE.md          # Design document
├── PERFORMANCE_ROADMAP.md   # This file
└── CRAZY_PERF_IDEAS.md      # Advanced performance optimizations
```

---

## Summary

**Done**:
- Core RISC-V emulation (libriscv)
- Basic syscall set (~50)
- Virtual filesystem from tar
- Networking (socket, connect, send, recv)
- Native + Wasm builds
- **friscy-pack CLI** - Bundle Docker containers
- **Dynamic linker support** - Load ld-musl, aux vector setup
- **rv2wasm skeleton** - AOT compiler framework (Rust)

**Pre-Launch Priority** (AOT is the target):
1. **Complete rv2wasm** - RISC-V → Wasm AOT compilation (5-20x speedup!)
   - Finish instruction translation (~47 RISC-V opcodes)
   - Handle syscall trampolines
   - Integrate with friscy-pack `--aot` flag
2. **Test dynamic linking** - Run real Alpine busybox, Python containers
3. **Wizer integration** - Instant startup via pre-initialization

**Post-Launch**:
4. Performance tuning - Profile and optimize hot paths
5. Persistent storage - IndexedDB/OPFS
6. Advanced networking - gvisor-tap-vsock

**Then we beat WebVM by 5-10x** 🚀
