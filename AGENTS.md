# AI Agent Instructions for xll-gen/shm

This file contains instructions for AI Agents working on this repository.

## **Companion Repos**

This module is one of four coordinated repos. When work crosses boundaries, read the relevant `AGENTS.md`:

* **`github.com/xll-gen/xll-gen`** — XLL generator; consumes this module via `pkg/server` and via the C++ `FetchContent` block.
* **`github.com/xll-gen/types`** — FlatBuffers protocol schema shared with `xll-gen`. Message framing/IDs live there; transport (this repo) must remain protocol-agnostic.
* **`github.com/xll-gen/sugar`** — unrelated Excel COM library; not in the runtime path.

This repo's contract is **transport only**: never embed protocol-specific assumptions (e.g., message IDs from `types`) into `shm`.

## **Platform Targets**

* **Primary deployment target**: Windows x86 / x86-64 (Intel/AMD), as the production consumer is `xll-gen`, which is Windows-only (see `xll-gen/AGENTS.md` §0.1). On x86/x64, TSO hardware ordering covers most of the acquire-release contract; correctness MUST still be implemented via proper atomic ops (no `relaxed` on synchronizing operations).
* **Library compile targets**: Linux (GCC/Clang) and Windows (MSVC 2019+) per the Coding Standards section below. Linux support exists for developer convenience, CI, and potential reuse — NOT as an `xll-gen` production deployment target.
* **No ARM support**: neither Windows-on-ARM nor Apple Silicon is a target. Cache-line sizes other than 64 bytes are out of scope for tuning.
* **Single-architecture matching at runtime**: C++ Host and Go Guest in a given deployment MUST run on the same architecture and bitness. Cross-arch IPC is not supported.

## **Codebase Authority**

*   **SPECIFICATION.md:** This file is the **single source of truth** for the protocol, memory layout, and architecture. Always consult `SPECIFICATION.md` before making changes to the core IPC logic.
*   **Feature Parity:** All features must be implemented in both C++ (Host) and Go (Guest). If you add a feature to one, you **must** add it to the other.
*   **Tests:** New features must include regression tests.
*   **Versioning:** Patch version updates (x.y.Z) must maintain API compatibility and memory layout (ABI) compatibility. Breaking changes require a Major version update.

## **Development Environment**

*   **Task Runner:** For better Developer Experience (DX), install `go-task` (Taskfile runner) at the start of work.
    *   Command: `go install github.com/go-task/task/v3/cmd/task@latest`
    *   Run `task --list` to check available tasks.

## **Project Structure**

*   **include/shm/**: C++ Header-only library. **Do not create .cpp files for the library logic.** Implementation goes in `.h` files.
*   **go.mod**: Go module definition (at root).
*   **go/**: Go library source code. **Do not use external dependencies.**
*   **benchmarks/**: Performance tests and examples.
*   **tests/**: Regression and unit tests.

## **Coding Standards**

*   **C++:**
    *   **Header-only architecture.**
    *   Use `doxygen` style comments (`/** ... */`).
    *   Strict memory alignment (cache-line friendly).
    *   **Cross-Platform:** Must compile on Linux (GCC/Clang) and Windows (MSVC 2019+). Use `Platform.h` for OS primitives.
*   **Go:**
    *   Use `cgo` only when necessary for OS primitives (e.g. `sem_open`).
    *   Use `go fmt`.
    *   Follow standard Go idioms.

*   **Comments:**
    *   **Self-Documenting Code:** Avoid verbose or redundant comments if the code itself is self-explanatory.
    *   **No "Chatty" Comments:** Remove comments that merely repeat what the code does (e.g., `// Increment i` before `i++`).
    *   **Doc Comments:** Maintain Public API documentation (Doxygen/GoDoc), but keep them concise.

## **Verification**

*   **Byte Alignment:** When modifying structs, verify that C++ and Go structs match exactly in size and padding.
*   **Benchmarks:** Run `task run:benchmark` to verify performance regressions.
*   **Pre-commit:** Always verify your changes with `read_file` or `ls` before submitting.

## **Tag Message Guidelines**

When creating a release tag, follow these steps:

1.  **Update the `VERSION` file:** Before creating the tag, ensure the `VERSION` file at the root of the repository is updated to the new version number.

2.  **Format the tag message:** The message must follow this format:
    *   **Summary of Major Changes:**
        Group significant changes by category (e.g., Performance, Features, Bug Fixes).
        *   **Category Name**:
            *   Description of change ([commit_hash](https://github.com/xll-gen/shm/commit/commit_hash))

    *   **Full Changelog:**
        List all commits included in the release, referencing the commit hash with a hyperlink.
        *   [commit_hash](https://github.com/xll-gen/shm/commit/commit_hash) Commit message

## **General Rules**

*   **Do not** delete this file.
*   **Do not** create `src/` directory for C++.
*   **Do not** use Queue-based logic (SPSC/MPSC); strictly use the Direct Exchange (Slot) model defined in `SPECIFICATION.md`.
*   **Streaming:** For large data transfer, use the Streaming API (`shm::StreamSender` / `shm.NewStreamReassembler`) which implements double-buffering over the Direct Exchange model.
*   **Git Workflow:** Always resolve conflicts with the `main` branch before submitting.
*   All documentation, code comments, commit messages, and other project-related text must be in English.

## **Co-Change Clusters**

Certain parts of the codebase are tightly coupled and must be updated together to preserve consistency.

### **Protocol & Memory Layout**
The Shared Memory layout is the contract between C++ (Host) and Go (Guest).
1.  **Specification**: `SPECIFICATION.md` is the Single Source of Truth. Update this first.
2.  **C++ Headers**: `include/shm/IPCUtils.h` (Structs like `ExchangeHeader`, `SlotHeader`).
3.  **Go Types**: `go/direct.go` (Structs like `ExchangeHeader`, `SlotHeader`).
**Constraint**: These must be byte-compatible. Check alignment and padding carefully.

### **Platform Primitives**
Synchronization primitives must behave identically across languages and OSs.
1.  **C++ Interface**: `include/shm/Platform.h`.
2.  **Go Implementation**: `go/platform.go`, `go/platform_linux.go`, `go/platform_windows.go`.
**Constraint**: If you add a feature (e.g., timeout) to C++, you must implement it in Go.

### **Feature Parity (Host <-> Guest)**
Logic changes often require symmetric updates.
1.  **Host Logic**: `include/shm/DirectHost.h` (e.g., `Send`, `ProcessGuestCalls`).
2.  **Guest Logic**: `go/direct.go` / `go/client.go` (e.g., `workerLoop`, `SendGuestCall`).
**Constraint**: A new message type or flow (e.g., Streaming) must be supported on both sides.

## **Known Improvement Backlog**

From a code review on 2026-05-16. Address as part of normal work.

* **`go/client.go` `Start()`**: replaces `panic("Handler not set")` with a returned error. Production libraries should not panic on a recoverable misconfiguration. Document in the same change whether `Connect()` can validate the handler eagerly.
* **Crash-time slot cleanup**: a host or guest crashing mid-exchange leaves slots in `SlotGuestBusy` / `SlotReqReady` forever. Add an optional liveness/lease field to `SlotHeader` (next minor version, ABI-bumped) so the surviving side can reclaim abandoned slots after a configurable timeout. Update `SPECIFICATION.md` in lockstep. *(2026-05-16 partial: `ProtocolViolation` error paths in `DirectHost.h` — `ZeroCopySlot::Send`, `ZeroCopySlot::SendFlatBuffer`, `WaitForSlot`, `SendAcquired` — now release the slot back to `SLOT_FREE` before returning, so the four msgSeq-mismatch sites no longer leak slots into the zombie path. Process-crash cleanup still needs the lease field.)*
* **Linux semaphore lifetime**: `platform_linux.go` uses CGO `sem_open`. POSIX semaphores persist until explicitly `sem_unlink`'d. Document the cleanup responsibility (host-side `UnlinkEvent` on graceful shutdown) and add a regression test that runs N times and asserts no `/dev/shm/sem.*` leak.
* **Doc comments on `MsgType` constants** (`go/direct.go:12–61`): each one needs a `// MsgFoo ...` line so godoc surfaces them. Mirror in Doxygen for C++.
* **Nested IPC deadlock note**: currently only in README. Add a runtime assert (debug-mode) in `DirectGuest.SendGuestCall` that detects re-entrant calls without sufficient free slots and fast-fails instead of deadlocking.

### **Completed in v0.6.0 (2026-05-16 audit follow-up)**

The shm-protocol-guardian audit findings of 2026-05-16 have been addressed:

* SPECIFICATION.md §2.1 `version` field corrected to `0x00060000`.
* `MSG_TYPE_SYSTEM_ERROR` (127) documented in SPECIFICATION.md §3.2.
* `alignas(64)` added to C++ `SlotHeader`; `static_assert`s added for `SlotHeader` (128 bytes), `ExchangeHeader` (64 bytes), and `StreamHeader` (24 bytes).
* Go-side compile-time size assertions added for `SlotHeader`, `ExchangeHeader`, `StreamHeader`, `ChunkHeader`.
* SPECIFICATION.md §4.4 "Memory Ordering Contract" added — codifies release/acquire pairing on the `state` field.
* `SLOT_DONE = 3` annotated as reserved-for-future-extensions in C++, Go, and SPECIFICATION.md §3.1.
* CHANGELOG.md v0.6.0 entry augmented.
* ChunkHeader C++ side padded to 24 bytes — cross-language parity restored. static_assert added. SPECIFICATION.md §3.3.2 was already canonical.

## **CLAUDE.md / Agent Tool Compatibility**

This repository is configured so that AI tools using `CLAUDE.md` (Claude Code) read this `AGENTS.md` as the authoritative source. **All durable agent guidance must live here, not in `CLAUDE.md`.** `CLAUDE.md`, if present, must contain only a one-line redirect to this file.
