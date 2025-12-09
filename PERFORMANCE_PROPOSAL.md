# Performance Improvement Proposal

This document outlines proposed measures to enhance the throughput (OPS) and efficiency of the `xll-gen/shm` Direct Exchange IPC, specifically targeting the Sandbox (Container) environment.

## 1. Zero-Allocation API (High Impact)

**Problem:**
Currently, the `DirectHost::Send` family of functions (C++) and `Client.SendGuestCall` (Go) perform heap allocations for every operation to store the response data.
- **C++:** `std::vector<uint8_t>& outResp` is resized (potentially reallocated) and `memcpy` is used.
- **Go:** `make([]byte, respSize)` creates a new slice for every Guest Call response.

**Proposal:**
Introduce "Zero-Allocation" overloads that allow the caller to provide a pre-allocated buffer.

### C++ Changes
Add an overload to `DirectHost` and `ZeroCopySlot`:

```cpp
// Existing
int Send(..., std::vector<uint8_t>& outResp, ...);

// Proposed (Zero-Alloc)
int Send(const uint8_t* req, int32_t reqSize, uint32_t msgType,
         uint8_t* respBuf, uint32_t respCap,
         uint32_t timeoutMs = USE_DEFAULT_TIMEOUT);
```

This allows the user to reuse a single buffer across millions of operations, eliminating `malloc/free` overhead.

### Go Changes
Add a buffer-accepting method to `DirectGuest` and `Client`:

```go
// Existing
func (c *Client) SendGuestCall(data []byte, msgType MsgType) ([]byte, error)

// Proposed (Zero-Alloc)
// Returns number of bytes read into respBuf
func (c *Client) SendGuestCallBuffer(data []byte, msgType MsgType, respBuf []byte) (int, MsgType, error)
```

## 2. Lightweight Callbacks (Medium Impact)

**Problem:**
`DirectHost::ProcessGuestCalls` accepts a `std::function`. While flexible, `std::function` incurs overhead (virtual dispatch, potential allocation for captures) compared to a raw function pointer or a template.

**Proposal:**
Convert `ProcessGuestCalls` to a template method. This allows the compiler to inline the handler lambda completely.

```cpp
// Proposed
template <typename HandlerFunc>
int ProcessGuestCalls(HandlerFunc&& handler) {
    // ... logic ...
    // handler(...) calls are now direct/inlined
}
```

## 3. Sandbox-Aware Spin Tuning (Environment Specific)

**Problem:**
The current "Hybrid Wait" strategy spins up to 50,000 times (C++) or 2,000 times (Go) before sleeping.
In a **Sandbox/Container** environment with shared CPU cores, excessive spinning by one process ("Host") steals CPU cycles from the other process ("Guest") running on the same physical core. This increases latency and reduces effective throughput.

**Proposal:**
1.  **Expose Spin Configuration:** Allow users to configure `minSpin`/`maxSpin` via `DirectHost::Init` or `SetSpinLimits`.
2.  **Sandbox Preset:** Recommend (or default to) a "Low Spin" configuration for containers.
    -   *Host:* Max Spin 100-500 (vs 50,000).
    -   *Guest:* Max Spin 100-500 (vs 2,000).
    -   *Yield Strategy:* Use `_mm_pause` (or `PAUSE`) more aggressively.

## 4. Memory Ordering Optimization (Micro-Optimization)

**Problem:**
The current implementation uses `std::memory_order_seq_cst` (Sequential Consistency) for state transitions (`SLOT_REQ_READY`, `SLOT_RESP_READY`). This enforces a full memory barrier (e.g., `MFENCE` on x86), which is expensive.

**Proposal:**
Downgrade to `std::memory_order_release` for signaling (Writer) and `std::memory_order_acquire` for checking (Reader).

-   **Signal:** `state.store(SLOT_REQ_READY, std::memory_order_release)`
-   **Wait:** `state.load(std::memory_order_acquire)`

*Note:* This requires careful verification to ensure no race conditions exist with the `guestState`/`hostState` sleep flags.

## Summary of Estimated Impact

| Optimization | Complexity | Estimated OPS Gain (Sandbox) |
| :--- | :--- | :--- |
| **Zero-Allocation API** | Low | **+10% - 20%** |
| **Lightweight Callbacks** | Low | **+2% - 5%** (Guest Call only) |
| **Spin Tuning** | Low | **+5% - 15%** (Highly variable) |
| **Memory Ordering** | High (Risk) | **< 5%** |
