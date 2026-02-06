```
 ____ ____  __  __    ___ _  _
||    || \\ || (( \  //   \\//
||==  ||_// ||  \\  ((     )/ 
||    || \\ || \_))  \\__ //
```
```
◈ PRE-RUNTIME PIPELINE
 ────────────────────────────────────────────────────────────────────────────
 [ Input ]          [ Transform ]                      [ Optimization ]
 
 Docker Export  ──▶  friscy-pack  ──────────────────▶  rv2wasm (AOT)
 Rootfs Tar         ● CLI Tool [✓]                     ● ELF Parsing [○]
                    ● Manifest Gen [✓]                 ● CFG Builder [○]
                                                       ● Wasm Codegen [○]
                                                              │
                                                              ▼
 [ Bundle ]         [ Snapshot ]                       [ Final Artifact ]
 
 index.html     ◀──  Wizer                              friscy.wasm
 rootfs.tar     ◀──  ● Pre-initialization [ ]          friscy.js
 manifest.json  ◀──  ● VFS/Mem Snapshot [ ]            (Output Bundle)
 ```

 ```
◈ BROWSER RUNTIME (WASM ISOLATION)
 ────────────────────────────────────────────────────────────────────────────
 
   ┌─ EXECUTION CORE ──────────────────────────────────────────────────────┐
   │ libriscv RV64GC                                                       │
   │ ├─ [✓] Threaded dispatch (br_table)                                   │
   │ ├─ [✓] 512MB Arena Memory                                             │
   │ └─ [✓] SIMD + Bulk-memory extensions                                  │
   └───────────────────────────────┬───────────────────────────────────────┘
                                   │
   ┌─ SYSTEM ABSTRACTION LAYER ────▼───────────────────────────────────────┐
   │ POSIX Syscall Interface (~50 entry points)                            │
   │ ├─ [✓] File, Process, Memory, Time                                    │
   │ └─ [✓] Socket (TCP/UDP abstraction)                                   │
   └───────────────────────────────┬───────────────────────────────────────┘
                                   │
   ┌─ OS PRIMITIVES ───────────────▼───────────────────────────────────────┐
   │ Dynamic Linker & VFS                                                  │
   │ ├─ [✓] ELF Loader (PT_INTERP & Aux Vector)                            │
   │ ├─ [✓] ld-musl-riscv64.so.1 Integration                               │
   │ ├─ [✓] Virtual File System (Tar/Dir tree)                             │
   │ └─ [ ] Lazy Loading / OPFS Persistence                                │
   └───────────────────────────────┬───────────────────────────────────────┘
                                   │ 
 ◈ HOST INTEROP & NETWORKING       │ (JS Promises / SharedBuffer)
 ──────────────────────────────────┼─────────────────────────────────────────
                                   │
   ┌─ JAVASCRIPT BRIDGE ───────────▼───────────────────────────────────────┐
   │ ├─ [✓] Network: WebSocket ↔ Socket Bridge                             │
   │ ├─ [~] Terminal: xterm.js UI                                          │
   │ └─ [ ] Storage: IndexedDB Backend                                     │
   └───────────────────────────────┬───────────────────────────────────────┘
                                   │
                                   │ (Encapsulated WebSocket)
                                   ▼
   ┌─ EXTERNAL PROXY (Go) ─────────────────────────────────────────────────┐
   │ host_proxy                                                            │
   │ └─ [✓] Native TCP/UDP Socket Mapping                                  │
   └───────────────────────────────────────────────────────────────────────┘
````
