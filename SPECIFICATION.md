# Shared Memory IPC Specification (wire protocol `SHM_VERSION 0x00070000`)

> Document tracks the `shm` release line (see `VERSION`). The **wire** protocol version
> (`SHM_VERSION 0x00070000`) is intentionally pinned across the ABI-compatible v0.7.x series;
> patch releases add fields carved from reserved space without bumping it.

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
| `reserved` | `uint8[36]`| 28 | Reserved to ensure 64-byte alignment. |

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
| `SLOT_DONE` | 3 | Transaction complete (Transient state). |
| `SLOT_BUSY` | 4 | Slot is claimed by Host (for Host Slots), data is being written. |
| `SLOT_GUEST_BUSY` | 5 | Slot is claimed by Guest (for Guest Slots), data is being written. |

*Note: `SLOT_DONE` (3) is reserved for future flow extensions and is not currently used by any transition in the protocol. Implementations must define the constant for parity, but should not produce or consume it.*

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

### 3.4. Host-to-Guest Flow

1.  **Claim:** Host finds a slot in state `SLOT_FREE` and atomically sets it to `SLOT_BUSY`.
2.  **Write:** Host writes data to the Request Buffer and sets `reqSize`, `msgSeq`, and `msgType`.
3.  **Signal:** Host atomically sets state to `SLOT_REQ_READY` and signals the Guest (via Event/Semaphore).
4.  **Process:** Guest wakes up, reads the Request, processes it, and writes to the Response Buffer.
5.  **Reply:** Guest sets `respSize`, updates state to `SLOT_RESP_READY` and signals the Host.
6.  **Complete:** Host wakes up, reads the Response, and sets state back to `SLOT_FREE`.

### 3.5. Guest-to-Host Flow (Guest Call)

1.  **Claim:** Guest finds a slot in the **Guest Slot** range (offset by `numSlots`) in state `SLOT_FREE`.
2.  **Write:** Guest writes data, sets `msgType` to `MSG_TYPE_GUEST_CALL`.
3.  **Signal:** Guest sets state to `SLOT_REQ_READY` and signals the Host.
4.  **Process:** Host (polling `ProcessGuestCalls`) detects `SLOT_REQ_READY`, processes, and writes response.
5.  **Reply:** Host sets `SLOT_RESP_READY` and signals Guest.
6.  **Complete:** Guest reads response and resets state to `SLOT_FREE`.

*Consume-claim note (step 6):* a Guest implementation SHOULD atomically CAS `SLOT_RESP_READY` → `SLOT_GUEST_BUSY` before reading the response, and release via CAS `SLOT_GUEST_BUSY` → `SLOT_FREE` when done. This keeps the slot visibly owned while the response is being consumed, so intra-process zombie-reclaim heuristics cannot steal it mid-read, and a blind `SLOT_FREE` store can never clobber a concurrent reclaimer's transaction. The Host is unaffected: it only acts on `SLOT_REQ_READY` in the Guest Slot range, and the claiming CAS refreshes `lease` per §3.6. If the wait for `SLOT_RESP_READY` times out, the Guest MUST NOT store `SLOT_FREE` (the Host may still own the slot in `SLOT_REQ_READY`/`SLOT_BUSY`); the slot is recovered later by a reclaim path once the late response arrives or the lease goes stale.

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

**Rationale**: The primary deployment target is Windows x86/x64, where TSO hardware ordering covers most reorderings the C++/Go memory models permit. Even on this strongly-ordered ISA, however, compilers may reorder data-region accesses across non-atomic stores; only the atomic operations on `state` act as compiler barriers. Implementations must therefore use the rules above as a *language-level* contract, not a hardware-level one — `relaxed` is unsafe even on x86 because of compiler-side reordering of the surrounding data writes/reads. (Per `AGENTS.md` §"Platform Targets", ARM is explicitly not supported, but the same rules would also keep the protocol correct on weakly-ordered ISAs should that ever change.)

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

| Side | Source |
| :--- | :--- |
| C++ Host | `Platform::MonotonicNanos()` — `GetSystemTimePreciseAsFileTime`, normalised to ns-since-Unix-epoch. |
| Go Guest | `shm.MonotonicNanos()` — `time.Now().UnixNano()`. |

The name "MonotonicNanos" predates the wall-clock choice; it stays for API stability. Wall-clock was selected so the values are comparable across processes AND across languages without coordinating clock epochs. NTP steps can move the clock backward; a backward step causes at most a spurious reclamation candidate (guarded by the CAS check against `state`), never data corruption.

**v0.7.2 — reclamation**:

1. Waiter side reads `lease`. If `now - lease > kLeaseTimeoutNs` (suggested: 5 × `responseTimeoutMs` converted to ns), the slot is *presumed abandoned*.
2. Waiter attempts `state.compare_exchange_strong(observed_state, SLOT_FREE)`. If the CAS succeeds the slot is reclaimed; if it fails, the live owner made progress in the meantime — abort the reclamation, retry the normal flow.
3. Reclamation never fires on non-crash code paths because the heartbeat is refreshed inside the timeout window.

Picking the wrong heartbeat cadence or timeout could reclaim a slot a slow-but-live peer is still using → double-use → data corruption. v0.7.2 shipped the reclamation API together with a property-based crash-injection test that asserts no double-claim across N random crash points.

**Why this was patch-layout-compatible**: `reserved[36]` was never written by v0.6.x code, so adding `atomic<uint64> lease` at offset 96 (with 4 bytes of natural alignment padding before it) is invisible to old readers. Old writers don't touch the slice. Mixed-version deployments degrade gracefully — v0.6.x peers participating in the protocol simply never publish a lease; v0.7.x reclaimers see `lease == 0` for those slots and skip reclamation for them, falling back to the pre-v0.7 forever-busy behavior.

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
- **The reclaimer** snapshots `gen` first, validates staleness, then wins the exclusive right to reclaim via `compare_exchange(gen, gen+1)`. This single CAS linearizes reclaim against claim:
  - If a claim's `gen` bump landed first, the reclaimer's `gen` CAS fails → refuse (covers the lease-lag window).
  - If the reclaimer's `gen` CAS lands first, it then publishes `SLOT_FREE` via `compare_exchange(state, observed, SLOT_FREE)` — *not* a blind store — so a concurrent `SLOT_RESP_READY` zombie re-claim that bumped `gen` afterwards is arbitrated by this final `state` CAS. Exactly one of {reclaimer frees, claimant claims} wins.

A burned `gen` tick (reclaimer won `gen` but lost the final `state` CAS) is harmless: `gen` only ever advances.

**Limitation (documented, not a defect)**: a *fully* airtight guard against an adversarial peer that re-claims and immediately republishes within the reclaimer's window would require folding the generation into the `state` synchronizing word itself (a `state`-field ABI/semantics change). The `gen` handshake closes the realistic crash-recovery race — where the abandoned owner does not relinquish and a distinct new owner can only claim a `SLOT_FREE` slot — and is verified by the property/stress tests in `go/reclaim_aba_test.go`. Tightening beyond this is a protocol-redesign item, not a v0.7.x patch.

**Mixed-version compatibility**: a peer that does not advance `gen` (pre-v0.7.5) leaves it at a fixed value; the reclaimer's `gen` CAS then behaves like the pre-v0.7.5 bare reclaim for that slot — correct degradation, no corruption.
