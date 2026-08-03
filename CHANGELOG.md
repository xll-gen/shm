# Changelog

## [Unreleased]

### Added — `StreamHeader.appMsgType` reaches the receive side

**No wire change and NO `SHM_VERSION` bump.** `StreamHeader.appMsgType@20` has
been on the wire since `0x00080000`; only local API surface changes here. That
the slot was claimed at that bump is exactly what makes this free — it is the
plan being spent, not a new one.

The field was **write-only** for the whole of 0.9.0: both senders wrote it, both
reassemblers parsed past it, and the application had no way to read it. Both
peers now surface it through a **parallel, additive** entry point:

- **Go** — `shm.TypedStreamHandler` (`func(streamID uint64, appMsgType uint32, data []byte)`)
  and `shm.NewStreamReassemblerTyped`. `NewStreamReassembler` and `StreamHandler`
  are unchanged.
- **C++** — `StreamReassembler::OnStreamTypedFn` and the
  `StreamReassembler(OnStreamTypedFn, cfg)` constructor overload. The
  `StreamReassembler(OnStreamFn, cfg)` constructor is unchanged.

On both sides the untyped form is *defined as* the typed form with the tag
discarded, so there is one reassembly implementation rather than two kept in
sync. The tag is captured into the per-stream reassembly context at
`STREAM_START` and delivered from there on **both** completion paths (the
`totalChunks == 0` stream, which completes inside `STREAM_START`, and ordinary
chunk completion), so interleaved streams each report their own tag. A dropped
or evicted stream delivers nothing, as before. `0` is delivered as `0`: there is
no validation, normalisation or default substitution.

`appMsgType` remains **inert to reassembly** — nothing in the accept/reject path
reads it, and the existing inertness tripwires
(`TestStreamReassembly_AppMsgTypeIsInertToReassembly` and its C++ twin) stay in
place unchanged. Delivery and inertness are the two halves of SPEC §3.3.1 and
neither implies the other.

New regression pair, one artifact in two languages:
`go/stream_apptype_delivery_test.go` and
`tests/test_reassembler_appmsgtype_delivery.cpp`. They exist because neither
pre-existing guard could fail for "the tag never reaches the handler" — the
send-side test stops at wire offset 20, and the inertness tripwire is a negative
assertion that was green throughout the write-only period. Non-vacuity is
confirmed by mutation on both sides (deliver a constant; read `totalChunks@16`
instead of `appMsgType@20`; keep a reassembler-wide "last tag seen"): every
mutant flips the new tests red while the parity table, the `dstOffset` placement
test and the reassembler contract test all stay green.

`SPECIFICATION.md` §3.3.1 carries the four normative obligations (additive,
per-stream, both completion paths, unvalidated).

**"Additive" has one C++ exception, and the spec now says so.** Adding the
`StreamReassembler(OnStreamTypedFn, cfg)` overload makes an argument convertible
to BOTH function types ambiguous: `StreamReassembler r(nullptr)`, `r({})` and
`r([](auto&&...){})` compiled against the pre-change header and no longer do
(one TU compiled against both headers; g++ 14.2 `-std=c++17`). No caller in this
tree or any documented usage is affected and the failure is always a compile
error, but the earlier "purely additive / an existing caller MUST NOT have to
change" wording was unqualified and therefore false. Recorded on the `OnStreamFn`
constructor, in SPEC §3.3.1 obligation 1 and in `AGENTS.md`. Go is unaffected —
`NewStreamReassemblerTyped` is a new name, not an overload.

**Chunk-completion retires the context by `extract`, not `erase`.** Two values
have to come out of the context at completion (the payload, moved out; the
routing tag, copied out) and both are read AFTER the node leaves the map. Under
`streams.erase(it)` that ordering is a use-after-free that the shipped test suite
cannot fail — the freed node's bytes are still intact, so
`test_reassembler_appmsgtype_delivery` exits 0 under the mutation. `extract`
transfers ownership of the node to a local, so the reference stays valid and the
hazard is gone by construction rather than guarded by a comment. Demonstrated by
rebuilding that test against a poisoning global `operator delete` (0xAB fill,
leak): `extract` passes, `erase` fails 7 checks with `delivered appMsgType
0xABABABAB`. No ASAN on this toolchain, hence the hand-rolled poisoner.

## [0.9.0] - 2026-08-03

**BREAKING WIRE CHANGE.** `SHM_VERSION` goes `0x00070000` → `0x00080000`, the
first bump since v0.7.0 and the reason this is a minor rather than a patch
release (`AGENTS.md` §Codebase Authority: breaking changes need a major bump, and
on the 0.x line the minor slot plays that role). Two `reserved` slots in the
streaming headers acquire meaning; no struct moves, grows, or shrinks —
`StreamHeader` and `ChunkHeader` are still 24 bytes each, every existing field
keeps its offset, and `SlotHeader`/`ExchangeHeader` are untouched.

**`VERSION` goes `0.8.20` → `0.9.0`.** It had gone stale: `v0.8.21` was tagged
while `VERSION` still read `0.8.20`, so that tag's CMake package version
(`CMakeLists.txt` derives `project(shm VERSION …)` by reading this file) was wrong,
and this release opened with three disagreeing numbers. The bump is to `0.9.0`
rather than to `0.8.22` because a breaking wire change cannot be a patch bump and
the 0.x minor slot is the major slot (`AGENTS.md` §Codebase Authority) — which
holds regardless of which `0.8.z` the file was sitting on, so no intermediate
correction to `0.8.21` is needed.

**Host and Guest MUST be rebuilt from this tag together.** Both constants must
read `0x00080000` at the tag — C++ `include/shm/IPCUtils.h::SHM_VERSION` and Go
`go/direct.go::Version` — because the two are compared byte-for-byte at attach: a
mismatch is refused outright (`protocol version mismatch: 0x… (expected 0x…)`), so
a half-applied bump is a total outage, not a subtle bug. That hard failure is
also the entire justification for fixing this with a version bump. `dstOffset`
below could not have been carved from `reserved` the way `lease`, `gen` and
`fastPathAllowed` were: an old sender leaves the field `0`, and `0` is a
*legitimate* offset — precisely what chunk 0 of every conforming stream declares —
so a new receiver against an old sender would stack every chunk at offset 0 and
complete a silently corrupt stream. No in-band value can distinguish "offset
zero" from "sender predates the field", so the discriminator has to be `version`,
where it is loud and caught once. See `SPECIFICATION.md` §2.1.

### Changed (BREAKING)

- **`ChunkHeader.reserved@16` → `dstOffset` (`uint32`)** — the **absolute** byte
  offset of this chunk's payload within the destination buffer. Senders MUST set
  it to the sum of the `payloadSize`s of all lower chunk indices; both senders
  now do. Receivers write **every** chunk straight to `destination[dstOffset]` on
  arrival, in whatever order chunks arrive, so the out-of-order side map no
  longer holds payload bytes at all — only a `{dstOffset, payloadSize}` record
  per index, enough to advance the in-order cursor and detect completion.
  `include/shm/Stream.h`, `include/shm/StreamReassembler.h`, `go/stream.go`,
  `go/stream_sender.go`; contract in `SPECIFICATION.md` §3.3.2 / §3.3.4 "Chunk
  placement".

  The payoff is on the out-of-order path, which is what a pipelining sender
  (`maxInFlight > 1`) and any multi-slot delivery actually exercise: **an
  out-of-order chunk now costs zero parked payload copies.** v0.8.18 had already
  removed the `totalChunks`-sized array and the `2 × totalSize` completion peak
  (per-chunk vectors concatenated into a second full-size buffer), but it left one
  copy standing — its own entry says so: *"only ahead-of-cursor chunks still take
  a parked copy."* That copy is now gone, and with it the second `memcpy` that
  moved those bytes into the destination when the gap filled. Parked residency for
  a stream held at the `maxParkedChunks` bound drops from up to
  `1024 × maxPayload` bytes of duplicated payload to `1024` twelve-byte records;
  the drain loop copies nothing and only advances bookkeeping.

  `dstOffset` is peer-controlled, so it is validated on two axes and **either
  failure drops the whole stream** (new rows in the §3.3.4 rejection table):
  `dstOffset + payloadSize > totalSize`, evaluated by subtraction so a hostile
  offset near 2^32 cannot wrap past the bounds check; and `dstOffset` disagreeing
  with the running in-order cursor at the moment its index becomes in-order —
  checked at *both* detection sites, the immediately-in-order arrival and the
  drain of a previously parked index. The second check is exact rather than
  heuristic: every index passes through it exactly once, so a sender that
  misplaces any chunk is caught before the stream can complete. That is what lets
  the receiver keep per-index dedup instead of tracking covered byte intervals,
  and it is why placement can be trusted without an overlap check — bounds keep
  writes inside the buffer, and the cursor check guarantees a self-contradictory
  stream is dropped rather than delivered garbled.

### Added

- **`StreamHeader.reserved@20` → `appMsgType` (`uint32`)** — an
  application-defined routing tag, **opaque to shm** (`0` = unspecified). shm
  neither interprets nor validates it; there is no reserved range and no value a
  receiver may refuse. Senders write it: Go `StreamSender.SetAppMsgType`, C++
  `StreamSender::Send(..., appMsgType = 0)`.

  **Receive-side surfacing is deferred and is NOT part of this release.** The
  completion callback is still `(streamId, data)` on both sides; nothing delivers
  the tag to a stream handler yet. The slot is claimed *now*, at a bump that was
  happening anyway, so that adding the receive-side API later needs **no second
  `SHM_VERSION` bump** — the bytes are already on the wire. The only property
  assertable today is therefore a negative one, and it is asserted: `appMsgType`
  MUST NOT alter reassembly in any way. `SPECIFICATION.md` §3.3.1.

- **New public C++ API for the 1 GiB stream ceiling** (`include/shm/Stream.h`,
  `include/shm/StreamReassembler.h`). The bound that makes a `uint32` `dstOffset`
  sufficient was documentation only until now:
  - **`shm::kMaxStreamSize`** (`static constexpr size_t`, `1 << 30`) — the single
    named ceiling constant. There is deliberately no second, looser "what a uint32
    can physically address" (4 GiB) limit anywhere: 1 GiB is what the Go peer's
    `MaxStreamSize` const accepts and what `SPECIFICATION.md` §3.3.4 states
    normatively, so enforcing 4 GiB instead would have left the accept/reject
    parity break across the whole 1–4 GiB range — the actual defect.
  - **`StreamReassemblerConfig::IsValid()`** — reports whether
    `maxStreamSize <= kMaxStreamSize`. For callers that build a config and pass it
    somewhere that does *not* get the constructor clamp below.
  - **`StreamReassembler::Config()`** — the configuration actually in force after
    construction, so a caller (or a test) can observe that an out-of-range
    `maxStreamSize` did not survive.
  - **`StreamReassembler::PeekStream()` / `StreamReassembler::StreamSnapshot`** —
    a read-only snapshot of an in-flight stream's cursor and destination bytes,
    taken under `streamsMutex`. It exists so a test can observe *placement* while a
    stream is still incomplete, which is the one property output bytes cannot show
    (see `### Verified`). `StreamContext` and `Placed` stay private; the snapshot
    copies plain values out and holds no reference into live state.
  - A `static_assert` on `StreamReassemblerConfig{}.maxStreamSize` pins the
    *default* at compile time, which no runtime clamp or test can do.

- **Guard files added for invariants that previously had no regression test.**
  Recorded here because the failure they close is a guard quietly stopping:
  - `tests/test_go_version_parity.cpp` + `go/version_wire_guard_test.go` — the
    cross-language `SHM_VERSION` / `shm.Version` pair, each half parsing the
    *other* language's source as data so neither needs the other toolchain. This
    bump **shipped half-applied once** (`IPCUtils.h` at `0x00080000`, `go/direct.go`
    still `0x00070000`) with **both suites green**:
    `tests/test_protocol_version.cpp` compares a C++ constant against the value C++
    itself wrote into the segment, and `tests/test_stream_integration.cpp` — the
    only test that runs Go — returns CTest `SKIP` whenever `find_program(NAMES go)`
    misses, i.e. on the ordinary C++-only build.
  - `tests/test_stream_size_ceiling.cpp` — both halves of the ceiling
    (`IsValid()`/constructor clamp, and `StreamSender::Send` refusing) —
    and `go/stream_sender_size_ceiling_test.go` for the Go sender half, which
    asserts that *no request reaches the host* and the guest slot is left
    `SLOT_FREE` (the observable form of "before transmitting anything") and pins
    the comparison as strict `>` so an exactly-ceiling stream still sends.
  - `tests/test_reassembler_dstoffset_placement.cpp` — the C++ half of the
    early-placement pair, reading the destination buffer while the stream is still
    incomplete through the new `StreamReassembler::PeekStream` seam. It is the
    **only** ctest guard for "the bytes are at `buf[dstOffset]` before the cursor
    arrives": under a mutation that writes at `ctx.offset` instead, and under an
    honest revert to the pre-`0x00080000` parking shape, the contract, parity-table,
    ooo-overflow and park-cap tests all stay green.
  - `tests/test_stream_chunk_padding.cpp` — `ChunkHeader.padding@20` is zero in the
    real sender's request bytes. Its stack-scribbling is best-effort by its own
    measurement (catches a default-initialized `ChunkHeader` under MinGW/GCC 14,
    passes under MSVC 19.41 `/O2` where `Send` is inlined), so it is a tripwire for
    removing the value-initialization, not a proof of it.
  - `go/stream_sender_appmsgtype_test.go` — the *write* side of `appMsgType`. Every
    other test of that field hand-builds the header bytes, so the tag could have
    been dropped or written at the wrong offset with the suite still green.
  - Two `static_assert`s on `StreamContext::Placed` (`include/shm/StreamReassembler.h`):
    `sizeof(Placed) == 16` and `is_trivially_copyable<Placed>`. These make a
    reintroduced parked payload copy — an owning `vector`/`string` member, or an
    inline byte array — a **compile** error, which is stronger than the Go byte-budget
    test that can only see a copy once it clears the test's slack.

### Changed

- **`StreamSender::Send` now REFUSES a stream larger than `kMaxStreamSize` (1 GiB)
  with `Error::InvalidArgs`, where it previously truncated.** This is a
  consumer-visible behaviour change on an existing call: a caller that passed a
  2 GiB buffer used to get a `Success` and a silently misassembled stream
  (`ch.dstOffset = (uint32_t)(size - remaining)` wraps past 4 GiB, so every chunk
  after the wrap lands at a wrong but *in-bounds* — therefore undetectable —
  address), and now gets a hard error before a single slot is acquired. If you were
  relying on the old behaviour you were relying on corruption; chunk the transfer
  at 1 GiB. The check is deliberately placed before slot acquisition and before
  `data` is dereferenced, so the guard is reachable without committing the buffer.
  `SPECIFICATION.md` §3.3.2 obligation 2.

- **The Go `StreamSender` now refuses the same way**, in `sendStart` before
  `acquireSlot`: `Send` on a slice longer than `MaxStreamSize` returns an error
  naming both the size and the bound, where it previously narrowed the offset with
  `uint32(dstOffset)` and sent. Same consumer-visible shape as the C++ change
  above; §3.3.2 obligation 2 is a MUST on **both** peers, and until this release
  only C++ satisfied it. Go callers relying on the old behaviour were relying on
  the peer's `STREAM_START` bound to reject the transfer for them, which the spec
  explicitly forbids because a truncated offset is a legal-looking value.

- **The installed CMake package version file is now `COMPATIBILITY
  SameMinorVersion`** (was `SameMajorVersion`). On the 0.x line CMake reads
  major = 0, so *every* `0.y` satisfied every other: `find_package(shm 0.8.21)`
  was answered `COMPATIBLE` by an installed 0.9.0 — i.e. a consumer pinned to the
  `0x00070000` wire was silently allowed to link `0x00080000` headers. Since this
  repo makes the minor slot the breaking slot on 0.x, the minor slot is also the
  compatibility slot. Consumer-visible: `find_package(shm 0.8.x)` now fails at
  configure time against 0.9.0, and going forward a 0.9.x pin will refuse 0.10.0.
  Nothing in this ecosystem is affected — `xll-gen` consumes shm via
  `FetchContent` + `add_subdirectory`, not `find_package` — but the policy changed,
  so it is recorded here rather than only in the `CMakeLists.txt` comment.

- **A `StreamReassembler` constructed with `maxStreamSize > kMaxStreamSize` is now
  clamped down to 1 GiB instead of honouring the value.** The field is a public
  mutable `size_t`; raising it made this peer accept a `STREAM_START` its Go
  counterpart rejects outright, which §3.3.4 forbids in so many words ("a stream
  accepted by one peer is accepted by the other"). The clamp is **silent in a
  release build** — it emits `SHM_LOG_WARN`, and `include/shm/Logger.h` compiles
  every log macro to `do {} while(0)` unless `SHM_DEBUG` is defined — so a consumer
  that needs to know must read back `StreamReassembler::Config()` or pre-check with
  `StreamReassemblerConfig::IsValid()`. Refusing construction was rejected as the
  remedy: the constructor has no error channel, and clamping to the wire ceiling
  cannot break a conforming deployment (no conforming stream exceeds it).

### Fixed

- **The C++ `StreamSender` put uninitialised host stack bytes on the wire.**
  `ChunkHeader` was declared default-initialized (`ChunkHeader ch;`) and then
  `memcpy`'d whole into the guest-visible segment, so `padding@20` — the field
  neither side reads — shipped whatever the host stack held. Now `ChunkHeader ch{}`
  (and `StreamHeader header{}`, which had all its fields assigned anyway, so the
  `{}` is there to keep that true for the next field somebody adds). Two distinct
  consequences: a 4-byte-per-chunk disclosure of host process memory to the peer,
  and — the larger one — a forfeited compatibility carve-out. `SPECIFICATION.md`
  §2.1's argument for why `lease`, `gen` and `fastPathAllowed` needed no version
  bump is that older peers wrote `0` into the reserved space they were carved from;
  `padding@20` is the **last** reserved slot in `ChunkHeader`, so a shipped sender
  writing garbage there would permanently prevent a future reader from telling "old
  peer" from "new peer wrote this value". Go's `putChunkHeader` already wrote an
  explicit `0`.

### Verified

- Both parity halves (`go/stream_parity_table_test.go`,
  `tests/test_reassembler_parity_table.cpp`) gained `dstOffset` cases and keep
  asserting against duplicated constant digests, not against each other:
  out-of-bounds offset, in-bounds offset with an overhanging payload, overhang on
  the ahead-of-cursor path (the bounds guard is not in-order-only), a cursor
  mismatch caught on each of the two detection sites separately,
  `dstOffset = 0xFFFFFFFE` and `0xFFFFFFFF` against `totalSize = 4` (the wrap
  cases — they pass a `dstOffset + payloadSize` sum computed in 32 bits and are
  what make the subtraction form load-bearing rather than stylistic), and a
  **zero-length** chunk at an out-of-bounds offset, which an "empty payload writes
  nothing" fast path would wave through into the parked map. 29 cases, identical
  names and identical digests on both sides.

  **These cases pin the accept/reject contract only — the parity table cannot see
  placement, and an earlier draft of this entry claimed otherwise.** The retracted
  claim was that the `nonuniform_offsets_reverse_completes` case proves offsets are
  "honoured as an absolute destination", on the theory that a receiver ignoring
  `dstOffset` would only agree with a *uniform*-size case by coincidence. That
  reasoning is wrong, and the flaw is structural rather than a missing case:
  placement check 2 forces `dstOffset == the running cursor` for every index at the
  moment it becomes in-order, on every stream that is not dropped, so a
  `dstOffset`-honouring receiver and one that appends at its own cursor write every
  byte to the same address and deliver byte-identical output — for *all* streams,
  uniform or not. No output-byte test can distinguish them, which is also why all
  20 pre-existing digests survived this rework unchanged. Measured, not argued:
  reimplementing the pre-bump receiver (park a payload copy, drain in index order,
  append at the cursor, never consult `dstOffset` for placement) leaves all 29
  parity cases and the whole `AppMsgTypeIsInertToReassembly` replay **passing**, and
  flips exactly four tests red out of the whole Go suite —
  `TestStreamReassembly_AheadOfCursorChunkIsResidentAtDstOffset`,
  `TestStreamReassembly_ParkedChunksAreAllResidentBeforeTheGapFills`,
  `TestStreamReassembly_ParkedRecordHoldsNoPayload` and
  `TestStreamReassembly_ParkedResidencyIndependentOfChunkSize`. Placement and
  the zero-copy property are guarded by those residency tests, by the
  early-placement tests in `go/stream_dstoffset_placement_test.go`
  (`TestStreamReassembly_AheadOfCursorChunkIsResidentAtDstOffset`,
  `TestStreamReassembly_ParkedChunksAreAllResidentBeforeTheGapFills`) and by their
  C++ mirror `tests/test_reassembler_dstoffset_placement.cpp` — all of which
  inspect the destination buffer before the in-order cursor reaches the chunk — and
  by nothing else. The same measurement on the C++ side: the honest revert to the
  parking shape (which deletes `Placed` and both its static_asserts with it) leaves
  the contract, parity-table, ooo-overflow and park-cap tests green and fails only
  the placement test and the completion-peak budget. `AGENTS.md`
  §"Confirmed-Correct Decisions" records the split.
- `TestStreamReassembly_ReservedFieldsAreInert` is **replaced**, as its own
  comment required once these fields acquired meaning: `ChunkHeader@16` is now
  load-bearing, so replaying the table with it set to garbage would assert the
  opposite of the contract. The `dstOffset` cases take over that half, and
  `TestStreamReassembly_AppMsgTypeIsInertToReassembly` (plus its C++ twin) keeps
  the inertness half for `StreamHeader@20` only — every parity case replayed with
  the tag set to `0xCAFEBABE`, every digest byte-identical.
- The dedup gate still runs ahead of every guard, now including both placement
  checks: a duplicate index declaring a different `payloadSize` *or* an
  out-of-bounds `dstOffset` is ACK-ignored, not written and not cause to drop the
  stream.
- Suites: `ctest` **46/46** (was 42/42 at `0.8.21`; `test_stream_integration`
  genuinely ran rather than returning `SKIP`), `go test ./go/` ok, and
  `go test ./go/ -race` ok.

### Specification (normative text added or corrected)

- **§3.3.2 "Reserved and padding space" — new MUST.** Reserved and padding bytes
  MUST reach the wire as zero and receivers MUST ignore them. `ChunkHeader.padding`
  (offset 20) is now explicitly covered, and the clause states *why* rather than
  leaving it as hygiene: the whole "no version bump needed" argument for `lease`,
  `gen` and `fastPathAllowed` rests on an older peer having written `0` into
  reserved space, so a sender that leaks uninitialised bytes there destroys the
  only inference that lets the next reader distinguish "old peer, field absent"
  from a legitimate value — and `padding@20` is the last reserved slot in
  `ChunkHeader`, i.e. the one place that carve-out could still be used. The clause
  distinguishes the per-message stream headers (the sender must write the zero)
  from the segment-resident `ExchangeHeader`/`SlotHeader` (discharged by the Host's
  `memset` of the whole segment in `DirectHost::Init`).
- **§3.3.2 / §3.3.4 — the 1 GiB `maxStreamSize` is stated as a normative ceiling,
  binding the sender as well as the reassembler.** It was previously prose
  (`dstOffset` "suffices only because maxStreamSize is 1 GiB") against a table row
  that read like a default. Now: the configuration MUST NOT be set above `1 << 30`
  and an implementation exposing it as mutable MUST refuse or clamp a larger value;
  and a **sender MUST refuse** a stream whose offsets it cannot express rather than
  truncate `dstOffset` into 32 bits and rely on the peer's `STREAM_START` bound to
  catch it. The §3.3.4 bounds table gained a "Configurable?" column separating the
  one hard wire ceiling (`maxStreamSize`) from the accept/reject parity values
  (`maxStreamChunks`, `maxParkedChunks`) and the one genuinely local knob
  (`maxConcurrentStreams`), and now names the concrete C++/Go identifiers —
  including that the C++ field for the third is spelled `maxStreams`.
- **§3.3.4 "Where each guard fires" — new normative step table.** The
  accept/reject table said *whether* a rejection retires the context but not
  *where* the rejection happens; each row now carries the step at which it may be
  raised. The clause the implementation established during this work is stated
  explicitly: **a drain-time contradiction is always a misplacement (check 2) and
  never a byte overflow**, because the drain no longer re-evaluates the
  running-length bound — park-time accounting plus releasing a record's
  ahead-of-cursor bytes before folding it into the cursor keeps
  `offset + oooBytes` invariant, so a chunk that fitted when recorded still fits
  when drained. Conversely a byte-overflow rejection can only come from the
  arriving chunk's own check, never later. The "release before accounting" clause
  was also corrected: it used to be justified by a drain-time re-check that no
  longer exists, so it now rests on the invariant itself and says so.
- **§2.1 — corrected a dangling code reference.** The attach-time version check
  was cited as `AttachDirect`, which does not exist; the function is
  **`NewDirectGuest`** (`go/direct.go`). This is the most load-bearing normative
  clause in the bump — per §2.1's own argument the version check is the *only* safe
  discriminator against a pre-`0x00080000` sender — so a reader who cannot find the
  code cannot audit the obligation.

## [0.8.21] - 2026-08-03

No wire-format/ABI change; `SHM_VERSION` untouched. All four new members are
non-virtual public additions to header-only classes, so no vtable or layout moves.

### Added

- **`DirectHost::WaitForGuestCall(timeoutMs)` / `WakeGuestCallWaiter()`** — the
  §3.5 doorbell gate, for a host that runs its own processing loop instead of
  `Start()`. Such a host previously had no way to reach the gate at all, and the
  consequence is easy to miss: `hostState` on the guest slots is written only from
  inside the worker's wait, so a host that never runs that wait leaves it
  `HOST_STATE_ACTIVE` forever, the Guest sender's `hostState == HOST_STATE_WAITING`
  doorbell never fires, and a hand-rolled wait expires on its own timeout on every
  call. That is why such hosts have had to burn a core spinning — the spin was not
  merely wasteful, it was the only thing holding latency at ~150 ns. Single-waiter
  only; do not combine with `Start()`.

### Changed

- **The wait gate now has exactly ONE implementation.** `GuestWorkerLoop`'s wait
  triple (the `anyRequestReady` seq_cst predicate, the
  `HOST_STATE_WAITING → recheck → WaitEvent → ACTIVE` Dekker, and the spin phase)
  moved into `GuestCallWorker::WaitForRequest`, which `GuestWorkerLoop` now calls.
  A second copy of a Dekker is how a lost wakeup ships.

- **`Stop()` wakes the waiter before joining.** Without it the join blocked for the
  full 1 s park on every shutdown of an idle host: a flag flip is invisible to a
  thread already blocked in `WaitForSingleObject`.

### Verified

- ctest 42/42 (the new `test_guest_worker_foreign_wait` included). It asserts the
  Dekker and the on-demand wake **separately, on elapsed time against a 4 s park** —
  an outcome-only test ("the request was eventually serviced") passes while the gate
  is entirely broken, because the timeout self-heals it. Both FAIL-first stubs from
  the design were run: dropping the `HOST_STATE_WAITING` publication makes the peer
  never publish and the wait run its full 4004 ms; stubbing `WakeWaiter()` makes the
  on-demand release take 4002 ms.
- Guest-call A/B against a baseline worktree, same session, medians of 3: 1T/64B
  +2.3%, 1T/1024B +2.2%, multi-thread cells all inside their own spread. **The
  single-shot version of that A/B showed −17.6% on 4T/64B and would have been read as
  a regression** — see EXPERIMENTS.md.
- The two event-less fallback paths keep the pre-extraction 100 ms poll cadence
  rather than inheriting the caller's 1 s park (`kFallbackPollMs`); honouring the
  park there would have been a 10x latency and shutdown regression.

## [0.8.20] - 2026-08-02

No wire-format/ABI change; `SHM_VERSION` untouched.

### Fixed

- **The C++ reassembler reclaimed stale streams later than the Go one.** It ran
  `PruneInternal` only when a new stream would exceed `maxStreams` (1024), so a
  host that stays under the cap and never calls `Prune()` held every abandoned
  stream context — each pinning its full advertised `totalSize` — for the life of
  the process. Go prunes on every `StreamStart`. SPEC §3.3.4 leaves the reclaim
  mechanism implementation-defined, so this was not a contract violation, but it
  was an asymmetry with real memory behind it between two implementations that
  are meant to be interchangeable peers. START now prunes unconditionally; the
  cap check that follows is reached with a freshly pruned map, so its second
  prune was removed as redundant. Cost is one pass over at most 1024 entries on a
  path that is already allocating `totalSize` bytes.

  Regression: `tests/test_reassembler_start_prune.cpp`, which drives the case FAR
  below the cap on purpose — at or above it the old code pruned too, so a test
  that filled the map would have passed either way. Mutation-verified against the
  cap-only prune.

Suite: 41/41.

## [0.8.19] - 2026-08-02

No wire-format/ABI change — `SHM_VERSION` is untouched and every shared struct
keeps its layout. The behavior change is confined to host startup.

### Fixed

- **A second `DirectHost::Init` for the same SHM name inside one process
  succeeded and wiped the live host's segment.** `Platform::LockShm` guards the
  name with a `Local\<name>_lock` named mutex, which enforces "one host per
  name" ACROSS PROCESSES correctly — two Excel instances on the same project
  were never affected, which is why this survived. It cannot enforce it WITHIN a
  process: a Win32 mutex is owned by a *thread* and is *recursive*, so a second
  `Init` on the same thread finds `ERROR_ALREADY_EXISTS`, the zero-timeout
  `WaitForSingleObject` returns `WAIT_OBJECT_0` because that thread already owns
  the mutex, and the caller "acquires" the lock — after which `Init` runs
  `memset(shmBase, 0, totalSize)` over the running host's shared memory.
  `LockShm` now claims the name in a process-local registry first and refuses a
  duplicate; `UnlockShm` releases the claim (keyed by handle, signature
  unchanged). `tests/test_double_host` has asserted this contract since it was
  written and had been failing the whole time — the test was right and the
  implementation was half-missing.

### Tests

- **`tests/test_slot_recovery` was a coin flip, not a flaky test, and it hid the
  fact that slot recovery had no regression gate at all.** Its mock guest bounded
  waits by ITERATION COUNT (`++sanity > 1000000` around `yield()`); measured,
  1,000,000 yields is ~105 ms, so the second wait began at t~250 ms and gave up
  at t~355 ms while the host's recovery send lands at t~350 ms — a 5 ms margin
  decided every run, and on expiry the guest published anyway instead of
  failing. Waits are now bounded by wall clock (10 s, ~2 orders of magnitude
  over the intervals the test depends on) and an expiry is a loud, named
  failure. Verified by mutation: disabling the zombie-steal branch in
  `SlotAllocator.h` now fails the test with the exact diagnostic, which the old
  version could not detect.
- **`tests/test_stream_integration` always failed, and the cause was not
  `PATH`.** It shelled out to source-relative paths (`cd tests/go_integration
  && go build`) while ctest runs from the build tree, so the `cd` failed before
  Go was ever invoked. CMake now injects the absolute `go` path (found at
  configure time), the source dir, and a build-tree output path; a missing Go
  toolchain is reported as a CTest SKIP rather than a FAIL, because a hard
  failure there is indistinguishable from the protocol regression the test
  exists to catch.

Full suite: 40/40 (was 37/40).

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
