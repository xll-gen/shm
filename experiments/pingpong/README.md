# Ping-Pong Benchmark Experiment

This experiment tests the shared memory visibility and raw performance between a C++ Host and a Go Guest using a simplified manual shared memory layout and busy-wait loops.

## Architecture

*   **Host (C++)**: Creates shared memory (`/pingpong_shm`), writes request data, sets a `REQ_READY` flag, and busy-waits for `RESP_READY`.
*   **Guest (Go)**: Attaches to shared memory, busy-waits for `REQ_READY`, processes data (sum), sets `RESP_READY`.
*   **Synchronization**: Atomic flags (`std::atomic<uint32_t>` in C++, `sync/atomic` in Go) with busy loops.
*   **Data**: A simple packet structure padded to 64 bytes.

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

## Results

| Iterations | Total Time (s) | OPS (Operations/sec) | Note |
| :--- | :--- | :--- | :--- |
| 10,000 | ~0.0042 | ~2.40 Million | Initial test |
| 100,000 | ~0.0236 | ~4.24 Million | Scale up |

## How to Run

```bash
./experiments/pingpong/run.sh
```

## Observations

*   **Latency**: Extremely low due to busy-waiting (no scheduler overhead).
*   **Throughput**: High (~4M OPS) for sequential ping-pong.
*   **Visibility**: Confirming that Go `sync/atomic` and C++ `std::atomic` interact correctly over shared memory mapped files on Linux.
