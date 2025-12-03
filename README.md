# SimpleIPC Library

SimpleIPC is a high-performance, low-latency shared-memory IPC library connecting C++ (Host) and Go (Guest). It supports two operating modes: **Queue** (SPSC Ring Buffer) and **Direct** (Slot-based Exchange), unified under a single API.

## Features

*   **Unified API:** Write code once, switch between Queue and Direct modes via configuration.
*   **Low Latency:**
    *   **Queue Mode:** Lock-free SPSC ring buffers for high-throughput streaming.
    *   **Direct Mode:** Slot-based atomic CAS operations with adaptive spinning for sub-microsecond latency (1:1 thread mapping).
*   **Protocol Agnostic:** Transmits raw bytes. A minimal 8-byte `TransportHeader` allows for async request/response matching.
*   **Cross-Language:** Seamless integration between C++ and Go.
*   **Cross-Platform:** Supports Linux (shm_open/named semaphores) and Windows (File Mapping/Events).

## Architecture

### Modes
1.  **Queue Mode (`IPCMode::Queue` / `shm.ModeQueue`)**
    *   Uses two SPSC (Single Producer Single Consumer) ring buffers in shared memory (Request & Response).
    *   Optimized for high throughput and variable message sizes.
    *   Uses "Signal-If-Waiting" optimization to reduce system calls.

2.  **Direct Mode (`IPCMode::Direct` / `shm.ModeDirect`)**
    *   Uses a fixed set of "Slots" in shared memory (Lanes).
    *   Each slot corresponds to a worker thread.
    *   Uses atomic state transitions (`Free` -> `Busy` -> `RespReady`) instead of queues.
    *   Ideal for request-response patterns where latency is critical.

### Protocol
The library operates on two layers:

1.  **Wire Layer:**
    *   **MsgId (4 bytes):** Identifies the message type (`Normal`, `Heartbeat`, `Shutdown`).
    *   **Payload:** Raw bytes.

2.  **Facade Layer (`IPCHost` / `IPCGuest`):**
    *   When using the `IPCHost` C++ class and `IPCGuest` Go struct, a **Transport Header** (8 bytes) is prepended to the payload.
    *   **Header:** `uint64_t req_id`.
    *   This ID is used to match asynchronous responses to their original requests (Promises/Futures).
    *   **Important:** The Guest (Go) must preserve this 8-byte header when sending a response.

## C++ Host Usage

The `IPCHost` class acts as a facade, handling mode selection and request matching.

```cpp
#include "IPCHost.h"
#include <vector>
#include <iostream>

int main() {
    shm::IPCHost host;

    // 1. Initialize
    // Mode: Queue (32MB Buffer)
    // bool success = host.Init("MyIPC", shm::IPCMode::Queue, 32 * 1024 * 1024);

    // Mode: Direct (4 Worker Lanes)
    bool success = host.Init("MyIPC", shm::IPCMode::Direct, 4);

    if (!success) {
        std::cerr << "Failed to initialize IPC" << std::endl;
        return -1;
    }

    // 2. Call Guest
    // User Payload: { 0x01, 0x02 }
    // IPCHost will prepend 8-byte req_id automatically.
    std::vector<uint8_t> req = { 0x01, 0x02 };
    std::vector<uint8_t> resp;

    // Call blocks until response is received or timeout (implicit)
    if (host.Call(req.data(), req.size(), resp)) {
        // resp contains only User Payload (header is stripped)
        std::cout << "Received response of size: " << resp.size() << std::endl;
    }

    // 3. Cleanup
    host.SendShutdown(); // Signal Guest to exit
    host.Shutdown();     // Close resources
    return 0;
}
```

## Go Guest Usage

The Go `IPCGuest` (accessed via `shm.Connect`) acts as the server, processing requests.

**Important:** The handler receives the *full* payload, including the 8-byte `TransportHeader`. You must include the ID in your response.

```go
package main

import (
    "encoding/binary"
    "fmt"
    "github.com/xll-gen/shm/go"
)

func main() {
    // 1. Connect to Host
    // Automatically retries until Host creates shared memory.
    client, err := shm.Connect("MyIPC", shm.ModeDirect)
    if err != nil {
        panic(err)
    }
    defer client.Close()

    // 2. Register Handler
    client.Handle(func(req []byte) []byte {
        if len(req) < 8 {
            return nil
        }

        // Read TransportHeader (req_id)
        reqId := binary.LittleEndian.Uint64(req[0:8])

        // Read User Payload
        payload := req[8:]

        // Process...
        result := process(payload)

        // Construct Response
        // Must start with the SAME req_id
        resp := make([]byte, 8 + len(result))
        binary.LittleEndian.PutUint64(resp[0:8], reqId)
        copy(resp[8:], result)

        return resp
    })

    // 3. Start Processing (Non-blocking usually, but Wait blocks)
    go client.Start()

    // 4. Wait for Shutdown Signal from Host
    client.Wait()
    fmt.Println("Shutting down...")
}

func process(data []byte) []byte {
    return data // Echo
}
```

## Building Benchmarks

The project includes a benchmark suite to measure round-trip latency.

```bash
# 1. Build C++ Host
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
cd ..

# 2. Build Go Guest
cd benchmarks/go
go build -o guest
cd ../..

# 3. Run Benchmark
# Ensure both 'shm_benchmark' (C++) and 'guest' (Go) are built.
# You typically run the Host, which waits for the Guest.
# Or use the Taskfile if available.
```

## Requirements
*   **Linux:** Kernel 4.x+ (supports `memfd_create` or `/dev/shm`), `pthread`.
*   **Windows:** Windows 10/Server 2016+ (Named Events, File Mapping).
*   **Compiler:** GCC 8+ or Clang 10+ (C++17 support).
*   **Go:** 1.18+ (Generics support).
