# IPC Library & Benchmarks

This repository contains a high-performance IPC (Inter-Process Communication) library using Shared Memory and Lock-Free MPSC Queues. It supports bidirectional communication between C++ and Go.

## Architecture

- **Core Library**: `shm/go` (Go) and `src/`/`include/` (C++).
- **Transport**: Shared Memory with Ring Buffers.
- **Signaling**:
  - **Windows**: Named Events (`CreateEvent`, `SetEvent`).
  - **Linux**: Named Semaphores (`sem_open`, `sem_post`) or generic EventFD (implementation dependent, currently using Semaphores for portability).
- **Protocol**:
  - **Wire Format**: 8-byte Request ID + FlatBuffers Payload.
  - **Serialization**: FlatBuffers (Google).

## Directory Structure

- `go/`: Go library code (`shm` package).
- `src/`: C++ library implementation (`IPC.cpp`, `Platform.cpp`).
- `include/`: C++ library headers.
- `benchmarks/`: Benchmark applications.
  - `main.cpp`: C++ Client (Producer).
  - `go/main.go`: Go Server (Consumer).

## License

This project includes external code in the `external/` directory which is subject to its own license terms (e.g., FlatBuffers, ExcelSDK). Please refer to the respective licenses in those directories.
The core library code is provided under the terms found in `LICENSE`.

## Building & Running

### Requirements

- **C++**: GCC (Linux) or MinGW/MSVC (Windows), CMake.
- **Go**: Go 1.18+.

### Build C++ Benchmark

```bash
cd benchmarks
mkdir build && cd build
cmake ..
make
```

### Build Go Benchmark

```bash
cd benchmarks/go
go build -o server main.go
```

### Running Benchmarks

1. Start the Go Server (Consumer):
   ```bash
   ./server -w 4
   ```
   (Runs with 4 worker threads)

2. Run the C++ Client (Producer):
   ```bash
   ./benchmarks_cpp -t 8
   ```
   (Runs with 8 producer threads)

   Or run the full suite:
   ```bash
   ./benchmarks_cpp
   ```
