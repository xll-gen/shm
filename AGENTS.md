# AGENTS.md

This document provides context, rules, and guidelines for AI Agents working on this codebase.
**CRITICAL:** If you modify code that affects architecture, memory layout, or build processes, you **MUST** update this file and `README.md` to reflect those changes.

## 1. Project Overview

This project implements a high-performance, lock-free Shared Memory IPC (Inter-Process Communication) library between **C++ (Host)** and **Go (Guest)**.

-   **Host (C++):** Creates shared memory, acts as the server/initiator.
-   **Guest (Go):** Attaches to shared memory.
-   **Philosophy:** Zero-copy (where possible), cache-line aligned, direct slot exchange, adaptive hybrid wait.

### Directory Structure

-   `include/shm/`: C++ Header-only library (`DirectHost.h`, `Platform.h`, `IPCUtils.h`, etc.).
-   `go/`: Go Guest implementation (`direct.go`, `platform.go`, `client.go`).
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
4.  **Memory Ordering:** Critical state transitions must use `seq_cst`.

#### SlotHeader Layout (Direct Mode)
*Reference: `include/shm/IPCUtils.h` (C++) and `go/direct.go` (Go)*

| Field | Size (Bytes) | Notes |
| :--- | :--- | :--- |
| `pre_pad` | 64 | **Padding to avoid false sharing** |
| `State` | 4 | Atomic |
| `ReqSize` | 4 | int32 (Negative = End-aligned) |
| `RespSize` | 4 | int32 (Negative = End-aligned) |
| `MsgId` | 4 | |
| `HostState` | 4 | Atomic |
| `GuestState` | 4 | Atomic |
| `padding` | 40 | **Aligns struct to 128 bytes** |

### 2.2. Synchronization Primitives

-   **Hot Path:** Do not use OS Mutexes in the hot loops.
-   **Adaptive Wait:**
    -   Spin Phase: Check state atomic with `CpuRelax()` / `runtime.Gosched()`.
    -   Sleep Phase: If spin fails, set `Host/GuestState = WAITING`, double-check, then sleep on OS Event/Semaphore.
    -   Wakeup: Signal Event/Semaphore only if peer is `WAITING`.
-   **Platform:**
    -   Use `Platform::SignalEvent` and `Platform::WaitEvent`.

---

## 3. Development Guidelines

### 3.1. Code Modifications
-   **Cross-Language Changes:** If you change a header in `include/shm/`, you **MUST** change the corresponding struct in `go/`.
-   **Platform:** Keep `Platform` implementations (Linux/Windows) consistent in behavior.
-   **Feature Parity:** Supported languages (C++ and Go) must achieve functional parity. If a feature is added to the Host (C++), the Guest (Go) must expose the necessary API to interact with it.

### 3.2. Performance
-   **No Logging in Loops:** Never put `fmt.Println` or `std::cout` in the critical path. It invalidates benchmarks.
-   **Allocations:** Minimize Go heap allocations in the hot loop.

### 3.3. Verification
Before submitting, you must run the benchmarks to ensure no regressions.

```bash
# Run all benchmarks (Requires CMake and Go)
task run:benchmark
```

If the benchmark hangs or produces "ID Mismatch" errors, you have broken the memory layout or synchronization logic.

## 4. Common Pitfalls

1.  **Padding drift:** Adding a field to structs without adjusting padding.
2.  **Zombie Semaphores (Linux):** If the benchmark crashes, shared memory files (`/dev/shm/*`) may remain. Use `rm /dev/shm/SimpleIPC*` to clean up.
3.  **SeqCst:** Use `std::memory_order_seq_cst` for `State` transitions to ensure visibility.

## 5. Architectural Standards

### 5.1. Header-Only C++ Library
The C++ Host library is **header-only**. All source code must reside in `include/shm/`.
-   **Do not** add `.cpp` files to the library core.
-   **Include Path:** Consumers must add the parent `include/` directory to their path and include via `#include <shm/File.h>`.

### 5.2. Direct Exchange Mode
The project exclusively uses the 'Direct Exchange' model (1:1 Slot Mapping).
-   **Queues/Lanes:** Concepts like "Queues" or "Lanes" are deprecated. Use "Slots".
-   **ZeroCopySlot:** Use `DirectHost::GetZeroCopySlot()` for zero-copy message construction.
