# Shared Memory IPC Specification (wire protocol `SHM_VERSION 0x00080000`)

> Document tracks the `shm` release line (see `VERSION`). The **wire** protocol version
> (`SHM_VERSION`) is bumped only by a breaking wire change. It was pinned at `0x00070000`
> across the whole ABI-compatible v0.7.x–v0.8.x line, because every field added in that
> line was carved from reserved space that no peer had ever written (e.g. `fastPathAllowed`
> in v0.8.8 — see §2.1). `0x00080000` is the first bump since: it gives meaning to two
> stream-header fields that *were* reserved, `StreamHeader.appMsgType` (§3.3.1) and
> `ChunkHeader.dstOffset` (§3.3.2), and the latter cannot degrade gracefully — see the
> compatibility clause in §2.1.

This document defines the specification for the `xll-gen/shm` Shared Memory IPC system. It serves as the authoritative reference for implementing the protocol in any language (C++, Go, Python, Rust, etc.).

## 1. Overview

The `xll-gen/shm` protocol is a low-latency, lock-free Inter-Process Communication (IPC) system designed for high-throughput "Host-Guest" architectures. It utilizes a **Direct Exchange** model where a pool of fixed-size slots in shared memory is used to pass messages.

- **Host:** The process that creates the Shared Memory and manages the lifecycle. It sends requests to Guests and processes Guest Calls.
- **Guest:** The process that attaches to the Shared Memory. It processes requests from the Host and sends Guest Calls to the Host.
- **Direct Exchange:** A 1:1 mapping between a thread/worker and a slot, eliminating the need for central queues or a scanner thread.

## 2. Shared Memory Layout

The Shared Memory region is a single contiguous block of memory organized as follows:

```
[ ExchangeHeader (64 bytes) ]
[ Slot 0 (slotSize)         ]
[ Slot 1 (slotSize)         ]
...
[ Slot N (slotSize)         ]
```

### 2.1. ExchangeHeader

The first 64 bytes of the shared memory are reserved for the `ExchangeHeader`. This header contains the metadata required for the Guest to map the rest of the memory.

| Field | Type | Offset | Description |
| :--- | :--- | :--- | :--- |
| `magic` | `uint32` | 0 | Magic Number (0x584C4C21). |
| `version` | `uint32` | 4 | Protocol Version (0x00080000). |
| `numSlots` | `uint32` | 8 | Number of Host-to-Guest slots. |
| `numGuestSlots` | `uint32` | 12 | Number of Guest-to-Host (Guest Call) slots. |
| `slotSize` | `uint32` | 16 | Total size of a single slot in bytes. |
| `reqOffset` | `uint32` | 20 | Offset of the Request Buffer within a slot (relative to Slot start). |
| `respOffset` | `uint32` | 24 | Offset of the Response Buffer within a slot (relative to Slot start). |
| `fastPathAllowed` | `uint32` | 28 | **v0.8.8+**: `1` = Host guarantees auto-reclaim is off, so the Guest responder may take the no-claim fast path (§3.4); `0` (default / reclaim-on Host / pre-v0.8.8 Host) = Guest must use the full-claim path. Safe-by-default polarity — an older *release* on either side degrades to the slow path, never to an unsafe fast path. Carved from `reserved`; did not bump the wire version (`0x00070000` at the time), which is why such a release pair attaches at all. |
| `reserved` | `uint8[32]`| 32 | Reserved to ensure 64-byte alignment. |

**Total Size:** 64 Bytes.

**Version compatibility is a HARD FAILURE at attach, and that is what licenses the bump.** The Host *writes* `version` when it creates the segment (`DirectHost::Init`). Every peer that **attaches** to a segment it did not create MUST compare this field against its own `SHM_VERSION` before using anything else in the mapping, and MUST refuse the segment outright on any mismatch — not degrade, not negotiate, not ignore the fields it does not recognise. In this codebase the attaching peer is the Go Guest, and the check is in **`NewDirectGuest`** (`go/direct.go`): it copies `magic` and `version` out of the mapping, then on mismatch calls `CloseShm` and returns `protocol version mismatch: 0x… (expected 0x…)`, alongside the identical treatment of a bad `magic`. It compares against the Go constant `shm.Version` (`go/direct.go`), whose C++ counterpart is `SHM_VERSION` (`include/shm/IPCUtils.h`); both read `0x00080000`. There is no C++ guest, so the C++ side of this obligation is the write plus the layout guards; a third-party implementation of any role that attaches inherits the full obligation. Net effect: a peer pair either agrees on the entire wire layout or does not communicate at all, and mixed-version traffic is impossible by construction rather than by convention.

That non-silence is the entire reason a version bump is a *sufficient* remedy for the `0x00080000` change. Every earlier v0.7.x/v0.8.x field addition was carved from bytes an older peer had never written, so a zero read out of `reserved` meant "peer does not implement this" and each side could degrade on its own. `ChunkHeader.dstOffset` (§3.3.2) breaks that pattern: an old sender leaves it `0`, and `0` is a perfectly **legal** offset — it is exactly what chunk 0 of every conforming stream declares. A new receiver reading an old sender would therefore place every chunk at offset 0 and complete a silently corrupt stream with no in-band signal that anything was wrong. There is no value the field could take that distinguishes "offset zero" from "sender predates the field", so the discriminator has to sit outside the field, in `version`, where the mismatch is caught once at attach and is loud. Accordingly: **both peers MUST ship from the same `shm` release** (already the standing requirement — see §3.5's `hostState` note), and `dstOffset` MUST NOT be treated as optional or best-effort by a receiver that has completed attach.

**The two constants are guarded against a half-applied bump, in both directions.** `SHM_VERSION` and `shm.Version` are one value written twice, so the failure mode that matters is not a wrong number but an edit that lands on one side only — which is exactly what happened during this bump (`include/shm/IPCUtils.h` moved to `0x00080000`, `go/direct.go` stayed at `0x00070000`) while **both** suites reported green. `tests/test_protocol_version.cpp` could not see it, because it compares the value the C++ host wrote into the segment against the C++ constant it wrote it from — tautological across the language boundary; and `tests/test_stream_integration.cpp`, the only test that runs Go, returns CTest's `SKIP_RETURN_CODE` whenever `find_program(NAMES go)` misses, i.e. on the ordinary C++-only build. The cross-language guard is therefore a *pair*, each half reading the other language's source as data so neither needs the other toolchain: `tests/test_go_version_parity.cpp` (parses `go/direct.go`; the path is injected as `SHM_GO_DIRECT_GO` by `tests/CMakeLists.txt` and a missing definition is `#error`) and `go/version_wire_guard_test.go` (parses `include/shm/IPCUtils.h`). Both are written so that every way of failing to *reach* a value — file missing, no declaration, more than one declaration, unparseable or trailing-garbage literal — is a hard FAIL rather than a skip or a vacuous pass, because "parse miss degrades to nothing to check" is the same self-disabling trap one layer down.

### 2.2. Slot Layout

Each slot follows this structure:

```
[ SlotHeader (128 bytes) ]
[ Request Buffer ...     ]
[ Response Buffer ...    ]
```

The specific offsets for the Request and Response buffers are defined in the `ExchangeHeader` (`reqOffset`, `respOffset`).

#### 2.2.1. SlotHeader

The `SlotHeader` controls the state of the transaction. It **must** be aligned to 128 bytes to prevent false sharing between CPU cores.

| Field | Type | Offset | Description |
| :--- | :--- | :--- | :--- |
| `pre_pad` | `uint8[64]` | 0 | Padding to isolate the header. |
| `state` | `atomic<uint32>` | 64 | Current state of the slot (see Section 3.1). |
| `hostState` | `atomic<uint32>` | 68 | Host activity state (Active/Waiting). |
| `guestState` | `atomic<uint32>` | 72 | Guest activity state (Active/Waiting). |
| `msgSeq` | `uint32` | 76 | Unique Message Sequence Number. |
| `msgType` | `uint32` | 80 | Message Type (e.g., Normal, Shutdown). |
| `reqSize` | `int32` | 84 | Size of Request payload (see Section 3.3). |
| `respSize` | `int32` | 88 | Size of Response payload (see Section 3.3). |
| _(align pad)_ | `uint32` | 92 | 4 bytes of padding to 8-byte-align `lease`. |
| `lease` | `atomic<uint64>` | 96 | **v0.7.0+**: wall-clock-ns heartbeat written by the current owner immediately after CAS'ing `state` to a non-FREE value. See §3.6. |
| `gen` | `atomic<uint64>` | 104 | **v0.7.5+**: claim generation counter; advanced by every slot-claiming path *before* its state-claiming CAS. Makes crash-recovery reclamation airtight against the ABA hazard. See §3.6.1. |
| `reserved` | `uint8[16]` | 112 | Reserved to reach 128 bytes. |

**Total Size:** 128 Bytes.

**Compatibility note**: the `lease` slot used to be inside `reserved[36]` (v0.6.x and earlier), and `gen` was carved from the remaining `reserved[24]` in v0.7.5. Older readers see those bytes as opaque padding and behave exactly as before — they never wrote into `reserved`, so introducing typed fields there is forward-compatible and the layout stays 128 bytes. Neither addition bumped the protocol version (`0x00070000` at the time): both are ABI-compatible and degrade gracefully (a peer that does not advance `gen` falls back to the pre-v0.7.5 reclaim behavior for its slots). The `SlotHeader` layout is untouched by the `0x00080000` bump, which is confined to the stream headers of §3.3.1/§3.3.2.

### 2.3. Slot Partitioning

The total pool of slots is divided into two ranges:
1.  **Host Slots:** Indices `0` to `numSlots - 1`. Used for Host-initiated requests (Host sends to Guest).
2.  **Guest Slots:** Indices `numSlots` to `numSlots + numGuestSlots - 1`. Used for Guest-initiated calls (Guest sends to Host).

## 3. Protocol & State Machine

### 3.1. Slot States

The `state` field in `SlotHeader` manages the ownership of the slot.

| State Constant | Value | Description |
| :--- | :--- | :--- |
| `SLOT_FREE` | 0 | Slot is empty. Owner (Host for Host Slots) can claim it. |
| `SLOT_REQ_READY` | 1 | Request data is written. Receiver (Guest) can process it. |
| `SLOT_RESP_READY` | 2 | Response data is written. Owner (Host) can read it. |
| `SLOT_DONE` | 3 | **Responder publish lock** (transient). The responder holds the slot here while it writes the response header fields; see §3.4/§3.5 "Responder publish rule". |
| `SLOT_BUSY` | 4 | Slot is claimed by Host (for Host Slots), data is being written. |
| `SLOT_GUEST_BUSY` | 5 | Slot is claimed by Guest (for Guest Slots), data is being written. |

*Note: `SLOT_DONE` (3) was reserved and unused up to v0.8.16. It is now the responder's **publish lock**: a responder that owns a slot transitions it into `SLOT_DONE`, writes `respSize`/`msgType`, then transitions on to `SLOT_RESP_READY` (§3.4 step 5 / §3.5 step 5). Both implementations therefore **produce and consume** `SLOT_DONE`, and both MAY observe it on any slot. `SLOT_DONE` is deliberately **NOT** in the zombie-steal candidate set `{SLOT_REQ_READY, SLOT_RESP_READY, SLOT_GUEST_BUSY}` (§3.6.1), which is exactly what makes it usable as a lock: a slot parked there cannot be stolen out from under the publish. It is transient by construction — the interval between the two transitions contains no blocking call and no peer round-trip, only the header stores.*

### 3.2. Message Types

| Constant | Value | Description |
| :--- | :--- | :--- |
| `MSG_TYPE_NORMAL` | 0 | Standard data payload. |
| `MSG_TYPE_HEARTBEAT_REQ` | 1 | Keep-alive request. |
| `MSG_TYPE_HEARTBEAT_RESP` | 2 | Keep-alive response. |
| `MSG_TYPE_SHUTDOWN` | 3 | Signal to terminate worker loop. |
| `MSG_TYPE_FLATBUFFER` | 10 | Zero-Copy FlatBuffer (End-aligned). |
| `MSG_TYPE_GUEST_CALL` | 11 | Guest-initiated call to Host. |
| `MSG_TYPE_STREAM_START` | 13 | Start of a streaming transfer. |
| `MSG_TYPE_STREAM_CHUNK` | 14 | Chunk of a streaming transfer. |
| `MSG_TYPE_SYSTEM_ERROR` | 127 | System-level rejection (e.g., buffer overflow, malformed request). Sent by receiver in lieu of NORMAL response. The receiver overwrites `msgType` to SYSTEM_ERROR in the response slot; payload may be empty. Senders must check for this value before parsing response data. |
| `MSG_TYPE_APP_START` | 128 | Start of user-defined message types. |

### 3.3. Streaming Protocol

For large data transfers, the `STREAM_START` and `STREAM_CHUNK` message types are used. These messages contain specific headers within the Request Buffer.

#### 3.3.1. StreamHeader (MSG_TYPE_STREAM_START)

| Field | Type | Offset | Description |
| :--- | :--- | :--- | :--- |
| `streamId` | `uint64` | 0 | Unique identifier for the stream. |
| `totalSize` | `uint64` | 8 | Total size of the data to be transferred. |
| `totalChunks` | `uint32` | 16 | Total number of chunks. |
| `appMsgType` | `uint32` | 20 | **`0x00080000`+**: application-defined routing tag, OPAQUE to `shm`. `0` = unspecified. Was `reserved` at `0x00070000`. |

**Total Size:** 24 Bytes.

**`appMsgType` is opaque (MUST).** `shm` neither interprets nor validates this field. It carries no meaning at the transport layer: there is no reserved range, no registry, and no value that `shm` rejects — a receiver MUST accept every one of the 2^32 encodings, including `0`. It exists so an application can route a completed stream without inventing a second framing layer inside the payload, and giving it a name is a **transport-neutral** act in the sense of `AGENTS.md` ("never embed protocol-specific assumptions into `shm`"): the tag's vocabulary belongs to the application, not to this specification.

Normatively, therefore, the only property this field has today is a *negative* one: **`appMsgType` MUST NOT affect reassembly in any way.** Two `STREAM_START`s identical except for this field MUST drive byte-identical reassembly behavior — same acceptances, same rejections, same delivered bytes. (Tripwires: `go/stream_parity_table_test.go::TestStreamReassembly_AppMsgTypeIsInertToReassembly` and the matching check at the end of `tests/test_reassembler_parity_table.cpp`, which replay every parity case with the field set to garbage and compare digests.)

The *write* side, by contrast, is a positive obligation and needs its own guard: every other `appMsgType` test hand-builds the header bytes, so none of them exercises the real sender, and the tag could have been dropped, written at the wrong offset, or written into the `ChunkHeader` instead with the whole suite still green. `go/stream_sender_appmsgtype_test.go` drives the exported `StreamSender` against a real segment and reads the bytes back off the wire from the host side (`TestStreamSender_WritesAppMsgTypeAtWireOffset20`, `TestStreamSender_DefaultAppMsgTypeIsZero`).

**Receive-side surfacing is DEFERRED and is NOT part of the API.** Senders write the field (Go `StreamSender.SetAppMsgType`, C++ `StreamSender::Send(..., appMsgType)`), and a reassembler parses past it, but **neither implementation delivers it to the stream handler** — the completion callback signature is still `(streamId, data)` on both sides. Do not describe or rely on a capability the receive side does not have. The wire slot is claimed now, at the bump that was happening anyway, precisely so that adding the receive-side API later needs **no further version bump**: the slot is already reserved-by-name and already carries the sender's value, so surfacing it changes only local API surface, not the bytes on the wire.

#### 3.3.2. ChunkHeader (MSG_TYPE_STREAM_CHUNK)

| Field | Type | Offset | Description |
| :--- | :--- | :--- | :--- |
| `streamId` | `uint64` | 0 | Unique identifier for the stream. |
| `chunkIndex` | `uint32` | 8 | Index of this chunk (0-based). |
| `payloadSize` | `uint32` | 12 | Size of the payload following this header. |
| `dstOffset` | `uint32` | 16 | **`0x00080000`+**: **ABSOLUTE** byte offset of this chunk's payload within the destination buffer (i.e. within the reassembled stream), not a delta and not relative to anything. Was `reserved` at `0x00070000`. Load-bearing: see the placement contract in §3.3.4. |
| `padding` | `uint32` | 20 | Padding to ensure 8-byte alignment (Total 24 bytes). **Senders MUST write `0`; receivers MUST ignore it.** See "Reserved and padding space" below. |

**Total Size:** 24 Bytes.
The payload follows immediately after the `ChunkHeader`.

**Reserved and padding space: it MUST reach the wire as zero, and receivers MUST ignore it.** This binds every byte of every header in this specification that is not a named, meaningful field — `ChunkHeader.padding` (offset 20), the `SlotHeader` `pre_pad`/align-pad/`reserved` regions of §2.2.1, and `ExchangeHeader.reserved` of §2.1. A receiver MUST NOT read those bytes, MUST NOT validate them, and MUST NOT branch on them. The producing obligation attaches to whoever writes them:

- **Stream headers (§3.3.1/§3.3.2) are composed per message** into the Request Buffer by the sender, so **the sender MUST write `0`** into `ChunkHeader.padding`. It MUST NOT leave the field uninitialised — a `ChunkHeader` declared on the stack and `memcpy`'d whole puts host stack residue on the wire and violates this clause even though the receiver never looks at the field.
- **Segment-resident headers (`ExchangeHeader`, `SlotHeader`) are written once at creation.** The obligation is discharged by the Host zero-filling the whole segment in `DirectHost::Init` before publishing it; no per-transaction writer is required to re-zero them, and none may write anything but zero into them.

The obligation is not hygiene, it is what §2.1's compatibility argument is built on. Every field this protocol has added without a version bump — `SlotHeader.lease` (v0.7.0), `SlotHeader.gen` (v0.7.5), `ExchangeHeader.fastPathAllowed` (v0.8.8) — was carved from reserved space and was safe to carve *only because the older peer had written `0` there*, which let the new peer read a zero and conclude "peer does not implement this field". A sender that puts garbage in reserved space destroys that inference: the next reader to give those bytes a meaning cannot distinguish "old peer, field absent" from "new peer, value 0xDEADBEEF", and the graceful-degradation carve-out is gone. `ChunkHeader.padding@20` is the **last** reserved slot in `ChunkHeader` (offsets 0–19 are all named as of `0x00080000`), so it is the one remaining place in that struct where the carve-out could ever be used again — a sender leaking stack bytes into it forfeits exactly that.

Zeroing is also what makes the wire deterministic for the digest-based parity tests, and it removes an information leak: the padding bytes are host-process memory copied into a shared segment the peer can read.

Regression: `tests/test_stream_chunk_padding.cpp` reads `padding@20` out of the C++ sender's real `STREAM_CHUNK` request bytes. Note the failability caveat it documents: uninitialised stack is very often incidentally zero, so the test also scribbles the frame the sender will occupy — which is best-effort and toolchain-dependent (it catches a default-initialized `ChunkHeader` under MinGW/GCC but not under MSVC `/O2`, where the frame is inlined away). Do not read a green run of that test as proof the field is assigned; the assignment (`ChunkHeader ch{}` in `include/shm/Stream.h`, `putChunkHeader`'s explicit `0` in `go/stream_sender.go`) is the contract.

**Both header structs stay exactly 24 bytes**, and every field keeps the offset it had at `0x00070000` — the bump renames two reserved slots and gives them meaning; it moves, adds and removes nothing. `StreamHeader` is 24 bytes and `ChunkHeader` is 24 bytes, as they were, which is why the payload still begins at byte 24 of the request buffer on both sides. Compile-time guards on both peers pin this, and the two sides are symmetric field-for-field:

- **C++** (`include/shm/Stream.h`): `static_assert(sizeof(...) == 24)` for each struct, plus a per-field `static_assert(offsetof(...) == N)` for **every named field** — `StreamHeader.streamId@0`/`totalSize@8`/`totalChunks@16`/`appMsgType@20` (four) and `ChunkHeader.streamId@0`/`chunkIndex@8`/`payloadSize@12`/`dstOffset@16`/`padding@20` (five). The `padding@20` assert is **mirror completeness**, not a detection guard: `padding` is the last field and every other offset is already pinned, so any edit that moves it also trips `sizeof == 24` or `dstOffset@16`. It exists so the C++ and Go guard sets are literal mirrors (R25) — Go pins `Padding@20` bidirectionally — and so the field's normative must-be-zero status has a compile-time anchor.
- **Go** (`go/stream.go`): dual zero-array `unsafe.Sizeof` asserts for both structs (`[24 - sizeof]byte` *and* `[sizeof - 24]byte`), plus a second `var(...)` block of **nine bidirectional `unsafe.Offsetof` guards** — `[N - off]byte` *and* `[off - N]byte` for each of the same nine fields, so a field moving in *either* direction fails the build rather than only a field moving one way.

Per `AGENTS.md` §"Protocol & Memory Layout", the offset guards are the SSOT enforcement for these tables — a size-preserving field swap would otherwise pass a size-only assert.

**`dstOffset` is `uint32`, so 1 GiB is a wire ceiling, not a policy default — and it binds the sender as well as the reassembler (MUST).** The field can express offsets in `[0, 2^32)` only, so the largest stream whose chunk offsets are all representable is 4 GiB − 1; §3.3.4 sets the normative bound an order of magnitude below that, at `maxStreamSize` = 1 GiB. Two obligations follow, and neither is optional:

1. **Configuration.** `maxStreamSize` MUST NOT be configured above `1 << 30`. It is the one bound in the §3.3.4 table that is **not** a tuning knob: it is simultaneously an accept/reject parity value (a peer that raised it would accept a `STREAM_START` its counterpart rejects, and §3.3.4 requires the two verdicts to agree) and a precondition for `dstOffset` being wide enough at all. An implementation that exposes it as a mutable setting MUST refuse or clamp a larger value rather than honour it, and SHOULD pin the default at compile time. (C++: `shm::kMaxStreamSize` in `include/shm/Stream.h`, `StreamReassemblerConfig::IsValid()`, the `static_assert` on the default, and the constructor clamp in `include/shm/StreamReassembler.h`. Go: `MaxStreamSize` in `go/stream.go` is a `const` and is not configurable.)
2. **Sender.** A sender MUST refuse, before transmitting anything, a stream whose byte offsets it cannot express — i.e. `totalSize > maxStreamSize`. It MUST NOT truncate `dstOffset` into 32 bits and send anyway, and it MUST NOT rely on the receiver's `STREAM_START` bound to catch the case for it: a truncated offset is a *legal-looking* value, so the resulting stream would either be silently misassembled or be rejected only after the sender had committed chunks. (C++: `StreamSender::Send` returns `Error::InvalidArgs` for `size > kMaxStreamSize`. Go: `sendStart` in `go/stream_sender.go` returns an error naming both the size and the bound, placed before `acquireSlot`.)

Raising the ceiling past 4 GiB is therefore not a configuration change at all — it requires widening `dstOffset`, which moves `padding@20`, changes both header layouts, and is a `SHM_VERSION` bump in its own right.

Regression for both obligations: `tests/test_stream_size_ceiling.cpp` (the reassembler-config half — `IsValid()` reports an over-ceiling value and the constructor clamp is observable through `StreamReassembler::Config()`; and the C++ sender half — `StreamSender::Send` returns `Error::InvalidArgs`), and `go/stream_sender_size_ceiling_test.go` for the Go sender half (it asserts no request reaches the host and the guest slot is left `SLOT_FREE`, which is the observable form of "before transmitting anything", and pins the comparison as strict `>` so an exactly-ceiling stream is not refused). The sender check is placed before any slot acquisition and before `data` is dereferenced, which is what makes the guard reachable without committing a 1 GiB buffer; both tests depend on that ordering.

#### 3.3.3. Data Alignment (Negative Size)

The `reqSize` and `respSize` fields indicate the location of the data within the buffer:
- **Positive (> 0):** Data starts at the beginning of the buffer (Offset 0).
- **Negative (< 0):** Data is aligned to the **end** of the buffer. The actual size is `abs(size)`. The start offset is `BufferSize - abs(size)`. This is typically used for FlatBuffers construction.

#### 3.3.4. Reassembly Limits & Completion Contract

A reassembler accepts `StreamHeader`/`ChunkHeader` messages from an untrusted peer. The wire layout (§3.3.1/§3.3.2) is the only thing the peer controls, so corrupt or malicious values MUST NOT drive unbounded allocation, silent truncation, or mis-delivery. Both the C++ Host (`StreamReassembler`) and the Go Guest (`NewStreamReassembler`) implement the **identical** contract below; a stream accepted by one peer is accepted by the other, and a stream rejected by one is rejected by the other.

**Bounds.** The first two are header bounds, checked at `STREAM_START` before any allocation: a `StreamHeader` violating either MUST be rejected with `MSG_TYPE_SYSTEM_ERROR`. The last two bound in-flight reassembly state and are enforced where their own clauses below say:

| Bound | Value | Configurable? | Field guarded |
| :--- | :--- | :--- | :--- |
| `maxStreamSize` | `1 << 30` (1 GiB) | **NO — normative ceiling.** MUST NOT be raised; MUST be refused or clamped if configured higher. Also binds the sender. See §3.3.2. | `totalSize` |
| `maxStreamChunks` | `1 << 20` | Parity value: MAY be changed, but only on **both** peers together | `totalChunks` |
| `maxConcurrentStreams` | `1024` | Local: bounds this peer's own memory only — but see the caveat below, since `maxParkedChunks` is *derived* from it | number of in-flight (partially reassembled) streams |
| `maxParkedChunks` | `1024` (= `maxStreamChunks / maxConcurrentStreams`) | Parity value: MUST be equal on both peers | number of chunks one stream may hold recorded (payload-free, see "Chunk placement") ahead of its in-order cursor |

The distinction in column 3 is normative. `maxStreamSize` is the only row that is a **hard wire ceiling**: it is pinned by `ChunkHeader.dstOffset` being 32 bits wide (§3.3.2), so no deployment may raise it, and both the reassembler configuration and the *sender* are bound by it — the sender MUST refuse a stream it cannot express rather than truncate its offsets. `maxStreamChunks` and `maxParkedChunks` are accept/reject parity values: any value works, but a peer that changes one alone will reject a stream its counterpart accepts, which §3.3.4 forbids. `maxConcurrentStreams` is the only genuinely local knob — it bounds nothing on the wire, only how many contexts this peer keeps, and the peers already differ in the reclaim mechanism they use to honour it. **Caveat:** because the default `maxParkedChunks` is *derived* as `maxStreamChunks / maxConcurrentStreams`, changing `maxConcurrentStreams` on one peer alone silently changes a parity value. A peer that raises or lowers it MUST therefore pin `maxParkedChunks` to the value the other peer uses.

Concrete field names: C++ `StreamReassemblerConfig::{maxStreamSize, maxStreamChunks, maxStreams, maxParkedChunks}` (`include/shm/StreamReassembler.h`; note the third is spelled `maxStreams`) with the ceiling constant `shm::kMaxStreamSize` in `include/shm/Stream.h`; Go compile-time constants `MaxStreamSize`, `MaxStreamChunks`, `MaxConcurrentStreams`, `MaxParkedChunks` (`go/stream.go`).

`totalChunks` MUST be validated **before** any allocation is sized, and it MUST NOT itself size an allocation: see "Residency bound" below.

**Residency bound (MUST).** Per-stream reassembly state MUST be `O(totalSize)` — the advertised size — plus the bounded parked-chunk bookkeeping below. It MUST NOT be `O(totalChunks)`. An implementation that indexes a `totalChunks`-sized array (one element per chunk, a presence bitmap, etc.) violates this clause: `totalChunks` and `totalSize` are independent header fields, so a peer advertising `totalSize = 1` with `totalChunks = maxStreamChunks` would commit per-chunk overhead × 2^20 for a one-byte stream (measured ~24 MiB per stream, ~25 GiB at `maxConcurrentStreams`), which no payload-byte guard can see. Both peers therefore preallocate a destination buffer of `totalSize` at `STREAM_START` and write every chunk straight into it (see "Chunk placement" below), keeping **only** a sparse, payload-free record for ahead-of-cursor chunks (`go/stream.go`: `streamContext.buf` / `ooo`; `include/shm/StreamReassembler.h`: `StreamContext::buf` / `ooo`). Peak residency for a completing stream is likewise bounded by `totalSize`: assembling into a second full-size buffer at completion time (`2 × totalSize`, i.e. 2 GiB for a 1 GiB stream) is not permitted.

**Chunk placement (`dstOffset`) — MUST.** Since `0x00080000` the destination of a chunk's payload is stated on the wire rather than inferred from arrival order.

*Sender obligation.* For every `STREAM_CHUNK`, the sender MUST set `dstOffset` to the sum of the `payloadSize`s of all **lower** chunk indices of that stream — the absolute byte position at which this chunk's payload begins in the reassembled stream. Equivalently: `dstOffset(0) = 0` and `dstOffset(i) = dstOffset(i-1) + payloadSize(i-1)`. This holds regardless of the order in which the sender transmits the chunks, and regardless of chunk sizes being uniform or not. It follows that the accepted chunks' destination ranges **tile** `[0, totalSize)` exactly — no gap, no overlap — which is what makes the completion contract's byte-sum test equivalent to "the buffer is fully written".

*Receiver obligation — placement.* On every accepted `STREAM_CHUNK`, the receiver MUST write the payload directly to `destination[dstOffset]`, **on arrival, in whatever order chunks arrive**. A receiver MUST NOT hold payload bytes anywhere else waiting for a gap to fill: out-of-order arrival parks **bookkeeping only** — `{dstOffset, payloadSize}` per index, so the in-order cursor can be advanced and completion detected — and **never payload bytes**. An out-of-order chunk therefore costs zero parked payload copies, and the drain that follows a gap being filled copies nothing at all; it only advances the cursor. (This is the whole point of the field: before it, an out-of-order chunk was copied into a parked buffer and then copied a second time into the destination.)

The "bookkeeping only, never payload" half of that obligation is guarded **structurally** on the C++ side: `StreamContext::Placed` (`include/shm/StreamReassembler.h`) carries `static_assert(sizeof(Placed) == 16)` and `static_assert(std::is_trivially_copyable<Placed>::value)`, so every way of reintroducing a parked copy — an owning `std::vector`/`std::string` member, or an inline byte array — is a **compile** error rather than a byte-budget test that only notices once the copies are large enough to clear its slack. The Go analogue is a runtime assertion on the record's size (`TestStreamReassembly_ParkedRecordHoldsNoPayload`, `go/stream_dstoffset_residency_test.go`). The *positive* half — the bytes really are at `destination[dstOffset]` **before** the cursor arrives — cannot be asserted this way and needs a test that inspects the destination buffer early; see `AGENTS.md` §"Confirmed-Correct Decisions" for why no output-byte or digest test can substitute.

*Receiver obligation — the two checks, both MUST.* `dstOffset` is **peer-controlled**, so it is validated twice, on different axes:

1. **Bounds, on every chunk, before the payload is written anywhere.** The receiver MUST reject the message when `dstOffset + payloadSize > totalSize`, and MUST evaluate that condition without unsigned wrap — i.e. by subtraction (`dstOffset > totalSize || payloadSize > totalSize - dstOffset`), never by computing the sum in the field's own width, where a hostile `dstOffset` near 2^32 would wrap to a small value and admit an out-of-buffer write. This is the bounds check for the write, so it applies to in-order and ahead-of-cursor chunks alike; an implementation that bounds-checks only the in-order path does not satisfy this clause. It also applies when `payloadSize == 0`: `dstOffset <= totalSize` is required of a zero-length chunk too, and MUST NOT be short-circuited by an "empty payload writes nothing" fast path — such a chunk writes nothing but is still *recorded*, and a nonsense offset admitted here resurfaces as a drain-time contradiction later.
2. **Self-consistency, when the index becomes in-order.** At the moment a chunk index becomes the in-order cursor's next index — whether it arrived in order or was parked earlier and is reached by the drain — its recorded `dstOffset` MUST equal the running cursor (`Σ payloadSize` of all lower indices, which the receiver has already accumulated). If it does not, the peer has contradicted its own framing, and the receiver MUST treat the stream as corrupt and **drop** it.

Check 2 is **exact, not heuristic**: every index passes through it exactly once (either directly on an in-order arrival or on the drain), so a sender that misplaces *any* chunk is caught before the stream can complete. That is what allows the receiver to keep cheap per-index dedup instead of tracking covered byte intervals. Note the deliberate limit of check 1: it bounds writes to inside the destination buffer but does **not** detect overlap *within* it, so a lying peer's chunk can transiently overwrite bytes belonging to another chunk. That is harmless because check 2 guarantees such a stream is dropped rather than delivered — the guarantee is "never garbled data delivered", not "never a wasted write".

Both checks run **before** the payload is written (bounds) or before the index is accounted (consistency), and both rejections retire the context — see the drop table below.

**Where each guard fires (normative ordering).** The drop table below says *whether* a rejection retires the context; this clause says **at which step** each guard is allowed to fire, which is what makes the table unambiguous. A conforming receiver evaluates a `STREAM_CHUNK` in exactly these steps, in this order:

| Step | Guard | Outcome on failure |
| :--- | :--- | :--- |
| 1 | Framing: `reqSize >= sizeof(ChunkHeader)` and `reqSize >= sizeof(ChunkHeader) + payloadSize`, evaluated without overflow | reject, stream **kept** |
| 2 | Stream lookup: the `streamId` is known and its context is still live (not already delivered, dropped, or reclaimed); `chunkIndex < totalChunks` | reject (unknown / already-retired id: nothing to drop), stream **kept** |
| 3 | Duplicate index (already assembled **or** already recorded ahead of the cursor) | ACK-ignore (`MSG_TYPE_NORMAL`); no later step runs |
| 4 | Running-length byte guard: `offset + oooBytes + payloadSize <= totalSize` | **drop** |
| 5 | Placement bounds (check 1), by subtraction | **drop** |
| 6 | Write the payload to `destination[dstOffset]` | — (cannot fail; steps 4–5 have already bounded it) |
| 7a | *If the index is the cursor's next:* placement self-consistency (check 2) | **drop** |
| 7b | *Else:* parked-record count bound (`maxParkedChunks`) | **drop** |
| 8 | *After 7a only:* drain — for each successive recorded index, release its ahead-of-cursor byte accounting, then re-check placement self-consistency (check 2) for that index | **drop** |
| 9 | Completion contract (all indices filled ⇒ `Σ payloadSize == totalSize`) | **drop** |

**A drain-time (step 8) contradiction is always a MISPLACEMENT and never a byte overflow.** The drain loop deliberately does **not** re-evaluate the step-4 running-length bound, and it MUST NOT: park-time accounting already charged those bytes, and the drain releases a record's ahead-of-cursor accounting *before* folding it into the cursor, so the running received length `offset + oooBytes` is invariant across the release-then-account pair. A chunk that fitted when it was recorded therefore still fits when it is drained, by construction — a re-check there could only ever fire spuriously, and adding one would reject well-formed out-of-order streams. Consequently the *only* guard that can reject at step 8 is check 2, and a step-8 rejection carries exactly one diagnosis: the sender's declared `dstOffset` for that index disagrees with the cursor. Conversely, a byte-overflow rejection can only ever come from step 4, i.e. at the arrival of the offending chunk and never later. This is why the drop table's two placement rows name *both* detection sites for check 2 (step 7a and step 8) while the byte-guard row names only one.

Note that steps 4 and 5 are independent and both mandatory: step 4 bounds *how many bytes in total* the peer has made resident, step 5 bounds *where this particular write lands*. Neither implies the other — a chunk can be within the running byte budget yet aimed outside the buffer, and a chunk aimed inside the buffer can still push the running total past `totalSize` (e.g. by overlapping a range another index already accounted for).

**Completion contract.** A stream completes — and the `onStream` callback fires exactly once — only when **both**:
1. `received == totalChunks` (every chunk index filled exactly once), and
2. `Σ payloadSize == totalSize` (the assembled byte count equals the advertised total).

If all chunks have arrived but `Σ payloadSize != totalSize`, the stream is **dropped** with `MSG_TYPE_SYSTEM_ERROR` rather than delivered truncated or garbled. The completion check is the only thing that can catch an **under**-sized stream.

**Per-chunk overflow guard (order-independent, MUST).** On every `STREAM_CHUNK`, and **before** the payload is copied or buffered anywhere, the reassembler MUST compute the running *received* length — `Σ payloadSize` over every chunk index accepted so far — and reject the message with `MSG_TYPE_SYSTEM_ERROR`, dropping the stream, if accepting this chunk would make that sum exceed `totalSize`. The running length MUST include chunks that arrived **ahead of the in-order cursor** and are not yet accounted in order because an earlier index is missing; an implementation that applies the guard only to the in-order path does not satisfy this clause. Since `0x00080000` those bytes are already resident in the destination buffer, which makes counting them structurally mandatory rather than merely prudent: the in-order cursor alone *understates* what the peer has made resident. Such an implementation lets a peer advertise a tiny `totalSize`, then park unbounded bytes under out-of-order indices — the completion check above would eventually reject the stream, but only once every byte was already resident, which is precisely the unbounded allocation this section forbids. Rejection MUST therefore happen at arrival time, not at completion time. This bounds reassembly memory for any single stream to `totalSize`, and — combined with `maxConcurrentStreams` below — total reassembly memory to `maxConcurrentStreams × maxStreamSize`.

The guard is a strict `>` comparison: a chunk that exactly fills the remaining advertised space is valid and MUST be accepted. Implementations MUST account for the bytes of ahead-of-cursor chunks separately from the in-order cursor and release that accounting exactly when the corresponding index is accounted in order (i.e. as the drain reaches it — since `0x00080000` the bytes themselves are already in the destination, so "released" means the count moves from *placed* to *assembled*, not that anything is copied). The release MUST happen **before** the index is folded into the cursor, and the two operations MUST stay adjacent, so that `offset + oooBytes` is the exact running received length at **every** point at which a guard could read it — never a moment in which the same bytes are counted twice. Historically this order was pinned because the drain re-evaluated the byte guard, and an inverted order made it double-count and reject a well-formed out-of-order stream as over-sized. Since `0x00080000` the drain performs **no** byte re-check at all (see "Where each guard fires", step 8), so an inverted order is no longer caught at the drain: the requirement now stands on the invariant itself, which is the weaker position of the two. Treat the pair as one indivisible step (`go/stream.go`: `streamContext.oooBytes` / `fits`; `include/shm/StreamReassembler.h`: `StreamContext::oooBytes` / `Fits`).

**Parked-chunk bound (count dimension, MUST).** The byte guard above bounds *payload*; it cannot bound per-chunk bookkeeping, for two independent reasons: a parked record costs memory even at zero payload, and a **zero-length chunk can never trip the byte guard** (`Σ payloadSize + 0 > totalSize` is false for any stream that is still alive). A reassembler MUST therefore also refuse, with `MSG_TYPE_SYSTEM_ERROR` and dropping the stream, an ahead-of-cursor chunk that would make the number of simultaneously parked chunks for that stream exceed `maxParkedChunks`. The check MUST run **before** the parked record is created. Since `0x00080000` that record is the *only* thing this bound bounds — the payload is no longer duplicated into it (see "Chunk placement") — which makes the bound no less necessary: the count dimension is exactly the dimension the byte guard cannot see. Without it a peer advertises `totalSize = 1` with `totalChunks = maxStreamChunks` and parks 2^20−1 zero-length chunks — measured at ~80 B per entry on the Go side (hash-map overhead dominates; the `{dstOffset, payloadSize}` record itself is 12 bytes), i.e. ~80 MiB resident for a stream that advertised one byte, times `maxConcurrentStreams`.

`maxParkedChunks` is `maxStreamChunks / maxConcurrentStreams` (1024 with the values above): the bound that keeps the *aggregate* parked-entry count across all in-flight streams no larger than the chunk count of one maximal stream, i.e. ~64–80 MiB of bookkeeping worst case. It is far above any legitimate sender — `StreamSender` parks at most `maxInFlight − 1` chunks and the library default in-flight depth is 1 — but it does mean a stream delivered in strictly descending index order is only accepted while its chunk count stays within the bound. The value is part of this accept/reject contract, not a local tuning knob: changing it on one peer alone makes a stream that peer accepts get rejected by the other. Combined with the byte guard, per-stream residency is `totalSize` plus at most `maxParkedChunks` entries of bookkeeping, and total reassembly memory is bounded by `maxConcurrentStreams ×` that.

**Which rejections drop the stream.** `MSG_TYPE_SYSTEM_ERROR` on a `STREAM_CHUNK` does not always retire the reassembly context, and the split is normative because a sender treats a chunk rejection as terminal for the whole stream (it never retries an index):

The "Step" column refers to the evaluation order fixed in "Where each guard fires" above; it is normative, so a rejection may only be raised at the step named here.

| Rejection | Step | Stream context |
| :--- | :--- | :--- |
| `reqSize` too small for `ChunkHeader`, or smaller than `ChunkHeader + payloadSize` | 1 | **kept** (framing is rejected before the stream is looked up; the stream may still complete from its other chunks) |
| `chunkIndex >= totalChunks` | 2 | **kept** |
| unknown / already-reclaimed `streamId` | 2 | n/a (nothing to drop) |
| running-length overflow (byte guard) | 4 **only** — never at the drain | **dropped** |
| parked-chunk bound exceeded | 7b | **dropped** |
| `dstOffset + payloadSize > totalSize` (placement bounds, check 1) | 5 | **dropped** |
| `dstOffset` ≠ the in-order cursor when the index becomes in-order (placement self-consistency, check 2) | 7a **and** 8 | **dropped** — on both detection sites: the immediately-in-order arrival *and* the drain of a previously parked index |
| all indices filled but `Σ payloadSize != totalSize` | 9 | **dropped** |
| the reassembler cannot store an accepted chunk (local allocation failure) | 7b | **dropped** |

The two placement rows are **dropped** rather than **kept** for the reason that separates the two halves of this table: the `kept` rejections are framing faults the receiver can attribute to a single message and about which the rest of the stream says nothing, whereas a bad `dstOffset` is the peer contradicting information it already put on the wire. A stream whose sender declares offsets inconsistent with its own `payloadSize`s can no longer be trusted to tile the destination, so retaining the context would at best pin `totalSize` bytes for a stream that can never legitimately complete, and at worst deliver bytes assembled from a framing the sender itself does not agree with. Note that this makes `dstOffset` rejections behave like the byte guard and unlike `chunkIndex >= totalChunks`, even though all three are "one bad field in one header".

The last row is what makes the set complete: a context that could not absorb a chunk it had already accepted can never satisfy the completion contract, and since the sender will not retry, retaining it would only pin `totalSize` bytes until the reclaim mechanism below fires.

**Duplicate chunk index (first arrival wins).** A `chunkIndex` that has already been filled — whether it is already assembled into the destination or still parked — is **ACK-ignored**: the reassembler answers `MSG_TYPE_NORMAL`, keeps the bytes it received first, and discards the duplicate without consulting its `payloadSize`. This holds even when the duplicate declares a *different* `payloadSize` **or a different `dstOffset`** than the first arrival: the duplicate gate runs **before** the running-length guard *and* before both placement checks, so a duplicate declaring a huge length or an out-of-bounds offset is neither counted against `totalSize`, nor written anywhere, nor cause to drop the stream. Consequently `Σ payloadSize` and the placement in the completion contract are determined by *first arrivals only*, and a peer cannot revise — or re-place — a chunk it has already sent.

**Empty stream.** `totalChunks == 0` ⇔ `totalSize == 0`. A `STREAM_START` with `totalChunks == 0` and `totalSize != 0` (or vice versa) MUST be rejected with `MSG_TYPE_SYSTEM_ERROR`. A valid empty stream completes immediately at `STREAM_START` (callback fires once with empty data); no chunks follow.

**Concurrent-stream bound (reclaim mechanism is implementation-defined).** Both peers MUST bound the number of simultaneously in-flight streams to `maxConcurrentStreams`, so a peer that opens streams but never finishes them cannot grow memory without limit. The mechanism used to reclaim capacity when the bound is reached is **implementation-defined and not part of the wire contract**: the C++ Host prunes streams older than a timeout (`streamTimeoutMs`, default 10 s), and the Go Guest applies both an age-based timeout prune (`DefaultStreamTimeout`, default 10 s, matching C++) at `STREAM_START` and a least-recently-active eviction when the count bound is hit. The timeout prune reclaims a lone stalled stream — and the full `totalSize` destination buffer it committed at `STREAM_START` — without waiting for the count bound to fill; the count-LRU eviction bounds the absolute number of in-flight streams. Either reclaim strategy (age-prune, count-LRU, or both) satisfies the contract. A chunk that arrives for a stream whose context was reclaimed is rejected with `MSG_TYPE_SYSTEM_ERROR`.

### 3.4. Host-to-Guest Flow

1.  **Claim:** Host finds a slot in state `SLOT_FREE` and atomically sets it to `SLOT_BUSY`.
2.  **Write:** Host writes data to the Request Buffer and sets `reqSize`, `msgSeq`, and `msgType`.
3.  **Signal:** Host atomically sets state to `SLOT_REQ_READY` and signals the Guest (via Event/Semaphore).
4.  **Process:** Guest wakes up, reads the Request, processes it, and writes to the Response Buffer.
5.  **Reply:** Guest locks the slot at `SLOT_DONE`, sets `respSize`/`msgType`, publishes `SLOT_RESP_READY` and signals the Host (see the responder publish rule below — the two-phase transition is mandatory).
6.  **Complete:** Host wakes up, reads the Response, and sets state back to `SLOT_FREE`.

*Guest responder consume-claim (steps 4–5).* By default the Guest, on observing `SLOT_REQ_READY`, advances `gen` (§3.6.1) and CAS's `SLOT_REQ_READY → SLOT_GUEST_BUSY` and refreshes `lease` before processing, so a Host-side crash-recovery reclaimer cannot free the slot mid-processing. This machinery has exactly one counterparty — the Host reclaimer — and each host slot is serviced by exactly one Guest worker, with the Host requester waiting only on `SLOT_RESP_READY` (it never observes `SLOT_GUEST_BUSY`).

*Host requester consume-claim (step 6, v0.8.13).* The Host's response *consumption* — the `msgSeq`/`msgType` validation, the `respSize` read, and the copy-out (or, for zero-copy holders, the whole interval in which the caller reads the response buffer) — happens between observing `SLOT_RESP_READY` and the terminal `SLOT_FREE` store. During that interval the slot MUST stay out of the intra-process zombie-steal set (§3.6.1 requires `ActiveWait == 0` **and** `state ∈ {REQ_READY, RESP_READY, GUEST_BUSY}`). A multi-threaded Host therefore MUST, on a successful wait, store `SLOT_BUSY` **before** dropping its local active-wait flag, and only then consume and release to `SLOT_FREE`; the release order is mandatory for the same reason as the Guest consume-claim in §3.5 — dropping the flag first lets a sibling Host thread CAS the still-parked `SLOT_RESP_READY` slot to `SLOT_BUSY` and republish it under a *new* transaction, whose claim the consumer's blind `SLOT_FREE` store then clobbers (double-ownership) while its copy-out reads bytes the Guest is concurrently overwriting. On **timeout** the Host MUST NOT park the slot: the transaction is still in flight, so the Host disowns it (no state store) and the zombie/lease reclaim paths recover it. This is wire-neutral — the Guest only ever acts on `SLOT_REQ_READY` in the Host Slot range and never observes or requires `SLOT_FREE` between exchanges — and the resulting `SLOT_BUSY → SLOT_FREE` release MAY stay a plain store, since no other party can own a `SLOT_BUSY` slot. Held-slot sessions (§3.6) are the same rule with the park kept as the resting state. `ActiveWait` is a **process-local** field, so this obligation binds only a Host with concurrent slot-acquiring threads; it is invisible to, and requires nothing of, the Guest.

*No-reclaim fast path (v0.8.8+, narrowed in v0.8.14).* When the Host publishes `ExchangeHeader.fastPathAllowed == 1` (it does so iff auto-reclaim is disabled, §3.6), the reclaimer never runs, so the Guest responder MAY skip the `gen` bump and the `lease` refresh around its consume-claim — two of the three atomic RMWs v0.8.8 removed. The **`SLOT_REQ_READY → SLOT_GUEST_BUSY` consume-claim CAS itself is MANDATORY on both paths** (v0.8.14; v0.8.8–v0.8.13 wrongly skipped it — see the responder-publish rule below). This is wire-identical to the slow path from the Host's perspective (it only ever waits on `SLOT_RESP_READY`). The **data-visibility handshake is unchanged**: the acquire-load that observes `SLOT_REQ_READY` and the `seq_cst` `SLOT_RESP_READY` release still bracket every request read and response write. The flag is safe-by-default: `0` (a pre-v0.8.8 Host, or any Host with auto-reclaim enabled) forces the slow path, so no mismatch can select the fast path while a reclaimer is armed. The reclaim policy is a **startup-time** setting — set `SetAutoReclaimTimeoutNs` before traffic; the Guest reads `fastPathAllowed` once at attach (a runtime flip after attach is not honored until re-attach). This mirrors the held-slot lease contract in §3.6.

*Responder publish rule (step 5, v0.8.14; extended post-v0.8.16) — MANDATORY on both paths.* **The responder MUST NOT write ANY header field — including `respSize` and `msgType` — before it has proven ownership of the slot.** Publication is a **two-phase transition**:

1. Compute the response metadata (`respSize`, response `msgType`) into **local variables**.
2. Lock ownership with `compare_exchange(state, SLOT_GUEST_BUSY, SLOT_DONE)`. **If this fails, write nothing at all** — not one header field, not one response byte more — log the drop and abandon the transaction.
3. Only now store `respSize` and `msgType` into the header.
4. Publish with `compare_exchange(state, SLOT_DONE, SLOT_RESP_READY)`. This MUST be **seq_cst**: the §4.2 doorbell Dekker rests on it (the peer stores its waiting-state seq_cst and re-checks `state` before parking, so a seq_cst publish followed by a seq_cst waiting-state load can never both miss). This CAS **cannot legitimately fail** — only the lock holder can be at `SLOT_DONE` — so a failure is an invariant violation and MUST be logged as an error, not swallowed.
5. The doorbell condition is unchanged: signal only when the peer published its waiting state (`hostState == HOST_STATE_WAITING` on the response side).

*Implementation note (non-normative, v0.8.21).* On the request side the waiting-state publication is performed by one function, `GuestCallWorker::WaitForRequest`, exposed to hosts that run their own processing loop as `DirectHost::WaitForGuestCall`. This matters for interop rather than for the wire: a host that never enters that wait never publishes `HOST_STATE_WAITING`, so a conforming Guest correctly elides no doorbell **and** the host receives no signal it can park on — the gate degrades to polling. Nothing here changes what either side is permitted to put on the wire.

The lock state is `SLOT_DONE` precisely because it is **not** in the §3.6.1 zombie-steal candidate set `{SLOT_REQ_READY, SLOT_RESP_READY, SLOT_GUEST_BUSY}`: a slot parked there cannot be stolen mid-publish, so phases 2–4 are atomic with respect to every stealer.

v0.8.14 sealed only the publishing *transition* and left the field writes preceding it unconditional, which left the defect below only half closed. After a steal those writes landed on the **stealer's new transaction**; the publish CAS then failed and the response bytes were correctly dropped, **but the header was already poisoned**. Because each host slot is serviced by exactly one Guest worker goroutine, that same goroutine then read the poisoned `msgType` on its next iteration — in program order, deterministically, not as a race — and dispatched the new request to the wrong handler branch. `msgSeq` is untouched (the responder never writes it), so the requester's `msgSeq` guard validates and the caller silently receives a wrong value. The mirror hazard exists in the panic/error-recovery path: a recovery that treats `SLOT_REQ_READY` as "still ours" stamps `SYSTEM_ERROR` onto a stolen, republished transaction and completes a request that was **never executed**. Recovery paths MUST therefore also publish only from the owned state (`SLOT_GUEST_BUSY` on a host slot), never from `SLOT_REQ_READY`.

This is the responder-side counterpart of the §3.5 "Disown is terminal" rule: that clause binds a **sender** that forfeits its slot, this one binds a **responder** that has lost its slot. The obligation is the same — a party without proven ownership writes nothing.

**Wire/ABI impact: none.** The header layout is unchanged, no field is added, and `SHM_VERSION` was not bumped (`0x00070000` at the time; the later `0x00080000` bump is confined to the §3.3 stream headers and does not touch this rule). No new state *value* is introduced either — `SLOT_DONE = 3` has been defined on both sides since v0.6.0. Compatibility with an older peer is therefore unaffected: an old requester waits only on `SLOT_RESP_READY`, and `SLOT_DONE` is not in its zombie-steal set, so it neither acts on the transient state nor steals a slot parked in it. The exposure window is exactly two atomic operations wide.

**Residual hazards deliberately left open** (consequences of keeping `SLOT_GUEST_BUSY` in the steal set, which §3.6.1 and this section define as intended behavior):

- **Torn request bytes.** During the theft window the new owner overwrites `reqBuffer` while the old handler is still reading the *same* slice. This rule does not prevent that and is not intended to: the mitigation is the handler's own robustness plus panic recovery, and the fact that the resulting response is dropped at the publish lock. Callers whose handlers parse untrusted framing MUST tolerate a mid-parse mutation (both implementations recover from a handler panic).
- **`SLOT_DONE` is reclaimable.** `TryReclaimAbandonedSlot` treats every non-`SLOT_FREE` state as a reclaim candidate, so a stale `lease` plus an armed reclaimer can free a slot inside the `SLOT_DONE` window; the publish CAS then fails and the response is dropped. The window is two atomic ops wide (no blocking call, no syscall), and the §3.6 threshold contract requires the reclaim threshold to exceed the maximum call duration by orders of magnitude, so this is negligible in practice — but it is real and recorded here rather than papered over.

The original v0.8.14 reasoning for *why the transition itself* must be a CAS follows. Responder ownership of a Host slot is not eternal. On a response timeout the Host disowns the slot *without writing `state`* (see the requester consume-claim above), and its next acquisition **steals** the abandoned slot via `tryClaimSlot`'s zombie branch — which is gated only on the Host's process-local `activeWait`, **not** on `autoReclaimTimeoutNs`, and therefore runs even under `fastPathAllowed == 1`. A late handler's blind `SLOT_RESP_READY` store then lands on the stealer's *new* transaction; since the responder never rewrites `msgSeq`, the requester's `msgSeq` guard validates and silently accepts the stale response bytes. Two weaker guards were evaluated and rejected: (i) an `msgSeq` snapshot re-check before the publish is check-then-act and leaves a race before the stealer republishes `SLOT_REQ_READY`; (ii) a CAS *expecting* `SLOT_REQ_READY` does not help at all, because the stolen slot is republished at exactly `SLOT_REQ_READY` and the CAS succeeds. Only `SLOT_GUEST_BUSY` is discriminating: on a Host slot it is written **solely** by the owning Guest responder (the C++ Host never writes `SLOT_GUEST_BUSY` anywhere), and a steal moves the slot `SLOT_GUEST_BUSY → SLOT_BUSY → SLOT_REQ_READY` and can never put it back. This is what makes the consume-claim CAS mandatory even when reclaim is off: it is not reclaim machinery, it is the responder's ownership token. Wire/ABI-neutral — no new state value, no header change, and the Host's view (`SLOT_RESP_READY` arrives or the slot stays stealable) is unchanged. Cost is one extra locked RMW per round-trip on the Guest side.

### 3.5. Guest-to-Host Flow (Guest Call)

1.  **Claim:** Guest finds a slot in the **Guest Slot** range (offset by `numSlots`) in state `SLOT_FREE`.
2.  **Write:** Guest writes data, sets `msgType` to `MSG_TYPE_GUEST_CALL`.
3.  **Signal:** Guest sets state to `SLOT_REQ_READY` and signals the Host — but MAY elide the doorbell if the Host is spinning (see the doorbell-gate note below).
4.  **Process:** Host detects `SLOT_REQ_READY` (an adaptive spin worker, or polling `ProcessGuestCalls`), CAS-claims it to `SLOT_BUSY`, processes, and writes response.
5.  **Reply:** Host locks the slot at `SLOT_DONE` **via `compare_exchange(state, SLOT_BUSY, SLOT_DONE)`**, writes `respSize`/`msgType`, publishes **via `compare_exchange(state, SLOT_DONE, SLOT_RESP_READY)`** and signals Guest.
6.  **Complete:** Guest reads response and resets state to `SLOT_FREE`.

*Responder publish rule (step 5, v0.8.14; extended post-v0.8.16) — the mirror of the §3.4 rule.* Every clause of the §3.4 responder publish rule binds the Host guest-call responder identically, with `SLOT_BUSY` as the owned state instead of `SLOT_GUEST_BUSY`: the responder MUST NOT write any header field before proving ownership, the transition is two-phase `SLOT_BUSY → SLOT_DONE → SLOT_RESP_READY`, a failed lock CAS means write nothing and drop the response, and the publish CAS is `seq_cst` for the §4.2 doorbell Dekker. A handler slower than the Guest sender's response timeout can outlive its claim: the sender disowns the slot on timeout, the Guest's opt-in lease reclaimer (`TryReclaimAbandonedSlot`) frees it, and a fresh sender claims and republishes it under a new transaction whose `msgSeq` the late blind store would not disturb — the same silent-wrong-value shape as §3.4, and with the same header-poisoning tail (this direction writes `msgType = SYSTEM_ERROR` on a response-size overflow and `respSize` unconditionally, so the next `ProcessGuestCalls` sweep dispatched the new request with the previous transaction's `msgType`). Note this direction's asymmetry: the Host's request-claim CAS was never optional (concurrent `ProcessGuestCalls` callers arbitrate on it), so only the publish changes, and a `seq_cst` CAS replaces a `seq_cst` store at essentially no cost. Regression test: `tests/test_guest_call_publish_lock.cpp` (the C++ mirror of Go's `TestGuestResponderSteal_NoHeaderWriteBeforeOwnership`).

*Doorbell-gate note (steps 3–4, v0.8.6+):* the request doorbell is the exact mirror of the response-side `guestState` gate (§4.2 step 3). The Host guest-call worker MAY run an adaptive spin over the guest-slot `SLOT_REQ_READY` predicate before parking on the shared request event; before it parks it publishes `hostState = HOST_STATE_WAITING` (seq_cst) on **every** guest slot and rechecks the predicate once, restoring `HOST_STATE_ACTIVE` while it spins/processes. The Guest, after its seq_cst `SLOT_REQ_READY` store, loads `hostState` (seq_cst) and signals the request event **only** when it reads `HOST_STATE_WAITING`. This is a two-sided Dekker: the seq_cst total order forbids both the Guest's `hostState` load and the Host's per-slot `state` recheck from missing, so a parked worker is never left asleep on a pending request. A spinning worker costs the Guest no doorbell syscall at all. The worker mode is host-local (`HostConfig::guestWorkerSpin`, default on); the sleep-only fallback maintains the identical `hostState` publish so the gate stays correct either way. Because the Guest gate reads `hostState`, **Host and Guest MUST be from the same shm release ≥ v0.8.6** (guaranteed by same-tag pinning): an older Host that never publishes `hostState` leaves it `HOST_STATE_ACTIVE`, degrading to ≤1s request latency (the worker's park timeout) — a bounded slowdown, never a lost wakeup. `hostState` on guest slots is written only by the Host worker and read only by the Guest sender; it never overlaps the Direct-Exchange use of `hostState` on host slots `[0, numSlots)`.

*Consume-claim note (step 6):* a Guest implementation SHOULD atomically CAS `SLOT_RESP_READY` → `SLOT_GUEST_BUSY` before reading the response, and release via CAS `SLOT_GUEST_BUSY` → `SLOT_FREE` when done. This keeps the slot visibly owned while the response is being consumed, so intra-process zombie-reclaim heuristics cannot steal it mid-read, and a blind `SLOT_FREE` store can never clobber a concurrent reclaimer's transaction. The Host is unaffected: it only acts on `SLOT_REQ_READY` in the Guest Slot range, and the claiming CAS refreshes `lease` per §3.6. If the wait for `SLOT_RESP_READY` times out, the Guest MUST NOT store `SLOT_FREE` (the Host may still own the slot in `SLOT_REQ_READY`/`SLOT_BUSY`); the slot is recovered later by a reclaim path once the late response arrives or the lease goes stale.

*Disown is terminal (step 6) — MANDATORY (v0.8.16+).* **Scope: this clause binds a SENDER.** Its responder-side counterpart is the "Responder publish rule" in §3.4/§3.5 — a responder that has lost its slot is under the identical obligation (write no header field without proven ownership), enforced there by the two-phase publish lock rather than by handle invalidation. A sender that forfeits a slot — response timeout, or a consume-claim CAS lost to a reclaimer — MUST treat its slot handle as **dead for every purpose**, not merely as "do not store `SLOT_FREE` on release". Once the slot is forfeited it is eligible for the §3.6.1 zombie steal and for lease reclamation, so a *different* owner may already hold a live transaction on it. The forfeiting sender therefore MUST NOT, until it re-acquires: write `reqSize`, `msgType` or `msgSeq`; set its process-local active-wait flag; publish `SLOT_REQ_READY` (or any other `state`); or hand out the request/response buffers. Any one of those silently overwrites the new owner's in-flight transaction — a corruption the `msgSeq` guard cannot catch, because the intruder rewrites `msgSeq` too. Both implementations enforce this by invalidating the handle at the forfeit point: C++ `DirectHost::ZeroCopySlot`/`HeldSlot` set `slotIdx = -1` so `IsValid()` gates every accessor, and Go `GuestSlot` sets `lost` so `Send`/`SendWithTimeout` return an error and `RequestBuffer`/`ResponseBuffer` return `nil`. This is wire-neutral: it removes writes the protocol never sanctioned and adds none. Both sides now carry a per-forbidden-act regression test: Go `go/guest_slot_forfeit_test.go` and C++ `tests/test_disown_terminal.cpp`. Assert the individual acts, not just the outcome — an outcome-only test (the slot ends at `SLOT_FREE`, the wrapper reports disowned) still passes while a forfeited handle hands out buffers or republishes `SLOT_REQ_READY`, because those land on a slot the test has stopped inspecting.

*No-reclaim fast path (v0.8.9+):* the mirror of the §3.4 fast path, applied to the guest-call direction, active only while the Host publishes `fastPathAllowed == 1` (§2.1 — host auto-reclaim off) and, for the Guest-side variants, the Guest's own reclaim is also off:

- **Worker request-claim (step 4):** the Host worker MAY skip the `gen` bump and the `lease` refresh around its `SLOT_REQ_READY → SLOT_BUSY` claim. The claim **CAS itself is kept** — `ProcessGuestCalls` is a public API that permits concurrent callers, and the CAS is what arbitrates two processors racing for one request. Reclaim safety with a Go-side reclaimer still enabled rests on the sender's claim-time lease stamp plus the standing §3.6 threshold contract (threshold ≫ max call duration).
- **Sender consume (step 6, copy-out senders such as `SendGuestCall`):** the Guest MAY skip the consume-claim entirely (no `gen` bump, no `SLOT_RESP_READY → SLOT_GUEST_BUSY` CAS, no `lease` refresh), retaining ownership through its local active-wait flag alone — the zombie-steal heuristic (§3.6.1) requires `ActiveWait == 0`, which the consuming sender keeps at 1 until release. Release order is **mandatory**: drop the active-wait flag FIRST, then CAS `SLOT_RESP_READY → SLOT_FREE`. The reverse order could clobber a new owner's active-wait flag after it claims the freed slot; the brief `SLOT_RESP_READY`+`ActiveWait==0` window before the CAS is benign (the response was already copied out — a stealer recycling the slot there merely fails our CAS).
- **Zero-copy senders (`GuestSlot.Send`):** the consume **CAS is kept** (the caller holds the slot and reads `ResponseBuffer()` until an explicit `Release`, so the slot must park in the non-stealable `SLOT_GUEST_BUSY`); only the `gen` bump and `lease` refresh are skipped.

**Manual reclaim caveat:** calling `TryReclaimAbandonedSlot` on a **guest** slot while `fastPathAllowed == 1` is published is **undefined behavior by contract** — publishing the flag asserts no reclaimer runs, and the fast paths above stop maintaining the gen/lease machinery a reclaimer would need. (Configuring reclaim off and then reclaiming by hand is self-contradictory; a threshold honoring the §3.6 "≫ max call duration" rule remains safe in practice, but do not rely on it.)

## 4. Synchronization & Signaling

Platform-specific primitives are used to wake up sleeping threads.

### 4.1. Naming Convention

All synchronization primitives are named based on the Shared Memory name (`SHM_NAME`) and the slot index.

- **Request Event (Host -> Guest):** `{SHM_NAME}_slot_{INDEX}`
- **Response Event (Guest -> Host):** `{SHM_NAME}_slot_{INDEX}_resp`
- **Guest Call Event (Guest -> Host):** `{SHM_NAME}_guest_call` (Global event for all guest slots)

*Note: On Windows these names are created under the `Local\` namespace, so the actual object name is `Local\SimpleIPC_slot_0`.*

### 4.2. Hybrid Wait Strategy

Implementations should use a hybrid wait strategy for optimal latency:
1.  **Spin:** Busy-wait for a short period (checking atomic `state`).
2.  **Yield:** Call `std::this_thread::yield()` or `runtime.Gosched()`.
3.  **Sleep:** Wait on the OS primitive (named Event) if the peer signals it is sleeping (`hostState`/`guestState` flags).

### 4.3. Platform Implementation

This library is **Windows-only** (MSVC 2019+ / MinGW). All primitives map onto the Win32 API:

| Feature | Windows |
| :--- | :--- |
| **Shared Memory** | `CreateFileMapping` / `MapViewOfFile` |
| **Signaling** | Named Events (`CreateEvent`) |
| **Atomic Wait** | `WaitForSingleObject` / `SetEvent` |

### 4.4. Memory Ordering Contract

The `state` field of `SlotHeader` is the synchronizing variable that publishes ownership transfers between Host and Guest. All implementations must respect the following ordering rules:

- **Publishing transitions** (state -> `REQ_READY`, state -> `RESP_READY`, state -> `FREE`-after-completion) **MUST use at least release semantics** (C++: `memory_order_release` or stronger; Go: `sync/atomic.Store*` which is sequentially consistent).

- **Consuming transitions** (acquire-load of state expecting `REQ_READY`, `RESP_READY`, or `FREE`; CAS attempts to claim a free slot) **MUST use at least acquire semantics** (C++: `memory_order_acquire` or stronger; Go: `sync/atomic.Load*` / `sync/atomic.CompareAndSwap*`).

- **Data-region fields** (`msgSeq`, `msgType`, `reqSize`, `respSize`, slot payload bytes) are NOT individually atomic. They MUST be written by the producer BEFORE the publishing release-store of `state`, and read by the consumer AFTER the consuming acquire-load of `state`. Re-ordering of any data-region access across the synchronizing atomic is a protocol violation.

- **No plain (non-atomic) access** to `state` is permitted in any implementation. `memory_order_relaxed` is NOT acceptable on synchronizing operations.

- **Defensive re-checks** (e.g., re-reading `msgSeq` after acquire-load to detect zombie slots, range-checking `reqSize` / `respSize` against `maxReqSize`) are part of the contract; they protect against ABA hazards and msgSeq epoch confusion. Implementations MUST preserve them.

**Rationale**: The sole deployment target is Windows x64 (amd64); 32-bit x86 is not supported (see `AGENTS.md` §"Platform Targets"). On x64, TSO hardware ordering covers most reorderings the C++/Go memory models permit. Even on this strongly-ordered ISA, however, compilers may reorder data-region accesses across non-atomic stores; only the atomic operations on `state` act as compiler barriers. Implementations must therefore use the rules above as a *language-level* contract, not a hardware-level one — `relaxed` is unsafe even on x86 because of compiler-side reordering of the surrounding data writes/reads. (Per `AGENTS.md` §"Platform Targets", ARM is explicitly not supported, but the same rules would also keep the protocol correct on weakly-ordered ISAs should that ever change.)

## 5. Implementation Guidelines

- **Header Only (C++):** The C++ implementation must remain header-only in `include/shm/`.
- **Zero Dependency (Go):** The Go implementation should avoid external dependencies (cgo is permitted for system calls).
- **Endianness:** The protocol assumes all peers are **Little Endian**.
- **Padding:** Strict adherence to the padding bytes in `SlotHeader` and `ExchangeHeader` is required for binary compatibility.

### 3.6. Lease Heartbeat (v0.7.0)

**Status**: Shipped. Layout + heartbeat-on-CAS landed in v0.7.0; the reclamation policy (`TryReclaimAbandonedSlot`) shipped in v0.7.2.

**Problem**: A host or guest crashing mid-exchange leaves the slot in `SLOT_BUSY` / `SLOT_REQ_READY` / `SLOT_GUEST_BUSY` forever. The surviving side has no mechanism to distinguish a slow peer from a dead one.

**v0.7.0 mechanism — write-only heartbeat**:

Every site that CAS's `state` from `SLOT_FREE` (or `SLOT_RESP_READY`/`SLOT_REQ_READY` reclaim paths) to a non-FREE value MUST, immediately after the successful CAS, store `Platform::MonotonicNanos()` into `lease` with `memory_order_release` (or Go's `atomic.StoreUint64`). This marks the slot as "actively owned at time T".

In v0.7.0 nothing yet *read* `lease` to make decisions — the field was observable through `SlotHeader::lease` for diagnostics, but no code reclaimed based on it. v0.7.2 introduced the reclamation policy described below.

**Clock contract**: `lease` is **wall-clock nanoseconds since the Unix epoch**, not a monotonic counter. Both sides use it:

| Side | Source | Resolution |
| :--- | :--- | :--- |
| C++ Host | `Platform::MonotonicNanos()` — `GetSystemTimeAsFileTime`, normalised to ns-since-Unix-epoch. | System timer tick (0.5–15.6 ms). |
| Go Guest | `shm.MonotonicNanos()` — `time.Now().UnixNano()`. | ~µs or better. |

The name "MonotonicNanos" predates the wall-clock choice; it stays for API stability. Wall-clock was selected so the values are comparable across processes AND across languages without coordinating clock epochs. NTP steps can move the clock backward; a backward step causes at most a spurious reclamation candidate (guarded by the CAS check against `state`), never data corruption.

**Resolution note (2026-07-03)**: the C++ side deliberately uses the *coarse* system time (KUSER_SHARED_DATA tick, ~5 ns/read) rather than `GetSystemTimePreciseAsFileTime` (~26 ns, QPC-backed) because the lease stamp sits on every slot claim. Millisecond granularity is sufficient by construction: reclaim thresholds are seconds-scale, and the §3.6.1 claim-generation handshake — not lease value comparison — is the airtight ABA guard. The lease equality re-check in the reclaim paths only ever compares a fresh stamp against one already stale by the multi-second threshold, so tick granularity can never make those values collide. Two invariants make the asymmetric resolution safe: (a) a coarse `now` on the reclaiming side understates `now - lease`, so C++-side reclamation only becomes *more* conservative, never spurious; (b) cross-language skew (precise Go `now` vs coarse C++-written lease) overstates age by at most one tick (~15.6 ms), so reclaim thresholds MUST stay well above ~16 ms — the standing "5 × `responseTimeoutMs`" guidance (≥ tens of seconds) already guarantees this. Implementations MAY use any wall-clock source in the Unix-epoch timeline with resolution no coarser than ~1/10 of the smallest supported reclaim threshold.

**v0.7.2 — reclamation**:

1. Waiter side reads `lease`. If `now - lease > kLeaseTimeoutNs` (suggested: 5 × `responseTimeoutMs` converted to ns), the slot is *presumed abandoned*.
2. Waiter attempts `state.compare_exchange_strong(observed_state, SLOT_FREE)`. If the CAS succeeds the slot is reclaimed; if it fails, the live owner made progress in the meantime — abort the reclamation, retry the normal flow.
3. Reclamation never fires on non-crash code paths because the heartbeat is refreshed inside the timeout window.

Note that the reclaimer's candidate set is "**any** state other than `SLOT_FREE`", which is wider than the zombie-steal set of §3.6.1 and therefore includes the transient `SLOT_DONE` publish lock (§3.4/§3.5). A reclaimer armed with a stale-enough `lease` can free a slot inside that two-atomic-op window; the responder's publish CAS then fails and the response is dropped rather than mis-delivered. This is recorded as a known residual, not a defect: the §3.6 threshold contract ("≫ max call duration", tens of seconds) puts it many orders of magnitude outside the window.

Picking the wrong heartbeat cadence or timeout could reclaim a slot a slow-but-live peer is still using → double-use → data corruption. v0.7.2 shipped the reclamation API together with a property-based crash-injection test that asserts no double-claim across N random crash points.

**Why this was patch-layout-compatible**: `reserved[36]` was never written by v0.6.x code, so adding `atomic<uint64> lease` at offset 96 (with 4 bytes of natural alignment padding before it) is invisible to old readers. Old writers don't touch the slice. Mixed-version deployments degrade gracefully — v0.6.x peers participating in the protocol simply never publish a lease; v0.7.x reclaimers see `lease == 0` for those slots and skip reclamation for them, falling back to the pre-v0.7 forever-busy behavior.

**Held-slot sessions (v0.8.5)**: a Host that statically owns a slot MAY skip the per-send claim cycle: claim once (ordinary gen-bump + CAS per §3.6.1), then for each exchange publish `SLOT_REQ_READY` directly from the held state and, after consuming the response, park the slot at `SLOT_BUSY` instead of `SLOT_FREE` (host-side consume-claim: the `SLOT_BUSY` store MUST precede dropping the local active-wait flag, mirroring the Guest consume-claim note in §3.5). This is wire-compatible because the Guest only ever acts on `SLOT_REQ_READY` in the Host Slot range — it never observes or requires `SLOT_FREE` between exchanges (the Guest→Host direction has re-armed this way since v0.6). Obligations: (a) `gen` is bumped once at the initial claim only — parking at `SLOT_BUSY` is not a new claim; (b) `lease` MUST be refreshed on every held send, and a held slot idle between sends becomes reclaimable once its lease outages the opt-in reclaim threshold, so callers MUST keep `autoReclaimTimeoutNs` above their maximum inter-send gap (the standing long-transaction contract); (c) on send timeout the transaction is still in flight — the holder MUST disown the slot (no state store) and let the zombie/lease reclaim paths recover it.

#### 3.6.1. Claim Generation & the Reclamation ABA Guard (v0.7.5)

**Status**: Shipped in v0.7.5. ABI-compatible (`gen` carved from `reserved`, total size unchanged at 128 bytes; no protocol-version bump — it was `0x00070000` at the time).

**Problem (ABA)**: `TryReclaimAbandonedSlot` (§3.6) decides a slot is abandoned from an observed `state` and a stale `lease`, then CAS's `state` from the observed value back to `SLOT_FREE`. Between the observation and the CAS a peer can legitimately:

1. finish the slot (`state` → `SLOT_FREE`),
2. have it reused, and
3. re-claim it so `state` lands back on the **same** observed value, now backed by a **fresh** lease.

A bare `state` CAS then succeeds and wrongly reclaims a live slot. Re-reading `lease` immediately before the CAS narrows the window but is **provably insufficient**: per §3.6 the fresh lease is published *after* the claiming CAS, so there is a window in which `state` is already re-claimed but `lease` still reads the stale value (the *lease-publication lag*).

**Mechanism — claim generation**:

`gen` is a monotonic `atomic<uint64>` at offset 104. The synchronization contract is:

- **Every slot-claiming path** (Host `AcquireSlot` / `AcquireSpecificSlot` / `ProcessGuestCalls`; Guest `SendGuestCall` acquire, `AcquireGuestSlot`, response consume-claim, worker request-claim) **MUST advance `gen` (`fetch_add(1)` / `atomic.AddUint64`) immediately BEFORE its state-claiming CAS**, with at least acq_rel/release semantics. The bump *precedes* the `state` transition, so any in-flight claim — even one whose lease store has not yet landed — is observable in `gen`.
  - **Reclaim-off carve-out (v0.8.8/v0.8.9 fast paths, §3.4/§3.5):** when the relevant reclaimers are provably disabled (`fastPathAllowed == 1` for host-side, plus the Guest's own reclaim setting for guest-side variants), the *re-claim* paths — guest responder consume (§3.4), worker request-claim, and sender response consume (§3.5) — MAY skip their **`gen` bump and `lease` refresh**. This does not weaken the invariant for its consumers because (a) the reclaimer, the only party whose `gen` handshake depends on those bumps, does not run, and (b) cross-transaction ABA remains fenced by the *acquisition* paths (`AcquireSlot`/`tryClaimGuestSlot`/`AcquireGuestSlot`), which bump unconditionally on every new transaction. Any new claiming path outside this enumerated set MUST still bump. **The carve-out never licenses dropping a claiming CAS** — a claim CAS is an ownership token whose counterparty is the *stealer*, not the reclaimer, and stealing is not gated on the reclaim setting. v0.8.8 read the carve-out too broadly and dropped the guest responder's `SLOT_REQ_READY → SLOT_GUEST_BUSY` CAS outright; v0.8.14 restored it (§3.4 responder publish rule). Only the sender response consume in §3.5 may skip its whole claim, and only because it substitutes the process-local `ActiveWait` flag — which the *same* stealer heuristic honors — for the duration of the consume.
- **The reclaimer** snapshots `gen` first, validates staleness, then wins the exclusive right to reclaim via `compare_exchange(gen, gen+1)`. This single CAS linearizes reclaim against claim:
  - If a claim's `gen` bump landed first, the reclaimer's `gen` CAS fails → refuse (covers the lease-lag window).
  - If the reclaimer's `gen` CAS lands first, it then publishes `SLOT_FREE` via `compare_exchange(state, observed, SLOT_FREE)` — *not* a blind store — so a concurrent `SLOT_RESP_READY` zombie re-claim that bumped `gen` afterwards is arbitrated by this final `state` CAS. Exactly one of {reclaimer frees, claimant claims} wins.

A burned `gen` tick (reclaimer won `gen` but lost the final `state` CAS) is harmless: `gen` only ever advances.

**Claim paths that *steal* a non-`SLOT_FREE` slot use the reclaimer's handshake, not a bare bump (2026-07-02).** A claimant that recycles a slot which is *not* `SLOT_FREE` — the Guest zombie steal (`SLOT_RESP_READY` with no local active waiter, in `AcquireGuestSlot` / `SendGuestCall`'s Case-2 scan) and the Host steal of an abandoned `SLOT_REQ_READY`/`SLOT_RESP_READY`/`SLOT_GUEST_BUSY` slot (`AcquireSlot` / `AcquireSpecificSlot`) — faces the **identical ABA hazard** as the reclaimer. Between observing the zombie's `state` and CAS'ing it to a busy value, a *different* claimant can legitimately recycle the same slot and cycle `state` back to the observed value under a **different, live** transaction, whose response the stale `state` CAS would then hijack (the rightful owner's consume-claim CAS then fails spuriously, destroying the response). These steal paths therefore snapshot `gen` **before** the `state` load and condition the claim on `compare_exchange(gen, gen+1)` — exactly the reclaimer's linearizer — instead of a bare `fetch_add`; a losing `gen` CAS makes the stealer yield. A `SLOT_FREE` claim carries no such transaction-identity ambiguity and keeps the plain `fetch_add` bump (racers arbitrate on the `state` CAS). The shared helpers are `tryClaimGuestSlot` (Go, `direct.go`) and `SlotAllocator::tryClaimSlot` (C++). This still satisfies the invariant above — every *successful* claim advances `gen` before its `state` CAS — and is verified by `go/zombie_steal_test.go::TestGuestSlot_ZombieSteal_YieldsToConcurrentReclaim`.

**Limitation (documented, not a defect)**: a *fully* airtight guard against an adversarial peer that re-claims and immediately republishes within the reclaimer's window would require folding the generation into the `state` synchronizing word itself (a `state`-field ABI/semantics change). The `gen` handshake closes the realistic crash-recovery race — where the abandoned owner does not relinquish and a distinct new owner can only claim a `SLOT_FREE` slot — and is verified by the property/stress tests in `go/reclaim_aba_test.go`. Tightening beyond this is a protocol-redesign item, not a v0.7.x patch.

**Mixed-version compatibility**: a peer that does not advance `gen` (pre-v0.7.5) leaves it at a fixed value; the reclaimer's `gen` CAS then behaves like the pre-v0.7.5 bare reclaim for that slot — correct degradation, no corruption.
