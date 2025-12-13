# Performance Tuning Experiments Summary

This document summarizes the performance tuning experiments conducted on the `experiments/pingpong` benchmark.

## Environment
- **Benchmark:** `experiments/pingpong` (1 Thread)
- **Baseline Metric:** System Effective OPS (Operations Per Second)

## Summary of Results

| Experiment ID | Description | Configuration / Changes | Result (OPS) | Status |
| :--- | :--- | :--- | :--- | :--- |
| **Baseline** | Initial State | Default Spin Strategy (2k loops), `-O3` | **2.19M** | - |
| **Exp 1** | Memory Order | `seq_cst` -> `acquire/release` | ~2.41M | **Reverted** (Safety risk) |
| **Exp 2** | Cache Padding | Pad `Packet` to 128B | ~2.20M | **Reverted** (No gain) |
| **Exp 3** | **Spin Strategy** | **Limit 2k -> 20k, Aggressive Steps** | **2.27M** | **Adopted** |
| **Exp 4** | **Compiler Flags** | **`-march=native -flto -funroll-loops`** | **2.53M** | **Adopted** |
| **Exp 5** | Huge Pages | `madvise(MADV_HUGEPAGE)` | ~2.21M | Rejected (Regression) |
| **Exp 6** | SW Prefetching | `_mm_prefetch` in spin loop | ~1.96M | Rejected (Regression) |
| **Exp 7** | GOAMD64 | `GOAMD64=v3` (AVX2) | ~2.47M | Rejected (No gain) |
| **Exp 8** | Go Loop Unroll | Manual 8x unroll of spin check | ~2.40M | Rejected (Regression) |
| **Exp 9** | Go PGO | Profile Guided Optimization | ~2.18M | Rejected (Regression) |

## Detailed Analysis

### 1. Spin Strategy Tuning (Adopted)
Increasing the busy-wait limit from 2,000 to 20,000 cycles significantly reduced the frequency of fallback system calls (`sem_wait`), improving latency in this low-contention scenario.
- **Change:** `experiments/pingpong/main.cpp` and `experiments/pingpong/go/main.go`

### 2. C++ Compiler Optimizations (Adopted)
Enabling architecture-specific instructions and link-time optimization provided the largest single performance boost (~10-15%).
- **Flags:** `-O3 -march=native -flto -funroll-loops`
- **Change:** `experiments/pingpong/run.sh`

### 3. Rejected Attempts
- **Memory Ordering:** Relaxing `std::memory_order_seq_cst` to `acquire/release` showed promise (~2.41M) but introduced a potential deadlock race condition in the wake-up logic, so it was reverted for correctness.
- **Go Optimizations (PGO, Unroll, AVX2):** None of the Go-specific optimizations yielded improvements. The Go runtime's scheduler and the simple nature of the memory synchronization loop seem to be already near optimal or bounded by other factors (cache coherence, syscall overhead).
