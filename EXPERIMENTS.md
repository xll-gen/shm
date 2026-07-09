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

## 2026-07-04 guest→host kernel-wake elimination (round 5)

The 2026-07-04 round-4 hunt confirmed (p1) that the guest-call cell was
kernel-wake-bound: the C++ GuestCallWorker only ever parked on the shared
request event (WaitEvent), and the Go sender always fired a request-doorbell
SetEvent syscall. This round implements the two halves of the fix and A/B's
the guest-call cell (Ryzen 9 3900X, `harness.ps1 -HighPriority`, 1T/64B echo,
best of 3, same-session v0.8.5 baseline re-measured).

### Adopted

1. **GuestCallWorker adaptive spin + `HOST_STATE_WAITING` publish**
   (`include/shm/GuestCallWorker.h`). The worker now runs the shared
   `WaitStrategy` spin over the "any guest slot `SLOT_REQ_READY`" predicate
   before parking; before it parks it publishes `hostState = HOST_STATE_WAITING`
   (seq_cst) on every guest slot and rechecks the predicate (Dekker), restoring
   `HOST_STATE_ACTIVE` on wake. Gated by `HostConfig::guestWorkerSpin` (default
   on); the sleep-only fallback maintains the identical publish so the sender
   gate stays correct when spin is disabled.
2. **Go request-doorbell gate** (`go/direct.go` `sendGuestCallInternal`,
   `go/guest_slot.go` `SendWithTimeout`). After the seq_cst `SLOT_REQ_READY`
   store the sender loads `hostState` (seq_cst) and signals the request event
   only when it reads `HOST_STATE_WAITING` — the guest→host mirror of the
   response-side `guestState` gate. Two-sided seq_cst Dekker: no lost wakeup
   (proven per-slot over the total order; verified by shm-protocol-guardian).

Both halves ship together (v0.8.6); host+guest must be same-tag-pinned. Wire
ABI unchanged (`SHM_VERSION` 0x00070000, no layout change — `hostState` is an
existing field, previously unused on guest slots).

Result (guest-call 1T/64B echo, ops/s, best of 3):

| | v0.8.5 baseline | v0.8.6 (spin + doorbell gate) | Δ |
|:---|---:|---:|---:|
| throughput | 226,819 | 2,122,249 | **+836% (≈9.4×)** |

Even the slowest new run (1,581,782) is ~7× the fastest baseline. The cell
moved from wake-bound (one WaitEvent round + one SetEvent syscall per call)
into the spin ping-pong regime. Errors 0. This is the guest→host RTD-update
path in xll-gen (Go backend → XLL), so it is a production win, not only a
benchmark cell. Normal-mode ping-pong is unaffected (1T/64B 10.4M, 8T/64B
50.3M — within v0.8.5 run-to-run spread; the guest-call worker is a separate
thread and the shared files gained only additive methods).

### Also added

- **`TryAcquireSlot` / `TryAcquireHeldSlot`** (non-blocking, one round-robin
  sweep via the same `tryClaimSlot` handshake, `-1` when the pool is full).
  Prerequisite for a future xll-gen held-slot hybrid that falls back to a
  per-call claim instead of blocking (backlog). Guards `numSlots == 0`.

### Deferred / follow-up

- Sleep-only (`guestWorkerSpin=false`) cross-process Dekker test: the spin
  default is covered end-to-end by the benchmark (0 errors, 2.1M ops/s) and
  the guest_call_* ctest suite; the sleep-only branch shares the identical
  publish/Dekker logic. A dedicated fake-guest-peer test is recommended but
  needs new harness scaffolding (backlog).

## 2026-07-04 stream slot co-location (round 6, S6)

The 2026-07-04 round-4 hunt flagged (S6) that stream mode scrambles the
sibling/CCX affinity that Direct-Exchange enjoys: `StreamSender` draws chunk
slots from the shared pool (`AcquireSlot`), so a pinned host sender thread's
chunks land in slots whose Go guest workers are pinned to *other* physical
cores, adding cross-core coherence traffic on every chunk. Direct-Exchange
avoids this because host worker `id` always uses slot `id`, co-located with Go
worker `id` on the two SMT LPs of one physical core (AffinitySibling).

### Adopted

`StreamSender(host, inFlight, baseSlot=-1)` gained an optional fixed slot
range: when `baseSlot >= 0` it draws slots round-robin from
`[baseSlot, baseSlot+inFlight)` via `AcquireSpecificSlot` instead of the pool.
The benchmark constructs `StreamSender(host, maxInFlight, id*maxInFlight)`, so
with the in-flight-1 default each worker uses slot `id` — restoring the sibling
co-location. Guest side unchanged; ABI unchanged (host-local policy only,
`baseSlot < 0` keeps the pool path).

Result (stream profile, 4 KiB chunks, in-flight 1, best of 2, ops/s):

| Stream | 1T (pool→fixed) | 4T | 8T |
|:---|---:|---:|---:|
| 64 KiB  | 58,249 → 61,498 (+5.6%) | 32,706 → 50,192 (**+53%**) | 26,712 → 45,324 (**+70%**) |
| 1 MiB   | 4,130 → 3,945 (−4.5%, noise) | 2,350 → 3,746 (**+59%**) | 2,448 → 3,610 (**+47%**) |
| 16 MiB  | 178 → 184 (+3%) | 149 → 191 (**+28%**) | 125 → 167 (**+34%**) |

The multi-thread win (+28…+70% across all stream sizes) is the co-location
effect: at 1T there is a single sender so pool vs fixed barely differ (the 1MiB
1T −4.5% is within best-of-2 spread); at 4T/8T the pool scrambles slots across
cores and the fix restores sibling placement. Errors 0. Even the 16 MiB
plateau cell improves at 4T/8T (co-location relieves cross-core traffic that
was capping the multi-controller bandwidth).

### Also this round

- **`test_guest_worker_sleep_only.cpp`** — covers the v0.8.6 guest-call
  doorbell gate on the `guestWorkerSpin=false` (sleep-only) worker path with a
  fake in-process guest peer: request published while the worker is parked
  (hostState=WAITING) must be serviced within the 1s park cap (a lost wakeup
  would hang). Closes the coverage gap both reviewers flagged for v0.8.6.

### Not pursued this round (documented in IMPROVEMENT_BACKLOG.md)

- **S3 (state+gen32 single-CAS repack)** — deferred: after the held-slot API
  (v0.8.5) removed the per-RTT claim from the dominant hot path and guest-call
  went spin-bound (v0.8.6), the remaining benefit is <2% on any cell, against a
  wire-ABI break and a formally-UB mixed-size-atomic (32-bit state store vs
  64-bit CAS on the same word) in the most safety-critical structure.
- **xll-gen held-slot adoption** — deferred: real UDF round-trips are µs-to-
  tens-of-µs (flatbuffer serialization + the Go handler's actual compute), so
  the ~9 ns claim saving is <1%, not worth adding a per-thread held-slot
  lifecycle + a §20 (DllMain unload) crash surface to the XLL. `TryAcquireSlot`
  /`TryAcquireHeldSlot` (v0.8.6) remain for a future tight-loop low-RTT
  host-send use case.

## 2026-07-04 guest responder no-reclaim fast path (round 7, S7)

The host held-slot API (v0.8.5) removed the per-RTT claim from the Host side of
Direct Exchange, but the Go **guest responder** (`workerLoopInternal`) still ran
a full consume-claim on every request: `claimSlotGen` (LOCK XADD on `Gen`) +
`CAS(State REQ_READY→GUEST_BUSY)` (LOCK CMPXCHG) + `Store(Lease)` (XCHGQ) — three
locked ops whose sole purpose is the crash-recovery reclaim / §3.6.1 ABA guard.

### Key observation

On a host slot serviced 1:1 by one guest worker, with the Host requester
waiting only on `RESP_READY` (never observing `GUEST_BUSY`), the only actor that
machinery protects against is the **Host-side reclaimer** — which is opt-in and
off by default (`autoReclaimTimeoutNs==0`). When reclaim is off, all three
locked ops guard a disabled feature.

### Adopted

The Host publishes a safe-by-default permission flag
`ExchangeHeader.fastPathAllowed` (offset 28, carved from `reserved`, no
SHM_VERSION bump): `1` iff auto-reclaim is off, `0` otherwise (and `0` is the
zeroed default a pre-v0.8.8 host never writes). The guest reads it once at
attach; when set, `workerLoopInternal` skips the gen bump + consume-claim CAS +
lease refresh, processing while `State` stays `REQ_READY` and publishing
`RESP_READY` exactly as before. The **data-visibility handshake is unchanged**
(acquire-load of `REQ_READY`, seq_cst `RESP_READY` publish bracket all buffer
access). A version/config mismatch degrades to the slow path — never to an
unsafe fast path (polarity), so no wire-version bump is needed. Reclaim policy
is startup-time (SPEC §3.4 / §3.6 contract). shm-protocol-guardian gave a
pre-implementation SAFE-TO-IMPLEMENT design verdict; the enumerated constraints
(safe polarity, preserved acquire/seq_cst handshake, atomic flag, startup-only)
are all honored.

Result (normal-mode ping-pong, best of 3, matched host+guest pairs):

| Cell | v0.8.7 (slow) | v0.8.8 (fast) | Δ |
|:---|---:|---:|---:|
| 1T / 64 B   | 9,883,265 | 13,982,028 | **+41.5%** |
| 4T / 64 B   | 26,318,606 | 35,354,132 | **+34.3%** |
| 8T / 64 B   | 45,180,314 | 58,425,966 | **+29.2%** |
| 1T / 1024 B | 7,690,383 | 9,669,146 | **+25.7%** |
| 4T / 1024 B | 19,119,674 | 21,908,798 | **+14.6%** |
| 8T / 1024 B | 36,408,030 | 39,284,621 | **+7.9%** |

Errors 0. The gain exceeded the ~15-25% estimate (1T/64B +41.5%), confirming
the three guest-side locked ops were a large fraction of the guest responder
RTT — the mirror of the host-side held-slot win. Coverage: the existing Go
suite builds `fastPathAllowed==0` so it exercises the slow path; new
`go/fastpath_test.go` sets the flag and drives the responder end-to-end (2
round-trips, run under -race).

### Note

Together with held-slot (host) this removes the per-RTT claim from BOTH ends of
Direct Exchange when auto-reclaim is off (the common case). With reclaim on,
both ends keep the full gen/claim/lease dance (correctness over speed), gated
by the same startup contract.

## 2026-07-09 guest-call co-location + S8 fast-claim/consume + inFlight default + scaling-gap closure (round 8)

Four items from the post-v0.8.8 improvement review, all measured same-session
(Ryzen 9 3900X, `harness.ps1 -HighPriority`, best-of-3 unless noted).

### 1. Guest-call endpoint co-location (adopted — the round's headline)

Diagnosis: guest-call RTT was ~6.5× the Direct-Exchange RTT (471 ns vs 72 ns)
with both sides spin-bound after v0.8.6. Root cause: **both guest-call
endpoints ran unpinned** — the C++ `GuestCallWorker` thread and the Go sender
goroutine float on the OS scheduler, so every call pays cross-core cache-line
transfers (and placement luck), while Direct-Exchange has enjoyed SMT-sibling
co-location since v0.8.2.

Fix: `HostConfig::guestWorkerAffinity` (mask, 0 = no pin) pins the worker
thread; new exported Go `PinCurrentGoroutine(mask)` lets a dedicated sender
goroutine pin itself. The bench wires both to the first SMT pair not used by
the Direct-Exchange workers (`pairs[NUM_THREADS % pairs]`, host LP ↔ guest LP).

Result (guest-call 1T/64B echo, with S8 below included):

| | v0.8.8 | pinned (+S8) | Δ |
|:---|---:|---:|---:|
| best of 3 | 2,750,631 | 5,192,284 | **+88.8% (≈1.9×)** |
| spread | ~8% | **2.2%** | |
| RTT | 364 ns | **193 ns** | |

A no-pin control run of the same new binaries scattered 1.70M–3.16M (86%
spread) — the placement lottery in one picture. Pinning buys both the
throughput and the determinism. S8's individual contribution is not separable
from that lottery noise; it is bundled here.

### 2. S8: guest-call reclaim-machinery diet (adopted, bundled above)

The S7 reasoning applied to the guest-call path, no new ABI:
- **Worker claim** (`ProcessGuestCalls`): the gen bump + lease refresh around
  the REQ_READY→BUSY claim exist for reclaimers only → skipped when the host's
  `autoReclaimTimeoutNs == 0`. The claim **CAS is kept** — the public API
  permits concurrent ProcessGuestCalls callers and the CAS arbitrates them.
- **Sender consume** (`sendGuestCallInternal`): under
  `fastPathAllowed && guest reclaim off`, the consume-claim (gen + CAS + lease)
  is skipped entirely; ownership is retained by holding `ActiveWait==1` (the
  zombie-steal gate) until release, which drops ActiveWait first and then
  CAS's `RESP_READY→FREE` (that order — the reverse could clobber a new
  owner's ActiveWait; the brief RESP_READY+AW==0 window is benign since the
  response was already copied out).
- **GuestSlot.Send**: keeps the consume CAS (the zero-copy caller holds the
  slot until Release) but skips gen + lease under the same condition.
New regression: `TestGuestCallFastConsume` (-race).

### 3. Library stream in-flight default 2 → 1 (adopted; behavior default change)

The deferred re-measure after S6: with the slot-scramble confound removed
(both runs co-located via baseSlot), depth 2 still loses to depth 1 on EVERY
stream cell — and by more than the old pool-path comparison suggested
(64 KiB 1T 67.5K→26.5K, −61%; 16 MiB 4T 210→93, −56%): the stream plateau is
memory-controller-bound so overlapping host/guest memcpys buys nothing, the
second slot doubles the working set, and with a fixed range it spans a second
physical core, breaking the very co-location S6 added. **C++ `StreamSender`
and Go `NewStreamSender` defaults changed 2 → 1**; explicit `inFlight>=2`
callers are unaffected.

### 4. Normal-mode multi-thread scaling gap (investigated — closed as hardware)

Per-pair throughput: 1T 15.5M → 2T 10.8M (−30%) → 4T 8.9M → 8T 8.4M →
12T 6.5M. The shape rules out a software serialization point (that degrades
progressively per thread, like the pre-sharding benchstats collapse): here a
single step lands at 2T, 4–8T is flat, and only 12T (no idle cores left for
the Go runtime) drops again. Consistent with single-core boost residency +
shared uncore/IF effects. Absolute throughput rises monotonically (8T 66.8M,
12T 77.7M — a new peak), and sibling-vs-local re-measured at +69% (1T) /
+114% (8T). Closed; reopen only with HW-counter profiling (uProf).

**Session-freshness note:** this session's absolute normal-mode numbers ran
~10% above the round-7 session (1T 15.5M vs 14.0M) with identical binaries on
the baseline side — reaffirming the within-session-A/B-only comparison rule.
