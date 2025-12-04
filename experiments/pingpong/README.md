# Ping-Pong Benchmark Experiment

This experiment tests the shared memory visibility and raw performance between a C++ Host and a Go Guest using a manual shared memory layout. It implements an **Adaptive Wait Strategy** to balance low latency and CPU efficiency.

## Architecture

*   **Host (C++)**: Creates shared memory (`/pingpong_shm`), writes request data, sets a `REQ_READY` flag.
*   **Guest (Go)**: Attaches to shared memory, waits for `REQ_READY`, processes data (sum), sets `RESP_READY`.
*   **Adaptive Wait Strategy**:
    *   **Phase 1 (Spin)**: Both Host and Guest spin-loop for 2000 iterations to catch immediate responses (Latency Optimization). Host uses `_mm_pause()`, Guest uses `runtime.Gosched()`.
    *   **Phase 2 (Sleep)**: If data is not ready, they set a 'Sleeping' flag and block on a POSIX Named Semaphore (CPU Efficiency).
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

## Benchmark Results (Adaptive Wait)

Test configuration: 100,000 iterations per thread.

| Threads | Total OPS (Sum of Thread OPS) | Execution Time (Wall Clock) | System Effective OPS | Note |
| :--- | :--- | :--- | :--- | :--- |
| **1** | **2,361,600** | 0.043s | **~2.36 M** | Baseline |
| **2** | **2,843,630** | 0.078s | **~2.56 M** | **Optimal Efficiency** |
| **3** | **2,770,710** | 0.120s | **~2.50 M** | Saturation begins |
| **4** | 1,474,980 | 0.282s | ~1.42 M | **Resource Exhaustion** (Thrashing) |
| **8** | 4,230,860 | 0.437s | ~1.83 M | **Severe Load Imbalance** |

### Resource Exhaustion Analysis

1.  **Saturation Point (3 Threads)**:
    *   Throughput plateaus at 3 threads. While the total operations per second remains high, the scaling efficiency drops compared to 1-2 threads.

2.  **Resource Exhaustion (4 Threads)**:
    *   Performance drops significantly (System Effective OPS drops to ~1.4M).
    *   **Cause**: The number of active threads exceeds the available physical CPU cores (likely 2 cores in the test environment). This forces frequent context switches.
    *   The "Spin" phase of the adaptive strategy becomes detrimental here, as threads waste quantum spinning instead of yielding immediately to the thread that holds the lock/data.

3.  **Load Imbalance (8 Threads)**:
    *   The "Total OPS" metric (4.2M) is misleading. It is the sum of individual thread speeds.
    *   Some threads finish very quickly (high OPS) while others starve (low OPS).
    *   The **System Effective OPS** (1.83M) confirms that the overall job completion time is slower than the 1-thread case. This confirms severe contention and scheduler thrashing.

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
