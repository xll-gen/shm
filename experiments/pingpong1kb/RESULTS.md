# PingPong 1KB Benchmark Results

## Configuration
- **Payload Size:** 1024 bytes (Total Packet Size: 1088 bytes)
- **Verification:** Host generates random data; Guest performs bitwise NOT; Host verifies response.
- **Iterations:** 50,000 per thread.
- **Environment:** Linux (via `/dev/shm` and POSIX Semaphores).

## Performance Results

| Threads | Total Time (s) | System Effective OPS | Notes |
| :--- | :--- | :--- | :--- |
| **1** | 0.401 | 124,663 | Baseline latency for 1KB round-trip |
| **2** | 0.992 | 100,820 | |
| **3** | 1.537 | 97,610 | Slight contention observed |

## Analysis
1.  **Data Integrity:**
    -   All operations passed strict memory verification checks.
    -   The Host successfully verified that `Response == ~Request` for every 1KB payload, confirming that the memory layout and synchronization logic (spin-wait + semaphore) effectively prevent tearing and race conditions.

2.  **Throughput vs. Payload:**
    -   Compared to standard small-packet pingpong, the 1KB payload introduces memcpy overhead.
    -   Achieving ~100k OPS with verified 1KB transfers demonstrates high efficiency for larger messages.

3.  **Scaling:**
    -   Aggregate throughput shows mild degradation as threads increase. This is expected as the memory bandwidth usage increases (reading/writing 1KB buffers) and cache coherence traffic grows between cores.
