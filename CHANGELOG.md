# Changelog

## [v0.2.0] - 2025-12-05

### Features
- **Guest Call**: Allows Go Guest to initiate calls to C++ Host (Async/Callback pattern).
- **Protocol Update**: Added `NumGuestSlots` to `ExchangeHeader` and `MSG_ID_GUEST_CALL`.
- **Zero-Copy Support for Guest Calls**: Guest can send FlatBuffers end-aligned.
- **Listener API**: `DirectHost::ProcessGuestCalls` for polling Guest requests.

### API Changes
- Updated `DirectHost::ProcessGuestCalls` handler signature to include `maxRespSize`, enabling safe end-aligned writes for Zero-Copy workflows.

## [v0.1.0] - 2025-12-05

First stable release of the Shared Memory IPC library.

### Key Features
- **Direct Exchange Architecture**: 1:1 Slot Mapping for low latency.
- **Header-only C++ Host Library**: Easy integration via `include/shm/`.
- **High-Performance Go Guest**: Zero-allocation hot path, Direct Mode support.
- **Cross-Platform**: Support for Linux (shm/pthreads) and Windows (NamedSharedMemory/Events).
- **Zero-Copy Support**: Optimized for large payloads and FlatBuffers.
- **Adaptive Hybrid Wait**: Dynamic spin-backoff for efficient resource usage.

### Benchmarks (Effective OPS)
- 1 Thread: ~1.7M OPS
- 4 Threads: ~2.2M OPS
- 8 Threads: ~1.8M OPS
