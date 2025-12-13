# Benchmark Comparison: Original vs Optimized

**Environment:** Sandbox (4 Threads)

| Metric | Original (`experiments/pingpong`) | Optimized (`experiments/pingpong_optimized`) | Improvement |
| :--- | :--- | :--- | :--- |
| **System Effective OPS** | ~560,398 | ~935,963 | **+67.0%** |
| **Total Time (100k iters/thread)** | 0.714s | 0.427s | **-40.2%** |

## Analysis

The **Optimized** version introduces **128-byte alignment** (double cache line) for the `Packet` structure in shared memory.

1.  **False Sharing Elimination:**
    *   **Original:** `Packet` size was 64 bytes (exact cache line size). In a multi-threaded environment with adjacent packets, hardware prefetchers often pull in the *next* line, or slight misalignments (though checked) could cause cache line contention (False Sharing) if the CPU's coherency protocol invalidates the line due to writes in a neighbor.
    *   **Optimized:** Increasing the size to 128 bytes ensures that each thread's data resides on a distinct, isolated cache line (and potentially skips the adjacent line often hit by prefetchers). This drastically reduces the coherency traffic between cores.

2.  **Performance Gain:**
    *   The results show a massive **67% increase in throughput**. This confirms that thread contention on the shared memory lines was a significant bottleneck in the original implementation when running with 4 threads.
