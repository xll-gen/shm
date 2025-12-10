# Benchmark Results

## Architecture: Direct Exchange Mode

The project has transitioned to a "Direct Exchange" IPC mode, removing legacy queues in favor of a 1:1 slot mapping with adaptive hybrid waiting (Spin/Yield/Wait). The new architecture uses a shared memory layout optimized for cache-line alignment and zero-copy operations.

## Benchmark Performance

The benchmark evaluates the "System Effective OPS" (Total successful operations per second) and average Round-Trip Latency (RTT) between the C++ Host and Go Guest.

**Environment:** Sandbox (Containerized)
**Payload:** 64 bytes (Ping-Pong)

### 1 Thread
*   **Throughput**: 1,567,476 ops/s
*   **Avg Latency (RTT)**: 0.64 us

### 4 Threads
*   **Throughput**: 1,911,587 ops/s
*   **Avg Latency (RTT)**: 2.09 us

### 8 Threads
*   **Throughput**: ~2.0M OPS (Estimated based on 4T trend)
*   **Avg Latency (RTT)**: ~4.0 us

## Optimization Notes
*   **Single Thread Optimization**: Removed `runtime.Gosched()` from the hot loop to maximize single-thread throughput, restoring performance towards the Pingpong baseline.
*   **Wait Strategy**: Tuned to MaxSpin=5000 to accommodate Guest-side memory copy latency, preventing premature sleeping.
*   **Trade-off**: This optimization prioritizes low-latency single-thread operation. Heavily oversubscribed scenarios (e.g. 8 threads on 4 cores) may see slightly reduced aggregate throughput compared to proactive yielding.

## Guest Call Scenario

This scenario validates the bidirectional communication capability where the Guest triggers a call back to the Host during request processing.

**Environment:** Sandbox (Containerized)
**Payload:** 64 bytes (Ping-Pong) + Guest Call

### 1 Thread
*   **Throughput**: ~950,000 ops/s
*   **Avg Latency (RTT)**: ~1.0 us
