# Performance Tuning Experiments

> **Note:** This document records historical tuning experiments (v0.1.0 - v0.2.0). For current benchmark results, please refer to [BENCHMARK_RESULTS.md](../BENCHMARK_RESULTS.md).

This document records the results of tuning experiments conducted to optimize the Direct Exchange IPC in the Sandbox environment.

## Baseline (Starting Point)
- **WaitStrategy**: Min=100, Max=5000, Start=2000, Yield=64 (Go).
- **Result**: 1T=1.14M, 4T=2.36M, 8T=1.87M.
- **Issue**: 1-thread performance (latency) is lower than expected (user report: 1.8M -> 1.0M). 8-thread performance is bottlenecked.

## Experiments

### Exp 1: Aggressive Spin (Max=100k, Start=10k)
- **Hypothesis**: Spinning longer avoids sleep and improves latency.
- **Result**: 1T=1.05M, 4T=1.47M, 8T=0.99M.
- **Conclusion**: Failure. Too much CPU usage causes contention.

### Exp 2: Low Spin (Max=2k, Start=500)
- **Hypothesis**: Yielding sooner reduces contention in oversubscribed environment.
- **Result**: 1T=1.05M, 4T=2.14M, 8T=2.37M.
- **Conclusion**: Excellent for 8 threads (best so far for throughput), but 1 thread latency still stuck.

### Exp 3: Moderate Spin + Sparse Yield (Yield=128)
- **Hypothesis**: Tuning Go yield frequency reduces scheduler overhead while maintaining responsiveness.
- **Result**: 1T=1.10M, 4T=2.31M, 8T=2.42M.
- **Conclusion**: **Best overall balance.** 8-thread throughput maximizes at 2.42M. 1-thread is stable at 1.1M.

### Exp 4: Very Sparse Yield (Yield=1024)
- **Hypothesis**: Minimizing Go scheduler calls improves single-thread latency.
- **Result**: 1T=1.13M, 4T=2.26M, 8T=2.33M.
- **Conclusion**: No significant gain over Exp 3.

### Exp 5: LockOSThread (Affinity)
- **Hypothesis**: Pinning Go routine to OS thread prevents migration overhead.
- **Result**: 1T=16k, 4T=49k.
- **Conclusion**: Catastrophic. In sandbox, pinning likely causes thread starvation if Host/Guest share a core.

### Exp 6: Infinite Spin (Max=1B)
- **Hypothesis**: Never sleeping guarantees lowest latency.
- **Result**: 1T=1.03M, 4T=1.19M.
- **Conclusion**: Degraded performance. Confirming that yield/sleep is necessary.

### Exp 7: Remove `CpuRelax` (`_mm_pause`)
- **Hypothesis**: `_mm_pause` might be expensive in VM.
- **Result**: 1T=0.44M.
- **Conclusion**: `CpuRelax` is critical for performance.

### Exp 8: Match PingPong Experiment (Yield Every Iteration)
- **Hypothesis**: The raw PingPong experiment achieved 2.5M OPS. It yields every iteration.
- **Result**: 1T=1.01M, 4T=0.36M.
- **Conclusion**: DirectHost abstraction overhead interacts poorly with frequent yielding in multi-thread.

### Exp 9: Manual Inline in Go
- **Hypothesis**: Function call overhead in `WaitStrategy` closure is the bottleneck.
- **Result**: 1T=1.02M, 4T=1.57M.
- **Conclusion**: Inlining did not help 1-thread latency significantly.

### Exp 10: Infinite Timeout (Force `sem_wait`)
- **Hypothesis**: `sem_timedwait` adds overhead vs `sem_wait`.
- **Result**: 1T=1.05M.
- **Conclusion**: Timer setup is not the bottleneck.

## Final Configuration
We selected the configuration from **Experiment 3**:
- **C++ WaitStrategy**: Min=100, Max=5000, Start=2000.
- **Go WaitStrategy**: Min=100, Max=5000, Start=2000, **Yield Frequency=128**.

This configuration delivers **~2.4M OPS** at 8 threads (peak throughput) and maintains **1.1M OPS** at 1 thread (0.9us latency). The discrepancy with raw PingPong (2.5M/2.5M) is attributed to inherent overheads in the library abstraction (bounds checking, offsets, atomic RMWs) which are magnified in the single-thread latency test.

## 2026-06-26 SMT-sibling co-location (AffinitySibling A/B)

A follow-up experiment after the v0.7.x spin-tune asked whether
explicitly co-locating each slot's C++ host worker and Go guest goroutine
on the two SMT siblings of one physical core would beat the current
CCX-wide pinning (`AffinityLocal`). The hypothesis was modest: shared
L1d/L2 should drop the SlotHeader-line ping-pong from ~30 ns intra-CCX
to ~1–2 ns store-to-load forwarding, with a likely small (5–15 %) 1T
latency win and a possible regression at 8T from shared ROB / LSQ
contention.

### Setup

- **Host**: Ryzen 9 3900X (12 cores / 24 LPs, 4 CCXs × 3 cores), Windows.
- **Topology probe**: `GetLogicalProcessorInformationEx(RelationProcessorCore)`
  on both sides — `shm/go/platform_windows.go::enumerateSmtPairs` and
  `Platform::EnumerateSmtPairs`. Filters LTP_PC_SMT cores with exactly
  2 LPs in a single processor group. Lowest bit → host LP, next-lowest
  → guest LP. Both sides derive the same ordering.
- **Mode wiring**: `AffinityMode AffinitySibling = 3` (Go), `--affinity sibling` (C++).
- **Harness**: `benchmarks/harness.ps1 -Threads 1,4,8 -Payloads 64,1024 -Duration 10 -Repeats 3 -HighPriority` for both `local` and `sibling`. Ultimate Performance power plan, process priority HIGH, `shm_benchstats` for spin diagnostics.

### Result (best-of-3 ops/s)

| Cell | local | sibling | Δ |
|---|---:|---:|---:|
| 1T / 64 B   | 2,631,553 | 3,645,377 | **+38.5%** |
| 4T / 64 B   | 4,366,084 | 7,617,826 | **+74.5%** |
| 8T / 64 B   | 7,733,390 | 10,013,329 | **+29.5%** |
| 1T / 1024 B | 2,652,461 | 3,362,964 | **+26.8%** |
| 4T / 1024 B | 4,770,867 | 6,766,187 | **+41.8%** |
| 8T / 1024 B | 7,276,143 | 9,055,450 | **+24.5%** |

`AvgItersPerSpin` dropped in every sibling-mode cell measured (1T: 18.5 → 15.0; 8T: 42.5 → 38.2), corroborating the cache-locality story — fewer spin iters were needed before the consumer observed the producer's state-line write. `SleepFallback ≈ 0%` in both modes.

### Why the win was larger than predicted

The pre-experiment analysis attributed the expected gain solely to L1d/L2 cache locality. The measured **+74 %** at 4T / 64 B forced a re-read:

1. **CCX-wide masks let the OS scheduler put host and guest on the *same* LP.** The 6-LP mask of one CCX has 36 possible (host LP, guest LP) placements; only ~5/6 of them avoid same-LP collision. When a collision occurs, one endpoint must wait for the other's quantum to end before progressing — a multi-µs stall on a sub-µs ping-pong. Over a 10 s run the stalls compound.
2. **Sibling mode makes collision impossible by construction.** Single-bit masks force the two endpoints onto distinct LPs of the same physical core. Both threads always have execution resources during the partner's PAUSE.
3. **Cache locality is a real but secondary effect.** The `AvgItersPerSpin` drop (15–20 %) accounts for the 1T / 64 B win (+38 %) only partially; the bulk comes from collision elimination.
4. **The expected ROB / LSQ contention loss did not materialise at the payload sizes tested.** Even at 1024 B (where each side memcpys the request and the response into the shared slot), the cache-locality + collision-elimination wins dominate the pipeline-sharing loss. Larger payloads or asymmetric workloads with heavier consumer-side compute may behave differently.

### What stays open

- Hybrid Intel (Alder Lake P+E) hosts: `enumerateSmtPairs` skips E-cores (no LTP_PC_SMT). Untested whether the surplus-slots-round-robin behaviour over P-cores keeps the win.
- Realistic asymmetric workloads (e.g. RTD subscriber that mutates a hashmap on each response). Pure ping-pong understates the pipeline-sharing risk; we would expect the win to shrink but not invert based on the 1024 B numbers above.
- Monolithic-L3 Intel single-socket. Sibling mode is *expected* to help (still forces opposite-LP placement) but with no cross-CCD bounce to remove the magnitude should be smaller than on chiplet AMD.

### Disposition

`AffinitySibling` shipped as **opt-in in v0.8.2**, and the same release made `AffinityAuto` **chipset-aware**: when `len(SmtPairs()) > 0 && numSlots <= len(SmtPairs())`, Auto resolves to Sibling automatically. The fallback chain (Local on multi-CCX, None on monolithic / no-SMT / VM) preserves the v0.8.1 behaviour wherever Sibling is not safe — no host that pinned successfully under v0.8.1 loses its mask under v0.8.2. See `affinity.go::resolveAuto`, `AGENTS.md` §"Affinity Recommendations", and `BENCHMARK_RESULTS.md` §"SMT-sibling A/B" for the wiring and operator guidance.
