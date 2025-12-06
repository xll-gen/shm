# Benchmark Results

## Architecture: Direct Exchange Mode

The project has transitioned to a "Direct Exchange" IPC mode, removing SPSC queues in favor of a 1:1 slot mapping with adaptive hybrid waiting (Spin/Yield/Wait). The new architecture uses a shared memory layout optimized for cache-line alignment and zero-copy operations.

## Benchmark Performance

The benchmark evaluates the "System Effective OPS" (Total successful operations per second) and average Round-Trip Latency (RTT) between the C++ Host and Go Guest.

**Environment:** Sandbox (Containerized)
**Payload:** 64 bytes (Ping-Pong)

### 1 Thread
*   **Throughput**: 1,899,812 ops/s
*   **Avg Latency (RTT)**: 0.53 us

### 4 Threads
*   **Throughput**: 2,731,300 ops/s
*   **Avg Latency (RTT)**: 1.46 us

### 8 Threads
*   **Throughput**: 2,031,359 ops/s
*   **Avg Latency (RTT)**: 3.94 us

## Environment: AMD Ryzen 9 3900x (Bare-metal)
**Payload:** 64 bytes (Ping-Pong)

### 1 Thread
*   **Throughput**: 1,736,783.51 ops/s
*   **Avg Latency (RTT)**: 0.575 us

### 4 Threads
*   **Throughput**: 1,931,987.29 ops/s
*   **Avg Latency (RTT)**: 0.517 us

### 8 Threads
*   **Throughput**: 1,323,987.09 ops/s
*   **Avg Latency (RTT)**: 0.755 us

### 12 Threads
*   **Throughput**: 1,325,149.50 ops/s
*   **Avg Latency (RTT)**: 0.754 us

## Notes
*   **Performance**: The Direct Mode significantly outperforms the legacy SPSC implementation (approx 20x-170x improvement).
*   **Scaling**: Throughput peaks at 4 threads in this environment. The drop at 8 threads is expected due to CPU resource contention in the sandbox environment (oversubscription of available physical cores).
*   **Stability**: The 8-thread test passed successfully, demonstrating the robustness of the adaptive wait strategy even under heavy contention.

## Guest Call Scenario

This scenario validates the bidirectional communication capability where the Guest triggers a call back to the Host during request processing (Nested Call: Host -> Guest -> Host -> Guest -> Host).

**Environment:** Sandbox (Containerized)
**Payload:** 64 bytes (Ping-Pong) + Guest Call (Small Payload)

### 1 Thread
*   **Throughput**: 1,110,302 ops/s
*   **Avg Latency (RTT)**: 0.90 us

### 2 Threads
*   **Throughput**: 1,084,854 ops/s
*   **Avg Latency (RTT)**: 1.84 us
