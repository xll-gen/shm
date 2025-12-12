# AI Agent Instructions for xll-gen/shm

This file contains instructions for AI Agents working on this repository.

## **Codebase Authority**

*   **SPECIFICATION.md:** This file is the **single source of truth** for the protocol, memory layout, and architecture. Always consult `SPECIFICATION.md` before making changes to the core IPC logic.
*   **Feature Parity:** All features must be implemented in both C++ (Host) and Go (Guest). If you add a feature to one, you **must** add it to the other.
*   **Tests:** New features must include regression tests.
*   **Versioning:** Patch version updates (x.y.Z) must maintain API compatibility and memory layout (ABI) compatibility. Breaking changes require a Major version update.

## **Project Structure**

*   **include/shm/**: C++ Header-only library. **Do not create .cpp files for the library.**
*   **go.mod**: Go module definition (at root).
*   **go/**: Go library source code. **Do not use external dependencies.**
*   **benchmarks/**: Performance tests and examples.

## **Coding Standards**

*   **C++:**
    *   Header-only architecture.
    *   Use `doxygen` style comments (`/** ... */`).
    *   Strict memory alignment (cache-line friendly).
    *   **Cross-Platform:** Must compile on Linux (GCC/Clang) and Windows (MSVC 2019+). Use `Platform.h` for OS primitives.
*   **Go:**
    *   Use `cgo` only when necessary for OS primitives (e.g. `sem_open`).
    *   Use `go fmt`.
    *   Follow standard Go idioms.

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
*   All documentation, code comments, commit messages, and other project-related text must be in English.
