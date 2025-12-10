# Benchmark Results

## Architecture: Direct Exchange Mode

The project has transitioned to a "Direct Exchange" IPC mode, removing legacy queues in favor of a 1:1 slot mapping with adaptive hybrid waiting (Spin/Yield/Wait). The new architecture uses a shared memory layout optimized for cache-line alignment and zero-copy operations.

## Benchmark Performance

The benchmark evaluates the "System Effective OPS" (Total successful operations per second) and average Round-Trip Latency (RTT) between the C++ Host and Go Guest.

**Environment:** Sandbox (Containerized)
**Payload:** 64 bytes (Ping-Pong)

### 1 Thread
*   **Throughput**: 1,426,102 ops/s
*   **Avg Latency (RTT)**: 0.70 us

### 4 Threads
*   **Throughput**: 1,864,378 ops/s
*   **Avg Latency (RTT)**: 2.15 us

### 8 Threads
*   **Throughput**: 2,200,666 ops/s
*   **Avg Latency (RTT)**: 3.63 us

## Optimization Notes
*   **Single Thread Optimization**: `runtime.Gosched()` is automatically disabled when `NumSlots == 1`, prioritizing single-thread latency. This restores performance significantly compared to the proactive yielding baseline.
*   **Throughput Optimization**: For multi-thread scenarios (`NumSlots > 1`), `runtime.Gosched()` is enabled (every 128 iterations) to prevent starvation in oversubscribed environments.
*   **Wait Strategy**: Tuned to MaxSpin=5000 to accommodate Guest-side memory copy latency.

## Guest Call Scenario

This scenario validates the bidirectional communication capability where the Guest triggers a call back to the Host during request processing.

**Environment:** Sandbox (Containerized)
**Payload:** 64 bytes (Ping-Pong) + Guest Call

### 1 Thread
*   **Throughput**: ~950,000 ops/s
*   **Avg Latency (RTT)**: ~1.0 us
