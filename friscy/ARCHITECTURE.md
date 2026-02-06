# friscy: Docker Container → WebAssembly via libriscv

## Overview

friscy converts OCI/Docker containers to WebAssembly by:
1. Cross-compiling the container to RISC-V 64-bit
2. Extracting the rootfs
3. Running the entrypoint in libriscv (userland emulator)
4. Compiling the whole thing to WebAssembly via Emscripten

This is the **CheerpX model**: userland-only emulation, no kernel boot.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Browser / Node.js                         │
├─────────────────────────────────────────────────────────────────┤
│                     WebAssembly Runtime                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                    libriscv (Wasm)                        │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐   │   │
│  │  │ RISC-V CPU  │  │   Memory    │  │ Syscall Handler │   │   │
│  │  │  Emulator   │  │   (Arena)   │  │   (Linux ABI)   │   │   │
│  │  └─────────────┘  └─────────────┘  └─────────────────┘   │   │
│  │         │                │                  │             │   │
│  │         └────────────────┼──────────────────┘             │   │
│  │                          │                                │   │
│  │  ┌───────────────────────▼───────────────────────────┐   │   │
│  │  │              Virtual File System                   │   │   │
│  │  │  ┌─────────┐  ┌─────────┐  ┌─────────────────┐    │   │   │
│  │  │  │ /bin/*  │  │ /lib/*  │  │ /etc/* (config) │    │   │   │
│  │  │  │ busybox │  │ musl.so │  │ passwd, hosts   │    │   │   │
│  │  │  └─────────┘  └─────────┘  └─────────────────┘    │   │   │
│  │  └───────────────────────────────────────────────────┘   │   │
│  └──────────────────────────────────────────────────────────┘   │
│                              │                                   │
│                              ▼                                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                    JavaScript Bridge                      │   │
│  │  • Terminal I/O (xterm.js)                               │   │
│  │  • Network (WebSocket → TCP proxy)                        │   │
│  │  • Storage (IndexedDB, OPFS)                             │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

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
├── CMakeLists.txt          # Emscripten build config
├── main.cpp                # Host: libriscv + syscalls + VFS
├── vfs.hpp                 # Virtual filesystem implementation
├── syscalls.hpp            # Linux syscall handlers
├── network.hpp             # Socket syscall handlers
├── network_bridge.js       # JS WebSocket bridge for networking
├── elf_loader.hpp          # Dynamic ELF loading support
├── container_to_riscv.sh   # Docker → RISC-V extraction
├── harness.sh              # Docker-based Wasm build
├── host_proxy/             # Host-side network proxy
│   ├── main.go             # WebSocket → real network bridge
│   └── go.mod              # Go module dependencies
├── test_node.js            # Node.js test runner
└── PERFORMANCE_ROADMAP.md  # Strategy to beat WebVM
```
