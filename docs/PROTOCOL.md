# SHM IPC Implementation Guide

This document details the implementation of a high-performance, bidirectional Inter-Process Communication (IPC) library using Shared Memory (SHM) and Lock-Free queues. This design is intended to be portable across languages (C++, Go, Python, Java) running on Windows.

## 1. Architecture Overview

The system consists of two processes:
1.  **Client (e.g., C++/Excel)**: Initiates requests.
2.  **Server (e.g., Go/Python)**: Processes requests and returns responses.

Communication happens over **Shared Memory**, organized into two **Lock-Free MPSC (Multi-Producer Single-Consumer) Queues**:
*   `ReqQueue`: Client(s) write requests, Server reads.
*   `RespQueue`: Server writes responses, Client(s) read.

Signaling is handled via **OS Events** (Named Events on Windows) to avoid 100% CPU usage during idle times.

## 2. Memory Layout

The Shared Memory is a single contiguous block of memory mapped by both processes. It is divided into two sections for the two queues.

```
+-------------------------------------------------------+
|                 Shared Memory Region                  |
+-------------------------------------------------------+
|  ReqQueue (Client -> Server)  |  RespQueue (Server -> Client) |
+-------------------------------+-----------------------+
```

### 2.1 Queue Structure

Each Queue consists of a `QueueHeader` followed by the circular data buffer.

**QueueHeader (128 bytes)**
| Offset | Type | Name | Description |
| :--- | :--- | :--- | :--- |
| 0 | `atomic<uint64>` | `writePos` | Monotonically increasing byte offset for writing. |
| 8 | `byte[56]` | `pad1` | Cache-line padding (to avoid false sharing). |
| 64 | `atomic<uint64>` | `readPos` | Monotonically increasing byte offset for reading. |
| 72 | `byte[56]` | `pad2` | Cache-line padding. |
| 128 | `uint64` | `capacity` | Size of the Data Buffer in bytes. |
| 136 | `byte[40]` | `pad3` | Padding to align struct size to 128 bytes (optional/implementation detail). |

**Data Buffer**
Starts immediately after `QueueHeader`. Size is `capacity`.

### 2.2 Block Layout (Inside Data Buffer)

Data is written in **Blocks**. Each block has a header.

**BlockHeader (8 bytes)**
| Offset | Type | Name | Description |
| :--- | :--- | :--- | :--- |
| 0 | `uint32` | `size` | Size of the payload (excluding header). |
| 4 | `atomic<uint32>` | `magic` | State flag. 0=Writing, `0xDA7A0001`=Data Ready, `0xDA7A0002`=Padding. |

**Alignment**
*   `BlockHeader` is always 4-byte aligned.
*   `Payload` follows immediately.
*   **Important**: The `writePos` is reserved in chunks. To maintain alignment for the *next* block, the reservation size is `Align8(payload_size) + HeaderSize`.

## 3. Queue Algorithm

### 3.1 Enqueue (Multi-Producer)

Writers use `CAS` (Compare-And-Swap) on `writePos` to reserve space.

1.  **Calculate Size**: `totalSize = Align8(dataSize) + sizeof(BlockHeader)`.
2.  **Load State**: Read `wPos` (Relaxed) and `rPos` (Acquire).
3.  **Check Full**: If `(wPos - rPos) + totalSize > capacity`, the queue is full. Return False.
4.  **Calculate Offset**: `offset = wPos % capacity`.
5.  **Check Wrapping**:
    *   If `capacity - offset < totalSize`:
        *   **Not enough space at end**. We must insert a **PADDING** block to fill the rest of the buffer and wrap around.
        *   Try `CAS(&writePos, wPos, wPos + (capacity - offset))`.
        *   If success: Write `BlockHeader` at `offset` with `magic = PAD`, `size = (spaceToEnd - HeaderSize)`. Loop again to write data at 0.
        *   If fail: Another writer moved `writePos`. Loop again.
    *   **Special Case**: If `capacity - offset < sizeof(BlockHeader)`, we cannot even write a header.
        *   Try `CAS(&writePos, wPos, wPos + (spaceToEnd))`. If success, just skip. Reader detects this by checking bounds.
6.  **Reserve**:
    *   Try `CAS(&writePos, wPos, wPos + totalSize)`.
    *   If success: Proceed to write.
    *   If fail: Loop again.
7.  **Write**:
    *   Write `BlockHeader` (size = actual data size).
    *   `memcpy` data into payload area.
    *   **Commit**: Store `magic = DATA` (Release semantics).
8.  **Signal**: Call `SetEvent` to wake up the consumer.

### 3.2 Dequeue (Single-Consumer)

Reader tracks `readPos`.

1.  **Check Empty**: Read `wPos` (Acquire) and `rPos` (Relaxed). If `wPos == rPos`, queue is empty.
2.  **Calculate Offset**: `offset = rPos % capacity`.
3.  **Check Wrapping Bounds**:
    *   If `capacity - offset < sizeof(BlockHeader)`, we are at the tiny tail. Skip it: `rPos += (capacity - offset)`. Recurse.
4.  **Read Header**: Access `BlockHeader` at `offset`.
5.  **Wait for Commit**:
    *   Spin while `header.magic == 0`. (Writer reserved but hasn't finished writing).
    *   *Hybrid Wait*: Spin a few times, then Yield/Sleep if stuck (should be rare).
6.  **Process**:
    *   If `magic == PAD`:
        *   Clear magic to 0.
        *   Advance `readPos` by `header.size + sizeof(BlockHeader)`.
        *   Recurse to read next block (at index 0).
    *   If `magic == DATA`:
        *   Read payload.
        *   Clear magic to 0.
        *   Advance `readPos` by `Align8(header.size) + sizeof(BlockHeader)`.
        *   Return Data.

## 4. IPC Protocol

We use **FlatBuffers** for serialization.

**Schema:**
```fbs
union Payload { AddRequest, AddResponse, ... }
table Message {
  req_id: ulong;
  payload: Payload;
}
```

### 4.1 Request Flow (Client -> Server)
1.  Client generates a unique `req_id`.
2.  Client creates a `Message` FlatBuffer with the request payload.
3.  Client calls `ReqQueue.Enqueue(message_bytes)`.
4.  Client creates/resets a local Event and waits (stores `req_id` -> `Event` mapping).

### 4.2 Processing (Server)
1.  Server wakes up on `ReqEvent`.
2.  Server calls `ReqQueue.Dequeue()`.
3.  Server parses `Message`. Dispatches based on `Payload` type.
4.  Server computes result.
5.  Server creates a `Message` with `AddResponse` and the **same** `req_id`.
6.  Server calls `RespQueue.Enqueue(response_bytes)`.

### 4.3 Response Flow (Client)
1.  Client has a background thread (or checks periodically) on `RespQueue`.
2.  When `RespQueue` has data, it dequeues the message.
3.  Parses `req_id`.
4.  Looks up the waiting Client thread's Event.
5.  Signals the Event.
6.  Client thread wakes up, retrieves response.

## 5. Porting Guide

To implement this in a new language (e.g., Python, Java):

1.  **Shared Memory**: Use `mmap` (Python) or `FileChannel.map` (Java) to access the named shared memory.
2.  **Atomics**: You need 64-bit Atomic Load/Store/CAS.
    *   Python: tough, maybe use `ctypes` or a C-extension.
    *   Java: `sun.misc.Unsafe` or `VarHandle` (Java 9+).
3.  **Events**: Access Windows Named Events.
    *   Python: `pywin32`.
    *   Java: JNA/JNI to call `CreateEvent`/`SetEvent`/`WaitForSingleObject`.
4.  **Alignment**: Ensure you respect the 8-byte alignment logic in `Enqueue`/`Dequeue`.
5.  **FlatBuffers**: Use the official `flatc` compiler to generate classes for your language.

---
**Note on Performance**:
*   The "Hybrid Wait" (Spin then Sleep) is crucial for low latency.
*   Batching is not explicitly implemented but inherent: if the Reader is slow, it will see `wPos` far ahead and process multiple blocks in one go without sleeping.
