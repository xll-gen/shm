# AGENTS.md

This document provides context, rules, and guidelines for AI Agents working on this codebase.
**CRITICAL:** If you modify code that affects architecture, memory layout, or build processes, you **MUST** update this file and `README.md` to reflect those changes.

## 1. Project Overview

This project implements a high-performance, lock-free Shared Memory IPC (Inter-Process Communication) library between **C++ (Host)** and **Go (Guest)**.

-   **Host (C++):** Creates shared memory, acts as the server/initiator.
-   **Guest (Go):** Attaches to shared memory.
-   **Philosophy:** Zero-copy (where possible), cache-line aligned, direct slot exchange.

### Architecture (Updated)
-   **Direct Mode Only:** Each worker thread maps 1:1 to a specific slot.
-   **PingPong Logic:** Uses a simplified State Machine (`WaitReq` -> `ReqReady` -> `RespReady`).
-   **Split Buffer:** Each slot is divided into `Header` (128B), `ReqBuffer` (Half Slot), `RespBuffer` (Half Slot) to enable Zero Copy.

### Directory Structure

-   `src/`: C++ Host implementation (`IPCHost`, `DirectHost`, `Platform`).
-   `include/`: Shared C++ headers (`IPCUtils.h` - **CRITICAL**).
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

#### SlotHeader Layout (Direct Mode)
*Reference: `include/IPCUtils.h` (C++) and `go/direct.go` (Go)*

| Field | Size (Bytes) | Notes |
| :--- | :--- | :--- |
| `pre_pad` | 64 | **Padding to avoid false sharing** |
| `State` | 4 | Atomic (`SLOT_WAIT_REQ` etc.) |
| `ReqSize` | 4 | |
| `RespSize` | 4 | |
| `MsgId` | 4 | |
| `HostSleeping` | 4 | Atomic (1 = Sleeping) |
| `GuestSleeping` | 4 | Atomic (1 = Sleeping) |
| `padding` | 40 | **Aligns struct to 128 bytes** |

#### Slot Data Layout
`[SlotHeader (128B)] [ReqBuffer (HalfSize)] [RespBuffer (HalfSize)]`

### 2.2. Synchronization Primitives

-   **Hot Path:** Do not use OS Mutexes in the hot loops.
-   **Spin-Wait:**
    -   C++: Use `Platform::ThreadYield()` (wraps `sched_yield` or `_mm_pause`).
    -   Go: Use `runtime.Gosched()`.
-   **Signaling:**
    -   Hybrid approach: Adaptive Spin -> Sleep on Semaphore.
    -   Atomic Flags (`HostSleeping`, `GuestSleeping`) used to avoid unnecessary syscalls.

---

## 3. Development Guidelines

### 3.1. Code Modifications
-   **Cross-Language Changes:** If you change a header in `include/`, you **MUST** change the corresponding struct in `go/`.
-   **Platform:** Keep `Platform` implementations (Linux/Windows) consistent in behavior.

### 3.2. Performance
-   **No Logging in Loops:** Never put `fmt.Println` or `std::cout` in the critical path. It invalidates benchmarks.
-   **Allocations:** Minimize Go heap allocations in the hot loop.

### 3.3. Verification
Before submitting, you must run the benchmarks to ensure no regressions.

```bash
# Run all benchmarks
./benchmarks/run_bench.sh
```

If the benchmark hangs or produces "ID Mismatch" errors, you have broken the memory layout or synchronization logic.

## 4. Common Pitfalls

1.  **Padding drift:** Adding a field to structs without adjusting padding.
2.  **Zombie Semaphores (Linux):** If the benchmark crashes, shared memory files (`/dev/shm/*`) may remain. Use `rm /dev/shm/SimpleIPC*` to clean up.
