# AI Agent Instructions for xll-gen/shm

This file contains instructions for AI Agents working on this repository.

## **Codebase Authority**

*   **SPECIFICATION.md:** This file is the **single source of truth** for the protocol, memory layout, and architecture. Always consult `SPECIFICATION.md` before making changes to the core IPC logic.
*   **Feature Parity:** All features must be implemented in both C++ (Host) and Go (Guest). If you add a feature to one, you **must** add it to the other.
*   **Tests:** New features must include regression tests.

## **Project Structure**

*   **include/shm/**: C++ Header-only library. **Do not create .cpp files for the library.**
*   **go/**: Go library. **Do not use external dependencies (go.mod should be minimal).**
*   **benchmarks/**: Performance tests and examples.

## **Coding Standards**

*   **C++:**
    *   Header-only architecture.
    *   Use `doxygen` style comments (`/** ... */`).
    *   Strict memory alignment (cache-line friendly).
*   **Go:**
    *   Use `cgo` only when necessary for OS primitives (e.g. `sem_open`).
    *   Use `go fmt`.
    *   Follow standard Go idioms.

## **Verification**

*   **Byte Alignment:** When modifying structs, verify that C++ and Go structs match exactly in size and padding.
*   **Benchmarks:** Run `task run:benchmark` to verify performance regressions.
*   **Pre-commit:** Always verify your changes with `read_file` or `ls` before submitting.

## **General Rules**

*   **Do not** delete this file.
*   **Do not** create `src/` directory for C++.
*   **Do not** use Queue-based logic (SPSC/MPSC); strictly use the Direct Exchange (Slot) model defined in `SPECIFICATION.md`.
