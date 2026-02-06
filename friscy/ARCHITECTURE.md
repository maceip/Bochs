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

## File Structure

```
friscy/
├── CMakeLists.txt          # Emscripten build config
├── main.cpp                # Host: libriscv + syscalls + VFS
├── vfs.hpp                 # Virtual filesystem implementation
├── syscalls.hpp            # Linux syscall handlers
├── container_to_riscv.sh   # Docker → RISC-V extraction
├── pack_rootfs.py          # Convert rootfs.tar → embeddable
├── harness.sh              # Docker-based Wasm build
├── test_node.js            # Node.js test runner
└── guest.cpp               # Example RISC-V guest (for testing)
```
