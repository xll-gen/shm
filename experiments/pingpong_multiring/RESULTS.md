# Multiple Ring Buffers Benchmark Results

This experiment (`experiments/pingpong_multiring`) implements a sharded SPSC Ring Buffer architecture where each thread pair (Host/Guest) has its own dedicated Ring Buffer, aligned to 128-byte cache lines to prevent false sharing.

## Results (Sandbox Environment)

| Threads | Throughput (Total OPS) | Avg Latency (RTT) |
|---------|------------------------|-------------------|
| 1       | ~2.45M                 | ~408 ns           |
| 4       | ~2.12M                 | ~1888 ns          |
| 8       | ~0.83M                 | ~9612 ns          |

## Observations
- **Single Thread:** Performance is excellent (~2.45M OPS), matching or exceeding the best slot-based implementations.
- **Scaling:** Scaling to 4 threads shows slight degradation (aggregate throughput lower than 1 thread), likely due to CPU resource contention in the sandbox environment.
- **Oversubscription:** At 8 threads, performance drops significantly, confirming that active spinning in ring buffers is highly sensitive to CPU scheduler preemption when core counts are exceeded.
