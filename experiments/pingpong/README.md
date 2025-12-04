# Ping-Pong Benchmark Experiment

This experiment tests the shared memory visibility and raw performance between a C++ Host and a Go Guest using a simplified manual shared memory layout and busy-wait loops.

## Architecture

*   **Host (C++)**: Creates shared memory (`/pingpong_shm`), writes request data, sets a `REQ_READY` flag, and busy-waits for `RESP_READY`.
*   **Guest (Go)**: Attaches to shared memory, busy-waits for `REQ_READY`, processes data (sum), sets `RESP_READY`.
*   **Synchronization**: Atomic flags (`std::atomic<uint32_t>` in C++, `sync/atomic` in Go) with busy loops.
*   **Data**: A simple packet structure padded to 64 bytes (cache line aligned).
*   **Multi-threading**: Supports running multiple worker pairs (Host Thread <-> Guest Goroutine) on dedicated slots.

### Shared Memory Layout

```cpp
struct Packet {
    atomic<uint32_t> state; // 0=Wait, 1=ReqReady, 2=RespReady, 3=Done
    uint32_t req_id;
    int64_t val_a;
    int64_t val_b;
    int64_t sum;
    // Padding to 64 bytes
};
```

## Results (100,000 Iterations per Thread)

| Threads | Total OPS (Operations/sec) | Note |
| :--- | :--- | :--- |
| 1 | ~3.7 Million | Baseline |
| 2 | ~6.0 Million | Scaled (approx 1.6x) |

*Note: Results depend heavily on CPU core availability and frequency scaling.*

## How to Run

```bash
./experiments/pingpong/run.sh
```

## Observations

*   **Latency**: Extremely low due to busy-waiting (no scheduler overhead).
*   **Throughput**: Scales with thread count, limited by memory bus and cache coherence traffic.
*   **Visibility**: Confirming that Go `sync/atomic` and C++ `std::atomic` interact correctly over shared memory mapped files on Linux.
