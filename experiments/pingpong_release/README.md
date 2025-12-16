# Ping Pong Experiment (Release/Acquire)

This experiment replicates the standard `pingpong` benchmark but replaces the default `std::memory_order_seq_cst` with `std::memory_order_release` (for publishing) and `std::memory_order_acquire` (for consuming) in the payload transmission path.

## Key Changes

- **Host (C++):**
    - `state.store(..., release)` when signaling `STATE_REQ_READY` and `STATE_DONE`.
    - `state.load(acquire)` when checking for `STATE_RESP_READY`.
    - Note: Sleep/Wake flags (`host_sleeping`, `guest_sleeping`) retain `seq_cst` to prevent StoreLoad reordering race conditions (lost wakeups).

- **Guest (Go):**
    - Continues to use `atomic` package (effectively `seq_cst`) as Go does not expose explicit memory orderings.

## Benchmark Results (Sandbox Environment)

| Configuration | Baseline (`pingpong`) | This Experiment (`pingpong_release`) | Delta |
| :--- | :--- | :--- | :--- |
| **1 Thread** | ~2.51M OPS | ~2.62M OPS | +4.3% |
| **2 Threads** | ~2.29M OPS | ~0.32M OPS | -86.0% |
| **3 Threads** | ~1.55M OPS | ~0.31M OPS | -80.0% |

### Analysis

The single-threaded performance shows a slight improvement, likely due to reduced overhead of `release`/`acquire` compared to `seq_cst` (lock prefix/mfence on x86).

However, multi-threaded performance collapses. This suggests that the strict ordering of `seq_cst` in the baseline was masking a contention issue or race condition that becomes pathological with weaker ordering in this specific mixed-language, oversubscribed environment. It highlights that while `release`/`acquire` is theoretically faster, `seq_cst` can provide fairness or synchronization properties that prevent starvation in tight spin loops.
