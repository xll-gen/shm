# Benchmark Results

## Architecture: Direct Exchange Mode

The project has transitioned to a "Direct Exchange" IPC mode, removing legacy queues in favor of a 1:1 slot mapping with adaptive hybrid waiting (Spin/Yield/Wait). The new architecture uses a shared memory layout optimized for cache-line alignment and zero-copy operations.

## Benchmark Performance

The benchmark evaluates the "System Effective OPS" (Total successful operations per second) and average Round-Trip Latency (RTT) between the C++ Host and Go Guest.

**Environment:** Sandbox (Containerized)
**Payload:** 64 bytes (Ping-Pong)

### 1 Thread
*   **Throughput**: 1,108,028 ops/s
*   **Avg Latency (RTT)**: 0.28 us

### 4 Threads
*   **Throughput**: 2,314,772 ops/s
*   **Avg Latency (RTT)**: 1.05 us

### 8 Threads
*   **Throughput**: 2,424,747 ops/s
*   **Avg Latency (RTT)**: 2.52 us

## Optimization Notes
*   **Tuning**: Extensive experiments (documented in `EXPERIMENTS.md`) identified that a moderate spin strategy (MaxSpin=5000) combined with a tuned Go yield frequency (every 128 iterations) provides the best balance.
*   **Throughput**: The system achieves peak throughput of **~2.4M OPS** at 8 threads, handling oversubscription robustly.
*   **Latency**: Single-thread latency is consistently sub-microsecond (~0.9us RTT implied, ~0.3us avg latency per hop reported).

## Guest Call Scenario

This scenario validates the bidirectional communication capability where the Guest triggers a call back to the Host during request processing.

**Environment:** Sandbox (Containerized)
**Payload:** 64 bytes (Ping-Pong) + Guest Call

### 1 Thread
*   **Throughput**: ~950,000 ops/s
*   **Avg Latency (RTT)**: ~1.0 us
