# Benchmark Comparison: Aeron vs. Raw Shared Memory

This document compares the `pingpong_aeron` experiment (using Aeron IPC) with the baseline `pingpong` experiment (using Raw Shared Memory + Semaphores/Spinning).

## Architecture Differences

| Feature | Raw Shared Memory (`pingpong`) | Aeron IPC (`pingpong_aeron`) |
| :--- | :--- | :--- |
| **Transport** | Direct access to `mmap` region | Aeron `aeron:ipc` (Shared Memory) |
| **Synchronization** | Custom `std::atomic` flags + `sem_t` (Hybrid Wait) | Log Buffers + `BusySpinIdleStrategy` |
| **Messaging** | Raw struct cast (Zero Copy) | SBE/FlatBuffers or Raw Bytes (Log Buffer copy) |
| **Safety** | Manual memory management (risk of corruption) | Robust (Term buffers, heartbeats, flow control) |
| **Complexity** | High (manual sync logic) | Medium (Client API) |

## Performance Expectations

Since the live benchmark requires a running Aeron Media Driver and the Aeron C++ client library installed, which are not present in this sandbox environment, the following are expected results based on typical high-frequency trading (HFT) system characteristics:

### 1. Latency (Round-Trip Time)
*   **Raw SHM:** Extremely low (~500ns - 1µs). This is the theoretical minimum as it involves only memory barriers and pointer arithmetic.
*   **Aeron IPC:** Very low (~1µs - 3µs). Aeron introduces slight overhead for writing to the Term Buffer header and flow control checks, but it is highly optimized for cache friendliness.

### 2. Throughput (System Effective OPS)
*   **Raw SHM:** ~2.5M - 3.5M OPS (Single Thread). Limited mostly by memory bandwidth and CPU pipelining.
*   **Aeron IPC:** ~2.0M - 3.0M OPS. Slightly lower than raw memory due to the "safety rails" (sequence number generation, term rotation checks), but scales better across processes due to robust contention handling.

## Running the Benchmark

To run this comparison on a machine with Aeron installed:

1.  **Build Aeron C++ Driver & Client**: Follow instructions at [RealLogic/Aeron](https://github.com/real-logic/aeron).
2.  **Start Media Driver**: Run the C++ or Java Media Driver.
3.  **Build this experiment**:
    ```bash
    mkdir build && cd build
    cmake .. -DCMAKE_PREFIX_PATH=<path_to_aeron_install>
    make
    ```
4.  **Run**:
    ```bash
    ./run.sh <num_threads>
    ```

## Conclusion

*   **Use Raw SHM** (this library) when you have strict control over both processes, need absolute minimum latency (<1µs), and can tolerate the complexity of manual synchronization.
*   **Use Aeron** when you need reliable message streams, multiple subscribers, persistence, or cross-machine capability with near-raw-shm performance.
