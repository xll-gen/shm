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

### SMT-sibling A/B (2026-06-26, same Ryzen 9 3900X host)

`AffinityLocal` vs the new opt-in `AffinitySibling` mode (slot N → physical
core N, host on LP-low, guest on LP-high). Same `-HighPriority` harness
invocation, 10 s per cell, **best of 3** iterations. See
`EXPERIMENTS.md` §"2026-06-26 SMT-sibling co-location" for setup,
methodology, and the post-hoc analysis of why the win was larger than
predicted.

| Threads / Payload | local ops/s | sibling ops/s | Δ |
|:---|---:|---:|---:|
| 1T / 64 B   | 2,631,553 | 3,645,377 | **+38.5%** |
| 4T / 64 B   | 4,366,084 | 7,617,826 | **+74.5%** |
| 8T / 64 B   | 7,733,390 | 10,013,329 | **+29.5%** |
| 1T / 1024 B | 2,652,461 | 3,362,964 | **+26.8%** |
| 4T / 1024 B | 4,770,867 | 6,766,187 | **+41.8%** |
| 8T / 1024 B | 7,276,143 | 9,055,450 | **+24.5%** |

Spin diagnostics corroborate the cache-locality story — sibling-mode
**AvgItersPerSpin is consistently lower** because the SlotHeader cache
line is observed through shared L1d/L2 rather than crossing into a peer
L1 over the L3 ring:

| Threads | local AvgItersPerSpin | sibling AvgItersPerSpin |
|:---:|:---:|:---:|
| 1 | 18.5 | 15.0 |
| 8 | 42.5 | 38.2 |

`SleepFallback` stayed at ≈0% in both modes (the spin window resolves
before the OS-wait fallback fires).

**Default behaviour (v0.8.2+)** — `AffinityAuto` is chipset-aware: it
resolves to `AffinitySibling` automatically whenever the host reports
LTP_PC_SMT pairs and `numSlots <= len(SmtPairs())`. On the 3900X with
≤12 slots the table above is what `AffinityAuto` selects out of the box.
Surplus-slot cases fall back to `AffinityLocal` (multi-CCX) or no-pin
(monolithic / no-SMT / VM). See `AGENTS.md` §"Affinity Recommendations"
for the full resolution order and per-CPU-family guidance, and
`affinity.go::resolveAuto` for the implementation.

### Hot-path micro-optimizations + measurement-fidelity overhaul (2026-07-03)

Same Ryzen 9 3900X host, `harness.ps1 -HighPriority`, quick matrix, 10 s
cells, **best of 3**, AffinityAuto (→Sibling). Two orthogonal change groups
were A/B'd back-to-back in one session (see `EXPERIMENTS.md`
§"2026-07-03 hot-path timer/false-sharing pass" for the full method):

**Library changes** (coarse lease clock ~26→5 ns/claim; lazy acquire-timeout
QPC; `alignas(64)` host `Slot` bookkeeping), measured with the **unchanged old
benchmark binary** so the delta is purely the library:

| Cell | baseline (v0.8.3) | lib changes | Δ |
|:---|---:|---:|---:|
| 1T / 64 B   | 3,349,020 | 4,700,445 | **+40.4%** |
| 4T / 64 B   | 7,442,280 | 8,379,812 | **+12.6%** |
| 8T / 64 B   | 10,027,100 | 11,286,164 | **+12.6%** |
| 1T / 1024 B | 2,929,962 | 3,784,744 | **+29.2%** |
| 4T / 1024 B | 6,692,473 | 7,300,568 | **+9.1%** |
| 8T / 1024 B | 10,400,752 | 11,349,987 | **+9.1%** |

**Benchmark-fidelity changes** (per-thread padded non-atomic stats; 1-in-61
nanosecond latency sampling; hoisted invariant payload memcpy). The old timed
loop carried ~90 ns/op of its own overhead (2× QPC + 2 uncontended atomic
RMWs), which the old tables above therefore include. With that removed, the
same library (lib changes included) measures:

| Cell | Throughput (ops/s) | Avg RTT (µs, sampled ns) |
|:---|---:|---:|
| 1T / 64 B   | 8,366,386 | 0.16 |
| 4T / 64 B   | 14,905,702 | 0.32 |
| 8T / 64 B   | 19,059,993 | 0.44 |
| 1T / 1024 B | 6,109,365 | 0.21 |
| 4T / 1024 B | 12,169,816 | 0.37 |
| 8T / 1024 B | 17,570,169 | 0.48 |

**⚠ Baseline discontinuity:** rows in this table (and every future run of the
updated `benchmarks/main.cpp`) are **not comparable** to any table above this
section — earlier numbers under-report throughput by the removed bench
overhead and their sub-µs "Avg Latency" values were microsecond-truncation
artifacts. Compare new runs only against this section onward.

### Guest-call co-location + fast-claim/consume; stream default; scaling curve (2026-07-09, round 8)

Same host/method. Guest-call 1T/64B echo, matched pairs, best of 3 — the
worker thread and a dedicated sender goroutine are now co-located on one
physical core's SMT pair (`HostConfig::guestWorkerAffinity` + Go
`PinCurrentGoroutine`), with the S8 reclaim-machinery diet bundled:

| Guest-call 1T/64B | v0.8.8 | v0.8.9 (pin + S8) | Δ |
|:---|---:|---:|---:|
| Throughput (ops/s) | 2,750,631 | **5,192,284** | **+88.8%** |
| RTT | 364 ns | **193 ns** | |
| best-of-3 spread | ~8% | 2.2% | |

An unpinned control of the same binaries scattered 1.70M–3.16M (86% spread) —
the scheduler-placement lottery pinning eliminates.

**Stream in-flight library default 2→1**: re-measured with co-located slots
(the S6 confound removed), depth 2 loses to depth 1 on every cell (−56…−80%);
the plateau is memory-bound and the second slot both doubles the working set
and breaks co-location. Explicit `inFlight>=2` callers unaffected.

**Normal-mode scaling curve** (64B, per-pair ops/s): 1T 15.5M → 2T 10.8M →
4T 8.9M → 8T 8.4M → 12T 6.5M (absolute: 8T 66.8M, 12T **77.7M** peak). Shape
= boost-residency step + flat middle + runtime-core exhaustion at 12T, not a
software serialization point; closed as hardware-characteristic. Sibling vs
local re-measured: +69% @1T, +114% @8T. (This session ran ~10% hot vs round 7
— compare only within a session.)

### Guest responder no-reclaim fast path (2026-07-04, round 7, S7)

Same host/method, normal-mode ping-pong, best of 3, matched host+guest pairs.
The Go guest responder now skips the per-RTT gen bump + REQ_READY→GUEST_BUSY
consume-claim + lease refresh (3 locked ops) when the host publishes
`ExchangeHeader.fastPathAllowed==1` (auto-reclaim off, the default). Wire ABI
unchanged (flag carved from reserved; SHM_VERSION 0x00070000). See
`EXPERIMENTS.md` §2026-07-04 round 7.

| Cell | v0.8.7 (slow) | v0.8.8 (fast) | Δ |
|:---|---:|---:|---:|
| 1T / 64 B   | 9,883,265 | 13,982,028 | **+41.5%** |
| 4T / 64 B   | 26,318,606 | 35,354,132 | **+34.3%** |
| 8T / 64 B   | 45,180,314 | 58,425,966 | **+29.2%** |
| 1T / 1024 B | 7,690,383 | 9,669,146 | **+25.7%** |
| 4T / 1024 B | 19,119,674 | 21,908,798 | **+14.6%** |
| 8T / 1024 B | 36,408,030 | 39,284,621 | **+7.9%** |

Errors 0. This is the guest-side mirror of the host held-slot win — together
they remove the per-RTT claim from both ends of Direct Exchange when
auto-reclaim is off. The **new headline 1T/64B is ~14.0M ops/s**.

### Stream slot co-location (2026-07-04, round 6, S6)

Same host/method, stream profile (4 KiB chunks, in-flight 1, best of 2).
`StreamSender` now draws its slots from a fixed per-worker range
(`baseSlot = id*inFlight`) instead of the shared pool, so a pinned host sender
and its Go guest worker land on sibling LPs of one physical core (the win
Direct-Exchange already gets). Wire ABI unchanged.

| Stream (ops/s) | 1T pool→fixed | 4T | 8T |
|:---|---:|---:|---:|
| 64 KiB  | 58,249 → 61,498 (+5.6%) | 32,706 → 50,192 (**+53%**) | 26,712 → 45,324 (**+70%**) |
| 1 MiB   | 4,130 → 3,945 (−4.5%, noise) | 2,350 → 3,746 (**+59%**) | 2,448 → 3,610 (**+47%**) |
| 16 MiB  | 178 → 184 (+3%) | 149 → 191 (**+28%**) | 125 → 167 (**+34%**) |

The multi-thread gain is the co-location effect (1T has a single sender so
pool≈fixed; the 1MiB 1T −4.5% is within best-of-2 spread). Errors 0.

### Guest→host kernel-wake elimination (2026-07-04, round 5)

Same host/method, guest-call cell (1T/64B echo, best of 3), v0.8.5 baseline
re-measured in the same session. The C++ GuestCallWorker gained an adaptive
spin phase + `HOST_STATE_WAITING` publish, and the Go request doorbell is now
gated on it (see `EXPERIMENTS.md` §2026-07-04 round 5). Wire ABI unchanged.

| Guest-call 1T/64B | v0.8.5 | v0.8.6 | Δ |
|:---|---:|---:|---:|
| Throughput (ops/s) | 226,819 | 2,122,249 | **+836% (≈9.4×)** |

The cell moved from wake-bound (a WaitEvent round + a SetEvent syscall per
call) into the spin ping-pong regime; slowest new run (1.58M) still ~7× the
fastest baseline, 0 errors. This is xll-gen's Go→XLL RTD-update path.
Normal-mode ping-pong is unaffected (spot-check 1T/64B 10.4M, 8T/64B 50.3M).
**⚠ Guest-call baseline discontinuity:** the guest-call cell's absolute number
swings widely with host state between sessions (this session's v0.8.5 baseline
was 227K; the 2026-07-04 round-4 session measured ~393K for the same binary) —
compare only within a session's own A/B, never guest-call rows across sessions.

### Claim-cycle + benchstats-sharding + stream-reassembly pass (2026-07-04)

Same host and method (`harness.ps1 -HighPriority`, 10 s cells, best of 3,
AffinityAuto→Sibling). Three change groups A/B'd in one session against a
re-measured v0.8.4 baseline; see `EXPERIMENTS.md` §2026-07-04 for the
decomposition (Go benchstats sharding + C++ CopySmall measured via
`--legacy-claim`, then the held-slot send path on top).

**Normal mode** (bench now uses the held-slot session path by default;
`--legacy-claim` reproduces the per-op claim/free cycle):

| Cell | v0.8.4 baseline | legacy-claim (sharding+CopySmall) | held-slot (default) | total Δ |
|:---|---:|---:|---:|---:|
| 1T / 64 B   | 8,771,712  | 9,599,504  | 11,336,149 | **+29.2%** |
| 4T / 64 B   | 15,456,644 | 26,451,968 | 26,548,853 | **+71.7%** |
| 8T / 64 B   | 18,676,220 | 46,486,386 | 46,868,570 | **+150.9%** |
| 1T / 1024 B | 6,842,516  | 6,982,004  | 8,304,851  | **+21.4%** |
| 4T / 1024 B | 12,057,332 | 17,327,546 | 19,524,285 | **+61.9%** |
| 8T / 1024 B | 17,708,633 | 32,089,904 | 33,688,239 | **+90.2%** |

**⚠ 4T/8T discontinuity:** the multi-thread jump is dominated by the Go
`shm_benchstats` counter sharding — the old globally-contended counters
throttled every instrumented multi-thread cell, so 4T/8T rows above this
section under-report the library. 1T cells remain comparable to the
2026-07-03 table.

**Stream mode** (4 KiB chunks, in-flight 1, best of 2, ops/s; direct-into-
destination reassembler):

| Stream size \ Threads | 1 | 4 | 8 |
|:---|---:|---:|---:|
| 64 KiB (v0.8.4 → new)  | 30,142 → **54,204** | 24,980 → **44,884** | 20,767 → **30,577** |
| 1 MiB                  | 1,926 → **4,141**   | 2,284 → **3,049**   | 1,874 → **2,271** |
| 16 MiB                 | 106 → **186**       | 134 → **150**       | 150 → 126 |

In-flight depth 2 (the shipping library default) measured *slower* than
depth 1 on every one of these cells with the new reassembler — the bench
default stays 1 and the library default is under review (backlog; re-test
after the stream slot-affinity fix).

**Guest-call cell** (1T/64B, echo): the old hand-rolled bench listener ran at
the 15.6 ms Windows timer quantum → **~64 ops/s**; with the library
event-driven worker, response-SignalEvent gating, and the Go bench using
`SendGuestCallBuffer`: **~393,000 ops/s**. Still wake-bound (~2.5 µs/call);
the confirmed follow-up (worker spin phase + doorbell gating) is in the
backlog.

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
