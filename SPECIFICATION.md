# Shared Memory IPC Specification (v0.2.0)

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
| `version` | `uint32` | 4 | Protocol Version (0x00020000). |
| `numSlots` | `uint32` | 8 | Number of Host-to-Guest slots. |
| `numGuestSlots` | `uint32` | 12 | Number of Guest-to-Host (Guest Call) slots. |
| `slotSize` | `uint32` | 16 | Total size of a single slot in bytes. |
| `reqOffset` | `uint32` | 20 | Offset of the Request Buffer within a slot (relative to Slot start). |
| `respOffset` | `uint32` | 24 | Offset of the Response Buffer within a slot (relative to Slot start). |
| `padding` | `uint8[36]`| 28 | Padding to ensure 64-byte alignment. |

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
| `padding` | `uint8[36]` | 92 | Padding to reach 128 bytes. |

**Total Size:** 128 Bytes.

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
| `SLOT_BUSY` | 4 | Slot is claimed by Owner, data is being written. |

### 3.2. Message Types

| Constant | Value | Description |
| :--- | :--- | :--- |
| `MSG_TYPE_NORMAL` | 0 | Standard data payload. |
| `MSG_TYPE_HEARTBEAT_REQ` | 1 | Keep-alive request. |
| `MSG_TYPE_HEARTBEAT_RESP` | 2 | Keep-alive response. |
| `MSG_TYPE_SHUTDOWN` | 3 | Signal to terminate worker loop. |
| `MSG_TYPE_FLATBUFFER` | 10 | Zero-Copy FlatBuffer (End-aligned). |
| `MSG_TYPE_GUEST_CALL` | 11 | Guest-initiated call to Host. |
| `MSG_TYPE_APP_START` | 128 | Start of user-defined message types. |

### 3.3. Data Alignment (Negative Size)

The `reqSize` and `respSize` fields indicate the location of the data within the buffer:
- **Positive (> 0):** Data starts at the beginning of the buffer (Offset 0).
- **Negative (< 0):** Data is aligned to the **end** of the buffer. The actual size is `abs(size)`. The start offset is `BufferSize - abs(size)`. This is typically used for FlatBuffers construction.

### 3.4. Host-to-Guest Flow

1.  **Claim:** Host finds a slot in state `SLOT_FREE` and atomically sets it to `SLOT_BUSY`.
2.  **Write:** Host writes data to the Request Buffer and sets `reqSize`, `msgSeq`, and `msgType`.
3.  **Signal:** Host atomically sets state to `SLOT_REQ_READY` and signals the Guest (via Event/Semaphore).
4.  **Process:** Guest wakes up, reads the Request, processes it, and writes to the Response Buffer.
5.  **Reply:** Guest sets `respSize`, updates state to `SLOT_RESP_READY`, and signals the Host.
6.  **Complete:** Host wakes up, reads the Response, and sets state back to `SLOT_FREE`.

### 3.5. Guest-to-Host Flow (Guest Call)

1.  **Claim:** Guest finds a slot in the **Guest Slot** range (offset by `numSlots`) in state `SLOT_FREE`.
2.  **Write:** Guest writes data, sets `msgType` to `MSG_TYPE_GUEST_CALL`.
3.  **Signal:** Guest sets state to `SLOT_REQ_READY` and signals the Host.
4.  **Process:** Host (polling `ProcessGuestCalls`) detects `SLOT_REQ_READY`, processes, and writes response.
5.  **Reply:** Host sets `SLOT_RESP_READY` and signals Guest.
6.  **Complete:** Guest reads response and resets state to `SLOT_FREE`.

## 4. Synchronization & Signaling

Platform-specific primitives are used to wake up sleeping threads.

### 4.1. Naming Convention

All synchronization primitives are named based on the Shared Memory name (`SHM_NAME`) and the slot index.

- **Request Event (Host -> Guest):** `{SHM_NAME}_slot_{INDEX}`
- **Response Event (Guest -> Host):** `{SHM_NAME}_slot_{INDEX}_resp`

*Note: On Linux, named semaphores often require a leading `/`, so the actual name might be `/SimpleIPC_slot_0`.*

### 4.2. Hybrid Wait Strategy

Implementations should use a hybrid wait strategy for optimal latency:
1.  **Spin:** Busy-wait for a short period (checking atomic `state`).
2.  **Yield:** Call `std::this_thread::yield()` or `runtime.Gosched()`.
3.  **Sleep:** Wait on the OS primitive (Semaphore/Event) if the peer signals it is sleeping (`hostState`/`guestState` flags).

### 4.3. Platform Implementation

| Feature | Linux | Windows |
| :--- | :--- | :--- |
| **Shared Memory** | `shm_open` / `mmap` | `CreateFileMapping` / `MapViewOfFile` |
| **Signaling** | Named Semaphores (`sem_open`) | Named Events (`CreateEvent`) |
| **Atomic Wait** | `sem_wait` / `sem_post` | `WaitForSingleObject` / `SetEvent` |

## 5. Implementation Guidelines

- **Header Only (C++):** The C++ implementation must remain header-only in `include/shm/`.
- **Zero Dependency (Go):** The Go implementation should avoid external dependencies (cgo is permitted for system calls).
- **Endianness:** The protocol assumes all peers are **Little Endian**.
- **Padding:** Strict adherence to the padding bytes in `SlotHeader` and `ExchangeHeader` is required for binary compatibility.
