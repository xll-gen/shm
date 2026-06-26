# Benchmark Results

## Architecture: Direct Exchange Mode

The project has transitioned to a "Direct Exchange" IPC mode, removing legacy queues in favor of a 1:1 slot mapping with adaptive hybrid waiting (Spin/Yield/Wait). The new architecture uses a shared memory layout optimized for cache-line alignment and zero-copy operations.

## Benchmark Performance

The benchmark evaluates the "System Effective OPS" (Total successful operations per second) and average Round-Trip Latency (RTT) between the C++ Host and Go Guest.

### Windows (Ryzen 9 3900X 12-Core) — historical, pre-v0.7.x reclamation

**Environment:** Windows 10
**CPU:** AMD Ryzen 9 3900X 12-Core Processor
**Payload:** 64 bytes (Ping-Pong)

| Threads | Throughput (ops/s) |
|:---:|:---:|
| 1 | 2,922,546 |
| 4 | 2,194,114 |
| 8 | 1,443,110 |
| 12 | 1,451,307 |
| 24 | 1,784,417 |

### Windows native — Ryzen 9 3900X 12-Core (2026-06-26, post-spin-tune)

Same physical machine as the historical row above, re-measured after the
v0.7.x reclamation / lease tracking landed and after the 2026-06-26
spin_amd64.s tuning (PAUSE every iter + PCALIGN $16 + bottom-check loop).
Run with `benchmarks/harness.ps1 -HighPriority` (Ultimate Performance power
plan, process priority HIGH) and `shm_benchstats` build tag for spin
diagnostics. Best of 2–3 iterations per cell.

**Payload:** 64 bytes (Ping-Pong)

| Threads | Throughput (ops/s) | Avg RTT (us) | AvgItersPerSpin | SleepFallback |
|:---:|:---:|:---:|:---:|:---:|
| 1 | 2,236,512 | 0.04 | 21.3 | ~0% |
| 4 | 3,033,948 | 0.84 | 56.8 | ~0% |
| 8 | 5,514,047 | 0.85 | 60.6 | ~0% |

**Payload:** 1024 bytes (Ping-Pong)

| Threads | Throughput (ops/s) | Avg RTT (us) |
|:---:|:---:|:---:|
| 1 | 2,634,709 | 0.01 |
| 4 | 3,277,946 | 0.54 |
| 8 | 4,085,823 | 1.46 |

Multi-thread throughput is 3–4× the historical numbers because the modern
Direct Exchange mode pipelines per-slot rather than funnelling through a
single MPSC queue; the spin window almost never falls through to OS-wait
(SleepFallback ≈ 0% under load).

### Streaming chunk-size sweep (1 thread, native Windows, 2026-06-26)

Stream mode total bandwidth as a function of chunk size at fixed total
payload, single thread.

| Chunk → | 4 KiB | 16 KiB | 64 KiB | 256 KiB | 1 MiB | 4 MiB | 8 MiB | 16 MiB |
|:---|---:|---:|---:|---:|---:|---:|---:|---:|
| **1 MiB stream** (ops/s) | 1,363 | 1,810 | 1,925 | 2,603 | — | — | — | — |
| **16 MiB stream** (ops/s) | — | — | 154 | 167 | 172 | 227 | 228 | 223 |
| **16 MiB stream** (GB/s) | — | — | 2.46 | 2.67 | 2.75 | 3.62 | 3.64 | 3.56 |

**Operating point:** for large streams (≥16 MiB), bandwidth plateaus near
**3.6 GB/s with 4–8 MiB chunks**. Below 4 MiB the per-chunk IPC RTT
dominates; above 8 MiB cache and slot-allocation overhead reverses the win
by a few percent. Sub-1 MiB streams benefit from chunks sized roughly to
the full payload — essentially bypassing the streaming path. Tune the
`-c <chunk-bytes>` benchmark flag (and the application's chosen chunk size)
toward 4 MiB for bulk transfer.

### Sandbox (Containerized)
**Environment:** Sandbox (Containerized, 16 vCPUs)
**Payload:** 64 bytes (Ping-Pong)

### 1 Thread
*   **Throughput**: 1,480,997 ops/s
*   **Avg Latency (RTT)**: 0.67 us

### 4 Threads
*   **Throughput**: 1,871,576 ops/s
*   **Avg Latency (RTT)**: 2.14 us

### 8 Threads
*   **Throughput**: 1,916,499 ops/s
*   **Avg Latency (RTT)**: 4.17 us

## Optimization Notes
*   **Single Thread Optimization**: `runtime.Gosched()` is automatically disabled when `NumSlots == 1`, prioritizing single-thread latency.
*   **Throughput Optimization**: For multi-thread scenarios (`NumSlots > 1`), `runtime.Gosched()` is enabled (every 4096 iterations) to prevent starvation in oversubscribed environments.
*   **Wait Strategy**: `waitStrategyMaxSpin=50000`. The 2026-06-26 native-Windows run shows AvgItersPerSpin = 21–61 with SleepFallback ≈ 0%, so the spin window is oversized for current workloads but harmless (spin exits on the first iter the condition is true).
*   **Spin Hot Path (Go asm)**: `go/spin_amd64.s` issues PAUSE every iteration (HT/SMT cache-coherency hygiene; pause-every-N variants regressed 1T throughput up to 16% with no multi-thread gain), with a 16-byte aligned loop entry and a single bottom-check (CMPQ/JL) back-edge. Same-session A/B against the prior top-check+no-align variant gave a steady +2–4% at 1T/8T 64-byte ping-pong.
*   **Streaming Chunk Size**: ~4 MiB chunks plateau the bulk bandwidth at ≈ 3.6 GB/s on this host; growing chunks beyond 8 MiB pays for itself in extra slot/copy buffer footprint with no throughput return.

## Guest Call Scenario

This scenario validates the bidirectional communication capability where the Guest triggers a call back to the Host during request processing.

**Environment:** Sandbox (Containerized)
**Payload:** 64 bytes (Ping-Pong) + Guest Call

### 1 Thread
*   **Throughput**: ~950,000 ops/s
*   **Avg Latency (RTT)**: ~1.05 us
