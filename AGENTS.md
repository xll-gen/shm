# AI Agent Instructions for xll-gen/shm

This file contains instructions for AI Agents working on this repository.

## **Codebase Authority**

*   **SPECIFICATION.md:** This file is the **single source of truth** for the protocol, memory layout, and architecture. Always consult `SPECIFICATION.md` before making changes to the core IPC logic.
*   **Feature Parity:** All features must be implemented in both C++ (Host) and Go (Guest). If you add a feature to one, you **must** add it to the other.
*   **Tests:** New features must include regression tests.
*   **Versioning:** Patch version updates (x.y.Z) must maintain API compatibility and memory layout (ABI) compatibility. Breaking changes require a Major version update.

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
