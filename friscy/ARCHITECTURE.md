# friscy: Docker Container → WebAssembly via libriscv

## Overview

friscy converts OCI/Docker containers to WebAssembly by:
1. Cross-compiling the container to RISC-V 64-bit
2. Extracting the rootfs
3. Running the entrypoint in libriscv (userland emulator)
4. Compiling the whole thing to WebAssembly via Emscripten

This is the **CheerpX model**: userland-only emulation, no kernel boot.

---

## Component Status Map

```
Legend:  [✓] Done   [~] Partial/Testing   [ ] Not Started   [○] Skeleton

┌─────────────────────────────────────────────────────────────────────────────┐
│                            BUILD-TIME TOOLS                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│   ┌─────────────────┐      ┌─────────────────┐      ┌─────────────────┐     │
│   │  friscy-pack    │ ───▶ │    rv2wasm      │ ───▶ │  Wizer          │     │
│   │ [✓] CLI tool    │      │ [○] AOT compiler│      │ [ ] Pre-init    │     │
│   │                 │      │                 │      │                 │     │
│   │ • Docker export │      │ • ELF parsing   │      │ • Snapshot VFS  │     │
│   │ • Rootfs tar    │      │ • RISC-V disasm │      │ • Snapshot mem  │     │
│   │ • Manifest gen  │      │ • CFG builder   │      │ • Instant start │     │
│   │ • index.html    │      │ • Wasm codegen  │      │                 │     │
│   └────────┬────────┘      └────────┬────────┘      └─────────────────┘     │
│            │                        │                                        │
│            │    ┌───────────────────┘                                        │
│            │    │                                                            │
│            ▼    ▼                                                            │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                        Output Bundle                                  │   │
│   │   friscy.wasm + friscy.js + rootfs.tar + manifest.json + index.html  │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                              RUNTIME (Browser)                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                        friscy.wasm (Emscripten)                      │   │
│   │  ┌───────────────────────────────────────────────────────────────┐  │   │
│   │  │                   libriscv RV64GC Core                         │  │   │
│   │  │  [✓] Threaded dispatch (computed goto → br_table)             │  │   │
│   │  │  [✓] 512MB arena memory                                       │  │   │
│   │  │  [✓] RV64IMAFDC instruction set                               │  │   │
│   │  │  [✓] SIMD + bulk-memory enabled                               │  │   │
│   │  └───────────────────────────────────────────────────────────────┘  │   │
│   │                              │                                       │   │
│   │  ┌───────────────────────────▼───────────────────────────────────┐  │   │
│   │  │                    Syscall Layer (~50 syscalls)                │  │   │
│   │  │  [✓] syscalls.hpp - file, process, memory, time               │  │   │
│   │  │  [✓] network.hpp  - socket, connect, send, recv               │  │   │
│   │  └───────────────────────────────────────────────────────────────┘  │   │
│   │                              │                                       │   │
│   │  ┌───────────────────────────▼───────────────────────────────────┐  │   │
│   │  │                  Dynamic Linker Support                        │  │   │
│   │  │  [✓] elf_loader.hpp - PT_INTERP detection                     │  │   │
│   │  │  [✓] Aux vector setup (AT_PHDR, AT_ENTRY, AT_BASE, etc.)     │  │   │
│   │  │  [✓] Load ld-musl-riscv64.so.1 at 0x40000000                 │  │   │
│   │  │  [~] Integration testing with real containers                  │  │   │
│   │  └───────────────────────────────────────────────────────────────┘  │   │
│   │                              │                                       │   │
│   │  ┌───────────────────────────▼───────────────────────────────────┐  │   │
│   │  │                   Virtual File System                          │  │   │
│   │  │  [✓] vfs.hpp - tar loading, dir tree, symlinks                │  │   │
│   │  │  [ ] Lazy loading (on-demand from tar)                        │  │   │
│   │  │  [ ] Write support (IndexedDB/OPFS backed)                    │  │   │
│   │  └───────────────────────────────────────────────────────────────┘  │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                   │                                          │
│   ┌───────────────────────────────▼─────────────────────────────────────┐   │
│   │                      JavaScript Bridge Layer                         │   │
│   │  [✓] network_bridge.js - WebSocket ↔ socket syscalls                │   │
│   │  [~] Terminal I/O - xterm.js integration (in index.html)            │   │
│   │  [ ] Storage bridge - IndexedDB/OPFS for persistence                │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                   │                                          │
└───────────────────────────────────┼──────────────────────────────────────────┘
                                    │ WebSocket
┌───────────────────────────────────▼──────────────────────────────────────────┐
│                              HOST MACHINE                                     │
│   ┌─────────────────────────────────────────────────────────────────────┐    │
│   │                     host_proxy (Go)                                  │    │
│   │  [✓] WebSocket server → real TCP/UDP sockets                        │    │
│   │  [ ] gvisor-tap-vsock integration (advanced networking)             │    │
│   └─────────────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## Data Flow: Docker Image → Running in Browser

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   Docker     │     │  friscy-pack │     │    Deploy    │     │   Browser    │
│   Image      │ ──▶ │  CLI Tool    │ ──▶ │   to CDN     │ ──▶ │   Runtime    │
│              │     │              │     │              │     │              │
│ myapp:latest │     │ Extract RV64 │     │ friscy.wasm  │     │ libriscv +   │
│              │     │ rootfs + ELF │     │ rootfs.tar   │     │ syscalls +   │
│              │     │              │     │ index.html   │     │ VFS          │
└──────────────┘     └──────────────┘     └──────────────┘     └──────────────┘

Optional AOT path (future):
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  RISC-V ELF  │     │   rv2wasm    │     │  Native Wasm │
│  binaries    │ ──▶ │  AOT Compile │ ──▶ │  (no interp) │
│  from rootfs │     │  RV64→Wasm   │     │  5-20x faster│
└──────────────┘     └──────────────┘     └──────────────┘
```

---

## Architecture (Runtime)

## Pipeline

### Step 1: Build Container for RISC-V

```bash
# Use docker buildx with RISC-V target
docker buildx build --platform linux/riscv64 -t myapp:riscv64 .

# Or pull existing multi-arch image
docker pull --platform linux/riscv64 alpine:latest
```

### Step 2: Extract Rootfs

```bash
# Create container (don't run)
docker create --platform linux/riscv64 --name temp myapp:riscv64

# Export filesystem
docker export temp > rootfs.tar

# Get entrypoint/cmd
docker inspect temp --format '{{json .Config.Entrypoint}} {{json .Config.Cmd}}'

# Cleanup
docker rm temp
```

### Step 3: Pack for libriscv

Options:
- **Embedded**: Convert rootfs.tar to C byte array (small containers <10MB)
- **Fetch**: Load rootfs.tar via HTTP at runtime (larger containers)
- **9P**: Stream files on-demand from JavaScript (lowest memory)

### Step 4: Run in libriscv

The host (main.cpp) provides:
- RISC-V RV64GC emulation
- Linux syscall emulation (~100 syscalls for typical workloads)
- Virtual filesystem backed by the container rootfs
- stdin/stdout/stderr routing to JavaScript

## Key Design Decisions

### Static vs Dynamic Linking

| Approach | Pros | Cons |
|----------|------|------|
| Static (musl) | Simple, single binary | Larger binary, rebuild needed |
| Dynamic | Standard, smaller binaries | Need to emulate ld-linux, load .so files |

**Recommendation**: Start with static (Alpine/musl), add dynamic later.

### Filesystem Strategy

| Strategy | Memory | Latency | Complexity |
|----------|--------|---------|------------|
| Embedded tar | High | None | Low |
| HTTP fetch | Medium | Startup | Medium |
| 9P on-demand | Low | Per-file | High |

**Recommendation**: Start with embedded tar, add 9P for large containers.

### Syscall Coverage

Minimum viable set (~40 syscalls):
- Process: exit, exit_group, getpid, getuid, gettimeofday
- Memory: brk, mmap, munmap, mprotect
- Files: open, close, read, write, lseek, fstat, stat, readlink
- Dirs: getdents64, getcwd, chdir
- I/O: ioctl (basic), fcntl
- Misc: uname, clock_gettime, getrandom

Full compatibility (~100 syscalls) adds:
- Signals: rt_sigaction, rt_sigprocmask
- Threads: clone, futex (for multi-threaded apps)
- Network: socket, connect, bind, listen, accept, recvfrom, sendto
- Advanced: epoll, eventfd, pipe

## Networking Architecture

friscy provides network access to containers via a WebSocket bridge to a host-side
proxy. This enables socket syscalls (TCP/UDP) without browser networking restrictions.

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Browser                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │                     friscy.wasm                                │  │
│  │  ┌──────────────┐      ┌──────────────┐                       │  │
│  │  │ RISC-V Guest │ ──── │ network.hpp  │                       │  │
│  │  │ socket()     │      │ (syscalls)   │                       │  │
│  │  └──────────────┘      └──────┬───────┘                       │  │
│  └───────────────────────────────┼───────────────────────────────┘  │
│                                  │ EM_ASM                            │
│  ┌───────────────────────────────▼───────────────────────────────┐  │
│  │                  network_bridge.js                             │  │
│  │  • Translates socket calls to WebSocket messages               │  │
│  │  • Buffers received data                                       │  │
│  │  • Handles connect/send/recv                                   │  │
│  └───────────────────────────────┬───────────────────────────────┘  │
└──────────────────────────────────┼──────────────────────────────────┘
                                   │ WebSocket
                                   │ ws://localhost:8765
┌──────────────────────────────────▼──────────────────────────────────┐
│                          Host Machine                                │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │                    host_proxy (Go)                             │  │
│  │  • Accepts WebSocket connections                               │  │
│  │  • Creates real TCP/UDP sockets                                │  │
│  │  • Forwards data between browser and network                   │  │
│  │  • Optional: gvisor-tap-vsock for advanced networking          │  │
│  └───────────────────────────────┬───────────────────────────────┘  │
│                                  │                                   │
│                                  ▼                                   │
│                         Real Network / Internet                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Supported Socket Syscalls

| Syscall | Number | Status | Notes |
|---------|--------|--------|-------|
| socket | 198 | ✅ | AF_INET, AF_INET6, SOCK_STREAM, SOCK_DGRAM |
| bind | 200 | ✅ | Via proxy |
| listen | 201 | ✅ | Via proxy |
| accept | 202 | ⚠️ | Async, limited |
| connect | 203 | ✅ | Returns EINPROGRESS, async completion |
| getsockname | 204 | ✅ | Returns localhost |
| getpeername | 205 | ⚠️ | Stub |
| sendto | 206 | ✅ | Via proxy |
| recvfrom | 207 | ✅ | Buffered in JS |
| setsockopt | 208 | ✅ | Most options ignored |
| getsockopt | 209 | ✅ | SO_ERROR returns 0 |
| shutdown | 210 | ✅ | Via proxy |

### Running with Networking

```bash
# Terminal 1: Start host proxy
cd host_proxy && go run main.go -listen :8765

# Terminal 2: Run friscy in browser
# The network_bridge.js will connect to ws://localhost:8765
```

### Advanced: gvisor-tap-vsock Integration

For more advanced networking (HTTPS interception, custom routing), the host_proxy
can be extended to use gvisor-tap-vsock's userspace network stack:

```go
import (
    "github.com/containers/gvisor-tap-vsock/pkg/virtualnetwork"
    "github.com/containers/gvisor-tap-vsock/pkg/types"
)
```

This enables:
- Full TCP/IP stack in userspace
- MITM HTTPS proxying with dynamic certs
- Custom DNS resolution
- NAT traversal

## File Structure

```
friscy/
├── main.cpp                # Entry point, machine setup, dynamic linker
├── vfs.hpp                 # Virtual filesystem (tar-backed)
├── syscalls.hpp            # Linux syscall handlers (~50 syscalls)
├── network.hpp             # Socket syscall handlers
├── elf_loader.hpp          # ELF parsing, aux vector, dynlink namespace
├── network_bridge.js       # Browser WebSocket ↔ socket bridge
├── CMakeLists.txt          # Build config (Emscripten + native)
├── harness.sh              # Docker-based Wasm build script
│
├── friscy-pack             # [✓] CLI: Docker image → browser bundle
│
├── rv2wasm/                # [○] RISC-V → Wasm AOT compiler (Rust)
│   ├── Cargo.toml
│   └── src/
│       ├── main.rs         # CLI entry point
│       ├── elf.rs          # ELF parsing (goblin)
│       ├── disasm.rs       # RISC-V disassembler (RV64IMAFDC)
│       ├── cfg.rs          # Control flow graph construction
│       ├── translate.rs    # RISC-V → Wasm translation
│       └── wasm_builder.rs # Wasm module emission (wasm-encoder)
│
├── host_proxy/             # Host-side network proxy
│   ├── main.go             # WebSocket → real TCP/UDP
│   └── go.mod
│
├── tests/
│   ├── test_http_minimal.c # HTTP networking test
│   ├── test_server.py      # Test HTTP server
│   └── run_network_test.sh
│
├── ARCHITECTURE.md         # This file
├── PERFORMANCE_ROADMAP.md  # Implementation status & roadmap
└── CRAZY_PERF_IDEAS.md     # Advanced optimization strategies
```
