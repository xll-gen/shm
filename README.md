# xll-gen/shm

SimpleIPC is a high-performance, low-latency shared-memory IPC library connecting C++ (Host) and Go (Guest). It uses a lock-free, direct slot exchange model to achieve sub-microsecond latency.

> **⚠️ WARNING: EXPERIMENTAL STATUS**
>
> This project is currently in an **experimental stage** (v0.5.4) and is under active development.
> It is **NOT** recommended for use in production environments at this time.
> APIs and memory layouts are subject to change without notice.

## Features

*   **Low Latency**: Uses atomic spin-loops with adaptive backoff (Spin -> Yield -> Sleep) to minimize OS scheduler overhead.
*   **Direct Mode**: 1:1 Thread-to-Slot mapping eliminates contention and queuing delays.
*   **Zero Copy**: Data is written directly to shared memory slots.
*   **Cross-Platform**: Supports Linux (shm_open/sem_open) and Windows (CreateFileMapping/CreateEvent).
*   **Protocol Agnostic**: Transmits raw bytes with a minimal 8-byte Transport Header for request matching.
*   **Guest-to-Host Calls**: Supports async notifications from Go to C++.
*   **Header-Only C++**: Easy integration via `include/shm`.

## Performance Highlights

The project's "Direct Exchange" IPC mode significantly outperforms traditional methods, showcasing sub-microsecond latency and high throughput. This is achieved through a 1:1 thread-to-slot mapping, zero-copy operations, and adaptive hybrid waiting.

**Sandbox Environment (Containerized):**
*   **1 Thread**:
    *   **Throughput**: ~1.48M ops/s
    *   **Avg Latency (RTT)**: 0.67 us
*   **4 Threads**:
    *   **Throughput**: ~1.87M ops/s
    *   **Avg Latency (RTT)**: 2.14 us
*   **8 Threads**:
    *   **Throughput**: ~1.92M ops/s
    *   **Avg Latency (RTT)**: 4.17 us

**AMD Ryzen 9 3900x (Bare-metal):**
*   **1 Thread**: 1.74M ops/s (0.58 us)
*   **4 Threads**: 1.93M ops/s (0.52 us)
*   **8 Threads**: 1.32M ops/s (0.76 us)

For detailed benchmark results, methodology, and Guest Call scenarios, please refer to [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md).

## Architecture

The library operates in **Direct Mode**, where a fixed pool of "Slots" is allocated in shared memory.

*   **Host (C++)**: Creates the shared memory region and manages the slot pool. It acts as the initiator of requests.
*   **Guest (Go)**: Attaches to the shared memory and processes requests. Each worker goroutine is pinned to a specific slot.

### Memory Layout

The shared memory region consists of:
1.  **Exchange Header** (64 bytes): Global metadata (Magic, Version, number of slots, slot size).
2.  **Slot Array**: An array of Slots.

Each **Slot** (128-byte Header + Payload) contains:
*   **SlotHeader**: Atomic state variables (`State`, `HostState`, `GuestState`) and message metadata (`ReqSize`, `MsgSeq`, `MsgType`).
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
shm::HostConfig config;
config.shmName = "MyIPC";
config.numHostSlots = 4;
config.payloadSize = 1024 * 1024; // 1MB payload per slot
config.numGuestSlots = 0; // Set to >0 to enable Guest Calls

if (!host.Init(config).IsSuccess()) {
    std::cerr << "Failed to init host" << std::endl;
    return -1;
}

std::vector<uint8_t> resp;
// Send 4 bytes to any available slot
// Note: This blocks until response is received.
auto result = host.Send((const uint8_t*)"test", 4, shm::MsgType::NORMAL, resp);
if (result.HasError()) {
    // Handle error
}
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
// Signals MSG_TYPE_FLATBUFFER and handles negative size internally
// Returns Result<void>
auto res = slot.SendFlatBuffer(builder.GetSize());
if (res.HasError()) { /* Handle Error */ }

// 4. Access Response Directly (Zero-Copy)
uint8_t* respData = slot.GetRespBuffer();
int32_t respSize = slot.GetRespSize();
```

### Go Guest

First, install the module:

```bash
go get github.com/xll-gen/shm
```

Then import it:

```go
package main

import "github.com/xll-gen/shm/go"

func main() {
    // Basic Connection
    client, _ := shm.ConnectDefault("MyIPC")

    // Or Advanced Configuration
    /*
    client, _ := shm.Connect(shm.ClientConfig{
        ShmName: "MyIPC",
        ConnectionTimeout: 5 * time.Second,
    })
    */

    // Handler now receives msgType and returns msgType
    client.Handle(func(req []byte, respBuf []byte, msgType shm.MsgType) (int32, shm.MsgType) {
        if msgType == shm.MsgTypeFlatbuffer {
            // "req" automatically points to the FlatBuffer data
            // (even if it was sent with negative size alignment)
            // processFlatBuffer(req)
        }

        // Process req, write to respBuf
        // Return number of bytes written and the response type
        return int32(copy(respBuf, req)), msgType // Echo Type
    })

    client.Start()
    client.Wait()
}
```

### Application Specific Message Types

You can use custom message types to multiplex different types of operations on the same connection.
The system reserves types `0` through `127`. User-defined types should start at `MSG_TYPE_APP_START` (128).

**C++ Host:**
```cpp
#include <shm/IPCUtils.h>

// Define your custom Type
const uint32_t MY_OP_TYPE = (uint32_t)shm::MsgType::APP_START + 1;

// Send
host.Send(payload, size, (shm::MsgType)MY_OP_TYPE, resp);
```

**Go Guest:**
```go
const MyOpType = shm.MsgTypeAppStart + 1

client.Handle(func(req []byte, respBuf []byte, msgType shm.MsgType) (int32, shm.MsgType) {
    if msgType == MyOpType {
        // Handle custom op
        return 0, MyOpType
    }
    // ...
})
```

### Guest Call (Async)

The library supports Guest-initiated calls (e.g., for async callbacks). Specific slots are reserved for this purpose.

**C++ Host (Listener):**

```cpp
shm::HostConfig config;
config.shmName = "MyIPC";
config.numHostSlots = 4;
config.numGuestSlots = 2; // 2 Async Slots
host.Init(config);

// Start background worker for Guest Calls
host.Start([](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxRespSize, shm::MsgType msgType) -> int32_t {
    if (msgType == shm::MsgType::GUEST_CALL) {
         // Process Guest Request
    }
    return 0; // Return response size
});

// To stop:
// host.Stop();
```

**Go Guest (Caller):**

```go
// Send Guest Call
// msgType can be shm.MsgTypeGuestCall or custom
resp, err := client.SendGuestCall([]byte("AsyncData"), shm.MsgTypeGuestCall)
```

### Handling Long-Running Operations (Async Call Pattern)

The default timeout for operations is 10 seconds. For operations that may exceed this duration, or for asynchronous workflows, do **not** block the IPC channel. Instead, use the following pattern:

1.  **Host** sends a Request (e.g., `START_LONG_JOB`).
2.  **Guest** receives the request, starts the job in a background goroutine, and **immediately** returns an acknowledgement (Ack).
3.  **Host** receives the Ack and is free to process other tasks.
4.  When the job completes, the **Guest** sends the result back to the Host using a **Guest Call** (`SendGuestCall`).
5.  **Host** processes the result via the handler registered in `Start()`.

This ensures the 1:1 slot mapping remains available for high-frequency messages and prevents timeouts.

### Nested IPC & Recursion

The library supports recursive calls (e.g., calling `GetZeroCopySlot` or `Send` while already holding a slot) provided that sufficient slots are available.

**Important**: If you plan to use nested IPC (e.g. `Host -> Guest -> Host` or recursive Host calls), you **must** configure `numHostSlots` to be at least `N_threads * (Depth + 1)`.

*   **Example**: If you have 1 thread performing a nested call (Depth 1), you need at least **2 slots**.
*   **Failure to do so will result in Deadlock** (the inner call waiting forever for a slot held by the outer call).

For complex recursion, it is recommended to **double the slot count** to provide a safety margin.

> **Note on Corruption**: "Corruption" during nested calls usually stems from the application mistakenly reusing the *same* `ZeroCopySlot` object instance for the inner call. Always call `GetZeroCopySlot()` again to acquire a distinct slot for the nested operation.

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

#### Linux (Manual)

```bash
# Build C++ Benchmarks
mkdir build && cd build
cmake .. -DSHM_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
make

# Build Go Benchmark Server
cd ../benchmarks/go
go build
```

#### Windows (Manual with MSVC)

1.  **Generate Visual Studio Solution:**
    ```cmd
    mkdir build
    cd build
    cmake -S .. -B . -DSHM_BUILD_BENCHMARKS=ON
    ```

2.  **Build with Release Configuration:**
    ```cmd
    cmake --build . --config Release
    ```

3.  **Run Benchmarks:**
    Use `Taskfile` or run the executables directly from the `Release` folder.
    ```cmd
    task run:benchmark
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
*   `SPECIFICATION.md`: Protocol details and memory layout.
*   Source code is fully documented with Doxygen (C++) and GoDoc (Go) comments.
