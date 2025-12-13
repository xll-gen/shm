# API Safety Review & Improvement Proposal

This document outlines the findings of a safety audit performed on the `xll-gen/shm` library (v0.5.3) and proposes improvements to enhance memory safety, type safety, and error handling.

## 1. Executive Summary

The library currently exhibits a high degree of **Memory Safety** and **Concurrency Safety** due to its rigorous use of atomic state transitions, sequence numbering (`msgSeq`), and bounds checking in both C++ and Go.

However, we identified areas for improvement in **API Ergonomics**, **Initialization Safety**, and **Silent Failure Handling**. The most critical finding is the potential for silent failures when Guest requests exceed buffer limits, as the current implementation returns an empty success response instead of an explicit error.

## 2. Safety Analysis

### 2.1 C++ Host (`DirectHost.h`)

*   **Strengths:**
    *   **RAII Pattern:** `ZeroCopySlot` correctly uses `std::weak_ptr` to prevent Use-After-Free if the parent `DirectHost` is destroyed.
    *   **Bounds Checking:** `Send`, `SendToSlot`, and `ProcessGuestCalls` explicitly validate sizes against `maxReqSize`.
    *   **Integer Overflow Protection:** Safe handling of negative sizes (end-alignment) using `uint32_t` casting and absolute value logic.
    *   **Slot Recovery:** `AcquireSlot` correctly identifies and reclaims "zombie" slots (abandoned by timed-out Guests) using `activeWait` and `msgSeq`.

*   **Weaknesses:**
    *   **Raw Pointers:** The public API uses `const uint8_t* data, int32_t size`. This separates the pointer from its length, increasing the risk of user error (passing wrong size).
    *   **Initialization Race:** `Platform::CreateNamedShm` (Linux) relies on file size checks to detect existence. A race condition exists where two Hosts starting simultaneously could both attempt to initialize (memset) the same file.
    *   **Silent Failure on Guest Call:** In `ProcessGuestCalls`, if a Guest sends a request larger than `maxReqSize`, the Host resets the slot with `respSize = 0` and signals completion. The Guest interprets this as a successful 0-byte response, masking the error.

### 2.2 Go Guest (`go/direct.go`)

*   **Strengths:**
    *   **Panic Recovery:** The worker loop recovers from panics, releases the held slot (to unblock Host), and restarts.
    *   **Resource Cleanup:** `NewDirectGuest` ensures all file descriptors and event handles are closed if initialization fails mid-way.
    *   **Type Safety:** `MsgType` is a distinct type, preventing integer mix-ups.

*   **Weaknesses:**
    *   **Error Propagation:** Similar to the Host, if a handler returns a response larger than the buffer, the Guest worker returns a 0-byte response or logs an error but might not convey this clearly to the Host in all paths.

### 2.3 Protocol & Concurrency

*   **Strengths:**
    *   **Message Sequencing:** `msgSeq` robustly detects stale responses or cross-talk due to slot reclamation.
    *   **Atomic State Machine:** The `SLOT_FREE` -> `SLOT_BUSY` -> `READY` cycle is strictly enforced via CAS (Compare-And-Swap).

*   **Weaknesses:**
    *   **No Explicit Error Protocol:** The protocol lacks a standardized way to signal transport-level errors (e.g., "Buffer Too Small", "Invalid Magic") in the response channel.

## 3. Recommended Improvements

### High Priority (Robustness)

1.  **Implement `MsgType::SYSTEM_ERROR`**
    *   **Why:** To prevent silent failures when constraints are violated (e.g., request too large).
    *   **Plan:**
        *   Add `MsgType::SYSTEM_ERROR = 127` (reserved).
        *   Update `DirectHost::ProcessGuestCalls`: If validation fails, set `msgType = SYSTEM_ERROR` and `respSize` to an error code or 0.
        *   Update Go `SendGuestCall`: Check if response type is `SYSTEM_ERROR` and return a Go `error` instead of data.

2.  **Host Initialization Lock**
    *   **Why:** To prevent multiple processes from corrupting the shared memory header during startup.
    *   **Plan:** Use file locking (`flock` on Linux, `LockFile` on Windows) on the shared memory file (or a sidecar lock file) during `DirectHost::Init`.

### Medium Priority (API Safety)

3.  **C++ `std::span` Overloads (C++20) or Pointer/Size Wrappers**
    *   **Why:** To enforce coupling between data pointers and their sizes.
    *   **Plan:** Add `Send(std::span<const uint8_t> data, ...)` overloads. Since the project targets C++17, use a lightweight internal `Span` shim or just `std::string_view` (for read-only bytes).

4.  **Config Validation**
    *   **Why:** To prevent invalid configurations (e.g., 0-byte slots).
    *   **Plan:** Add strict validation in `DirectHost::Init` (e.g., `payloadSize >= 64`, `numSlots > 0`).

### Low Priority (Ergonomics)

5.  **Doxygen/GoDoc "Safety" Sections**
    *   **Why:** To inform users of thread-safety guarantees explicitly.
    *   **Plan:** Update documentation to explicitly state that `DirectHost` methods are thread-safe (due to internal locking/atomics) but `ZeroCopySlot` is not thread-safe (should be thread-local).

## 4. Next Steps

Upon approval, I recommend implementing the **High Priority** items immediately, starting with the **`MsgType::SYSTEM_ERROR`** protocol update, as silent data truncation/dropping is a critical correctness issue.
