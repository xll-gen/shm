# SimpleIPC Library

SimpleIPC is a high-performance, low-latency shared-memory IPC library connecting C++ (Host) and Go (Guest). It uses a **Direct** (Slot-based Exchange) mode optimized for latency.

## Features

*   **Low Latency:** Slot-based atomic CAS operations with adaptive spinning (1:1 thread mapping) and Zero Copy data exchange.
*   **Zero Copy (Go):** The Go Guest handler writes directly to the shared memory response buffer, avoiding allocation overhead.
*   **Protocol Agnostic:** Transmits raw bytes. A minimal 8-byte `TransportHeader` allows for async request/response matching.
*   **Cross-Language:** Seamless integration between C++ and Go.

## Architecture

### Direct Mode
*   **Shared Memory Slots:** Memory is divided into N slots.
*   **Concurrency:** 1:1 mapping between Host Worker Threads and Guest Worker Threads.
*   **Split Buffers:** Each slot contains a `SlotHeader`, a `ReqBuffer`, and a `RespBuffer`.
*   **Synchronization:** Hybrid Adaptive Spin + Sleep (using Semaphores).

## C++ Host Usage

The `IPCHost` class acts as a facade, handling slot management and request matching.

```cpp
#include "IPCHost.h"

int main() {
    shm::IPCHost host;

    // Initialize with 4 Slots (supporting 4 concurrent threads)
    bool success = host.Init("MyIPC", 4);

    if (!success) return -1;

    // Call Guest
    // User Payload: { 0x01, 0x02 }
    std::vector<uint8_t> req = { 0x01, 0x02 };
    std::vector<uint8_t> resp;

    // Call blocks until response is received or timeout
    if (host.Call(req.data(), req.size(), resp)) {
        // resp contains User Payload
    }

    // Cleanup
    host.SendShutdown(); // Signal Guest to exit
    host.Shutdown();     // Close resources
    return 0;
}
```

## Go Guest Usage

The Go `Client` acts as the server, processing requests.

**Important:** The handler receives the *full* payload (including 8-byte `TransportHeader`) and writes directly to the response buffer.

```go
package main

import (
    "encoding/binary"
    "github.com/xll-gen/shm/go"
)

func main() {
    // 1. Connect to Host
    client, err := shm.Connect("MyIPC")
    if err != nil {
        panic(err)
    }

    // 2. Register Zero-Copy Handler
    // req: Input slice (Directly from SHM)
    // respBuf: Output slice (Directly in SHM)
    // Return: Number of bytes written to respBuf
    client.Handle(func(req []byte, respBuf []byte) int {
        if len(req) < 8 {
            return 0
        }

        // Preserve TransportHeader (req_id)
        // Copy first 8 bytes from req to respBuf
        copy(respBuf[0:8], req[0:8])

        // Process Payload
        payload := req[8:]
        // ... processing ...

        // Write result after header
        // copy(respBuf[8:], result)

        return 8 + len(result)
    })

    // 3. Start Processing
    go client.Start()

    // 4. Wait for Shutdown Signal
    client.Wait()
    client.Close()
}
```

## Building Benchmarks

```bash
# Run all benchmarks (Requires CMake and Go)
./benchmarks/run_bench.sh
```

## Requirements
*   **Linux:** Kernel 4.x+ (supports `/dev/shm`), `pthread`.
*   **Compiler:** GCC 8+ or Clang 10+ (C++17 support).
*   **Go:** 1.18+.
