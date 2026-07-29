# Changelog

## [0.8.18] - 2026-07-29

No wire-format/ABI change — `SHM_VERSION` is untouched, and `SlotHeader`,
`ExchangeHeader`, `StreamHeader` and `ChunkHeader` keep their layouts. One
**wire behavior** change is called out under "Parked-chunk bound" below.

### Fixed

- **`StreamStart` committed ~25 MB per stream regardless of the advertised
  `totalSize`** (SPEC §3.3.4 "Residency bound"). The C++ reassembler sized its
  reassembly state by `totalChunks`: `ctx.chunks.resize(totalChunks)` built one
  `std::vector<uint8_t>` element per chunk plus a `std::vector<bool>` presence
  bitmap. `totalChunks` and `totalSize` are **independent** header fields, so a
  peer advertising `totalSize = 1` with the maximum `totalChunks` (2^20)
  committed **25,355,264 bytes per stream** — measured, 8 streams at once — and
  ~26 GB at `maxStreams` (1024). The v0.8.17 running-byte guard is blind to this
  dimension by construction: the overhead exists at zero payload, and a
  zero-length chunk can never trip a `Σ payloadSize > totalSize` test.
  Tightening `totalChunks <= totalSize` was not an option — the SPEC allows
  zero-length chunks, so the two dimensions are genuinely independent.

  The C++ storage layout is now the Go one (`go/stream.go`): allocate a
  destination buffer of exactly `totalSize` at `STREAM_START`, copy in-order
  chunks straight into it at a running cursor, and park only ahead-of-cursor
  chunks in a sparse map. No `totalChunks`-sized allocation exists. Measured per
  stream for the case above: **25,355,264 → 0 bytes** (below the
  `PrivateUsage` granularity). `include/shm/StreamReassembler.h`; regression
  `tests/test_reassembler_tiny_stream_footprint.cpp`.

  Two backlog items closed by the same rework:

  - **Peak residency at completion was `2 × totalSize`.** Every chunk was copied
    into its own vector and then concatenated into a second full-size buffer
    while the per-chunk copies were still alive — 2 GiB for a 1 GiB stream, a
    risk the Go reassembler never had. The destination is now handed to
    `onStream` by move. Measured peak commit for a 64 MiB stream:
    **2.013 × → 1.003 ×**. Regression
    `tests/test_reassembler_completion_peak.cpp`.
  - **Per-chunk heap allocation.** `chunks[idx].assign(...)` allocated and copied
    once per chunk, and the completion concatenation copied every byte again.
    In-order chunks are now copied once, into the destination; only
    ahead-of-cursor chunks still take a parked copy.

- **Parked-chunk bound: the count dimension of the same hazard** (SPEC §3.3.4
  "Parked-chunk bound"). Sparse parking fixes the *array*, not the *entry count*:
  a peer that floods zero-length chunks under ahead-of-cursor indices grows the
  side map to `totalChunks` entries, and zero-length chunks pass the byte guard
  by construction. **Both** peers had this exposure — the Go reassembler was
  measured at 1,048,575 parked entries, **83,989,416 bytes (80.1 MiB) of live
  heap, 0 rejections**, for a stream advertising 1 byte; the reworked C++ side at
  65,712,128 bytes. Both now refuse an ahead-of-cursor chunk that would exceed
  `maxParkedChunks` / `MaxParkedChunks` and drop the stream, with the check
  running before the parked copy is allocated. Post-fix residency for the same
  flood: **80.1 MiB → 0 bytes** (Go, live heap) and **62.7 MiB → 12,288 bytes**
  (C++, private bytes).

  The bound is `maxStreamChunks / maxConcurrentStreams` = **1024** on both sides:
  the value that keeps the aggregate parked-entry count across all in-flight
  streams no larger than the chunk count of one maximal stream (~64–80 MiB worst
  case, vs. ~82 GB unbounded). `StreamSender` parks at most `maxInFlight - 1`
  chunks and the default in-flight depth is 1, so legitimate senders have ~3
  orders of magnitude of headroom.

  **This is a wire behavior change**, the only one in this release: a stream
  delivered in strictly descending index order with more than 1024 chunks was
  previously accepted and is now rejected. It is symmetric across both peers and
  normative in SPEC §3.3.4. `go/stream.go::park`,
  `include/shm/StreamReassembler.h::StreamContext::Park`; regression
  `go/stream_park_cap_test.go`, `tests/test_reassembler_park_cap.cpp`.

- **A C++ chunk-path allocation failure left the stream in the map.** The
  `catch (...)` around the per-chunk copy answered `SYSTEM_ERROR` without
  `streams.erase(it)`, so the context stayed resident — with its state not
  advanced — while every *other* error path in the same function (and the Go
  guest, which retires a corrupt stream immediately) dropped it. The judgment,
  now written into SPEC §3.3.4 as a table of which rejections drop the stream:
  **drop**. Both senders treat a chunk `SYSTEM_ERROR` as terminal for the whole
  stream and never retry an index (`Stream.h::StreamSender::Send`,
  `go/stream_sender.go::sendChunk`), so no retry can ever arrive and the retained
  context only pins `totalSize` bytes until the timeout prune.

### Documentation

- SPEC §3.3.4 gained four normative clauses that were previously implicit: the
  residency bound (state sized by `totalSize`, never `totalChunks`; no `2 ×`
  completion peak), the parked-chunk count bound, a table of **which chunk
  rejections drop the stream** and which leave it completable (framing and
  out-of-range rejections do not — both peers already behaved this way), and
  **first-arrival-wins duplicate handling**. The last one is a pure
  documentation gap: a duplicate `chunkIndex` is ACK-ignored without consulting
  its `payloadSize`, so a duplicate declaring a *different* length is neither
  counted against `totalSize` nor cause to drop the stream, because the dedup
  gate precedes the overflow guard. Both peers already did this identically; no
  code changed.

### Testing

- `tests/test_reassembler_parity_table.cpp` + `go/stream_parity_table_test.go`:
  a 20-case accept/reject table whose case list **and** expected digests are
  duplicated verbatim in both languages, so each side asserts against a constant
  instead of against the other implementation's current behavior. Captured
  against the pre-rework code first; all 20 digests are byte-identical
  pre- and post-rework and identical across C++ and Go, which is what
  demonstrates the storage rework did not move the accept/reject boundary.

## [0.8.17] - 2026-07-29

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`, `SlotHeader`
and `ExchangeHeader` are untouched, and no state *value* is added (`SLOT_DONE = 3`
has been defined on both sides since v0.6.0). An older peer is unaffected: it
waits only on `SLOT_RESP_READY`, and `SLOT_DONE` is not in its zombie-steal set,
so it neither acts on the transient state nor steals a slot parked in it.

### Fixed

- **Responder wrote header fields before proving it still owned the slot**
  (SPEC §3.4/§3.5 "Responder publish rule"). v0.8.14 sealed the publish
  *transition* — `CAS(owned → SLOT_RESP_READY)` instead of a blind store — but
  the `respSize`/`msgType` stores that **precede** that CAS stayed
  unconditional. That left the corruption only half fixed:

  1. The Guest worker claims `SLOT_REQ_READY → SLOT_GUEST_BUSY` and runs the
     handler.
  2. The Host's response timeout fires. Per §3.4 it disowns the slot *without
     writing `state`* (`SlotAllocator::FinishWait(slot, false)` only drops
     `activeWait`), leaving `GUEST_BUSY` + `activeWait == 0` — a zombie-steal
     candidate.
  3. `AcquireSlot`'s `thread_local` slot cache makes the very thread that just
     timed out the first to steal it back: `gen` CAS,
     `CAS(GUEST_BUSY → BUSY)`, write a **new** transaction's
     `reqSize`/`msgType`/`msgSeq`, publish `REQ_READY`.
  4. The slow handler returns and unconditionally writes `header.RespSize` and
     `header.MsgType` — onto that new transaction. The following publish CAS
     fails and the response bytes are correctly dropped, **but the header is
     already poisoned.**
  5. Each host slot is serviced by exactly one worker goroutine, so the *same*
     goroutine reads the poisoned `MsgType` on its next iteration, in program
     order and deterministically. The new request is dispatched to the wrong
     handler branch. `msgSeq` is untouched (a responder never writes it), so the
     Host's `msgSeq` guard validates — a silently wrong value in a cell.

  Both responders now publish in **two phases**: compute the response metadata
  into locals, `CAS(owned → SLOT_DONE)` to lock ownership, and only then write
  the header, followed by `CAS(SLOT_DONE → SLOT_RESP_READY)` (`seq_cst`, so the
  §4.2 doorbell Dekker is unaffected). A failed lock writes **nothing** and
  drops the response. `SLOT_DONE` is usable as the lock precisely because it is
  outside the §3.6.1 zombie-steal set `{REQ_READY, RESP_READY, GUEST_BUSY}`, so
  a slot parked there cannot be stolen mid-publish. Go
  `go/direct.go::lockForPublish`/`publishResponse`; C++
  `include/shm/GuestCallWorker.h::LockForPublish`/`PublishResponse` and
  `ProcessGuestCalls`.

  The steal set itself is deliberately **unchanged**: SPEC §3.4 defines
  `SLOT_GUEST_BUSY` as stealable, and that is the responder's ownership token,
  not reclaim machinery. Narrowing it would invert a documented, tested design
  decision and change Host slot-recovery semantics.

- **Panic recovery completed a transaction that had never run.** The Go worker's
  `recover()` treated `state == SlotReqReady` as "we still hold the slot" and
  stamped `MsgType = SYSTEM_ERROR`, `RespSize = 0`, publishing with
  `CAS(observed → SlotRespReady)`. But the worker claims
  `REQ_READY → GUEST_BUSY` *before* it touches anything, so seeing `REQ_READY`
  at panic time means the slot was stolen and republished under a **different**
  transaction — and the CAS then *succeeded*, completing as a system error a
  request the handler was never even invoked for. Recovery now publishes only
  from the owned `SlotGuestBusy`, through the same two-phase lock.

- **Stream reassembly: the `totalSize` overflow guard did not cover
  out-of-order chunks** (SPEC §3.3.4 per-chunk overflow guard, MUST). Both
  peers checked the running assembled length only where a chunk landed at the
  in-order cursor. A chunk that arrived *ahead* of the cursor was buffered on
  nothing but an index-range and duplicate check — neither of which involves
  `totalSize` — so a peer could advertise `totalSize = 1` with
  `totalChunks = 1000` and then stream large chunks under indices `1..999`:
  every payload was copied and retained, and the byte-sum was only questioned
  once index 0 finally arrived. The final verdict (`Σ payloadSize == totalSize`)
  was correct, so no wrong value was ever delivered, but rejection happened
  *after* the bytes were resident — the unbounded allocation §3.3.4 exists to
  prevent, multiplied by `maxConcurrentStreams`. Measured on the Go side:
  33 MB retained for a stream advertising 1024 bytes (scaled-down repro; the
  real bound is `maxStreamSize` per stream). The C++ reassembler had no
  running-byte accounting at all, and its comment at the completion loop
  *claimed* a per-chunk guard existed — that comment is corrected.

  Both sides now carry an order-independent received-byte counter and apply the
  guard **before** the payload is copied: Go `streamContext.oooBytes` +
  `fits()`, checked in `park()` and `consume()` (`go/stream.go`); C++
  `StreamContext::receivedBytes`, checked ahead of the
  `chunks[idx].assign(...)` (`include/shm/StreamReassembler.h`). Draining a
  parked chunk into the destination releases its `oooBytes` first, so a
  well-formed out-of-order stream whose sum equals `totalSize` exactly still
  completes. **Wire-neutral**: no new field, no new message type, `SHM_VERSION`
  unchanged — the only observable difference is *when* a violating stream is
  refused. No conforming sender is affected; the library's own `StreamSender`
  never trips it.

### Tests

- `go/fastpath_test.go::TestGuestResponderSteal_NoHeaderWriteBeforeOwnership` —
  replays the §3.4 timeout steal with **different** `msgType`s on the two
  transactions (the pre-existing
  `TestGuestResponderFastPath_HostTimeoutStealDoesNotCorrupt` used
  `MsgTypeNormal` for both and asserted only on `RespSize`/bytes, so it could
  not observe `msgType` poisoning at all — a SCOPE LIMIT note now records that
  and points at the new test). Asserts the individual acts per the §3.5
  disown-terminal lesson: the header snapshot taken at txn#2's dispatch must
  carry txn#2's own `msgType`/`msgSeq`/`reqSize` and an untouched `respSize`
  sentinel.
- `go/fastpath_test.go::TestGuestResponderPanic_DoesNotStampErrorOnStolenSlot` —
  panics a handler whose slot has already been stolen and republished, and
  asserts the new transaction is *dispatched to the handler* rather than
  completed as `SYSTEM_ERROR`.
- `go/fastpath_test.go::TestGuestResponderTwoPhasePublish_RingsDoorbell` — the
  transient `SLOT_DONE` must not swallow the §4.2 doorbell; the mock host parks
  on the response event only and the host must never observe `SLOT_DONE`.
- `tests/test_guest_call_publish_lock.cpp` — the C++ mirror (guest-call
  direction, owned state `SLOT_BUSY`), driving the response-overflow branch so
  the `msgType = SYSTEM_ERROR` write is the poisoning vector.
- `go/stream_ooo_guard_test.go` — the out-of-order overflow guard.
  `TestStreamReassembly_OutOfOrderRunningLengthOverflow` (table-driven: first
  chunk already over, accumulation across several parked chunks, parked bytes
  constraining a later in-order chunk, zero-length park) and
  `TestStreamReassembly_OutOfOrderOverflowBoundsMemory`, which asserts on
  `MemStats.TotalAlloc` — cumulative and GC-independent, so it observes the
  parked copies even after they become garbage (33 MB before, < 1 MiB after).
  The counterpart no-regression tests are the load-bearing ones:
  `TestStreamReassembly_OutOfOrderPatternsComplete` (uneven chunk sizes
  including a zero-length one, delivered reversed / odd-then-even /
  drain-then-park / three fixed shuffles) and
  `TestStreamReassembly_OutOfOrderExactFillAccepted` (the `>` vs `>=`
  boundary). Verified by mutation: dropping the `oooBytes -=` on drain fails
  every pattern subtest plus four pre-existing ones.
- `tests/test_reassembler_ooo_overflow.cpp` — the C++ mirror of all of the
  above, driving `StreamReassembler::Handle()` with crafted wire buffers; plus
  an out-of-order overflow parity case added to
  `tests/test_reassembler_contract.cpp` so the Go/C++ accept-reject tables stay
  aligned.

### Known residuals (documented in SPEC §3.4, not fixed here)

- **Torn request bytes.** During the theft window the new owner overwrites
  `reqBuffer` while the old handler still reads the same slice, so a handler can
  parse a mid-flight mutation. This is a consequence of keeping
  `SLOT_GUEST_BUSY` stealable; the mitigation is panic recovery plus the publish
  drop.
- **`SLOT_DONE` is reclaimable.** `TryReclaimAbandonedSlot` treats every
  non-`SLOT_FREE` state as a candidate, so a stale lease plus an armed reclaimer
  can free a slot inside the two-atomic-op `SLOT_DONE` window; the publish CAS
  then fails and the response is dropped rather than mis-delivered.

## [0.8.16] - 2026-07-26

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`, `SlotHeader`
and `ExchangeHeader` are untouched. Guest-side only; the byte stream a peer
sees is unchanged (the fix *removes* writes the protocol never sanctioned).

### Fixed

- **`GuestSlot` forfeit is now terminal, matching C++** (SPEC §3.5 "Disown is
  terminal"). `GuestSlot.lost` — set when a `Send` times out or loses its
  consume-claim to a reclaimer — was consulted in exactly one place,
  `Release()`. Every other accessor ignored it, so a caller that retried
  `Send` on a forfeited handle wrote `ReqSize`/`MsgType`/`MsgSeq`, stored
  `ActiveWait = 1` and published `SlotReqReady` **into a slot the Case-2
  zombie steal or lease reclamation may already have handed to a new owner** —
  silently destroying that owner's in-flight transaction, past the `MsgSeq`
  guard (the intruder rewrites `MsgSeq` too). `Send`/`SendWithTimeout` now
  return an error before touching shared memory, and
  `RequestBuffer`/`ResponseBuffer` return `nil`. This is the parity fix for
  what C++ `ZeroCopySlot`/`HeldSlot` have always done via `slotIdx = -1` +
  `IsValid()`. A `Send` after `Release()` also returns an error instead of
  nil-dereferencing. Regression:
  `go/guest_slot_forfeit_test.go::TestGuestSlot_ForfeitedHandleCannotClobberNewOwner`
  (deterministic — it does not depend on `-race` catching an interleaving).
- **`go test -race` no longer misreports every guest-slot handoff.** Slot
  ownership is handed over through the atomics on `SlotHeader.State`, which
  lives in the file-mapped region. The Go race detector drops every
  acquire/release annotation whose address falls outside the Go arena and data
  segment (`runtime.isvalidaddr`), so those handoffs were invisible to it and
  the next owner's perfectly legal use of `slotContext.nextMsgSeq` was reported
  as a data race — reproducible at HEAD with
  `go test ./go/... -race -run TestStreamSender_ChunkRejection_ReturnsError -count=12`.
  The handoff is now mirrored on a Go-heap address at the real
  release/acquire points (`releaseSlotHandoff`/`acquireSlotHandoff`,
  `go/race_annotate_on.go`). Annotations only: `//go:build !race` compiles them
  to empty inlined functions, so production builds are byte-identical and the
  v0.8.9 hot-path baselines are untouched. Supersedes the test-local `raceHB`
  workaround in `zombie_steal_test.go`.

## [v0.8.15] - 2026-07-26

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`. Pure Go API
addition.

### Added

- **`Client.MaxRequestSize()` / `DirectGuest.MaxRequestSize()`** — the raw
  capacity of a slot request buffer, read straight from the geometry captured
  at attach. Until now the only way to learn it was to claim a slot and read
  `RequestBuffer()`, so consumers computing a chunk budget had to acquire and
  immediately release a slot, and xll-gen carried that probe in two places.
  Host and guest slots share the value (both sides derive it once from the
  exchange header), so the accessor needs no slot index. Framing overhead is
  deliberately NOT deducted — the caller subtracts its own header, and must not
  double-count shm's 24-byte streaming ChunkHeader, which StreamSender already
  subtracts internally. Returns 0 for a nil or unconnected receiver rather than
  panicking.

## [v0.8.14] - 2026-07-26

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`, `SlotHeader`
and `ExchangeHeader` are untouched, and no state value was added. Both sides of
the fix are publish-side only, so a peer that has not taken the fix sees an
identical byte stream.

### Fixed

- **Responder published `SLOT_RESP_READY` blindly, clobbering a stolen slot's
  new transaction** (SPEC §3.4/§3.5 "Responder publish rule"). The Guest
  responder (`go/direct.go::workerLoopInternal`) and its C++ mirror, the Host
  guest-call responder (`include/shm/GuestCallWorker.h::ProcessGuestCalls`),
  ended every transaction with an unconditional `state` store. Responder
  ownership is not eternal: once the peer's response timeout elapses it disowns
  the slot *without writing `state`* (§3.4/§3.5 timeout rule), and the slot is
  then recycled — on the Host side by `SlotAllocator::tryClaimSlot`'s zombie
  steal, which is gated only on the process-local `activeWait` and **not** on
  `autoReclaimTimeoutNs`, so it fires even under `fastPathAllowed == 1`. A late
  handler's blind store then stamped `SLOT_RESP_READY` over the new owner's
  in-flight transaction while the response buffer still held the *previous*
  one's bytes. Because a responder never rewrites `msgSeq`, the requester's
  `msgSeq` guard validated and silently accepted the stale response: a wrong
  value in a cell, or a crash on a mis-sized/mis-typed payload. Low frequency
  (a prior timeout is a necessary trigger), silent when it hits.

  Both responders now publish with a CAS from their owned state — Go
  `CAS(SLOT_GUEST_BUSY → SLOT_RESP_READY)`, C++ `CAS(SLOT_BUSY →
  SLOT_RESP_READY)`, both `seq_cst` so the §4.2 doorbell Dekker is unaffected —
  and drop the response when the CAS fails, logging and leaving the new owner's
  transaction untouched (it is served by the next loop iteration).

  This required **narrowing the v0.8.8 no-reclaim fast path**: it had dropped
  the responder's `SLOT_REQ_READY → SLOT_GUEST_BUSY` consume-claim CAS, and
  without it there is no airtight state to publish from — a CAS expecting
  `SLOT_REQ_READY` succeeds anyway, because a stolen slot is republished at
  exactly `SLOT_REQ_READY`. Only `SLOT_GUEST_BUSY` discriminates: on a Host slot
  it is written solely by the owning Guest responder (C++ never writes it at
  all), and a steal goes `GUEST_BUSY → BUSY → REQ_READY`, never back. The claim
  CAS is therefore mandatory on both paths; the fast path still skips the `gen`
  bump and the `lease` refresh (SPEC §3.6.1 reclaim-off carve-out, which now
  states explicitly that it never licenses dropping a claiming CAS). Measured
  cost on the Host→Guest direction, 1 thread / 64 B, within-session A/B/A:
  ~13.2–14.3 M ops/s → ~11.7–12.1 M ops/s (≈ −9 % to −18 %, ≈ +15 ns/RTT, one
  extra locked RMW). Guest-call direction is unchanged within noise (the C++
  claim CAS was already there; only a `seq_cst` store became a `seq_cst` CAS).

  Regression tests: `go/fastpath_test.go::TestGuestResponderFastPath_HostTimeoutStealDoesNotCorrupt`
  replays `tryClaimSlot`'s steal verbatim after a simulated timeout (no contract
  violation needed). `..._HostReclaimDuringHandlerCorrupts` is redefined as
  `..._HostReclaimDuringHandlerIsRejected` — it asserted the corruption; it now
  asserts the publish CAS fails, the stale response is dropped, and the new
  owner's transaction is served correctly. The 2026-07-24 verdict that the
  hazard was "unreachable by a conforming host" is retracted: the zombie steal,
  not auto-reclaim, is the recycler.

## [v0.8.13] - 2026-07-26

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`, `SlotHeader`
and `ExchangeHeader` are untouched, and the fix is invisible to the Guest
(which only ever acts on `SLOT_REQ_READY` in the Host Slot range). C++ Host
only; `activeWait` is a process-local field with no Go counterpart, so no
parity change is required.

### Fixed

- **Host response consumption was unprotected against sibling-thread slot
  theft** (SPEC §3.4 "Host requester consume-claim"). `WaitResponse` and
  `WaitForSlot` dropped the process-local `activeWait` flag while `state` was
  still `SLOT_RESP_READY`, and only *then* validated `msgSeq`/`msgType`, read
  `respSize`, and copied the response out. `tryClaimSlot`'s zombie branch
  steals any slot in {`REQ_READY`, `RESP_READY`, `GUEST_BUSY`} with
  `activeWait == 0`, so on a multi-threaded Host a sibling `AcquireSlot` /
  `AcquireSpecificSlot` / `TryAcquireSlot` could claim the slot mid-consume:
  the consumer then read bytes the Guest was concurrently overwriting for the
  thief's new transaction, and its blind terminal `SLOT_FREE` store clobbered
  the thief's live claim (double-ownership). Exposed `ZeroCopySlot::Send` /
  `SendFlatBuffer` (the longest window — the caller holds the response buffer
  until the wrapper dies — and the xll-gen production path), `SendAcquired`,
  and `WaitForSlot` (hence `Send`, `SendToSlot`, and the `Stream.h` chunk
  paths). `SendHeld` was already correct. The shared
  `SlotAllocator::FinishWait` now parks a responded slot at `SLOT_BUSY` before
  dropping `activeWait`, reusing the v0.8.5 held-slot consume-claim ordering;
  the timeout path is unchanged (the transaction is still in flight, so the
  slot is disowned for the zombie/lease reclaim paths). Regression test:
  `tests/test_host_consume_claim.cpp`.

## [v0.8.12] - 2026-07-25

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`. Go stream
path only; the public API is unchanged.

### Changed

- **Per-stream locking in the reassembler.** The reassembler-wide mutex used to
  be held across the StreamStart allocation (up to 1 GiB) *and* every chunk
  payload copy, so multi-stream receive serialized on one lock — the shape
  behind the 8T/16MiB reverse-scaling (1T 186 → 8T 126 ops/s). Each
  `streamContext` now carries its own mutex covering buf/next/offset/ooo/done;
  the map mutex only guards lookup, insert, delete and the LRU counter. The
  map-side reclaim paths (age prune, count-LRU, same-ID replacement) mark the
  context `dead` atomically as it leaves the map, and a chunk handler that
  snapshotted it observes that flag after taking the context lock — so a
  reclaimed context can never absorb further bytes or fire `onStream`. Lock
  order is map → context, never the reverse. Verified under `-race`.
- **`StreamSender` takes an inline path at `maxInFlight == 1`** (the library
  default since v0.8.9). At depth 1 the semaphore was acquired at the top of
  the loop and released only after `Send`+`Release`, so nothing ever
  overlapped: the per-chunk goroutine and channel handoff were pure overhead
  (~0.7-1.5 µs each). The pipelined path for depth > 1 is untouched.
- **Slot acquisition unified** (R17). `Send` was split up, the hardcoded `24`
  now comes from `chunkHeaderSize` (pinned by the compile-time size assert),
  and StreamStart's fixed 1000 × 1 ms retry / the chunk loop's unbounded 100 µs
  retry — which also discarded every intermediate error — became one
  deadline-bounded helper.
- StreamStart parsing dropped reflection-based `binary.Read` for the manual
  little-endian decode the chunk path already used.

## [v0.8.11] - 2026-07-24

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`. Go-side
hardening + C++ parity, all host/guest-local.

### Added

- **Partial-stream time-based expiry in the Go reassembler** (C++ parity —
  `StreamReassembler`'s 10 s `streamTimeoutMs` had no Go counterpart, so a
  lone stalled stream held its parked out-of-order bytes indefinitely until
  1024 other streams forced the count-LRU bound). Same semantics as C++:
  age measured from stream start, `DefaultStreamTimeout = 10s`, expired
  streams pruned at StreamStart admission; `timeout <= 0` disables. Public
  API unchanged (clock/timeout injection is an unexported core).
  SPECIFICATION.md §3.3.4 updated. Tests: `stream_timeout_test.go`.
- **Fast-path zombie-steal contract pinned by tests** (`fastpath_test.go`):
  a slow handler under `FastPathAllowed==1` completes its original
  transaction safely because the fast path requires host auto-reclaim OFF
  (§3.4); a contract-violating host that reclaims mid-handler reproduces
  the documented blind RESP_READY hazard. No code change — the guarding
  invariant is the no-reclaim contract, not the msgSeq guard.

### Changed

- `recordOutcome` adaptive spin-limit update is now a CAS loop instead of a
  Load→Store RMW (defensive: lost-update-proof if a WaitStrategy is ever
  shared; value formula unchanged, not a hot path).

## [v0.8.10] - 2026-07-24

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`. Platform
contract clarification + wire-boundary hardening (bundles the three
post-v0.8.9 hardening/docs commits).

### Breaking (build-time only)

- **GOARCH=386 builds now fail at compile time.** The Go guest was never
  functional on 386 — `memoryBasicInformation` mirrors the 64-bit
  `MEMORY_BASIC_INFORMATION`, so a 386 build misreads `RegionSize` and every
  `openShm` attach is rejected with a misleading error (verified on WOW64).
  A `windows && !amd64` guard file makes the amd64-only contract explicit
  with a single clear error. AGENTS.md / SPECIFICATION.md §4.4 now state
  windows/amd64-only; 32-bit x86 is unsupported.

### Fixed

- **Wire-boundary reads hardened** (from `02fd409`): `RingBufferReceiver.Read`
  clamps a corrupt `WriteOffset` (available > capacity) and the stream chunk
  path rejects hostile `PayloadSize >= 2^31` — both could previously drive a
  copy/slice past the backing array. Pinned by `wire_guard_test.go`.
- Stale reclaim-API version comments in `direct.go` (v0.7.1 → v0.7.2; spec
  §3.6 is authoritative).

### Docs

- Platform claims corrected across README/AGENTS (Windows-only, Linux backend
  removed), AffinityMode doc comments describe the `AffinityAuto` zero-value
  default, README documents the three biggest measured performance factors.

## [v0.8.9] - 2026-07-09

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`; no
shared-memory layout change this round (all additions are host/guest-local).
Guest-call endpoint co-location + reclaim-machinery diet, and a measured
stream pipelining default change (2026-07-09 round-8 pass; same-session A/B on
Ryzen 9 3900X, see `EXPERIMENTS.md`/`BENCHMARK_RESULTS.md` §2026-07-09).

### Changed

- **Stream pipelining default `inFlight` 2 → 1** (C++ `StreamSender`, Go
  `NewStreamSender`). Re-measured with the S6 slot-scramble confound removed
  (co-located fixed slot ranges on both runs): depth 2 loses to depth 1 on
  every stream cell (−56…−80%) — the stream plateau is memory-controller-bound
  so overlapping host/guest memcpys buys nothing, the second slot doubles the
  working set, and with a fixed range it spans a second physical core,
  breaking endpoint co-location. Callers passing an explicit `inFlight >= 2`
  are unaffected.

### Added

- **`HostConfig::guestWorkerAffinity`** (mask, default 0 = no pin) — pins the
  guest-call worker thread. **Go `PinCurrentGoroutine(mask)`** — pins a
  dedicated guest-call sender goroutine (LockOSThread + affinity mask). The
  guest-call path had no sibling co-location (both endpoints floated on the
  scheduler, paying cross-core line transfers per call plus placement
  variance); pinning worker and sender to one physical core's two SMT LPs
  restores the Direct-Exchange placement. **Guest-call 1T/64B: +88.8%
  (2.75M → 5.19M ops/s, RTT 364→193 ns), best-of-3 spread 8%→2.2%**; an
  unpinned control scattered 1.70–3.16M (86% spread).

### Performance

- **Guest-call reclaim-machinery diet (S8)** — the S7 reasoning applied to the
  guest-call path, no ABI change: the worker's claim skips the gen bump +
  lease refresh when host auto-reclaim is off (the claim CAS is kept — it
  arbitrates concurrent `ProcessGuestCalls` callers); the Go sender's consume
  skips the gen + CAS + lease entirely under
  `fastPathAllowed && guest reclaim off`, retaining ownership via
  `ActiveWait==1` until a release that drops ActiveWait then CAS's
  `RESP_READY→FREE`; `GuestSlot.Send` keeps its consume CAS (zero-copy caller
  holds the slot) but skips gen + lease. Bundled into the number above.
  New regression: `go/fastpath_test.go::TestGuestCallFastConsume` (-race).

### Documented

- Normal-mode multi-thread per-pair scaling (1T 15.5M/pair → 8T 8.4M/pair)
  investigated and closed as hardware-characteristic (boost residency +
  uncore sharing; single 2T step then flat, not progressive) — no software
  serialization point; absolute peak 12T 77.7M ops/s. Details in
  EXPERIMENTS.md §2026-07-09.

## [v0.8.8] - 2026-07-04

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`;
`SlotHeader`/stream headers untouched, `ExchangeHeader` stays 64 bytes (the new
field is carved from `reserved`). Guest responder no-reclaim fast path
(2026-07-04 round-7 perf pass; same-session A/B on Ryzen 9 3900X, see
`EXPERIMENTS.md`/`BENCHMARK_RESULTS.md` §2026-07-04 round 7). Pre-implementation
design reviewed by shm-protocol-guardian (SAFE-TO-IMPLEMENT).

### Added

- **`ExchangeHeader.fastPathAllowed`** (offset 28, carved from `reserved`) — a
  host-published, safe-by-default permission flag: `1` iff the host's
  auto-reclaim is disabled, else `0` (also the zeroed default a pre-v0.8.8 host
  never writes). The host sets it at `Init` and in `SetAutoReclaimTimeoutNs`.

### Performance

- **Guest responder no-reclaim fast path** — the Go guest responder
  (`workerLoopInternal`) ran a full consume-claim on every Direct-Exchange
  request: `claimSlotGen` (LOCK XADD) + `CAS(REQ_READY→GUEST_BUSY)` (LOCK
  CMPXCHG) + `Store(Lease)` (XCHGQ) — three locked ops that exist solely for the
  crash-recovery reclaimer / §3.6.1 ABA guard. On a host slot serviced 1:1 by
  one worker, with the host waiting only on `RESP_READY` (never observing
  `GUEST_BUSY`) and the reclaimer off (default), that machinery has no
  counterparty. When the host publishes `fastPathAllowed==1`, the guest now
  skips all three and processes while `State` stays `REQ_READY`, publishing
  `RESP_READY` as before — the acquire/seq_cst data-visibility handshake is
  unchanged. **Normal-mode ping-pong: 1T/64B +41.5% (9.88M→13.98M ops/s),
  4T +34.3%, 8T +29.2%, 1024B +8…26%.** The guest-side mirror of the v0.8.5
  host held-slot win — together they drop the per-RTT claim from both ends of
  Direct Exchange when auto-reclaim is off. Safe-by-default polarity: a version
  or config mismatch degrades to the full-claim slow path, never to an unsafe
  fast path, so no SHM_VERSION bump. Reclaim policy is startup-time (SPEC §3.4).

### Tests

- **`go/fastpath_test.go`** — drives the responder end-to-end with
  `fastPathAllowed==1` across two round-trips (the rest of the Go suite builds
  the flag as 0, covering the slow path); run under `-race`.

## [v0.8.7] - 2026-07-04

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`;
`SlotHeader`/`ExchangeHeader`/stream headers untouched. Stream slot
co-location (2026-07-04 round-6 perf pass; same-session A/B on Ryzen 9 3900X,
see `EXPERIMENTS.md`/`BENCHMARK_RESULTS.md` §2026-07-04 round 6).

### Added

- **`StreamSender(host, inFlight, baseSlot = -1)`** — an optional fixed slot
  range. When `baseSlot >= 0` the sender draws its slots round-robin from
  `[baseSlot, baseSlot+inFlight)` via `AcquireSpecificSlot` instead of the
  shared pool (`AcquireSlot`). `baseSlot < 0` (default) keeps the pool path, so
  this is source- and ABI-compatible.

### Performance

- **Stream slot co-location** — the pool path hands `StreamSender` arbitrary
  slots whose Go guest workers are pinned to other physical cores, adding
  cross-core coherence traffic on every chunk (Direct-Exchange avoids this by
  using slot `id` for worker `id`, co-located on sibling SMT LPs). With one
  sender per pinned worker and `baseSlot = id*inFlight`, the stream path
  regains that co-location. **Stream profile (4 KiB chunks, in-flight 1):
  4T/8T +28…+70% across 64 KiB / 1 MiB / 16 MiB streams** (1T neutral — single
  sender). Host-local policy only; guest unchanged.

### Tests

- **`test_guest_worker_sleep_only.cpp`** — covers the v0.8.6 guest-call
  doorbell gate on the `guestWorkerSpin=false` (sleep-only) worker path via a
  fake in-process guest peer (request published while the worker is parked must
  be serviced). Closes the coverage gap both v0.8.6 reviewers flagged.

## [v0.8.6] - 2026-07-04

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`;
`SlotHeader`/`ExchangeHeader` layouts untouched. Guest→host kernel-wake
elimination for the guest-call path (2026-07-04 round-5 perf pass; same-session
A/B on Ryzen 9 3900X, see `EXPERIMENTS.md`/`BENCHMARK_RESULTS.md` §2026-07-04
round 5). **Host and Guest must ship from the same shm release** (the Go sender
reads a `hostState` flag the host worker now publishes; guaranteed by same-tag
pinning — a mismatch degrades to ≤1s request latency, never a lost wakeup).

### Added

- **`DirectHost::TryAcquireSlot()` / `TryAcquireHeldSlot()`** — non-blocking
  slot acquisition (one round-robin sweep via the same gen-CAS+lease
  `tryClaimSlot` handshake, no wait/reclaim, returns `-1` / an invalid
  `HeldSlot` when the pool is momentarily full; guards `numSlots == 0`).
  Enables a held-slot session that falls back to a per-call claim instead of
  blocking (the prerequisite for the deferred xll-gen UDF-path adoption).
- **`HostConfig::guestWorkerSpin`** (default `true`) — whether the guest-call
  worker runs the adaptive spin phase and publishes `HOST_STATE_WAITING`
  before parking. Set `false` on hosts that must not dedicate a
  briefly-spinning background thread to guest-call latency.

### Performance

- **Guest→host doorbell elision** — the guest-call path was kernel-wake-bound:
  the C++ `GuestCallWorker` only parked on the shared request event and the Go
  sender always fired a request-doorbell `SetEvent` syscall. Now the worker
  runs the shared `WaitStrategy` spin over the guest-slot `SLOT_REQ_READY`
  predicate before parking, and publishes `HOST_STATE_WAITING` (seq_cst) on
  every guest slot before it parks (rechecking the predicate first — Dekker,
  no lost wakeup), restoring `HOST_STATE_ACTIVE` while it spins/processes. The
  Go sender (`sendGuestCallInternal` and the zero-copy `GuestSlot.Send`) gates
  its request doorbell on `hostState == HOST_STATE_WAITING` — the guest→host
  mirror of the v0.8.5 response-side `guestState` gate. A spinning worker costs
  the sender no doorbell syscall. **Guest-call 1T/64B echo: +836% (≈9.4×),
  226,819 → 2,122,249 ops/s** best-of-3 (this is xll-gen's Go→XLL RTD-update
  path). Normal-mode ping-pong unaffected. SPECIFICATION §3.5 documents the
  gate and the same-release requirement.

## [v0.8.5] - 2026-07-04

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`;
`SlotHeader`/`ExchangeHeader` layouts untouched. Claim-cycle, benchmark
instrumentation, stream-reassembly, and guest-call wake-path performance
(2026-07-04 multi-agent perf hunt round 4; same-session A/B on Ryzen 9 3900X,
see `EXPERIMENTS.md` §2026-07-04 and `BENCHMARK_RESULTS.md` §2026-07-04).

### Added

- **Held-slot session API (C++ host)** — `SlotAllocator::SendHeld` /
  `ReleaseHeldSlot` and the `DirectHost::HeldSlot` RAII wrapper
  (`AcquireHeldSlot()` / `AcquireHeldSlot(slotIdx)`): claim a slot once, then
  re-arm it per send with no per-RTT gen-bump/CAS/FREE cycle, parking at
  `SLOT_BUSY` between sends. Wire-compatible (the guest only acts on
  `SLOT_REQ_READY`; the Go guest has re-armed this way since v0.6);
  contract documented in SPECIFICATION §3.6 "Held-slot sessions". The
  response consume-claim (RESP_READY→BUSY *before* dropping activeWait) also
  closes the pre-existing RESP_READY/!activeWait zombie-steal window on this
  path. Regression: `tests/test_held_slot.cpp`. Benchmark normal mode uses it
  by default (`--legacy-claim` / harness `-LegacyClaim` restores the per-op
  claim cycle). +18% at 1T on top of the items below; combined +29% at
  1T/64B vs v0.8.4.

### Performance

- **Per-WaitStrategy benchstats sharding (Go)** — the `shm_benchstats`
  counters moved from globally shared padded cells into each slot's
  `WaitStrategy` (value-embedded, struct padded to one cache line,
  compile-time size assert; aggregate getters sum a tag-gated registry, and
  a bare `&WaitStrategy{}` still works unregistered). Under the harness's
  `-tags shm_benchstats` build every RTT previously fired two LOCK XADDs on
  two globally contended lines across all pinned cores; instrumented
  multi-thread cells were throttled by their own instrumentation
  (+71%/+149% at 4T/8T 64B once removed). Production (untagged) builds were
  already zero-cost and are unaffected.
- **Inline small-copy fast path (C++)** — `Platform::CopySmall` replaces the
  out-of-line `call memcpy` (MinGW: IAT-indirect into msvcrt) at the 9
  hot-path payload-copy sites (`SendToSlot`/`Send` request, `WaitForSlot`/
  `SendAcquired`/`SendHeld` response) with an inline overlapping-window
  ladder for ≤256 B; larger copies fall back to `memcpy`.
- **Stream reassembly direct-into-destination (Go)** — the reassembler
  preallocates the final buffer at `StreamStart` and copies in-order chunks
  straight into it at a running offset (was: per-chunk allocation into
  `[][]byte` + a second full-size copy at completion). Out-of-order chunks
  (pipelined senders) park in a lazily allocated side map. SPEC §3.3.4
  guards preserved bit-for-bit (per-chunk running-length, Σ==totalSize,
  exactly-once dedup incl. zero-length chunks); new regression suite
  `go/stream_reassembly_order_test.go`. Stream throughput +21…+115% on
  sub-plateau cells, 16 MiB 1T +75%.
- **Guest-call response signal gating (C++ `GuestCallWorker`)** — both
  `SLOT_RESP_READY` publish sites now elide the `SetEvent` syscall when the
  Go caller is spinning (`guestState != GUEST_STATE_WAITING`), mirroring the
  host→guest Dekker already used by `SlotAllocator`/the Go worker.
- **Lazy guest-call acquire deadline (Go)** — `sendGuestCallInternal` takes
  its acquisition-timeout timestamp on the first *failed* slot pass instead
  of every call (same rationale as the v0.8.4 C++ lazy QPC latch).

### Fixed (benchmark fidelity — changes what the harness reports)

- **Guest-call bench listener** — the hand-rolled 1 ms-poll listener
  actually polled at the 15.6 ms Windows timer quantum, capping the
  guest-call cell at ~64 ops/s and measuring the poll, not the IPC. The
  bench now uses the library's event-driven worker (`DirectHost::Start`).
  The Go bench also reports guest-call results reliably at shutdown (the old
  in-goroutine print never ran) and measures `SendGuestCallBuffer` with a
  hoisted response buffer. New cell reference: ~393K ops/s (1T/64B echo).
  **Old guest-call numbers are not comparable.**
- **Stream bench in-flight default stays 1** — briefly aligned to the
  library `StreamSender` default (2), then reverted after measurement:
  depth 2 lost to depth 1 on every stream-profile cell with the new
  reassembler (and doubles `numHostSlots`, breaking the 8T Sibling-affinity
  fit). Recorded in `benchmarks/main.cpp`; the *library* default is now a
  backlog review item. Help text corrected (previously claimed "default: 4").

## [v0.8.4] - 2026-07-03

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`;
`SlotHeader`/`ExchangeHeader` layouts untouched. Host hot-path
micro-optimizations plus a benchmark measurement-fidelity overhaul
(2026-07-03 multi-agent perf hunt; same-session A/B on Ryzen 9 3900X,
best-of-3, `-HighPriority`), and one Go-side default-affinity policy
change (see **Changed**).

### Changed

- **`AffinityAuto` oversubscription gate (Go)** — `resolveAuto` now returns
  `AffinityNone` when `GOMAXPROCS(0) < numSlots`, *before* consulting chipset
  topology. A pinned worker holds its P across each spin burst; with fewer Ps
  than slot workers, pinning yields no cache-locality benefit and instead
  deepens P-contention (runnable workers queue behind spinners for a P). This
  changes the resolved policy only for the default (`AffinityAuto`) on
  oversubscribed configs — explicit `AffinityLocal`/`AffinitySibling` are
  passed through untouched (caller owns that trade-off). `DirectGuest.Start`
  now emits a one-shot diagnostic (`slots`/`gomaxprocs`/`affinity`/`resolved`)
  so the back-off is observable. Go-side heuristic only; wire ABI unaffected.

### Performance

- **Coarse lease clock (C++ host)** — `Platform::MonotonicNanos` now reads
  `GetSystemTimeAsFileTime` (KUSER_SHARED_DATA tick, ~5 ns) instead of the
  QPC-backed `GetSystemTimePreciseAsFileTime` (~26 ns). The call sits on every
  slot claim (lease stamp, SPECIFICATION §3.6). Same Unix-epoch timeline;
  0.5–15.6 ms tick granularity is irrelevant to second-scale reclaim
  thresholds, and the §3.6.1 `gen`-CAS handshake — not lease equality — is the
  ABA guard. Go side unchanged (`time.Now()` already reads the shared page,
  ~6 ns). Note: `lease` is a cross-boundary value (host writes, Go reads) — its
  *resolution* coarsens (≤ one tick skew) though its type/units/offset do not;
  this only makes reclamation more conservative and cannot induce a collision
  against seconds-scale thresholds.
- **Lazy acquire-timeout clock** — `SlotAllocator::AcquireSpecificSlot` no
  longer takes a `steady_clock::now()` (one QPC) on the claim fast path; the
  timeout clock starts on first slow-path entry, latched once so the timeout
  still accumulates across retry sweeps.
- **`Slot` cache-line isolation** — host-process-local `shm::Slot` bookkeeping
  (waitStrategy limit, msgSeq, activeWait — all written per RTT) is now
  `alignas(64)` with size/alignment static_asserts. The 57 field-bytes already
  round to sizeof 64, but without alignas the `std::vector<Slot>` base is only
  8-aligned so consecutive slots straddle cache lines, false-sharing between
  pinned worker threads; alignas raises alignof 8→64 so each slot owns one line.
  Never enters the shared mapping — ABI unaffected.
- Combined library effect (identical old benchmark binary, best-of-3):
  1T/64B **+40.4%**, 4T/64B **+12.6%**, 8T/64B **+12.6%**, 1T/1024B
  **+29.2%**, 4T/8T 1024B **+9.1%**.

### Benchmark

- **Measurement-fidelity overhaul of `benchmarks/main.cpp`** — the timed loop
  was paying ~90 ns/op of its own overhead: two `steady_clock::now()` (QPC)
  calls per op plus two atomic RMWs on one globally shared stats cache line
  (the C++ analog of the Go `WaitStats` false-sharing fixed in v0.7.12).
  Stats are now per-thread, cache-line-aligned, non-atomic (aggregated after
  join); latency is sampled 1-in-61 in nanoseconds (the old per-op
  microsecond truncation reported ~0 for sub-µs RTTs); the invariant request
  payload is copied once per thread instead of per op. **Numbers from the new
  benchmark are NOT comparable to any previously published table** — with
  bench overhead removed the same library measures 1T/64B ≈ 8.3M ops/s
  (~0.16 µs RTT) and 8T/64B ≈ 19M ops/s on the 3900X. See
  BENCHMARK_RESULTS.md §2026-07-03.

### Fixed

- `tests/test_stream_integration.cpp` — adapted the `DirectHost::Start` lambda
  to the current `StreamReassembler::Handle(..., size_t&, MsgType&)` signature
  (pre-existing build drift; the test still requires repo-root CWD to run).

## [v0.8.3] - 2026-07-02

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`. Slot-claim
logic and error-propagation fixes only; `SlotHeader` layout untouched.

### Fixed

- **Cross-generation zombie-slot steal** — the Case-2 zombie-slot claim paths
  (Go `AcquireGuestSlot` / `sendGuestCallInternal`, C++ `AcquireSlot` /
  `AcquireSpecificSlot`) recycled a `RespReady && !activeWait` slot with only a
  bare `Gen` bump plus a `State` CAS, with no ABA protection. A stealer
  preempted between observing the zombie and its `State` CAS could hijack a
  concurrent claimant's fresh, live transaction — the rightful owner then hit a
  spurious "slot reclaimed while consuming response" failure (for rtd-once, a
  permanent `#GETTING_DATA`). Both sides now route these steals through a shared
  helper (`tryClaimGuestSlot` / `SlotAllocator::tryClaimSlot`) that snapshots
  `Gen` **before** the `State` load and wins the recycle via `CAS(Gen, observed,
  observed+1)` before the `State` CAS — the same linearizer
  `TryReclaimAbandonedSlot` uses (SPECIFICATION §3.6.1). `SLOT_FREE` claims keep
  the plain bump (no transaction-identity ambiguity); `ProcessGuestCalls` is
  unchanged (it claims a genuine `SLOT_REQ_READY`, not a steal).
- **`StreamSender` swallowed reassembler rejections** — `StreamSender.Send`
  discarded the response `MsgType`, so a receiver rejection surfaced as
  `MsgType::SYSTEM_ERROR` (size/stream-count overflow, OOM, unknown/evicted
  stream) was reported as success — silent data loss. It now converts
  `SYSTEM_ERROR` to an error (mirroring `SendGuestCall`), returning immediately
  on a StreamStart rejection so no chunks are wasted.

## [v0.8.2] - 2026-06-26

No wire-protocol/ABI change — `SHM_VERSION` remains `0x00070000`.

### Added

- **`AffinitySibling` (`AffinityMode = 3`)** — opt-in pinning that
  co-locates each slot's C++ host worker and Go guest goroutine on the
  two SMT siblings of one physical core. New `Platform::EnumerateSmtPairs`
  (C++) and `shm.SmtPairs()` (Go) enumerate LTP_PC_SMT physical cores via
  `GetLogicalProcessorInformationEx(RelationProcessorCore)` and return
  single-bit (host, guest) LP-mask pairs (lowest bit → host, next-lowest
  → guest; both sides derive the same ordering). Benchmark binary and
  `harness.ps1` accept `--affinity sibling` / `-Affinity sibling`.

### Changed

- **`AffinityAuto` is now chipset-aware.** `affinity.go::resolveAuto`
  picks the most specific mode the host topology supports:
  1. `len(SmtPairs()) > 0 && numSlots <= len(SmtPairs())` →
     `AffinitySibling`.
  2. `len(CcxMasks()) > 1` → `AffinityLocal`.
  3. otherwise → `AffinityNone`.
  Pre-v0.8.2 behaviour is preserved wherever Sibling is not safe (Auto
  never pins to a single LP without a documented SMT sibling). Threaded
  numSlots through `affinityMaskForSlot` / `pinSlotWorker`; the C++
  bench `WorkerThread` mirrors the same resolution against `totalSlots`.
- **`go/affinity_test.go`** — `TestAffinityAutoChipsetAware` covers the
  new resolution table; `TestAffinityMaskForSlot` simplified to focus on
  the explicit (non-Auto) CCX round-robin invariants;
  `TestSmtPairsTopology` / `TestAffinityMaskForSlotSibling` validate the
  new enumeration and slot-to-pair mapping.

### Measured

- Ryzen 9 3900X (2026-06-26, `harness.ps1 -HighPriority`, best-of-3):
  `AffinitySibling` beat `AffinityLocal` by **+24 % to +74 %** across
  1/4/8 threads × 64 B / 1024 B payloads. `AvgItersPerSpin` dropped
  15 – 38 % across the matrix; `SleepFallback` stayed at ≈0 %. The
  dominant effect is *deterministic non-collision placement* of host
  and guest LPs — CCX-wide masks let the OS scheduler co-locate both
  endpoints on the same LP, periodically stalling the spin. Shared-L1d
  cache locality is real but secondary. See `EXPERIMENTS.md` §"2026-06-26
  SMT-sibling co-location" and `BENCHMARK_RESULTS.md` §"SMT-sibling A/B"
  for setup and per-cell numbers.

### Operator notes

- See `AGENTS.md` §"Affinity Recommendations" for per-CPU-family guidance
  (chiplet AMD, monolithic-L3 Intel, hybrid Intel P+E, constrained VMs).
  Untested families: hybrid Intel P+E (Auto skips E-cores), monolithic-L3
  Intel single-socket, realistic asymmetric workloads (consumer doing
  real CPU work between requests).

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
