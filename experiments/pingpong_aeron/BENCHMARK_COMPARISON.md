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

## Sandbox Benchmark Results

The following results were obtained by running the benchmark in the current development environment (Sandbox). Note that absolute numbers are lower than bare-metal HFT production environments due to containerization overhead.

| Metric | Raw Shared Memory (Baseline) | Aeron IPC (`pingpong_aeron`) |
| :--- | :--- | :--- |
| **Throughput (1 Thread)** | ~1.56M OPS (from memory) | **~386k OPS** (Measured) |

### Analysis of Sandbox Results
The Aeron implementation achieved ~386k OPS, which is significantly lower than the Raw SHM implementation (~1.56M OPS) in this specific environment. Key factors include:
1.  **Driver Overhead:** The Aeron Media Driver (`aeronmd`) runs as a separate process, introducing context switching even for IPC, whereas the Raw SHM implementation uses a direct single-file map.
2.  **Sandbox Limitations:** The containerized environment likely exacerbates the cost of the additional process coordination required by Aeron's driver model compared to the simpler direct memory access of the baseline.
3.  **Tuning:** The Aeron driver was run with default settings. Production deployments typically require significant tuning of Term Buffer lengths, idle strategies, and thread pinning to achieve peak performance (>2M OPS).

## Running the Benchmark

This directory contains a fully functional build of the Aeron C++ Driver and the benchmark client.

1.  **Build**:
    ```bash
    cd experiments/pingpong_aeron/build
    cmake ..
    make
    ```

2.  **Run**:
    The `run.sh` script automatically starts the local `aeronmd` instance and runs the benchmark.
    ```bash
    cd experiments/pingpong_aeron
    ./run.sh <num_threads>
    ```

## Conclusion

*   **Use Raw SHM** (this library) when you have strict control over both processes, need absolute minimum latency (<1µs), and can tolerate the complexity of manual synchronization.
*   **Use Aeron** when you need reliable message streams, multiple subscribers, persistence, or cross-machine capability with near-raw-shm performance, provided you can dedicate resources to tuning the Media Driver.
