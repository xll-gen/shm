# IPC Library & Benchmarks

This repository contains a high-performance IPC (Inter-Process Communication) library using Shared Memory and Lock-Free SPSC (Single Producer Single Consumer) Queues. It supports bidirectional communication between C++ and Go.

## Architecture

- **Core Library**: `shm/go` (Go) and `src/`/`include/` (C++).
- **Transport**: Shared Memory with Ring Buffers (SPSC).
- **Signaling**:
  - **Windows**: Named Events (`CreateEvent`, `SetEvent`).
  - **Linux**: Named Semaphores (`sem_open`, `sem_post`).
- **Protocol**:
  - **Wire Format**: FlatBuffers Payload (Request ID embedded).
  - **Serialization**: FlatBuffers (Google).
- **Threading Model**:
  - **Dedicated I/O Threads**: To maintain high throughput and SPSC strictness, both the C++ Host and Go Guest use dedicated background threads for writing to the queue.
  - **Batching**: Writes are batched to minimize atomic operations and system calls (signaling).

## Directory Structure

- `go/`: Go library code (`shm` package).
  - `client.go`: High-level IPC Client with dedicated I/O threads.
  - `queue.go`: SPSC Queue implementation.
- `src/`: C++ library implementation (`IPCHost.cpp`, `Platform.cpp`).
- `include/`: C++ library headers.
- `benchmarks/`: Benchmark applications.
  - `main.cpp`: C++ Benchmark (Host).
  - `go/main.go`: Go Benchmark (Guest).

## License

This project includes external code in the `external/` directory which is subject to its own license terms (e.g., FlatBuffers, ExcelSDK). Please refer to the respective licenses in those directories.
The core library code is provided under the terms found in `LICENSE`.

## Building & Running

### Requirements

- **C++**: GCC (Linux) or MinGW/MSVC (Windows), CMake.
- **Go**: Go 1.18+.
- **Task**: (Optional) Taskfile is provided for automation.

### Build & Run via Taskfile

```bash
task run:benchmark
```

### Manual Build

#### C++ Benchmark

```bash
cd benchmarks/build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

#### Go Benchmark

```bash
cd benchmarks/go
go build -o server main.go
```

### Running Benchmarks

1. Start the Go Server (Guest):
   ```bash
   ./server -w 4
   ```
   (Runs with 4 worker threads, but uses a single dedicated writer thread for IPC).

2. Run the C++ Benchmark (Host):
   ```bash
   ./shm_benchmark
   ```
