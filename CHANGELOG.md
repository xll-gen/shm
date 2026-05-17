# Changelog

## [v0.7.1] - 2026-05-17

### Reclamation API

`SlotHeader::lease` (introduced in v0.7.0) now has consumers.

- **C++**: `DirectHost::TryReclaimAbandonedSlot(slotIdx, maxLeaseAgeNs)`.
- **Go**: `(*DirectGuest).TryReclaimAbandonedSlot(slotIdx, maxLeaseAge)`.

Reads `state` + `lease`; if `state != FREE` and `now - lease > maxLeaseAge`
and `lease != 0`, attempts `state.compare_exchange_strong(observed, FREE)`.
Returns `true` only if the CAS succeeds. The CAS guard ensures we never
race against a live heartbeat: if a peer refreshed `state` between our
observation and the CAS, the CAS fails and we return `false`.

`lease == 0` is the v0.6.x-peer signal — we refuse to reclaim because
we cannot distinguish "never heartbeated" from "stale". v0.7.x writers
always stamp a non-zero lease on every CAS-to-non-FREE.

### Tests

`go/reclaim_test.go` covers the API contract:

- `TestTryReclaim_StaleLeaseSucceeds` — happy path, 1-second-old lease,
  100 ms threshold.
- `TestTryReclaim_FreshLeaseRefuses` — refuses when peer just heartbeated.
- `TestTryReclaim_ZeroLeaseRefuses` — refuses for v0.6.x-peer signal even
  with zero threshold.
- `TestTryReclaim_FreeSlotNoOp` — refuses when slot is already free.
- `TestTryReclaim_OutOfRangeRefuses` — bounds check.
- `TestTryReclaim_NoDoubleClaim_Property` — 200 rounds × 4 slots,
  heartbeater goroutine racing the reclaimer; asserts the invariant
  that state is always either `SlotBusy` or `SlotFree` (never a
  corrupt intermediate). Runs in ~15 s.

### Out of scope for v0.7.1

- **Auto-reclamation hook** inside `WaitStrategy::Wait`: deferred. v0.7.1
  ships the opt-in API only. Callers who want self-healing must invoke
  `TryReclaimAbandonedSlot` from their own watchdog.
- **End-to-end crash-process test**: deferred. The current property
  test stresses the concurrent CAS handshake on a single machine but
  doesn't spawn a child process and kill it mid-handler. Documented in
  AGENTS.md as a follow-up.

## [v0.7.0] - 2026-05-17

### Protocol — Heartbeat Lease (write-only)

Bumps protocol version to `0x00070000`. Carves an `atomic<uint64> lease`
out of `SlotHeader::reserved[36]` at offset 96 (with 4 bytes of natural
alignment padding before it). Total `SlotHeader` size stays 128 bytes;
the field used to be opaque reserved bytes that v0.6.x peers ignored, so
this is forward-compatible with older readers.

Every site that CAS's slot `state` to a non-FREE value now writes
`Platform::MonotonicNanos()` (C++) / `shm.MonotonicNanos()` (Go) into
`lease` immediately after the CAS:

* `DirectHost::AcquireSlot` (cached and slow paths)
* `DirectHost::AcquireSpecificSlot`
* `DirectHost::ProcessGuestCalls` (worker pickup)
* `direct.go::sendGuestCallInternal` (both Free→GuestBusy and
  RespReady→GuestBusy reclaim paths)
* `direct.go::workerLoop` (worker pickup: ReqReady→GuestBusy)
* `guest_slot.go::Acquire` (both Free→GuestBusy and RespReady→GuestBusy)

No code reads `lease` in v0.7.0 yet — the value is observable through
the field for diagnostics. v0.7.1 introduces the reclamation policy
behind a property-based crash-injection test.

### Clock contract

`lease` is **wall-clock nanoseconds since the Unix epoch** (NOT a
monotonic counter). Both sides converged on wall-clock so the value is
comparable across processes AND across languages without coordinating
clock epochs:

| Side | Source |
| :--- | :--- |
| C++ | `GetSystemTimePreciseAsFileTime` (Windows) / `clock_gettime(CLOCK_REALTIME)` (Linux) |
| Go | `time.Now().UnixNano()` |

The function names retain "MonotonicNanos" for API stability despite the
wall-clock implementation; revisit when v0.7.1 lands.

### Tests

* `go/lease_test.go`:
  * `TestSlotHeader_LeaseOffset` — locks down `Lease` at offset 96.
  * `TestMonotonicNanos_NonZero` — sanity-checks the helper across a
    1 ms sleep.
  * `TestLeaseField_WriteableViaAtomic` — exercises atomic store/load
    against the field.

## [v0.6.5] - 2026-05-17

### C++ Parity
- Collapsed C++ `WaitStrategy` (`include/shm/WaitStrategy.h`) to the same
  single-adaptive form Go got in v0.6.1: parameterless constructor,
  `constexpr int kMinSpin/kMaxSpin/kIncStep/kDecStep` baked in. No diagnostic
  instrumentation. Internal API only — no consumers used the old per-instance
  constructor parameters.

### Diagnostics (debug-only)
- `DirectHost::AcquireSlot` (`include/shm/DirectHost.h`) now ships a debug-mode
  nested-IPC deadlock detector. When compiled with `SHM_DEBUG`, the function
  counts full slot sweeps that find zero free slots; after 10 000 fruitless
  sweeps it emits a one-shot `SHM_LOG_WARN` pointing at the README's "Nested
  IPC & Recursion" guidance. Production builds (no `SHM_DEBUG`) keep the
  spin-forever behaviour bit-for-bit identical.

### Documentation
- `Platform::UnlinkNamedEvent` (C++) and `unlinkEvent` (Go) gained explicit
  ownership/lifetime docs: the host is responsible for unlinking POSIX
  named semaphores on graceful shutdown; guests must NOT unlink. Aligns
  with `DirectHost::Shutdown` which already does the right thing.
- Added `SPECIFICATION.md §6 "Future: Crash-Time Slot Cleanup"` — design
  for a heartbeat `lease` field carved from `SlotHeader::reserved[36]`.
  Layout stays 128 bytes; v0.7.0 will ship the implementation behind a
  property-based crash-injection test.

### Tests
- New Linux-tagged regression `TestSemaphoreLifetime_NoLeakAcrossRuns`
  (`go/sem_lifetime_linux_test.go`) drives 50 CreateEvent/UnlinkEvent
  cycles and asserts `/dev/shm/sem.*` is back to baseline. Catches the
  silent class of bug where forgetting `sem_unlink` lets a stale
  semaphore satisfy the next `sem_open(O_CREAT)` with leftover counts.

## [v0.6.4] - 2026-05-17

### API
- `Client.Start()` now returns `error` instead of panicking when the
  handler isn't registered. Callers that ignored the return value (Go
  allows it) keep the same happy-path behavior; misconfigured callers
  can now surface the failure gracefully. Closes the long-standing
  backlog item from AGENTS.md "Known Improvement Backlog".

## [v0.6.3] - 2026-05-17

### Hygiene
- Remove `.jules/` and `.Jules/` tracked agent-scratch dirs. v0.6.0/v0.6.1
  could not be downloaded as Go modules from case-insensitive
  filesystems (Windows) because git tracked both case variants of the
  same directory. v0.6.2 only got half the rename; v0.6.3 completes it.

## [v0.6.1] - 2026-05-17

### WaitStrategy (Go)
- Collapsed Go `WaitStrategy` to a single adaptive strategy. Removed the
  `NewWaitStrategy(enableYield bool)` flag — tuned defaults are now
  package-level constants. Internal API only.
- New asm fast-path: `spinUntilEq32` (`go/spin_amd64.s`) implements the
  inner spin loop entirely in assembly, eliminating per-iteration Go→asm
  CALL / TLS reload cost. `(*WaitStrategy).WaitState(addr, want, sleep)`
  wraps it and is used by all production Direct-Exchange callsites
  (direct.go, guest_slot.go). The closure-based `Wait` path is retained
  for non-state-equality conditions.
- Yield cadence moved from 128 to 4096 spin iterations and made always-on
  (previously gated on `numSlots > 1`). Avoids the ~5 µs Gosched tax at
  high spin counts and prevents short-timeout callers from being broken
  by aggressive yielding.

### Diagnostics (temporary)
- Added package-global `WaitStatsSpinSuccess / SleepFallback / IterCount`
  counters and matching C++ `WaitStrategyStats` struct so the benchmark
  harness can report spin-success vs sleep-fallback ratios. Expect these
  to be removed once perf tuning settles — do not depend on the names.

### Tooling
- Added `benchmarks/harness.ps1`: matrix sweep driver that builds binaries,
  runs the C++/Go ping-pong across a `(threads × payload)` grid, and emits
  `results.csv` + `summary.md` (throughput/latency pivots, peak rows,
  anomaly table) plus per-case raw logs.

## [v0.6.0] - 2025-12-14

### Streaming API (Double Buffering)
- Introduced `StreamSender` (C++) and `NewStreamReassembler` (Go) for high-throughput large data transfer.
- Implements Double Buffering (pipelining) over the Direct Exchange protocol.
- Added `MSG_TYPE_STREAM_START` (13) and `MSG_TYPE_STREAM_CHUNK` (14).

### Protocol Additions
- Formalized `MSG_TYPE_SYSTEM_ERROR` (127) as the receiver's out-of-band rejection sentinel (e.g., buffer overflow, malformed request). Senders must check `msgType` against this value before parsing response data.

### API Additions
- `DirectHost::SendAcquiredAsync`: Non-blocking send for pipelining.
- `DirectHost::WaitForSlot`: Deferred response waiting.

### ABI Safety
- Added `alignas(64)` to C++ `SlotHeader` and `static_assert`s for `SlotHeader`/`ExchangeHeader`/`StreamHeader` sizes. Layout is unchanged; these are compile-time guards against accidental drift.
- Added compile-time size assertions to Go `SlotHeader`, `ExchangeHeader`, `StreamHeader`, and `ChunkHeader` using the zero-length-array pattern.

### Documentation
- Updated `README.md` and `SPECIFICATION.md` with Streaming details.
- Codified the memory-ordering contract for the `state` synchronizing variable in `SPECIFICATION.md` §4.4 (release/acquire pairing, data-region ordering, defensive re-checks).
- Corrected `SPECIFICATION.md` §2.1 `version` field example to `0x00060000`.
- Documented `SLOT_DONE` (3) as reserved (not currently used by protocol transitions).

## [v0.5.4] - 2025-12-13

### Experimental Status
- Designated as **Experimental**. Not for production use.
- Performance tuning for containerized environments.

### Features
- **Adaptive Wait Strategy**: Unified Hybrid Spin (Spin -> Yield -> Sleep) for Host and Guest.
- **Zero-Copy Slot API**: Generic `Send` with negative size support for end-alignment (replacing `SendFlatBuffer`).
- **Safety**: Added robust checks for `Init` failures (e.g., file descriptor limits).

## [v0.5.0] - 2025-12-12

Major protocol overhaul and architecture simplification.

### Breaking Changes
- **Protocol v0.5.0**: Added Magic (`0x584C4C21`) and Version (`0x00050000`) to ExchangeHeader.
- **Removed Queues**: SPSC/MPSC implementations removed. Only **Direct Mode** is supported.
- **Header-Only C++**: Library moved entirely to `include/shm/`. `src/` directory removed.

### Features
- **Guest Call**: Allows Go Guest to initiate calls to C++ Host (Async/Callback pattern).
- **Guest Slots**: Dedicated slot range for Guest-to-Host calls.
- **Zero-Copy Support**: `ZeroCopySlot` (C++) and `GuestSlot` (Go) for direct shared memory access.
- **Taskfile**: Unified build and benchmark automation.

## [v0.2.0] - 2025-12-05

### Features
- **Guest Call**: Prototype implementation.
- **Listener API**: `DirectHost::ProcessGuestCalls` for polling Guest requests.

## [v0.1.0] - 2025-12-05

First stable release of the Shared Memory IPC library.

### Key Features
- **Direct Exchange Architecture**: 1:1 Slot Mapping for low latency.
- **Header-only C++ Host Library**: Easy integration via `include/shm/`.
- **High-Performance Go Guest**: Zero-allocation hot path, Direct Mode support.
- **Cross-Platform**: Support for Linux (shm/pthreads) and Windows (NamedSharedMemory/Events).
