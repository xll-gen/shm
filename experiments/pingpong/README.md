# Ping-Pong Benchmark Experiment

This experiment tests the shared memory visibility and raw performance between a C++ Host and a Go Guest using a manual shared memory layout. It implements an **Adaptive Wait Strategy** to balance low latency and CPU efficiency.

## Architecture

*   **Host (C++)**: Creates shared memory (`/pingpong_shm`), writes request data, sets a `REQ_READY` flag.
*   **Guest (Go)**: Attaches to shared memory, waits for `REQ_READY`, processes data (sum), sets `RESP_READY`.
*   **Adaptive Wait Strategy (Dynamic Spin-Backoff)**:
    *   **Concept**: Detects CPU contention and dynamically adjusts the behavior per thread.
    *   **Phase 1 (Spin)**: Attempts to busy-wait for a dynamic `spin_limit` (Range: 1 to 2000).
        *   **Success**: If data arrives during spin, `spin_limit` increases (+100) to reward low-latency behavior.
        *   **Failure**: If spin times out, `spin_limit` decreases (-500) to punish "thrashing" and force earlier sleep next time.
    *   **Phase 2 (Sleep)**: If data is not ready after spinning, threads block on a POSIX Named Semaphore (CPU Efficiency).
*   **Synchronization**: Atomic flags with Acquire/Release memory ordering to ensure data visibility.
*   **Multi-threading**: Supports running multiple worker pairs (Host Thread <-> Guest Goroutine) on dedicated slots.

### Shared Memory Layout

Each slot is 64 bytes (Cache Line Aligned):

```cpp
struct Packet {
    atomic<uint32_t> state;          // 0=Wait, 1=ReqReady, 2=RespReady, 3=Done
    uint32_t req_id;
    atomic<uint32_t> host_sleeping;  // 0=Active, 1=Sleeping (Needs Signal)
    atomic<uint32_t> guest_sleeping; // 0=Active, 1=Sleeping (Needs Signal)
    int64_t val_a;
    int64_t val_b;
    int64_t sum;
    uint8_t padding[24];             // Pad to 64 bytes
};
```

## Benchmark Results (Dynamic Spin-Backoff)

Test configuration: 4-Core CPU, 100,000 iterations per thread.

| Setting (N) | Active Threads (N*2) | Total OPS | Note |
| :--- | :--- | :--- | :--- |
| **1** | 2 | **2.48 M** | Single-thread latency baseline (Spinning active) |
| **2** | 4 | **2.94 M** | **Optimal Saturation**. Threads == Physical Cores (100% Load) |
| **3** | 6 | **1.85 M** | **Oversubscription** (150% Load). Graceful degradation. |
| **4** | 8 | **1.74 M** | **Heavy Load** (200% Load). Stable performance. |

### Analysis

1.  **Optimal Saturation (N=2)**:
    *   Since the benchmark runs pairs of Host and Guest threads, `N=2` creates **4 active threads**.
    *   On a 4-core system, this achieves perfect 1:1 mapping, resulting in peak throughput (2.94M OPS) with minimal context switching.

2.  **Graceful Degradation (N=3, N=4)**:
    *   When threads exceed physical cores (e.g., N=3 creates 6 threads on 4 cores), standard spinning causes "Thrashing" (threads waste CPU waiting for descheduled partners).
    *   The **Dynamic Spin-Backoff** algorithm detects this contention (spin failures) and automatically reduces the `spin_limit`.
    *   Excess threads switch to "Immediate Sleep" mode, acting like standard mutexes. This prevents the system from locking up and maintains steady throughput (~1.7-1.8M OPS) even under 200% load.

## How to Run

```bash
# Run with specific thread count
./experiments/pingpong/run.sh 3

# Run sequence (1, 2, 3 threads)
./experiments/pingpong/run.sh
```

## Implementation Notes

*   **Race Conditions**: Fixed using `std::memory_order_acquire` in C++ spin loops to ensure payload visibility before processing state changes.
*   **Spurious Wakeups**: C++ Host uses a `while` loop around `sem_wait` to robustness against spurious wakeups (e.g., `EINTR`).
*   **CGO**: Go implementation uses CGO to access `sem_open` directly, bypassing Go's lack of native support for named POSIX semaphores.
