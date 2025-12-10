# Performance Tuning Experiments

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
