# Benchmark Results: SPSC vs MPSC

This document summarizes the performance comparison between the Single-Producer Single-Consumer (SPSC) queue and the newly revived Multi-Producer Single-Consumer (MPSC) queue.

## Methodology

*   **Host**: C++ (Benchmark Client)
*   **Guest**: Go (Server/Worker)
*   **Scenario**: Request-Response pattern (Call/Return).
*   **Protocol**: 32MB Shared Memory, Event-based signalling with "Signal-If-Waiting" optimization.
*   **SPSC**: Uses `std::mutex` / `sync.Mutex` to protect the SPSC queue when multiple threads are used (concurrent sending).
*   **MPSC**: Uses a Lock-Free CAS loop for Enqueue, and Spin-on-Magic for Dequeue.

## Results

### SPSC (Mutex Protected)

| Threads | Total Ops | Time (s) | Throughput (ops/s) | Avg Latency (us) |
| :--- | :--- | :--- | :--- | :--- |
| 1 | 10,000 | 1.60 | 6,231 | 160.48 |
| 4 | 40,000 | 1.43 | 27,992 | 35.72 |
| 8 | 80,000 | 1.84 | 43,570 | 22.95 |

### MPSC (Lock-Free)

| Threads | Total Ops | Time (s) | Throughput (ops/s) | Avg Latency (us) |
| :--- | :--- | :--- | :--- | :--- |
| 1 | 10,000 | 1.63 | 6,153 | 162.53 |
| 4 | 40,000 | 1.38 | 29,056 | 34.42 |
| 8 | 80,000 | 1.64 | 48,858 | 20.47 |

## Analysis

*   **Single Thread**: Performance is identical. The lock-free overhead (CAS) vs Mutex overhead is negligible in a single-threaded scenario where no contention exists.
*   **Multi-Thread (4-8)**: The MPSC implementation shows a **performance improvement** over the SPSC (Mutex) implementation.
    *   At 8 threads, MPSC achieves **~48.8k ops/s** vs SPSC's **~43.6k ops/s**.
    *   Latency is also improved (20.47us vs 22.95us).
*   **Conclusion**: The revival of the MPSC queue logic has successfully delivered better scalability for multi-threaded workloads by removing the mutex bottleneck in the producer path.
