# xll-gen/shm

SimpleIPC is a high-performance, low-latency shared-memory IPC library connecting C++ (Host) and Go (Guest). It uses a lock-free, direct slot exchange model to achieve sub-microsecond latency.

## Features

*   **Low Latency**: Uses atomic spin-loops with adaptive backoff (Spin -> Yield -> Sleep) to minimize OS scheduler overhead.
*   **Direct Mode**: 1:1 Thread-to-Slot mapping eliminates contention and queuing delays.
*   **Zero Copy**: Data is written directly to shared memory slots.
*   **Cross-Platform**: Supports Linux (shm_open/sem_open) and Windows (CreateFileMapping/CreateEvent).
*   **Protocol Agnostic**: Transmits raw bytes with a minimal 8-byte Transport Header for request matching.

## Performance Highlights

The project's "Direct Exchange" IPC mode significantly outperforms traditional methods, showcasing sub-microsecond latency and high throughput. This is achieved through a 1:1 thread-to-slot mapping, zero-copy operations, and adaptive hybrid waiting.

On an AMD Ryzen 9 3900x system (Bare-metal), the benchmark demonstrated:
*   **1 Thread**:
    *   **Throughput**: 1,736,783.51 ops/s
    *   **Avg Latency (RTT)**: 0.575 us
*   **4 Threads**:
    *   **Throughput**: 1,931,987.29 ops/s
    *   **Avg Latency (RTT)**: 0.517 us
*   **8 Threads**:
    *   **Throughput**: 1,323,987.09 ops/s
    *   **Avg Latency (RTT)**: 0.755 us
*   **12 Threads**:
    *   **Throughput**: 1,325,149.50 ops/s
    *   **Avg Latency (RTT)**: 0.754 us

For detailed benchmark results and methodology, including Sandbox (Containerized) results, please refer to [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md).

## Architecture

The library operates in **Direct Mode**, where a fixed pool of "Slots" is allocated in shared memory.

*   **Host (C++)**: Creates the shared memory region and manages the slot pool. It acts as the initiator of requests.
*   **Guest (Go)**: Attaches to the shared memory and processes requests. Each worker goroutine is pinned to a specific slot.

### Memory Layout

The shared memory region consists of:
1.  **Exchange Header** (64 bytes): Global metadata (number of slots, slot size).
2.  **Slot Array**: An array of Slots.

Each **Slot** (128-byte Header + Payload) contains:
*   **SlotHeader**: Atomic state variables (`State`, `HostState`, `GuestState`) and message metadata (`ReqSize`, `MsgId`).
*   **Request Buffer**: Area where Host writes data.
*   **Response Buffer**: Area where Guest writes data.

### Synchronization

State transitions are handled via `std::atomic` (C++) and `sync/atomic` (Go).
*   `SLOT_FREE` -> Host claims -> `SLOT_BUSY` -> Host writes -> `SLOT_REQ_READY`
*   Guest sees `SLOT_REQ_READY` -> Processes -> Writes Response -> `SLOT_RESP_READY`
*   Host sees `SLOT_RESP_READY` -> Reads Response -> `SLOT_FREE`

If a peer is not responsive (spinning times out), the other peer will wait on a named OS event (Semaphore/Event) to save CPU.

## Usage

### C++ Host

```cpp
#include <shm/DirectHost.h>

shm::DirectHost host;
if (!host.Init("MyIPC", 4)) { // 4 Worker Slots
    return -1;
}

/*
// To enable Async Guest Calls (Guest -> Host), use the 4th argument:
if (!host.Init("MyIPC", 4, 1024*1024, 2)) { // 4 Worker Slots, 1MB Slot Size, 2 Async Slots
    return -1;
}
*/

std::vector<uint8_t> resp;
// Send 4 bytes to any available slot
// Note: This blocks until response is received.
host.Send((const uint8_t*)"test", 4, MSG_ID_NORMAL, resp);
```

### Zero-Copy (FlatBuffers)

To send FlatBuffers without copying the data, use the `ZeroCopySlot` helper:

```cpp
// 1. Acquire a Zero-Copy Slot
auto slot = host.GetZeroCopySlot();

// 2. Build FlatBuffer directly in shared memory
// slot.GetReqBuffer() returns the pointer to the buffer
flatbuffers::FlatBufferBuilder builder(slot.GetMaxReqSize(), nullptr, false, slot.GetReqBuffer());
// ... build your object ...

// 3. Send Request
// Signals MSG_ID_FLATBUFFER and handles negative size internally
slot.SendFlatBuffer(builder.GetSize());

// 4. Access Response Directly (Zero-Copy)
uint8_t* respData = slot.GetRespBuffer();
int32_t respSize = slot.GetRespSize();
```

### Go Guest

```go
package main

import "github.com/xll-gen/shm/go"

func main() {
    client, _ := shm.Connect("MyIPC")

    // Handler now receives msgId
    client.Handle(func(req []byte, respBuf []byte, msgId uint32) int32 {
        if msgId == shm.MsgIdFlatbuffer {
            // "req" automatically points to the FlatBuffer data
            // (even if it was sent with negative size alignment)
            // processFlatBuffer(req)
        }

        // Process req, write to respBuf
        // Return number of bytes written
        return int32(copy(respBuf, req)) // Echo
    })

    client.Start()
    client.Wait()
}
```

### Custom Message IDs

You can use custom message IDs to multiplex different types of operations on the same connection.
The system reserves IDs `0` through `127`. User-defined IDs should start at `MSG_ID_USER_START` (128).

**C++ Host:**
```cpp
#include <shm/IPCUtils.h>

// Define your custom ID
const uint32_t MY_OP_ID = MSG_ID_USER_START + 1;

// Send
host.Send(payload, size, MY_OP_ID, resp);
```

**Go Guest:**
```go
const MyOpId = shm.MsgIdUserStart + 1

client.Handle(func(req []byte, respBuf []byte, msgId uint32) int32 {
    if msgId == MyOpId {
        // Handle custom op
    }
    // ...
})
```

### Guest Call (Async)

The library supports Guest-initiated calls (e.g., for async callbacks). Specific slots are reserved for this purpose.

**C++ Host (Listener):**

```cpp
// Init with 4 Host Slots and 2 Guest Slots
host.Init("MyIPC", 4, 1024*1024, 2);

// In a background thread:
while (running) {
    host.ProcessGuestCalls([](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t msgId) -> int32_t {
        if (msgId == MSG_ID_GUEST_CALL) {
             // Process Guest Request
        }
        return 0; // Return response size
    });
    // Sleep/Yield to avoid 100% CPU if polling
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
```

**Go Guest (Caller):**

```go
// Send Guest Call
// msgId can be shm.MsgIdGuestCall or custom
resp, err := client.SendGuestCall([]byte("AsyncData"), shm.MsgIdGuestCall)
```

## Building

### Requirements
*   **Linux**: Kernel 4.x+, GCC 8+/Clang 10+
*   **Windows**: MSVC 2019+
*   **Go**: 1.18+
*   **CMake**: 3.10+

### Build Steps

The project uses `Taskfile` for automation (requires [Task](https://taskfile.dev/)).

```bash
# Run all benchmarks (Builds C++ and Go, runs tests)
task run:benchmark
```

If you need to build manually:

```bash
# Build C++ Benchmarks
mkdir build && cd build
cmake ../benchmarks
make

# Build Go Benchmark Server
cd ../benchmarks/go
go build
```

## Benchmarks

The `benchmarks` folder contains a latency/throughput test.

```bash
# Run benchmark (Helper script)
./benchmarks/run.sh
```

## Experiments

The `experiments` folder contains standalone latency tests (`pingpong`) used to validate the underlying synchronization primitives without the library overhead.

## Documentation

*   `AGENTS.md`: Developer guidelines and constraints.
*   Source code is fully documented with Doxygen (C++) and GoDoc (Go) comments.
