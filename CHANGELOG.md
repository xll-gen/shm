# Changelog

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
