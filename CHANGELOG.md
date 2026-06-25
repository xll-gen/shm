# Changelog

## [v0.7.10] - 2026-06-25

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`.

### Changed

- **Removed a dead `len(reqBuf) < 24` guard in `StreamSender.Send`'s chunk
  goroutine (`go/stream_sender.go`).** The immediately preceding overflow check
  `if len(reqBuf) < chunkHeaderSize+len(chunkSlice)` already guarantees
  `len(reqBuf) >= chunkHeaderSize` (`chunkHeaderSize == 24`, pinned by the
  compile-time size assert in `stream.go`), on the same un-resliced internal SHM
  slice, so the `< 24` branch was unreachable. Pure dead-code removal — no
  behavior change. Found by a cross-repo over-defensive-logic audit (2026-06-25).
  The analogous `< 24` check on the **exported `StreamStart` / C++↔Go wire path**
  was deliberately kept (see `AGENTS.md` → Confirmed-Correct Decisions).

## [v0.7.9] - 2026-06-22

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`. (Corrects the
`VERSION` file, which had lagged at 0.7.7 through the v0.7.8 tag.)

### Changed

- **Stream reassembler brought into contract parity with the Go reference
  (R26).** `StreamReassembler` (C++) and `NewStreamReassembler` (Go) processed
  the same wire format with different limits/completion checks. They are now
  unified on a single contract, documented in `SPECIFICATION.md` §3.3.4:
  - Config defaults aligned: `maxStreamSize` 128 MiB → 1 GiB, `maxStreams`
    100 → 1024, and a new `maxStreamChunks` (1<<20) bound checked before the
    chunk-vector resize.
  - **Stricter completion (behavior change):** a stream completes only when
    `receivedChunks == totalChunks` AND `Σ payloadSize == totalSize`; a
    size-mismatch is now dropped (`SYSTEM_ERROR`) instead of delivered
    truncated. Empty-stream rule tightened to `totalChunks==0 ⇔ totalSize==0`.
  - Dedup now uses an explicit per-index presence flag instead of vector
    emptiness, fixing a C++/Go divergence where a duplicated zero-length chunk
    was double-counted. The reclaim mechanism (Go LRU vs C++ time-prune) stays
    implementation-defined per the spec.
- **`DirectHost` decomposed (R44).** The twin adaptive wait loops in
  `WaitResponse`/`WaitForSlot` are unified into a single `WaitForRespReady`
  (removes the "one path hangs" drift hazard), and the ~1280-line god header is
  split into `SlotAllocator.h` (slot state machine) + `GuestCallWorker.h`
  (guest-call worker) with `DirectHost` as a façade. Public API byte-identical;
  memory orders, the §3.6.1 claim handshake, and geometry unchanged.
- Per-field offset guards added for `SlotHeader`/`ExchangeHeader` (R25), proving
  the layout is frozen against field reordering.
- `WaitStats` counters gated behind the `shm_benchstats` build tag.
- `SPECIFICATION.md` §3 heading drift fixed; reclamation version refs corrected.

## [v0.7.8] - 2026-06-18

### Fixed

- Guest-call path now waits for a free guest slot up to the call timeout instead
  of failing immediately when all guest slots are momentarily busy (fixes RTD
  once-grid stranding under slot pressure). `SPECIFICATION.md` + `SHM_VERSION`
  comments aligned with the shipped v0.7.x layout.

## [v0.7.7] - 2026-06-13

### Changed

- **The library is now Windows-only.** All Linux/POSIX cross-compilation
  support has been removed (a deliberate reversal of the previous
  developer-convenience cross-platform target; the only production consumer,
  `xll-gen`, is Windows-only). `include/shm/Platform.h` drops its `#else`
  POSIX branch — the Win32 path (`CreateFileMapping`/`MapViewOfFile`,
  `CreateEventW`, named mutex) is now unconditional and the header stays
  header-only. The Go package no longer uses cgo at all (pure `syscall`).
  `CMakeLists.txt`, `AGENTS.md`, and `SPECIFICATION.md` updated to Windows-only.
  No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`.

### Removed

- `go/platform_linux.go` and `go/sem_lifetime_linux_test.go` (POSIX cgo impl).
- `tests/test_init_safety.cpp` (simulated POSIX FD exhaustion — meaningless on
  the Windows HANDLE model) and a stray committed test ELF binary.
- `experiments/` — the Linux-only pingpong/aeron/coro benchmark scratch and the
  `xll` experiment (all POSIX cgo, never part of the shipped library or CI).

### Fixed

- `tests/test_shutdown_hang.cpp` watchdog ported from POSIX `SIGALRM`/`alarm`
  to a portable detached `std::thread` + `std::_Exit(1)`.

## [v0.7.6] - 2026-06-13

### Fixed

- `go`: bound in-flight stream reassembly memory with a `MaxConcurrentStreams`
  cap (1024) and LRU eviction keyed on activity sequence, so a zombie/malicious
  peer that sends `StreamStart` without delivering chunks can no longer grow
  reassembly state without bound.

## [v0.7.5] - 2026-06-11

### Fixed

- `go`: atomic `SetTimeout`, idempotent `Close`, and stream-sender drain on
  close; slot-reclaim generation handling.

## [v0.7.4] - 2026-06-10

### Fixed

- Added the missing `.gitmodules` URL entry for the `benchmarks/external/aeron`
  submodule. The gitlink (commit 707fa29) was recorded without a corresponding
  `.gitmodules` entry, so `git clone --recursive` and `git submodule update`
  aborted with "No url found for submodule path". Non-recursive clones are
  unaffected; aeron is only fetched on an explicit recursive clone / submodule
  init (when building the aeron benchmark).

### Docs

- Documented that `SlotHeader` atomic-field alignment (State @64, Lease @96) is
  guaranteed by the mapping layout (`base = 64 + N*(128 + slotSize)`, slotSize%8
  enforced in `Init`), not by Go struct alignment.

## [v0.7.3] - 2026-05-17

### Cross-process crash test

`go/reclaim_crash_test.go::TestCrashProcess_ReclaimAfterChildExit` is
the v0.7 series' missing test layer: real OS-process crash injection.

- Parent creates a fresh shm (1 host slot + 1 guest slot) and per-slot
  events the same way `TestSystemErrorReal` does.
- Parent re-execs the test binary with `SHM_CRASH_TEST_WORKER=1` and the
  shm name in `SHM_CRASH_TEST_NAME`. The child opens the existing shm
  (same kernel handle/object the parent's pointing at), CAS's the guest
  slot's `State` from `SlotFree` → `SlotGuestBusy`, writes
  `Lease = MonotonicNanos()`, then `os.Exit(0)` — never releases the
  slot, modeling a guest that crashed mid-handler.
- Parent waits up to 10 s for the child to exit, asserts the slot is
  in the post-claim state with a non-zero lease, then sleeps past a
  50 ms reclamation threshold and calls `TryReclaimAbandonedSlot`.
  Asserts the slot transitions back to `SlotFree`.

This validates that the v0.7.0–v0.7.2 lease/reclamation pipeline works
across real kernel-tracked shared memory and semaphore handles, not
just in-process CAS races. Runs in ~0.5 s. Cross-platform — uses the
standard `createShm`/`createEvent` primitives via cgo (Linux) or
Win32 (Windows).

With this test in place, the v0.7-series crash-recovery feature is
feature-complete: lease (v0.7.0) + opt-in API (v0.7.1) + auto-reclaim
integration (v0.7.2) + cross-process verification (v0.7.3).

## [v0.7.2] - 2026-05-17

### Auto-Reclaim Integration

Wires the v0.7.1 `TryReclaimAbandonedSlot` API into the natural "I need
a slot but none are free" code paths, opt-in via a single knob:

- **C++**: `DirectHost::SetAutoReclaimTimeoutNs(uint64_t timeoutNs)` /
  `GetAutoReclaimTimeoutNs()`. When `AcquireSlot`'s slow-path slot scan
  completes a full sweep without finding a free slot, it walks every
  slot and calls `TryReclaimAbandonedSlot(idx, timeoutNs)`. Zero
  (default) disables auto-reclaim entirely.
- **Go**: `(*DirectGuest).SetAutoReclaimTimeout(d time.Duration)` /
  `GetAutoReclaimTimeout()`. In `sendGuestCallInternal`'s fail path
  ("all guest slots busy"), if the threshold is non-zero, walk every
  guest slot and try to reclaim, then retry the acquisition once.

Both paths gate the action on an explicit non-zero threshold so existing
deployments see no behavior change. Typical recommended value is
5× the response timeout — long enough that a slow-but-live peer always
heartbeats within the window, short enough that a true crash is
recovered quickly.

CAS guard inside `TryReclaimAbandonedSlot` protects against racing a
live heartbeat: if the peer refreshes between our observation and the
CAS, the reclaim fails and we treat the slot as still owned.

### Tests

- `go/reclaim_test.go::TestAutoReclaimTimeout_RoundTrip` pins the
  setter/getter contract (default zero, round-trips arbitrary
  duration). The race-safety semantics are still covered by
  `TestTryReclaim_NoDoubleClaim_Property` since auto-reclaim is a thin
  wrapper around the same API.

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
