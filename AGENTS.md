# AI Agent Instructions for xll-gen/shm

This file contains instructions for AI Agents working on this repository.

## **Companion Repos**

This module is one of four coordinated repos. When work crosses boundaries, read the relevant `AGENTS.md`:

* **`github.com/xll-gen/xll-gen`** — XLL generator; consumes this module via `pkg/server` and via the C++ `FetchContent` block.
* **`github.com/xll-gen/types`** — FlatBuffers protocol schema shared with `xll-gen`. Message framing/IDs live there; transport (this repo) must remain protocol-agnostic.
* **`github.com/xll-gen/sugar`** — unrelated Excel COM library; not in the runtime path.

This repo's contract is **transport only**: never embed protocol-specific assumptions (e.g., message IDs from `types`) into `shm`.

## **Platform Targets**

* **Sole deployment target**: Windows x86 / x86-64 (Intel/AMD), as the production consumer is `xll-gen`, which is Windows-only (see `xll-gen/AGENTS.md` §0.1). On x86/x64, TSO hardware ordering covers most of the acquire-release contract; correctness MUST still be implemented via proper atomic ops (no `relaxed` on synchronizing operations).
* **Library compile targets**: Windows only — MSVC 2019+ and MinGW (GCC). This repo is **Windows-only**; there is no Linux/POSIX support. Use `Platform.h` for all OS primitives (Win32 Events, file mappings).
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
    *   **Windows-only:** Must compile on Windows with MSVC 2019+ and MinGW (GCC). There is no Linux/POSIX support. Use `Platform.h` for OS primitives.
*   **Go:**
    *   **No cgo.** OS primitives are accessed via `syscall` against `kernel32.dll` (see `go/platform_windows.go`). The Go side is pure Go + `syscall`.
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
**Per-field offset guards (R25)**: Beyond the total-size `static_assert`s, every named field of `SlotHeader`/`ExchangeHeader` now has a compile-time offset guard on **both** sides — C++ `static_assert(offsetof(...) == N)` (needs `<cstddef>`) in `IPCUtils.h`, and Go dual `unsafe.Offsetof` zero-array guards in `direct.go`. The Go guard is bidirectional (`[N - off]byte` *and* `[off - N]byte`) so moving a field either direction fails the build. A field reorder that preserves total size (e.g. swapping two same-width fields) slips past a size-only assert but is caught here. **When you add/move/resize a field, update the per-field offset asserts on both sides in the same commit** — they are the SSOT enforcement, with `SPECIFICATION.md §2.1/§2.2.1` carrying the human-readable offset columns.

### **Platform Primitives**
Synchronization primitives must behave identically across languages and OSs.
1.  **C++ Interface**: `include/shm/Platform.h`.
2.  **Go Implementation**: `go/platform.go` (shared dispatcher/doc comments), `go/platform_windows.go` (Win32 `syscall` implementation).
**Constraint**: If you add a feature (e.g., timeout) to C++, you must implement it in Go.

### **Feature Parity (Host <-> Guest)**
Logic changes often require symmetric updates.
1.  **Host Logic**: `include/shm/DirectHost.h` (e.g., `Send`, `ProcessGuestCalls`).
2.  **Guest Logic**: `go/direct.go` / `go/client.go` (e.g., `workerLoop`, `SendGuestCall`).
**Constraint**: A new message type or flow (e.g., Streaming) must be supported on both sides.

## **Confirmed-Correct Decisions (Do NOT Change)**

Synced from the workspace `IMPROVEMENT_BACKLOG.md` §6 (2026-06-12). These were
flagged by past reviews and confirmed correct — do not "fix" or re-propose:

* `go/direct.go` — the size/seq **re-validation after the `state` acquire-load**
  is a cache-visibility safety net, not redundancy. Do not remove it.
* The `state` store/load pair keeps **release/acquire** semantics — never
  "optimize" to `relaxed`. x86 TSO makes relaxed *appear* to work (see the
  deployment-target note above); the contract is codified in
  SPECIFICATION.md §4.4.
* The **header-only** C++ library structure is a design decision (see Project
  Structure) — no `.cpp` files for library logic.

## **Known Improvement Backlog**

From a code review on 2026-05-16. Address as part of normal work.

* **DONE (v0.6.4):** `go/client.go::Start()` now returns `error` instead of `panic("Handler not set")`. Callers that ignored the void return keep working (Go allows it); callers that want graceful handling can now assign and inspect the err.
* **MOSTLY DONE — v0.7.0 (lease) + v0.7.1 (reclamation API)**: Crash-time slot cleanup.
  * **v0.7.0 (DONE):** Carved `atomic<uint64> lease` from `SlotHeader::reserved[36]` at offset 96 (with 4 bytes of natural alignment padding before it). Total size still 128. Both C++ and Go sides write `Platform::MonotonicNanos()` / `shm.MonotonicNanos()` (wall-clock ns since Unix epoch) immediately after every CAS that claims a slot. Protocol version bumped to `0x00070000`. See `SPECIFICATION.md §2.2.1` (updated layout) and §3.6 (semantics).
  * **v0.7.1 (DONE):** `DirectHost::TryReclaimAbandonedSlot(slotIdx, maxLeaseAgeNs)` (C++) and `(*DirectGuest).TryReclaimAbandonedSlot(slotIdx, maxLeaseAge)` (Go). Opt-in API: caller's watchdog invokes it. CAS-guarded against live heartbeat races. `lease == 0` refused (v0.6.x peer signal). Property test (`go/reclaim_test.go::TestTryReclaim_NoDoubleClaim_Property`) drives 200 rounds × 4 slots concurrent heartbeater-vs-reclaimer and asserts the no-double-claim invariant.
  * **v0.7.2 (DONE):** Auto-reclamation integration. `DirectHost::SetAutoReclaimTimeoutNs` / `(*DirectGuest).SetAutoReclaimTimeout` enable the natural "I need a slot but none are free" code paths to walk all slots and call `TryReclaimAbandonedSlot` before failing. Zero (default) disables entirely so existing deployments see no behavior change.
  * **v0.7.3 (DONE):** End-to-end crash-process test. `go/reclaim_crash_test.go::TestCrashProcess_ReclaimAfterChildExit` re-execs the test binary with `SHM_CRASH_TEST_WORKER=1` — the child opens the parent's shm, CAS-claims the guest slot, writes Lease, then `os.Exit(0)` without releasing. Parent waits for child to exit, sleeps past the threshold, calls `TryReclaimAbandonedSlot`, asserts the slot returns to `SlotFree`. Validates the kernel-tracked cross-process lifecycle, not just in-process CAS races. Runs in ~0.5 s.

  The v0.7-series crash-recovery feature is **feature-complete**: lease (v0.7.0) + opt-in reclamation API (v0.7.1) + auto-reclaim integration (v0.7.2) + cross-process verification (v0.7.3).
* **OBSOLETE (was v0.6.5, removed when the repo went Windows-only):** Linux semaphore lifetime work. The POSIX named-semaphore backend (`go/platform_linux.go`), its regression test (`go/sem_lifetime_linux_test.go`), and the `/dev/shm/sem.*` leak concern no longer exist. `UnlinkEvent`/`UnlinkShm` (Go) and `Platform::UnlinkNamedEvent`/`Platform::UnlinkShm` (C++) are retained as effective no-ops on Windows for API symmetry and teardown calls — Windows kernel objects are reference-counted, so there is nothing to unlink.
* **DONE (v0.6.0):** Doc comments on `MsgType` constants — every entry in `go/direct.go:36–63` now has a `// MsgFoo ...` line and the C++ side mirrors it in `MsgType.h` Doxygen.
* **DONE (v0.6.5):** Nested IPC deadlock detector — `DirectHost::AcquireSlot` (C++, `SHM_DEBUG` only) counts full slot sweeps; after 10 000 fruitless sweeps it emits a one-shot `SHM_LOG_WARN` pointing at the README's "Nested IPC & Recursion" guidance (`numHostSlots >= N_threads × (Depth + 1)`). Production builds without `SHM_DEBUG` keep the old spin-forever semantics, so behavior is identical when compiled out.

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

### **Completed (post-v0.7.7)**

* **R25 (2026-06-18):** Per-field offset guards added for `SlotHeader`/`ExchangeHeader` on both C++ (`offsetof` `static_assert`) and Go (`unsafe.Offsetof` dual zero-array) sides — see the Protocol & Memory Layout constraint above. Catches a size-preserving field reorder that the existing size-only asserts miss.
* **spin.go benchstats gate (2026-06-19):** The `WaitStatsSpinSuccess`/`SleepFallback`/`IterCount` counters were `atomic.Add`-ed on every `Wait`/`WaitState` in the production hot path. The writes now route through `recordSpinSuccess`/`recordSleepFallback`/`recordIters`, which are no-ops unless built with `-tags shm_benchstats` (`spin_stats_on.go` / `spin_stats_off.go`); the counters stay declared so the benchmark module always compiles. The 5 benchmark build scripts (`run.ps1`/`run.sh`/`run_profile.sh`/`run_stream.sh`/`harness.ps1`) now pass the tag to keep reporting real numbers. Production build performs zero atomic adds on those three counters.

## **CLAUDE.md / Agent Tool Compatibility**

This repository is configured so that AI tools using `CLAUDE.md` (Claude Code) read this `AGENTS.md` as the authoritative source. **All durable agent guidance must live here, not in `CLAUDE.md`.** `CLAUDE.md`, if present, must contain only a one-line redirect to this file.
