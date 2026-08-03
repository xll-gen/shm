# Shared Memory IPC Specification (wire protocol `SHM_VERSION 0x00070000`)

> Document tracks the `shm` release line (see `VERSION`). The **wire** protocol version
> (`SHM_VERSION 0x00070000`) is intentionally pinned across the ABI-compatible v0.7.x–v0.8.x line;
> minor and patch releases add fields carved from reserved space without bumping it
> (e.g. `fastPathAllowed` in v0.8.8 — see §2.1).

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
| `version` | `uint32` | 4 | Protocol Version (0x00070000). |
| `numSlots` | `uint32` | 8 | Number of Host-to-Guest slots. |
| `numGuestSlots` | `uint32` | 12 | Number of Guest-to-Host (Guest Call) slots. |
| `slotSize` | `uint32` | 16 | Total size of a single slot in bytes. |
| `reqOffset` | `uint32` | 20 | Offset of the Request Buffer within a slot (relative to Slot start). |
| `respOffset` | `uint32` | 24 | Offset of the Response Buffer within a slot (relative to Slot start). |
| `fastPathAllowed` | `uint32` | 28 | **v0.8.8+**: `1` = Host guarantees auto-reclaim is off, so the Guest responder may take the no-claim fast path (§3.4); `0` (default / reclaim-on Host / pre-v0.8.8 Host) = Guest must use the full-claim path. Safe-by-default polarity — a version mismatch degrades to the slow path, never to an unsafe fast path. Carved from `reserved`; version stays `0x00070000`. |
| `reserved` | `uint8[32]`| 32 | Reserved to ensure 64-byte alignment. |

**Total Size:** 64 Bytes.

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

**Compatibility note**: the `lease` slot used to be inside `reserved[36]` (v0.6.x and earlier), and `gen` was carved from the remaining `reserved[24]` in v0.7.5. Older readers see those bytes as opaque padding and behave exactly as before — they never wrote into `reserved`, so introducing typed fields there is forward-compatible and the layout stays 128 bytes. The protocol version remains `0x00070000`: the `gen` addition is ABI-compatible and degrades gracefully (a peer that does not advance `gen` falls back to the pre-v0.7.5 reclaim behavior for its slots).

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
| `reserved` | `uint32` | 20 | Reserved (Padding to 24 bytes). |

**Total Size:** 24 Bytes.

#### 3.3.2. ChunkHeader (MSG_TYPE_STREAM_CHUNK)

| Field | Type | Offset | Description |
| :--- | :--- | :--- | :--- |
| `streamId` | `uint64` | 0 | Unique identifier for the stream. |
| `chunkIndex` | `uint32` | 8 | Index of this chunk (0-based). |
| `payloadSize` | `uint32` | 12 | Size of the payload following this header. |
| `reserved` | `uint32` | 16 | Reserved. |
| `padding` | `uint32` | 20 | Padding to ensure 8-byte alignment (Total 24 bytes). |

**Total Size:** 24 Bytes.
The payload follows immediately after the `ChunkHeader`.

#### 3.3.3. Data Alignment (Negative Size)

The `reqSize` and `respSize` fields indicate the location of the data within the buffer:
- **Positive (> 0):** Data starts at the beginning of the buffer (Offset 0).
- **Negative (< 0):** Data is aligned to the **end** of the buffer. The actual size is `abs(size)`. The start offset is `BufferSize - abs(size)`. This is typically used for FlatBuffers construction.

#### 3.3.4. Reassembly Limits & Completion Contract

A reassembler accepts `StreamHeader`/`ChunkHeader` messages from an untrusted peer. The wire layout (§3.3.1/§3.3.2) is the only thing the peer controls, so corrupt or malicious values MUST NOT drive unbounded allocation, silent truncation, or mis-delivery. Both the C++ Host (`StreamReassembler`) and the Go Guest (`NewStreamReassembler`) implement the **identical** contract below; a stream accepted by one peer is accepted by the other, and a stream rejected by one is rejected by the other.

**Bounds.** The first two are header bounds, checked at `STREAM_START` before any allocation: a `StreamHeader` violating either MUST be rejected with `MSG_TYPE_SYSTEM_ERROR`. The last two bound in-flight reassembly state and are enforced where their own clauses below say:

| Bound | Value | Field guarded |
| :--- | :--- | :--- |
| `maxStreamSize` | `1 << 30` (1 GiB) | `totalSize` |
| `maxStreamChunks` | `1 << 20` | `totalChunks` |
| `maxConcurrentStreams` | `1024` | number of in-flight (partially reassembled) streams |
| `maxParkedChunks` | `1024` (= `maxStreamChunks / maxConcurrentStreams`) | number of chunks one stream may hold buffered ahead of its in-order cursor |

`totalChunks` MUST be validated **before** any allocation is sized, and it MUST NOT itself size an allocation: see "Residency bound" below.

**Residency bound (MUST).** Per-stream reassembly state MUST be `O(totalSize)` — the advertised size — plus the bounded parked-chunk bookkeeping below. It MUST NOT be `O(totalChunks)`. An implementation that indexes a `totalChunks`-sized array (one element per chunk, a presence bitmap, etc.) violates this clause: `totalChunks` and `totalSize` are independent header fields, so a peer advertising `totalSize = 1` with `totalChunks = maxStreamChunks` would commit per-chunk overhead × 2^20 for a one-byte stream (measured ~24 MiB per stream, ~25 GiB at `maxConcurrentStreams`), which no payload-byte guard can see. Both peers therefore preallocate a destination buffer of `totalSize` at `STREAM_START`, copy in-order chunks straight into it at a running cursor, and buffer **only** ahead-of-cursor chunks, sparsely (`go/stream.go`: `streamContext.buf` / `ooo`; `include/shm/StreamReassembler.h`: `StreamContext::buf` / `ooo`). Peak residency for a completing stream is likewise bounded by `totalSize`: assembling into a second full-size buffer at completion time (`2 × totalSize`, i.e. 2 GiB for a 1 GiB stream) is not permitted.

**Completion contract.** A stream completes — and the `onStream` callback fires exactly once — only when **both**:
1. `received == totalChunks` (every chunk index filled exactly once), and
2. `Σ payloadSize == totalSize` (the assembled byte count equals the advertised total).

If all chunks have arrived but `Σ payloadSize != totalSize`, the stream is **dropped** with `MSG_TYPE_SYSTEM_ERROR` rather than delivered truncated or garbled. The completion check is the only thing that can catch an **under**-sized stream.

**Per-chunk overflow guard (order-independent, MUST).** On every `STREAM_CHUNK`, and **before** the payload is copied or buffered anywhere, the reassembler MUST compute the running *received* length — `Σ payloadSize` over every chunk index accepted so far — and reject the message with `MSG_TYPE_SYSTEM_ERROR`, dropping the stream, if accepting this chunk would make that sum exceed `totalSize`. The running length MUST include chunks that arrived **ahead of the in-order cursor** and are still buffered while an earlier index is missing; an implementation that applies the guard only to the in-order path does not satisfy this clause. Such an implementation lets a peer advertise a tiny `totalSize`, then park unbounded bytes under out-of-order indices — the completion check above would eventually reject the stream, but only once every byte was already resident, which is precisely the unbounded allocation this section forbids. Rejection MUST therefore happen at arrival time, not at completion time. This bounds reassembly memory for any single stream to `totalSize`, and — combined with `maxConcurrentStreams` below — total reassembly memory to `maxConcurrentStreams × maxStreamSize`.

The guard is a strict `>` comparison: a chunk that exactly fills the remaining advertised space is valid and MUST be accepted. Implementations that assemble into a preallocated destination at an in-order cursor MUST account for buffered out-of-order bytes separately and release that accounting exactly when those bytes are moved into the destination, or a well-formed out-of-order stream will be rejected as over-sized (`go/stream.go`: `streamContext.oooBytes` / `fits`; `include/shm/StreamReassembler.h`: `StreamContext::oooBytes` / `Fits`).

**Parked-chunk bound (count dimension, MUST).** The byte guard above bounds *payload*; it cannot bound per-chunk bookkeeping, for two independent reasons: a buffered chunk costs memory even at zero payload, and a **zero-length chunk can never trip the byte guard** (`Σ payloadSize + 0 > totalSize` is false for any stream that is still alive). A reassembler MUST therefore also refuse, with `MSG_TYPE_SYSTEM_ERROR` and dropping the stream, an ahead-of-cursor chunk that would make the number of simultaneously buffered (parked) chunks for that stream exceed `maxParkedChunks`. The check MUST run **before** the parked copy is allocated. Without it a peer advertises `totalSize = 1` with `totalChunks = maxStreamChunks` and parks 2^20−1 zero-length chunks — measured at ~80 B per entry on the Go side, i.e. ~80 MiB resident for a stream that advertised one byte, times `maxConcurrentStreams`.

`maxParkedChunks` is `maxStreamChunks / maxConcurrentStreams` (1024 with the values above): the bound that keeps the *aggregate* parked-entry count across all in-flight streams no larger than the chunk count of one maximal stream, i.e. ~64–80 MiB of bookkeeping worst case. It is far above any legitimate sender — `StreamSender` parks at most `maxInFlight − 1` chunks and the library default in-flight depth is 1 — but it does mean a stream delivered in strictly descending index order is only accepted while its chunk count stays within the bound. The value is part of this accept/reject contract, not a local tuning knob: changing it on one peer alone makes a stream that peer accepts get rejected by the other. Combined with the byte guard, per-stream residency is `totalSize` plus at most `maxParkedChunks` entries of bookkeeping, and total reassembly memory is bounded by `maxConcurrentStreams ×` that.

**Which rejections drop the stream.** `MSG_TYPE_SYSTEM_ERROR` on a `STREAM_CHUNK` does not always retire the reassembly context, and the split is normative because a sender treats a chunk rejection as terminal for the whole stream (it never retries an index):

| Rejection | Stream context |
| :--- | :--- |
| `reqSize` too small for `ChunkHeader`, or smaller than `ChunkHeader + payloadSize` | **kept** (framing is rejected before the stream is looked up; the stream may still complete from its other chunks) |
| `chunkIndex >= totalChunks` | **kept** |
| unknown / already-reclaimed `streamId` | n/a (nothing to drop) |
| running-length overflow (byte guard) | **dropped** |
| parked-chunk bound exceeded | **dropped** |
| all indices filled but `Σ payloadSize != totalSize` | **dropped** |
| the reassembler cannot store an accepted chunk (local allocation failure) | **dropped** |

The last row is what makes the set complete: a context that could not absorb a chunk it had already accepted can never satisfy the completion contract, and since the sender will not retry, retaining it would only pin `totalSize` bytes until the reclaim mechanism below fires.

**Duplicate chunk index (first arrival wins).** A `chunkIndex` that has already been filled — whether it is already assembled into the destination or still parked — is **ACK-ignored**: the reassembler answers `MSG_TYPE_NORMAL`, keeps the bytes it received first, and discards the duplicate without consulting its `payloadSize`. This holds even when the duplicate declares a *different* `payloadSize` than the first arrival: the duplicate gate runs **before** the running-length guard, so a duplicate declaring a huge length is neither counted against `totalSize` nor cause to drop the stream. Consequently `Σ payloadSize` in the completion contract is the sum over *first arrivals only*, and a peer cannot revise a chunk it has already sent.

**Empty stream.** `totalChunks == 0` ⇔ `totalSize == 0`. A `STREAM_START` with `totalChunks == 0` and `totalSize != 0` (or vice versa) MUST be rejected with `MSG_TYPE_SYSTEM_ERROR`. A valid empty stream completes immediately at `STREAM_START` (callback fires once with empty data); no chunks follow.

**Concurrent-stream bound (reclaim mechanism is implementation-defined).** Both peers MUST bound the number of simultaneously in-flight streams to `maxConcurrentStreams`, so a peer that opens streams but never finishes them cannot grow memory without limit. The mechanism used to reclaim capacity when the bound is reached is **implementation-defined and not part of the wire contract**: the C++ Host prunes streams older than a timeout (`streamTimeoutMs`, default 10 s), and the Go Guest applies both an age-based timeout prune (`DefaultStreamTimeout`, default 10 s, matching C++) at `STREAM_START` and a least-recently-active eviction when the count bound is hit. The timeout prune reclaims a lone stalled stream — and the out-of-order bytes it has parked — without waiting for the count bound to fill; the count-LRU eviction bounds the absolute number of in-flight streams. Either reclaim strategy (age-prune, count-LRU, or both) satisfies the contract. A chunk that arrives for a stream whose context was reclaimed is rejected with `MSG_TYPE_SYSTEM_ERROR`.

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

**Wire/ABI impact: none.** The header layout is unchanged, no field is added, and `SHM_VERSION` stays `0x00070000`. No new state *value* is introduced either — `SLOT_DONE = 3` has been defined on both sides since v0.6.0. Compatibility with an older peer is therefore unaffected: an old requester waits only on `SLOT_RESP_READY`, and `SLOT_DONE` is not in its zombie-steal set, so it neither acts on the transient state nor steals a slot parked in it. The exposure window is exactly two atomic operations wide.

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

**Status**: Shipped in v0.7.5. ABI-compatible (`gen` carved from `reserved`, total size unchanged at 128 bytes; protocol version stays `0x00070000`).

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
