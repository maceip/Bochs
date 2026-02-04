# Bochs in container2wasm: Architecture Analysis & Top 5 Optimizations

## How container2wasm Uses Bochs

container2wasm converts OCI container images into self-contained WebAssembly
binaries. For **x86_64 containers targeting WASI runtimes** (wasmtime, wazero,
wasmer), Bochs is the CPU emulation engine. The full stack looks like:

```
WASI Runtime (host)
  └─ Bochs (compiled to Wasm via WASI SDK + Binaryen Asyncify)
       └─ Linux kernel v6.1 (monolithic, single-CPU, virtio drivers)
            └─ runc
                 └─ Container process
```

Bochs is chosen over QEMU for WASI targets because it is a **pure C++
interpretive emulator** with no runtime code generation, no threads, and
minimal external dependencies — properties that make it straightforward to
compile with WASI SDK.

### The ktock/Bochs Fork

The container2wasm project uses a fork at `github.com/ktock/Bochs` that adds:

1. **`wasm.cc` / `wasm.h`** — Three virtio PCI devices (ported from TinyEMU):
   - **Virtio-9P**: Plan 9 filesystem sharing between WASI host and guest
   - **Virtio-Console**: stdin/stdout bridge via WASI fd operations
   - **Virtio-Net**: Ethernet-level networking via WASI sockets
2. **`wasi_extra/jmp/`** — Custom `setjmp`/`longjmp` using Binaryen Asyncify
   (WASI has no native setjmp)
3. **`wasi_extra/vfs/`** — WASI `poll_oneoff` wrapper for socket I/O
4. **Modified `main.cc`** — WASI initialization, Wizer snapshot support,
   Asyncify unwind/rewind loop

### Build Pipeline

```
WASI SDK 19 (clang → wasm32-wasi)
  → Bochs binary (wasm)
  → Binaryen wasm-opt --asyncify (instruments all function calls)
  → wasi-vfs (packs BIOS, kernel, rootfs into the .wasm binary)
  → Wizer (optional: snapshots post-boot state to skip kernel init)
  → Final out.wasm (~30-50 MB self-contained)
```

Key configure flags used:
```
--enable-x86-64 --with-nogui --enable-usb --enable-usb-ehci
--enable-repeat-speedups --enable-fast-function-calls
--enable-handlers-chaining --enable-avx
--disable-trace-linking          ← crashes in Wasm (OOB memory access)
--disable-large-ramfile --disable-show-ips --disable-stats
```

### Runtime Configuration

- **128 MB guest RAM** (default)
- **CPU model**: `corei7_haswell_4770` at 40 MIPS calibration
- **Boot**: GRUB ISO via ATA CDROM → Linux kernel
- **Rootfs**: USB EHCI CDROM device
- **Display**: `nogui` (headless, console-only via virtio-console)

---

## How qemu-wasm Compiles QEMU with Emscripten

For **browser targets** (`--to-js`), container2wasm uses QEMU compiled to Wasm
via Emscripten instead of Bochs. The qemu-wasm project (`github.com/ktock/qemu-wasm`)
achieves this through:

1. **Emscripten SDK 3.1.50** cross-compiles QEMU and all dependencies (zlib,
   GLib, pixman, libffi) to wasm32
2. **TCG Interpreter (TCI)** handles cold code; a **TCG→Wasm JIT backend**
   compiles hot translation blocks (~1000 executions) into individual
   `WebAssembly.Module` instances
3. **Emscripten Asyncify** enables cooperative yielding for the browser event
   loop
4. **`-sPROXY_TO_PTHREAD`** moves the main emulation loop to a Web Worker,
   keeping the UI thread responsive
5. **Emscripten fiber coroutines** replace ucontext for QEMU's coroutine system
6. **xterm-pty** bridges QEMU's serial console to xterm.js in the browser

The critical difference: QEMU-wasm has a **JIT path** (hot code → native Wasm
execution) while Bochs is **pure interpretation** (every x86 instruction
decoded and executed by C++ code running as Wasm). This means QEMU-wasm can
be significantly faster for long-running CPU-bound workloads, but Bochs has
simpler boot behavior and better WASI compatibility.

---

## The Execution Model: Where Time Is Spent

For the container2wasm use case, the execution profile is:

```
x86_64 guest instruction
  → Bochs C++ interpreter (instruction decode + execute)
    → [Asyncify instrumentation overhead on every function call]
      → Wasm runtime (browser JIT or wasmtime/wazero)
        → Host CPU
```

This creates roughly **100–1000× slowdown** vs. native. The dominant costs are:

| Component | Estimated % of Runtime | Notes |
|-----------|----------------------|-------|
| CPU instruction decode/execute loop | ~50-60% | The inner `while(1)` in `cpu_loop()` |
| Asyncify overhead | ~15-25% | Every function call instrumented |
| Memory access (TLB + page walks) | ~10-15% | Every guest memory access |
| Virtio I/O processing | ~5-10% | Console polling, 9p filesystem ops |
| Wasm runtime overhead | ~5-10% | Wasm→native translation, bounds checks |

---

## Top 5 Optimizations (Performance Gain / Implementation Complexity)

### 1. Fine-Grained Asyncify Function Whitelist

**Expected gain: 2–5× on CPU-bound workloads**
**Implementation complexity: Moderate**

**The problem**: Binaryen's Asyncify transformation instruments *every*
function call in the entire Bochs binary to support suspension/resumption.
This adds prologue/epilogue code to every call site — stack unwinding state
checks, buffer save/restore — even in the CPU hot loop where suspension
never occurs. The container2wasm build currently uses only
`--pass-arg=asyncify-ignore-imports` which skips imported (WASI) functions
but still instruments all internal Bochs functions.

**The optimization**: Create an explicit Asyncify whitelist that limits
instrumentation to only the functions that actually need to suspend:

- The `wasm_start()` → `bxmain()` call chain (top-level Asyncify loop)
- `setjmp`/`longjmp` wrappers in `jmp.c`
- Virtio device I/O entry points (console `poll`, 9p `read`/`write`, net
  `recv`/`send`)
- Timer callback handlers that may yield

All CPU-internal functions — `cpu_loop()`, `getICacheEntry()`,
`fetchDecode()`, individual instruction handlers, TLB lookup, page walk —
would be excluded from Asyncify instrumentation.

**How to implement**: Binaryen supports `--pass-arg=asyncify-onlylist@file`
which takes a newline-separated list of function names that *should* be
instrumented. All other functions are left untouched. The work is:

1. Compile Bochs with `-g` to get function name symbols in the Wasm
2. Trace which functions are on the call path from `wasm_start` to actual
   WASI I/O calls (the suspension points)
3. Build the whitelist (likely ~50-100 functions out of thousands)
4. Pass `--pass-arg=asyncify-onlylist@whitelist.txt` to `wasm-opt`

The CPU hot loop (`cpu.cc:166-228`) calls `getICacheEntry()` →
instruction handlers → memory access functions millions of times per
second. Removing Asyncify instrumentation from these paths eliminates
conditional branches and buffer operations on every call.

**Risk**: If the whitelist misses a function that actually needs to suspend,
the Wasm module will trap at runtime. Thorough testing with representative
container workloads is necessary.

---

### 2. Fix Trace Linking for Wasm (or Implement a Wasm-Safe Alternative)

**Expected gain: 5–10% (documented in upstream Bochs CHANGES)**
**Implementation complexity: Moderate**

**The problem**: Trace linking (`--enable-trace-linking`) is explicitly
disabled in the container2wasm build because it causes "out of bounds memory
access" in Wasm. The root cause is in `cpu.cc:323-400` — the `linkTrace()`
function uses two mechanisms that break under Wasm:

1. **Host stack depth detection** (`cpu.cc:349-355`):
   ```cpp
   size_t stack_depth = BX_CPU_THIS_PTR cpuloop_stack_anchor - &stack_anchor;
   if (stack_depth > BX_HANDLERS_CHAINING_MAX_STACK_DEPTH) { ... }
   ```
   This subtracts stack-allocated variable addresses to measure recursion
   depth. In Wasm, the execution stack and linear memory stack are separate.
   `&stack_anchor` gives a linear memory address, but the *actual* Wasm
   execution stack (which is what overflows) is invisible to the program.
   The check silently passes while the real Wasm stack overflows → trap.

2. **Recursive handler execution**: `BX_EXECUTE_INSTRUCTION(next)` calls
   the next trace's handler, which itself may call `linkTrace()` again,
   building up arbitrarily deep recursion. Wasm default stack is ~1MB
   (configurable but still limited).

**The optimization**: Replace the recursive trace-linking model with an
iterative one for the Wasm target:

```cpp
#ifdef WASI  // or a new BX_WASM_TARGET define
// Iterative trace linking: instead of recursive BX_EXECUTE_INSTRUCTION,
// set next_trace pointer and return to the main loop which picks it up
void BX_CPU_C::linkTrace(bxInstruction_c *i) {
    bxInstruction_c *next = i->getNextTrace(iCache.traceLinkTimeStamp);
    if (next) {
        BX_CPU_THIS_PTR next_linked_trace = next;  // new field
        return;  // main loop checks this field
    }
    // ... fall through to icache lookup
}
#endif
```

Then modify the main `cpu_loop()` to check for a linked trace before
calling `getICacheEntry()`:

```cpp
// In the handler-chaining loop:
if (BX_CPU_THIS_PTR next_linked_trace) {
    i = BX_CPU_THIS_PTR next_linked_trace;
    BX_CPU_THIS_PTR next_linked_trace = NULL;
} else {
    i = getICacheEntry()->i;
}
```

This eliminates all recursion, making trace linking safe for Wasm's
bounded stack while preserving the performance benefit of avoiding
redundant icache lookups on branch targets.

**Upstream Bochs CHANGES** documents trace linking as "extra 5-10%
emulation speedup" — this is essentially free performance left on the
table due to a Wasm-specific incompatibility.

---

### 3. Strip Unused CPU Features to Shrink the Wasm Binary

**Expected gain: 10–30% reduction in Wasm binary size → faster load, better icache locality**
**Implementation complexity: Low**

**The problem**: The container2wasm build enables `--enable-avx` and compiles
with `cpu: model=corei7_haswell_4770`. This includes the full Haswell
instruction set: AVX, AVX2, FMA, BMI1/2, plus the entire decoder tables
for these extensions. The `ia_opcodes.def` file alone defines thousands of
instruction handlers. For a typical container workload (busybox, runc,
shell scripts, Go/Rust binaries), the vast majority of these instructions
are never executed.

Each unused instruction handler:
- Occupies space in the Wasm binary (dead code not eliminated by wasm-opt
  because handlers are referenced from opcode dispatch tables)
- Pollutes the Wasm engine's code cache
- Increases Asyncify instrumentation scope (more functions to instrument)

**The optimization**: Create a minimal CPU profile for container workloads:

```bash
./configure \
  --enable-x86-64 \
  --disable-avx \             # Remove AVX/AVX2/FMA decoder tables
  --disable-evex \            # Remove AVX-512 support
  --disable-vmx \             # No nested virtualization needed
  --disable-svm \             # No AMD-V needed
  --disable-3dnow \           # Legacy AMD extension unused
  --disable-smp \             # Already single-CPU
  --disable-large-ramfile \   # Already disabled
  --disable-debugger \        # Already disabled
  --enable-cpu-level=6 \
  ...
```

Additionally, configure the CPU model to a simpler profile via the runtime
config:

```
cpu: model=core2_penryn_t9600
```

Penryn covers SSE through SSE4.1, which is sufficient for virtually all
userspace binaries compiled for generic x86_64. This eliminates AVX/AVX2
decoder tables, register file extensions (YMM→XMM only), and associated
state save/restore logic.

**How to measure**: Compare `wasm-opt --metrics` output before and after.
The binary size reduction directly translates to:
- Faster download/instantiation in browsers
- Better code cache utilization in Wasm JIT engines
- Less Asyncify instrumentation (fewer functions)

**Risk**: Very low. If a container binary happens to use AVX instructions
(rare for server workloads, common compiler targets use `-march=x86-64`),
Bochs would raise an `#UD` exception which the guest kernel translates to
`SIGILL`. This is an acceptable tradeoff for the common case.

---

### 4. Direct Linear Memory Fast Path for Guest RAM Access

**Expected gain: 10–20% on memory-intensive workloads**
**Implementation complexity: Medium**

**The problem**: Every guest memory access traverses multiple layers:

```
Guest virtual address
  → Segment check (protected mode)
    → TLB lookup (BX_TLB_ENTRY)
      → Hit: read/write via hostAddr pointer
      → Miss: Page table walk (CR3 → PML4 → PDPT → PD → PT)
        → Physical address
          → Memory handler chain search (linked list traversal)
            → RAM read/write
```

On a TLB hit (the common case), Bochs uses a `hostAddr` pointer that
maps directly to the host process's memory. In native Bochs, this is
efficient — it's just a pointer dereference. But in Wasm, every memory
access through a pointer goes through Wasm linear memory bounds checking.
More critically, the **memory handler chain** for misses (and I/O regions)
is a linked list traversal — terrible for Wasm performance due to
unpredictable indirect memory accesses.

**The optimization**: For the container2wasm use case, guest physical RAM
is a single contiguous allocation in Wasm linear memory. We can exploit
this:

1. **Guarantee contiguous RAM allocation**: Allocate all guest RAM as a
   single `malloc` at startup, giving a base pointer `ram_base` and known
   size `ram_size`.

2. **Add a physical-address fast path** that bypasses the handler chain for
   addresses known to be in RAM:

   ```cpp
   // In memory.cc read/write paths:
   BX_CPP_INLINE Bit8u* BX_MEM_C::get_ram_ptr(bx_phy_address pAddr) {
       if (BX_LIKELY(pAddr < ram_size)) {
           return ram_base + pAddr;  // Direct offset, no handler search
       }
       return NULL;  // Fall through to handler chain for MMIO
   }
   ```

3. **TLB entries store Wasm linear memory offsets** instead of raw host
   pointers — this is semantically the same in Wasm (all pointers are
   offsets into linear memory) but allows the JIT engine to optimize the
   access pattern.

4. **Replace the linked-list memory handler registry** with a page-table-style
   array lookup for MMIO regions. The current linked-list in `devices.cc`
   requires O(n) traversal per MMIO access:

   ```cpp
   // Current: linked list traversal
   struct memory_handler_struct *mh = memory_handlers[a20addr >> 20];
   while (mh) { ... mh = mh->next; }

   // Proposed: direct page-indexed array
   memory_handler_t handler = mmio_page_table[pAddr >> 12];
   if (handler) handler(pAddr, ...);
   ```

**Why this matters for Wasm specifically**: Wasm JIT engines (V8, wasmtime)
optimize linear memory accesses that follow predictable patterns. A single
base pointer + offset access compiles down to a single bounds-checked load.
Linked list chasing generates unpredictable memory access patterns that
defeat hardware prefetching and Wasm JIT optimizations.

---

### 5. Expand Wizer Pre-Initialization Beyond Kernel Boot

**Expected gain: 50–80% reduction in time-to-first-interaction**
**Implementation complexity: Low**

**The problem**: Wizer currently snapshots Bochs state after the Linux
kernel has booted. But there is still significant work between kernel boot
and the container process being ready:

```
[Wizer snapshot point] ← current
  → init process starts (Go binary)
    → Mount proc, sys, dev, overlay filesystem
    → Parse OCI image config
    → Set up networking (if enabled)
    → runc creates container
    → Container entrypoint executes
[User interaction possible] ← desired
```

The init process, filesystem mounting, and runc container creation are
deterministic for a given container image. All of this can be pre-computed.

**The optimization**: Move the Wizer snapshot point later in the boot
sequence:

1. **Phase 1** (low effort): Snapshot after `init` has mounted filesystems
   and is about to call `runc run`. This skips ~30-50% of post-kernel-boot
   work. Requires the init process to signal readiness (e.g., write a
   sentinel to the virtio console).

2. **Phase 2** (medium effort): Snapshot after `runc` has created the
   container and the entrypoint process is at its first blocking I/O (e.g.,
   waiting for stdin). This gets the user to an interactive prompt almost
   instantly.

**How to implement**: Wizer works by running the Wasm module until a
designated function returns, then capturing the entire linear memory state.
The `wizer.resume` entry point in `main.cc` already supports this. The
changes needed are:

1. Modify the init process to emit a well-known sentinel string at the
   desired snapshot point
2. Modify the Wizer invocation to wait for this sentinel before snapshotting
3. Ensure any non-deterministic state (timestamps, entropy) is re-seeded
   on resume

**Risk**: Wizer snapshots are specific to the exact container image. If
the container image changes, the snapshot must be rebuilt. This is already
the case for the kernel boot snapshot — extending it doesn't change the
rebuild model, just the snapshot contents.

**Interaction with other optimizations**: This optimization is
*multiplicative* with CPU execution speed improvements (#1-#4). Faster
Bochs means the remaining post-snapshot work completes sooner.

---

## Summary Table

| # | Optimization | Performance Gain | Complexity | Wasm-Specific? |
|---|-------------|-----------------|------------|----------------|
| 1 | Asyncify function whitelist | 2–5× CPU throughput | Moderate | Yes |
| 2 | Iterative trace linking | 5–10% CPU throughput | Moderate | Yes (fix for Wasm) |
| 3 | Strip unused CPU features | 10–30% binary size, indirect perf | Low | No (but Wasm amplifies benefit) |
| 4 | Direct RAM fast path | 10–20% memory throughput | Medium | Yes (Wasm linear memory) |
| 5 | Expanded Wizer snapshots | 50–80% startup reduction | Low | Yes (WASI tooling) |

Optimizations #1 and #5 have the highest gain-to-effort ratio. #1 requires
no Bochs source changes (only build tooling), and #5 requires only
modifications to the container2wasm init process. Combined, they address
the two biggest pain points: CPU throughput and startup latency.

Optimization #2 requires Bochs source changes but is a well-scoped
refactoring of `linkTrace()` in `cpu.cc:323-400` — converting recursion
to iteration with a new `next_linked_trace` field on the CPU class.

Optimization #3 is the lowest-effort change (configure flags only) and
should be done immediately as a baseline improvement.

Optimization #4 is the most invasive but targets a fundamental bottleneck
in the memory subsystem. It would benefit from profiling data to confirm
the expected gains before investing in implementation.
