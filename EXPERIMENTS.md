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

## 2026-07-03 hot-path timer/false-sharing pass (multi-agent hunt)

A five-lens multi-agent sweep (Go guest hot path / C++ host hot path /
benchmark fidelity / cache layout / stream path) with adversarial
verification produced 11 deduplicated candidates, 9 surviving. The four
smallest/highest-confidence ones were implemented and A/B'd in one session
(`harness.ps1 -HighPriority`, quick matrix, 10 s cells, best-of-3,
AffinityAuto→Sibling, Ryzen 9 3900X). Two groups were isolated by building
three binaries: baseline (v0.8.3), lib (library changes + old bench), full
(library + bench changes).

### Adopted — library (measured with the unchanged old bench)

1. **Coarse lease clock** (`Platform::MonotonicNanos`):
   `GetSystemTimePreciseAsFileTime` → `GetSystemTimeAsFileTime`. Microbench
   on this host: 26.3 → 5.3 ns/call; the call sits on every slot claim
   (§3.6 lease stamp). Same Unix-epoch timeline (coarse lags precise by at
   most one timer tick); §3.6.1 gen-CAS, not lease equality, is the ABA
   guard, so tick granularity is safe. Go side left on `time.Now()`
   (already ~6 ns via the same shared page; a direct KUSER_SHARED_DATA read
   measured 0.9 ns but the ~5 ns delta is sub-noise per RTT — not taken).
2. **Lazy acquire-timeout clock** (`AcquireSpecificSlot`): the eager
   `steady_clock::now()` (one QPC) ran on every RTT but is only needed on
   the slow path. Now latched once on first slow-path entry (latch is
   load-bearing: `retries` resets each sweep, so re-taking `start` would
   disable the timeout entirely).
3. **`alignas(64)` on host `Slot`**: sizeof was 57 — adjacent slots' per-RTT
   mutable bookkeeping (waitStrategy limit, msgSeq, activeWait) shared
   cache lines across pinned worker threads. Process-local only; ABI
   untouched.

Result (best-of-3, ops/s): 1T/64B 3.35M→4.70M (**+40.4%**), 4T/64B +12.6%,
8T/64B +12.6%, 1T/1024B +29.2%, 4T/8T/1024B +9.1%. Iteration spread ≤8%,
so all deltas are far above noise. The 1T win exceeding the ~15% static
estimate is consistent with serializing timer calls (QPC/rdtsc) costing
more inside a cache-hot IPC loop than in an isolated microbench loop.

### Adopted — benchmark fidelity (changes what the harness reports)

4. **Per-thread padded non-atomic stats + 1-in-61 ns latency sampling +
   hoisted invariant payload memcpy** (`benchmarks/main.cpp`). The old
   timed loop paid ~90 ns/op of its own overhead: 2× `steady_clock::now()`
   (QPC) + 2 atomic RMWs on one shared stats line (C++ analog of the Go
   WaitStats padding fix, v0.7.12). With it removed the same library
   measures 1T/64B 8.37M ops/s (~0.12 µs/op) and 8T/64B 19.06M ops/s —
   +78% over the lib binary at 1T/64B purely from bench overhead removal.
   Latency is now sampled in ns (stride 61); the old per-op µs cast
   truncated sub-µs RTTs to ~0. **All future numbers form a new baseline —
   do not compare against pre-2026-07-03 tables.**

### Deferred (verified survivors, not yet implemented)

- Per-WaitStrategy sharding of the Go `shm_benchstats` counters (guest-side
  analog of #4; global padded counters still take 2 LOCK XADDs/RTT across
  CCXs in harness builds).
- Stream reassembly direct-into-destination (drop per-chunk allocs + second
  copy on the Go side).
- Non-temporal (MOVNT+SFENCE) host chunk copy for stream mode.
- Go lease publish seq_cst→release downgrade (sub-noise; asm; low value).

### Refuted this round (do not re-propose on this host)

- `reqOffset=64` slot geometry to split header/data lines: ~0% on default
  cells because Sibling affinity already co-locates the endpoints on one
  physical core (shared L1d — no cross-core line transfer to save).
- Large-page (`SEC_LARGE_PAGES`) mapping for stream mode: stream plateau is
  memory-controller-bandwidth-bound (established by the maxInFlight
  experiment), not TLB-bound; plus SeLockMemoryPrivilege deployment burden.

## 2026-07-04 claim-cycle / benchstats-sharding / stream-reassembly pass (multi-agent hunt, round 4)

A fresh six-lens sweep (atomics-ordering budget / Go runtime / C++ codegen /
batching-pipelining / stream deep-dive / wake-tail-latency) produced 18
deduplicated candidates; adversarial verification confirmed 8 and refuted 10
(notably: REQ_READY seq_cst→release downgrade, KUSER_SHARED_DATA lease clock
— already sub-noise per the 2026-07-03 log — GOAMD64=v3/PGO, LazyProc.Call
alloc, MSG_TYPE_BATCH as a library change). Implemented and A/B'd this
session together with the two deferred 2026-07-03 survivors. All A/B best-of-3
(streams best-of-2), `harness.ps1 -HighPriority`, AffinityAuto→Sibling,
Ryzen 9 3900X, same-session baseline re-measured from v0.8.4 binaries.

### Adopted — normal-mode ping-pong

1. **Per-WaitStrategy benchstats sharding (Go)** + **inline small-copy fast
   path (C++ `Platform::CopySmall`)**, measured together on the legacy claim
   path: 1T/64B 8.77M→9.60M (+9.4%), 4T/64B 15.5M→26.5M (**+71%**), 8T/64B
   18.7M→46.5M (**+149%**), 8T/1024B +81%. The multi-thread explosion is the
   sharding: under `-tags shm_benchstats` every RTT fired two LOCK XADDs on
   two globally shared (padded) counter lines across all pinned cores — the
   v0.7.12 padding fixed false sharing *between* the counters but left the
   counters themselves globally contended. Counters now live value-embedded
   in each slot's `WaitStrategy` (padded to one line, tag-gated registry for
   the aggregate getters). Production builds were already zero-cost; this
   corrects the *instrumented* numbers, so 4T/8T cells form a new baseline.
   CopySmall replaces the out-of-line `call memcpy` (IAT-indirect into
   msvcrt) with an inline overlapping-window ladder for ≤256 B payload
   copies at 9 hot-path sites.
2. **Held-slot session API (S0)**: `SlotAllocator::SendHeld` +
   `DirectHost::HeldSlot` RAII — claim once, re-arm per send (publish
   REQ_READY from the parked state; consume-claim response RESP_READY→BUSY
   before dropping activeWait, closing the pre-existing
   RESP_READY/!activeWait steal window). Wire-compatible (guest only acts on
   REQ_READY; the Go guest has re-armed this way since v0.6); SPEC §3.6
   "Held-slot sessions" records the lease/reclaim obligations. Bench normal
   mode now uses it by default (`--legacy-claim` restores the old path).
   Isolated effect on top of #1: 1T/64B +18.1%, 1T/1024B +18.9%, 4T/1024B
   +12.7% (4T/8T 64B ~0% — those cells are guest-side-bound after #1).
   Regression tests: `tests/test_held_slot.cpp` (re-arm, parked-BUSY
   not stealable, stale-lease reclaimable, msgSeq-violation release).

   **Combined vs v0.8.4 baseline: 1T/64B 8.77M→11.34M (+29%), 4T/64B
   15.5M→26.5M (+72%), 8T/64B 18.7M→46.9M (+151%), 1024B column +21–90%.**

### Adopted — stream path

3. **Direct-into-destination reassembly (Go)**: the final buffer is
   preallocated at StreamStart and in-order chunks are copied straight into
   it (previously: per-chunk allocation into [][]byte + a second full copy at
   completion). Ahead-of-cursor chunks (pipelined senders) park in a lazily
   allocated side map, drained as the cursor reaches them; SPEC §3.3.4
   guards preserved (per-chunk running-length, Σ==totalSize, exactly-once;
   regression suite `stream_reassembly_order_test.go`). At in-flight 1
   (4 KiB chunks): 64 KiB streams +47–80%, 1 MiB +21–115%, 16 MiB 1T +75%
   (106→186 ops/s). 8T/16MiB read -16% best-of-2 — bandwidth-saturated cell,
   within that cell's historical spread; net strongly positive.

### Adopted — guest-call path

4. **Response SignalEvent gating (C++ `GuestCallWorker`)** — both signal
   sites now check `guestState == GUEST_STATE_WAITING` first (the same
   two-sided Dekker as the host→guest direction; the Go waiter's
   WAITING-store-then-recheck was verified present at all three sites), and
   **bench listener fidelity**: the hand-rolled 1 ms-poll listener actually
   ran at the 15.6 ms Windows timer quantum, capping the cell at **~64
   ops/s**; replaced with the library's event-driven worker
   (`DirectHost::Start`). With the Go-side lazy acquire-deadline +
   `SendGuestCallBuffer` bench fix: **~393K ops/s (≈6,100×)**. The cell is
   still wake-bound (~2.5 µs/call, one WaitEvent round per call) — the
   confirmed p1 follow-up (GuestCallWorker adaptive spin + HOST_STATE_WAITING
   publish + Go doorbell gating) is in the backlog and should move this into
   the multi-M regime.

### Reverted after measurement

- **Stream bench in-flight default 1→2** (to mirror the library
  StreamSender/stream_sender defaults): with the new reassembler, depth 2
  LOST to depth 1 on every stream-profile cell (1T/16MiB 186→139; 64 KiB
  cells −18…−36%; depth 2 also doubles numHostSlots, pushing 8T past the
  12-SMT-pair Sibling fit). Bench default restored to 1 with the measurement
  recorded in main.cpp; the *library* default (inFlight=2) is now a backlog
  review item — re-measure after the stream slot-affinity fix (S6) lands,
  since the slot scramble is a confound.

### Confirmed but deferred (backlog, verified this round)

- **p1**: guest→host kernel-wake elimination (GuestCallWorker spin phase +
  HOST_STATE_WAITING + Go doorbell gate; phase 2 wants a minor version bump).
- **p2**: stream slot/affinity co-location (StreamSender fixed slot range);
  state+gen32 single-CAS SlotHeader repack (wire-ABI, L).
- MOVNT host chunk copy remains deferred (uncertain vs the now-dominant
  reassembly win).
