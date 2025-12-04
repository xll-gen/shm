# AGENTS.md

This document provides context, rules, and guidelines for AI Agents working on this codebase.
**CRITICAL:** If you modify code that affects architecture, memory layout, or build processes, you **MUST** update this file and `README.md` to reflect those changes.

## 1. Project Overview

This project implements a high-performance, lock-free Shared Memory IPC (Inter-Process Communication) library between **C++ (Host)** and **Go (Guest)**.

-   **Host (C++):** Creates shared memory, acts as the server/initiator in some contexts or queue owner.
-   **Guest (Go):** Attaches to shared memory.
-   **Philosophy:** Zero-copy (where possible), cache-line aligned, lock-free ring buffers (SPSC), and direct slot exchange.

### Directory Structure

-   `src/`: C++ Host implementation (`IPCHost`, `DirectHost`, `Platform`).
-   `include/`: Shared C++ headers (`IPCUtils.h` - **CRITICAL**, `SPSCQueue.h`).
-   `go/`: Go Guest implementation (`queue.go`, `direct.go`, `platform.go`).
-   `benchmarks/`: C++ client and Go server for performance testing.
-   `Taskfile.yaml`: Automation for building and running benchmarks.

---

## 2. Critical Constraints (DO NOT BREAK)

### 2.1. Memory Layout & Alignment

The memory layout is manually synchronized between C++ and Go. **Any mismatch will cause silent data corruption or crashes.**

**Rules:**
1.  **Alignment:** Structs are padded to **128 bytes** to prevent false sharing (cache line contention).
2.  **Padding:** Never remove `_pad` fields without recalculating exact offsets.
3.  **Types:** `uint64_t` (C++) == `uint64` (Go), `std::atomic<T>` behaves like `atomic` package.

#### QueueHeader Layout
*Reference: `include/IPCUtils.h` (C++) and `go/queue.go` (Go)*

| Offset | Field | Size (Bytes) | Notes |
| :--- | :--- | :--- | :--- |
| 0 | `WritePos` | 8 | Atomic |
| 8 | `_pad1` | 56 | **Separates Write/Read Cache Lines** |
| 64 | `ReadPos` | 8 | Atomic |
| 72 | `Capacity` | 8 | Atomic |
| 80 | `ConsumerActive` | 4 | Atomic (0=Sleep, 1=Active) |
| 84 | `_pad2` | 44 | **Aligns struct to 128 bytes** |

#### BlockHeader Layout
*Reference: `include/IPCUtils.h`*
-   Total Size: **16 Bytes**
-   `size` (4), `msgId` (4), `magic` (4), `_pad` (4).

#### Magic Numbers
-   `BLOCK_MAGIC_DATA`: `0xAB12CD34`
-   `BLOCK_MAGIC_PAD`: `0xAB12CD35`

### 2.2. Synchronization Primitives

-   **Hot Path:** Do not use OS Mutexes (`std::mutex`, `sync.Mutex`) in the Enqueue/Dequeue hot loops.
-   **Spin-Wait:**
    -   C++: Use `CpuRelax()` (wraps `_mm_pause` or `yield`).
    -   Go: Use `runtime.Gosched()`.
-   **Signaling:**
    -   Hybrid approach: Spin for a set duration, then sleep on an OS Event (Semaphore/Event).
    -   Variables: `ConsumerActive` determines if the consumer needs a signal.

---

## 3. Development Guidelines

### 3.1. Code Modifications
-   **Cross-Language Changes:** If you change a header in `include/`, you **MUST** change the corresponding struct in `go/`.
-   **Platform:** Keep `Platform` implementations (Linux/Windows) consistent in behavior.

### 3.2. Performance
-   **No Logging in Loops:** Never put `fmt.Println` or `std::cout` in the critical path. It invalidates benchmarks.
-   **Allocations:** Minimize Go heap allocations in the hot loop. Use `sync.Pool`.

### 3.3. Verification
Before submitting, you must run the benchmarks to ensure no regressions.

```bash
# Run all benchmarks (Requires CMake and Go)
task run:benchmark
```

If the benchmark hangs or produces "ID Mismatch" errors, you have broken the memory layout or synchronization logic.

## 4. Common Pitfalls

1.  **Padding drift:** Adding a field to `QueueHeader` without adjusting `_pad2`.
2.  **Magic Number Mismatch:** Changing a constant in C++ but forgetting Go.
3.  **Zombie Semaphores (Linux):** If the benchmark crashes, shared memory files (`/dev/shm/*`) may remain. Use `rm /dev/shm/SimpleIPC*` to clean up.
