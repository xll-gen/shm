# SimpleIPC Library

SimpleIPC is a low-latency, shared-memory IPC library for C++ (Host) and Go (Guest). It supports two modes: **Queue** (SPSC/MPSC) and **Direct Exchange** (Slot-based), unified under a single API.

## Features

*   **Unified API:** Write code once, switch between Queue and Direct modes easily.
*   **Low Latency:** Uses shared memory and lock-free/wait-free structures (SPSC Queue or CAS Slots).
*   **Protocol Agnostic:** Transmits raw bytes. Includes a minimal 8-byte `TransportHeader` for request matching.
*   **Cross-Language:** C++ Host and Go Guest.

## C++ Host Usage

```cpp
#include "IPCHost.h"

// 1. Initialize Host
shm::IPCHost host;
bool success = host.Init("MyIPC", shm::IPCMode::Queue, 32 * 1024 * 1024); // 32MB Queue

// 2. Call Guest
std::vector<uint8_t> req = { ... };
std::vector<uint8_t> resp;
if (host.Call(req.data(), req.size(), resp)) {
    // Handle response
}

// 3. Shutdown
host.SendShutdown();
host.Shutdown();
```

### Direct Mode
To use Direct Mode (optimized for 1:1 worker mapping):
```cpp
host.Init("MyIPC", shm::IPCMode::Direct, 4); // 4 Lanes
```

## Go Guest Usage

```go
package main

import "github.com/xll-gen/shm/go"

func main() {
    // 1. Connect to Host (Auto-retry)
    client, err := shm.Connect("MyIPC", shm.ModeQueue)
    if err != nil {
        panic(err)
    }

    // 2. Register Handler
    client.Handle(func(req []byte) []byte {
        // Process request
        return []byte("response")
    })

    // 3. Start
    go client.Start()

    // 4. Wait for Shutdown
    client.Wait()
    client.Close()
}
```

## Architecture

*   **Queue Mode:** Uses a ring buffer in shared memory. Good for streaming or variable-sized messages.
*   **Direct Mode:** Uses fixed-size slots mapped to threads. Lowest latency for request-response patterns.
*   **Transport Header:** An 8-byte header containing the `req_id` is prepended to all messages to match responses to requests asynchronously.

## Building Benchmarks

```bash
# Generate FlatBuffers
flatc --cpp --go -o benchmarks/include benchmarks/ipc.fbs

# Build C++
mkdir build && cd build
cmake ..
make

# Build Go
cd benchmarks/go
go build
```
