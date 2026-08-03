# AI Agent Instructions for xll-gen/shm

This file contains instructions for AI Agents working on this repository.

## **Companion Repos**

This module is one of four coordinated repos. When work crosses boundaries, read the relevant `AGENTS.md`:

* **`github.com/xll-gen/xll-gen`** — XLL generator; consumes this module via `pkg/server` and via the C++ `FetchContent` block.
* **`github.com/xll-gen/types`** — FlatBuffers protocol schema shared with `xll-gen`. Message framing/IDs live there; transport (this repo) must remain protocol-agnostic.
* **`github.com/xll-gen/sugar`** — unrelated Excel COM library; not in the runtime path.

This repo's contract is **transport only**: never embed protocol-specific assumptions (e.g., message IDs from `types`) into `shm`.

## **Platform Targets**

* **Sole deployment target**: Windows **x86-64 (amd64)** on Intel/AMD, as the production consumer is `xll-gen`, which is Windows/x64-only (see `xll-gen/AGENTS.md` §0.1). **32-bit (x86 / GOARCH=386) is NOT supported** — the Go guest's Win32 struct layouts in `go/platform_windows.go` are hand-laid for the amd64 ABI, so `windows/386` is rejected at compile time (`go/platform_unsupported_arch.go`). On amd64, TSO hardware ordering covers most of the acquire-release contract; correctness MUST still be implemented via proper atomic ops (no `relaxed` on synchronizing operations).
* **Library compile targets**: Windows only — MSVC 2019+ and MinGW (GCC). This repo is **Windows-only**; there is no Linux/POSIX support. Use `Platform.h` for all OS primitives (Win32 Events, file mappings).
* **No ARM support**: neither Windows-on-ARM nor Apple Silicon is a target. Cache-line sizes other than 64 bytes are out of scope for tuning.
* **Single-architecture matching at runtime**: both the C++ Host and the Go Guest MUST be **windows/amd64 (64-bit)**. This is the only supported bitness — there is no 32-bit build of either side. Cross-arch / mixed-bitness IPC is not supported (and 32-bit is not buildable; see above).

## **Affinity Recommendations**

`ClientConfig.AffinityMode` (Go) / `--affinity` (bench binaries) controls
worker placement. The default `AffinityAuto` is **chipset-aware** as of
v0.8.2: it inspects the host topology at startup and picks the most
specific mode the hardware supports.

**`AffinityAuto` resolution order** (see `affinity.go::resolveAuto`):

1. If LTP_PC_SMT pairs are reported AND `numSlots <= len(SmtPairs())` →
   `AffinitySibling`. Each slot N pins to one SMT LP of physical core
   `N % numPairs`; the matching C++ host pins to the other LP, putting
   both endpoints on shared L1d/L2.
2. Else if `len(CcxMasks()) > 1` → `AffinityLocal` (CCX-wide mask). Covers
   chiplets where slot count exceeds SMT-pair count, plus multi-socket
   Xeon without exposed LTP_PC_SMT.
3. Else → `AffinityNone` (no-pin). Monolithic-L3 hosts, no-SMT CPUs, and
   constrained VMs land here.

The 2026-06-26 SMT-sibling A/B on Ryzen 9 3900X (see `EXPERIMENTS.md`
§"2026-06-26 SMT-sibling co-location" and the SMT-sibling table in
`BENCHMARK_RESULTS.md`) showed `AffinitySibling` beats `AffinityLocal`
by **+24 % to +74 %** across 1/4/8 threads × 64 B / 1024 B payloads. The
win comes from two effects: shared-L1d coherency on the SlotHeader line,
and *deterministic non-collision placement* of host and guest LPs (CCX-wide
masks let the OS scheduler occasionally co-locate both endpoints on the
same LP, periodically stalling the spin).

**Operator guidance** for explicit overrides:

* **Chiplet AMD (Ryzen / Threadripper / Epyc)** — `AffinityAuto` already
  picks `AffinitySibling` for typical slot counts. Verify on the *actual
  deployment host* before relying on the magnitude of the measured win.
* **Monolithic-L3 Intel single-socket with SMT** — `AffinityAuto` picks
  `AffinitySibling` if `numSlots <= numPairs`. Unmeasured on this family;
  the non-collision effect should still help even without a cross-CCD
  bounce to eliminate. If you observe regression, set
  `AffinityMode = AffinityNone` to opt out.
* **Hybrid Intel (Alder Lake P+E)** — `enumerateSmtPairs` skips E-cores
  (no LTP_PC_SMT). `AffinityAuto` picks `AffinitySibling` only when
  `numSlots <= numPCorePairs`; otherwise it falls through to no-pin
  (monolithic L3). Measure if your workload exceeds P-core count.
* **Constrained VMs / hosts without LTP_PC_SMT topology** —
  `AffinityAuto` degrades to either `AffinityLocal` (multi-CCX) or
  `AffinityNone`. No worker is ever pinned to a single LP without a
  documented sibling, so the §Exp 5 starvation pattern is avoided by
  construction.

## **Codebase Authority**

*   **SPECIFICATION.md:** This file is the **single source of truth** for the protocol, memory layout, and architecture. Always consult `SPECIFICATION.md` before making changes to the core IPC logic.
*   **Feature Parity:** All features must be implemented in both C++ (Host) and Go (Guest). If you add a feature to one, you **must** add it to the other.
*   **Tests:** New features must include regression tests.
*   **Versioning:** Patch version updates (x.y.Z) must maintain API compatibility and memory layout (ABI) compatibility. Breaking changes require a Major version update — on the 0.x line the minor slot plays that role (0.8.x → 0.9.0), since 0.y.z has no major slot to move.
    *   **`VERSION` is the release-version SSOT and the only place the number is written by hand.** `CMakeLists.txt` does `file(READ VERSION)` → `string(STRIP)` → `project(shm VERSION ...)`, so the CMake package version derives from it and cannot drift. Nothing else in the repo may restate the release version as a literal; a literal elsewhere is a bug to delete or to derive, not a second copy to maintain.
    *   **Bump `VERSION` in the release commit, before the tag** (see §Tag Message Guidelines). It went stale once and the failure mode is instructive: `v0.8.21` was tagged while `VERSION` still read `0.8.20`, so the CMake package version of the *tagged* tree was wrong, and the next release found three disagreeing numbers (`VERSION` 0.8.20, newest tag v0.8.21, `CHANGELOG.md` head 0.9.0). Resolved at the `0x00080000` wire bump by setting `VERSION` to **`0.9.0`**: a breaking wire change cannot be a patch bump, and on the 0.x line the minor slot is the major slot, so 0.8.x → 0.9.0 regardless of which 0.8.z the file happened to be sitting on. Do not "fix" a stale `VERSION` by retro-editing it to the last tag — jump it forward to the release being prepared.
*   **Wire version is separate from the release version.** `SHM_VERSION` (C++ `include/shm/IPCUtils.h`, Go `shm.Version` in `go/direct.go`) is **`0x00080000`** as of the stream-header bump; it was pinned at `0x00070000` across the whole v0.7.x–v0.8.x line. The two constants MUST be edited in the same commit — a mismatch is not a soft degradation, it is a total attach failure (`protocol version mismatch`), so a half-applied bump takes the product down rather than producing a subtle bug. **That MUST is enforced by a regression pair, not by discipline:** `tests/test_go_version_parity.cpp` and `go/version_wire_guard_test.go`, each reading the *other* language's source as data so neither can self-disable on a toolchain-less build. See §Co-Change Clusters → Protocol & Memory Layout for why the pre-existing tests could not see a half-applied bump (they were green through one). Bump it only when the *bytes* change meaning; a field carved from never-written `reserved` space does not qualify (that is why `lease`, `gen` and `fastPathAllowed` did not bump it), but giving meaning to a reserved field whose legacy zero value is **indistinguishable from a legitimate value** does — see `SPECIFICATION.md` §2.1 "Version compatibility is a HARD FAILURE at attach".

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
**Stream headers are the same cluster**: `StreamHeader`/`ChunkHeader` live in `include/shm/Stream.h` (C++) and `go/stream.go` (Go), are both **24 bytes**, and since `ChunkHeader.reserved@16` became `dstOffset` and `StreamHeader.reserved@20` became `appMsgType` at `SHM_VERSION 0x00080000` both carry per-field offset guards on **both** sides, field for field:

* **C++** (`include/shm/Stream.h`): `static_assert(sizeof(...) == 24)` per struct plus a `static_assert(offsetof(...) == N)` for every named field — `StreamHeader` `streamId@0`/`totalSize@8`/`totalChunks@16`/`appMsgType@20` (**four**) and `ChunkHeader` `streamId@0`/`chunkIndex@8`/`payloadSize@12`/`dstOffset@16`/`padding@20` (**five**).
* **Go** (`go/stream.go`): the original `var(...)` block of dual zero-array `unsafe.Sizeof` asserts (24 bytes, both structs, both directions), plus a **second** `var(...)` block of **nine bidirectional `unsafe.Offsetof` guards** — `[N - off]byte` *and* `[off - N]byte` per field — covering the same nine fields as the C++ list above.

`dstOffset` is read by the peer, so a field reorder here is a silent stream corruption, not a compile error, without those guards. `padding@20` is guarded on both sides even though nothing reads it, because the concrete way this table changes is a widening of `dstOffset`, which moves it. The senders (`StreamSender` on both sides) and the reassemblers (`include/shm/StreamReassembler.h`, `go/stream.go`) are part of the same co-change set: a sender that stops writing absolute offsets makes every receiver drop the stream. `SPECIFICATION.md` §3.3.2 carries the same two lists as normative text — update both when you add, move or resize a field.

**The `SHM_VERSION` / `shm.Version` pair has a cross-language regression guard, and it is a pair on purpose.** The two constants (`include/shm/IPCUtils.h`, `go/direct.go`) must be edited in the same commit; the guard that enforces it is `tests/test_go_version_parity.cpp` (a C++ test that parses `go/direct.go` as data — the path is injected as `SHM_GO_DIRECT_GO` by `tests/CMakeLists.txt`, and a missing definition is `#error`) together with `go/version_wire_guard_test.go` (a Go test that parses `include/shm/IPCUtils.h`). Neither needs the other language's toolchain, which is the whole design: during the `0x00080000` work the bump WAS half-applied — `IPCUtils.h` at `0x00080000`, `go/direct.go` still at `0x00070000` — and **both suites reported green**, because `tests/test_protocol_version.cpp` only compares a C++ constant against the value C++ itself wrote into the segment (tautological across the boundary), and `tests/test_stream_integration.cpp` — the only test that runs Go — returns CTest's `SKIP_RETURN_CODE` whenever `find_program(NAMES go)` misses, i.e. on the ordinary C++-only build. Both halves are written so that *every* way of failing to reach a value (file missing, no declaration, more than one, unparseable literal) is a hard FAIL and never a skip. **If you rename or reshape either constant's declaration, fix the regexes in the same commit** — the failure they exist to prevent is precisely "the guard quietly stops running", and both are written to fail loudly rather than tolerate a parse miss, so a rename breaks the build instead of disarming it. See `SPECIFICATION.md` §2.1.

**Per-field offset guards (R25)**: Beyond the total-size `static_assert`s, every named field of `SlotHeader`/`ExchangeHeader` now has a compile-time offset guard on **both** sides — C++ `static_assert(offsetof(...) == N)` (needs `<cstddef>`) in `IPCUtils.h`, and Go dual `unsafe.Offsetof` zero-array guards in `direct.go`. The Go guard is bidirectional (`[N - off]byte` *and* `[off - N]byte`) so moving a field either direction fails the build. A field reorder that preserves total size (e.g. swapping two same-width fields) slips past a size-only assert but is caught here. **When you add/move/resize a field, update the per-field offset asserts on both sides in the same commit** — they are the SSOT enforcement, with `SPECIFICATION.md §2.1/§2.2.1` carrying the human-readable offset columns.

### **Platform Primitives**
Synchronization primitives must behave identically across languages and OSs.
1.  **C++ Interface**: `include/shm/Platform.h`.
2.  **Go Implementation**: `go/platform.go` (shared dispatcher/doc comments), `go/platform_windows.go` (Win32 `syscall` implementation).
**Constraint**: If you add a feature (e.g., timeout) to C++, you must implement it in Go.

**One host per SHM name — the guard has two halves (2026-08-02).** `Platform::LockShm` holds a `Local\<name>_lock` named mutex, which enforces the rule ACROSS PROCESSES correctly (two Excel instances on the same project: the second host's `Init` is refused with `ResourceExhausted`). It cannot enforce it WITHIN a process: a Win32 mutex is owned by a *thread* and is *recursive*, so a second `Init` for the same name on the same thread finds `ERROR_ALREADY_EXISTS`, gets `WAIT_OBJECT_0` from the zero-timeout wait (that thread already owns it) and "acquires" the lock — after which `DirectHost::Init` runs `memset(shmBase, 0, totalSize)` over the LIVE host's segment. So `LockShm` also keeps a process-local registry of claimed names and refuses a duplicate before touching the mutex; `UnlockShm` drops the claim (keyed by handle, so the signature is unchanged). `tests/test_double_host.cpp` pins this and had been failing since it was written — the contract it asserts is real, the implementation was half-missing. **No Go parity work**: the Go side has no production host, only test mocks — nothing there creates and locks a segment as host.

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
* **`SLOT_GUEST_BUSY` stays in the zombie-steal set** (`SlotAllocator::tryClaimSlot`,
  the `{SLOT_REQ_READY, SLOT_RESP_READY, SLOT_GUEST_BUSY}` branch). A 2026-07-29
  review proposed removing it to fix the responder header-write defect; the
  proposal was **rejected**. `SPECIFICATION.md` §3.4 codifies stealability of
  `SLOT_GUEST_BUSY` as a *design premise* — it is what makes the responder's
  consume-claim CAS an ownership token rather than reclaim machinery — and
  narrowing the set would change Host slot-recovery semantics. The real defect
  was narrower (writing header fields before proving ownership) and is fixed by
  the two-phase publish below. Do not re-propose narrowing the steal set.
* **`SLOT_DONE = 3` is the responder's publish lock, no longer "reserved".** Both
  sides produce and consume it: a responder that owns a slot CAS's
  `owned → SLOT_DONE`, writes `respSize`/`msgType`, then CAS's
  `SLOT_DONE → SLOT_RESP_READY` (seq_cst, for the §4.2 Dekker). It works as a
  lock *only* because it is outside the zombie-steal set — do not add it there,
  and do not "simplify" the two phases back into one. See `SPECIFICATION.md`
  §3.1/§3.4/§3.5 and the note below on the historical v0.6.0 annotation.
* **The stream reassemblers' received-byte counters are the §3.3.4 memory bound,
  not bookkeeping** (2026-07-29). Go `streamContext.oooBytes` + `fits()`
  (`go/stream.go`) and C++ `StreamContext::oooBytes` + `Fits()`
  (`include/shm/StreamReassembler.h`) exist so the per-chunk overflow guard
  covers chunks that arrive **ahead of the in-order cursor**. Before they
  existed, only the in-order branch consulted `totalSize`, so an untrusted peer
  could advertise `totalSize = 1`, park unbounded bytes under out-of-order
  indices, and only be refused once the gap filled — after every byte was
  resident. Three parts are load-bearing and must move together: (a) the check
  runs **before** the payload write, not after; (b) it counts ahead-of-cursor
  bytes, so `offset + oooBytes` is the running received length; (c) the drain
  loop releases `oooBytes` for a parked index **before** accounting it in order,
  keeping the pair indivisible. Do not
  "simplify" the guard back onto the in-order path only, and do not move it after
  the write "since we reject anyway" — the point is *when* the rejection happens.
  Since `SHM_VERSION 0x00080000` an ahead-of-cursor chunk's bytes are already in
  the destination buffer, so `oooBytes` now counts *placed-but-not-accounted*
  bytes rather than parked copies; the counter is no less necessary — it is still
  the only thing that makes `offset` alone not understate the received length.
  **Note on (c), updated at `0x00080000`:** its old justification was "or a
  well-formed out-of-order stream is rejected as over-sized", which described a
  drain-time re-check of the byte bound. **That re-check no longer exists** — the
  drain only re-verifies `dstOffset` against the cursor (SPEC §3.3.4 "Where each
  guard fires", step 8), so an inverted order is no longer *caught* anywhere and
  (c) now rests on the `offset + oooBytes` invariant alone. That is the weaker of
  the two positions, so treat the release-then-account pair as one indivisible
  step, and do not read the absence of a drain-time byte check as licence to
  reorder it. Corollary for the accept/reject table: a drain-time rejection is
  **always** a misplacement and **never** a byte overflow; a byte overflow can
  only be raised at the arriving chunk's own check.
  Regression: `go/stream_ooo_guard_test.go`,
  `tests/test_reassembler_ooo_overflow.cpp`.
* **Stream reassembly state is sized by `totalSize`, never by `totalChunks`, and
  the parked-chunk count is capped** (2026-07-29, SPEC §3.3.4 "Residency bound" /
  "Parked-chunk bound"). `totalChunks` and `totalSize` are independent header
  fields, so any per-chunk array — the C++ reassembler's old
  `chunks.resize(totalChunks)` + `seen.resize(totalChunks)` — costs
  `O(totalChunks)` for a stream that advertises one byte: **measured 25.4 MB per
  stream** at `totalChunks = 2^20`, ~26 GB at `maxStreams`. The byte guard above
  cannot see that dimension (the overhead exists at zero payload, and a
  zero-length chunk can never trip a `Σ payloadSize > totalSize` test). Both
  peers now preallocate a `totalSize` destination, write every chunk straight
  into it at its `dstOffset`, keep only a sparse payload-free record for
  ahead-of-cursor indices, and refuse a park
  past `maxParkedChunks` / `MaxParkedChunks` (1024 =
  `maxStreamChunks / maxStreams`; keep the two sides equal — it is an
  accept/reject parity value, not a tuning knob). Do **not** reintroduce a
  chunk-indexed array, and do **not** assemble into a second full-size buffer at
  completion — that was a 2× peak (2 GiB for a 1 GiB stream) the Go side never
  had. Regression: `tests/test_reassembler_tiny_stream_footprint.cpp`,
  `tests/test_reassembler_completion_peak.cpp`,
  `tests/test_reassembler_park_cap.cpp`, `go/stream_park_cap_test.go`.
* **`tests/test_reassembler_parity_table.cpp` and
  `go/stream_parity_table_test.go` are one artifact** (2026-07-29). The case list
  *and* the expected digest strings are duplicated verbatim so each side asserts
  against a constant rather than against the other implementation's current
  behavior. They pin what the §3.3.4 prose cannot state chunk-by-chunk: which
  rejections drop the stream vs. leave it completable, first-arrival-wins dedup
  (including a duplicate that declares a different `payloadSize` **or
  `dstOffset`** — the dedup gate runs ahead of every guard), `STREAM_START`
  replacing a live context, and — added at `SHM_VERSION 0x00080000` — the
  `dstOffset` **drop** cases: an out-of-bounds offset, an in-bounds offset with an
  overhanging payload (on both the in-order and the ahead-of-cursor path), the
  32-bit wrap offsets that make the subtraction form of the bounds test
  load-bearing, a zero-length chunk at an out-of-bounds offset, and a cursor
  mismatch caught at *either* detection site (the in-order arrival and the drain).
  A digest change is a **wire behavior change**: update SPEC §3.3.4 and both
  halves in the same commit, or don't make it.

  **What the parity table canNOT see: placement.** This is a correction of an
  earlier version of this bullet, which claimed the table pins `dstOffset` as
  "honoured as an absolute destination on reverse-order delivery of unequal chunk
  sizes". **That claim is false, and the reason is structural rather than a gap in
  the case list — do not try to fix it by adding cases.** Placement check 2
  (SPEC §3.3.4) forces `dstOffset == the running cursor` for every index at the
  moment that index becomes in-order, on every stream that is not dropped.
  Therefore a `dstOffset`-honouring receiver and a receiver that ignores
  `dstOffset` entirely and appends at its own cursor write every byte to the same
  address, accept and reject exactly the same messages, and deliver
  byte-identical output — for every stream, not merely for the cases listed.
  **No output-byte test can distinguish them.** (That is also why all 20 legacy
  digests survived the `0x00080000` rework unchanged.) Verified, not reasoned:
  reimplementing the pre-bump receiver — park a payload copy under the
  out-of-order index, drain in index order, append at the cursor, never consult
  `dstOffset` for placement — leaves **all 29 parity cases and the whole
  `AppMsgTypeIsInertToReassembly` replay PASSING**, and flips exactly four tests
  red out of the entire Go suite:
  `TestStreamReassembly_AheadOfCursorChunkIsResidentAtDstOffset`
  ("destination buffer holds \"\\x00\\x00\\x00\" at dstOffset 3 while the stream is
  incomplete, want \"CCC\""),
  `TestStreamReassembly_ParkedChunksAreAllResidentBeforeTheGapFills`,
  `TestStreamReassembly_ParkedRecordHoldsNoPayload` ("parked record is 40 bytes,
  want <= 16") and
  `TestStreamReassembly_ParkedResidencyIndependentOfChunkSize`. Those four are the
  whole guard.

  So the guard set splits cleanly, and each half must be maintained on its own:
  * The **parity table** guards the accept/reject contract — every `dstOffset`
    row in it that distinguishes anything is a *drop* case. Those genuinely
    distinguish: a receiver that skips the bounds or the cursor-consistency check
    changes digests. The one non-drop row,
    `nonuniform_offsets_reverse_completes`, is the case that was believed to pin
    placement; it does not, and it is kept only as an accept-path smoke case.
  * **Placement and the zero-copy property** are guarded ONLY by the residency
    and early-placement tests — on the Go side
    `go/stream_dstoffset_residency_test.go`
    (`TestStreamReassembly_ParkedRecordHoldsNoPayload` asserts the parked record
    holds no payload bytes and no reference to any;
    `TestStreamReassembly_ParkedResidencyIndependentOfChunkSize` asserts parked
    residency does not grow with chunk size) together with the early-placement
    tests in `go/stream_dstoffset_placement_test.go`
    (`TestStreamReassembly_AheadOfCursorChunkIsResidentAtDstOffset`,
    `TestStreamReassembly_ParkedChunksAreAllResidentBeforeTheGapFills`), which
    inspect the destination buffer *before* the in-order cursor reaches the
    chunk's index and so assert the positive form of the contract — the bytes are
    at `buf[dstOffset]` on arrival — rather than the absence of a second copy.
    On the C++ side the mirror is
    `tests/test_reassembler_dstoffset_placement.cpp`.
    Deleting or weakening any of those leaves placement **unguarded**, and the
    parity table will stay green while it rots.

    **Parity status of that pair on the C++ side — the two halves differ, do not
    treat them as one gap.** Both Go tests above are Go-only as *tests*, but only
    one of the two properties they assert is unguarded in C++:

    * **The "no parked payload copy" half is guarded, and better than in Go — at
      compile time.** `include/shm/StreamReassembler.h` carries
      `static_assert(sizeof(StreamContext::Placed) == 16)` and
      `static_assert(std::is_trivially_copyable<StreamContext::Placed>::value)`.
      `Placed` is `{uint64_t dstOffset; uint32_t size;}` + tail padding, so every
      route back to a parked copy — an owning `std::vector`/`std::string` member,
      or an inline byte array — is a **build failure**, not a test failure. That is
      strictly stronger than Go's `TestStreamReassembly_ParkedRecordHoldsNoPayload`,
      which can only notice a copy once it clears the test's byte slack. Treat the
      two asserts as the C++ half of the residency pair and do not delete them as
      "redundant with the parity table" — the parity table cannot see this either.
      A field added to `Placed` is a deliberate act: update the number **and**
      SPEC §3.3.4, or do not add it.
    * **The early-placement half — "the bytes are at `buf[dstOffset]` BEFORE the
      in-order cursor reaches that index" — is guarded by
      `tests/test_reassembler_dstoffset_placement.cpp`**, which reads the
      destination buffer while the stream is still incomplete via the
      `StreamReassembler::PeekStream` / `StreamSnapshot` seam (added for this;
      `StreamContext` and `Placed` stay private, and the snapshot copies plain
      values out under `streamsMutex`). A `static_assert` cannot supply this half:
      a C++ regression that keeps `Placed` payload-free — asserts still satisfied —
      but writes each payload at its own running cursor instead of at `dstOffset`
      is invisible to the parity digests by the structural argument above, and the
      residency asserts are about the record's *type*, not about where bytes land.
      That test is the **only** ctest guard for it; confirmed by mutation twice, at
      the `0.9.0` cycle: writing at `ctx.offset` instead of `header->dstOffset`,
      and an honest revert to the pre-`0x00080000` parking shape (which deletes
      `Placed` and both static_asserts with it). Under either mutant
      `test_reassembler_contract`, `test_reassembler_parity_table`,
      `test_reassembler_ooo_overflow` and `test_reassembler_park_cap` all stay
      GREEN. Do **not** delete it as "redundant with the static_asserts" — they
      cover the parked-copy half only — and do **not** re-widen this bullet back to
      "C++ placement is guarded by nothing"; both halves are now guarded.

  Corollary for future work: if you change how chunks are placed, the parity
  digests staying identical is **expected and is not evidence the change is
  correct**. Reach for the residency/early-placement tests instead.
* **`ChunkHeader.dstOffset` is load-bearing; `StreamHeader.appMsgType` is
  delivered but INERT to reassembly (`SHM_VERSION 0x00080000` + receive-side
  delivery).** These two formerly-reserved
  slots were claimed in one bump, on purpose, but they are *not* symmetric.
  `dstOffset` is the absolute destination of the payload and both reassemblers
  place every chunk by it on arrival — the receiver enforces
  `dstOffset + payloadSize <= totalSize` (by subtraction; `dstOffset` is
  peer-controlled and the sum would wrap) and that the offset equals the running
  cursor when its index becomes in-order, dropping the stream on either failure.
  `appMsgType` is an application routing tag that shm never interprets. It was
  WRITE-ONLY for one release — senders wrote it, both reassemblers parsed past it,
  and nothing could read it. **That is closed** (SPEC §3.3.1 "Receive-side
  delivery"): both peers now hand the tag to the completion callback through a
  **parallel, additive** entry point — Go `NewStreamReassemblerTyped` /
  `TypedStreamHandler`, C++ the `StreamReassembler(OnStreamTypedFn, cfg)`
  constructor overload — while `NewStreamReassembler` / `StreamReassembler(OnStreamFn, cfg)`
  keep their signatures and are *defined as* the typed form with the tag dropped,
  so there is one reassembly implementation, not two. It cost **no `SHM_VERSION`
  bump**, which is exactly what claiming the slot at `0x00080000` bought.
  **"Additive" has one C++ exception — do not restate it as "purely additive".**
  Adding a constructor overload made arguments convertible to BOTH function types
  ambiguous: `StreamReassembler r(nullptr)`, `r({})`, `r([](auto&&...){})` compiled
  before 0.9.1 and no longer do (verified by compiling one TU against the old and
  new headers; g++ 14.2 `-std=c++17`). No caller here is affected and the failure is
  always a compile error, never a behavior change, but the doc line must not claim
  otherwise. Recorded on the `OnStreamFn` constructor in `include/shm/StreamReassembler.h`
  and in SPEC §3.3.1 obligation 1. Go has no such exception (new name, no overloading).
  Delivering the tag is **not** licence to branch on it: nothing in reassembly may
  read `appMsgType`, and if you make it a guard, a bound or a completion test, the
  peers stop agreeing on accept/reject.
  The field now has **three** guards, and they assert different things — keep all
  three; none implies another:
  * *Receive side (negative):* `TestStreamReassembly_AppMsgTypeIsInertToReassembly`
    (`go/stream_parity_table_test.go`) and its C++ twin at the end of
    `tests/test_reassembler_parity_table.cpp` replay every parity case through the
    UNTYPED entry point with the tag set to garbage and require byte-identical
    digests. This one is a *negative* assertion by construction: it was green
    through the entire write-only period and would be green against a reassembler
    that never touched offset 20.
  * *Receive side (positive):* `go/stream_apptype_delivery_test.go` and
    `tests/test_reassembler_appmsgtype_delivery.cpp` — one artifact, same cases on
    both sides. They are the ONLY thing that can fail for "the tag never reaches
    the handler". Non-vacuity is designed in and confirmed by mutation (deliver a
    constant; read `totalChunks@16` instead of `appMsgType@20`; keep a
    reassembler-wide "last tag seen"): every mutant flips these red while
    `test_reassembler_parity_table`, `test_reassembler_dstoffset_placement` and
    `test_reassembler_contract` all stay GREEN. Three properties are load-bearing
    and must survive any edit — a tag value that collides with no other header
    field, a case asserting a **zero** tag on a header whose every other field is
    nonzero, and a case interleaving two streams with different tags that complete
    in the reverse of their start order.
  * *Send side (positive):* `go/stream_sender_appmsgtype_test.go`
    (`TestStreamSender_WritesAppMsgTypeAtWireOffset20`,
    `TestStreamSender_DefaultAppMsgTypeIsZero`). Every *other* `appMsgType` test
    hand-builds the `StreamHeader` bytes, so none of them touches
    `StreamSender.SetAppMsgType` or `putStreamHeader` — the tag could have been
    dropped, written at the wrong offset, or written into the `ChunkHeader`
    instead, and the whole suite would still have been green. These two drive the
    exported sender against a real segment and read the bytes back off the wire.
    They stop at the wire and say nothing about delivery.
* **Reserved and padding bytes MUST reach the wire as zero** (SPEC §3.3.2
  "Reserved and padding space"). This is not hygiene: every field this protocol
  added *without* a version bump — `lease`, `gen`, `fastPathAllowed` — was safe to
  carve only because an older peer had written `0` into that space, which is what
  lets a new reader tell "peer does not implement this" from a real value. A sender
  that leaks uninitialised bytes forfeits that carve-out. `ChunkHeader.padding@20`
  is the **last** reserved slot in `ChunkHeader` as of `0x00080000`, so it is the
  only place in that struct the carve-out could ever be used again. Composing a
  stream header on the stack and `memcpy`'ing it whole is the way this goes wrong —
  the receiver never looks at the field, so nothing fails, and host stack bytes
  ship. Receivers, symmetrically, MUST NOT read, validate, or branch on those
  bytes.
  Both senders now discharge this explicitly — C++ value-initializes
  (`ChunkHeader ch{}` / `StreamHeader header{}` in `include/shm/Stream.h`, *not*
  `ChunkHeader ch;`), Go's `putChunkHeader` writes a literal `0`.
  Regression: `tests/test_stream_chunk_padding.cpp`, which reads `padding@20` out
  of the real sender's `STREAM_CHUNK` request bytes. **Read its failability caveat
  before trusting a green run**: uninitialised stack is very often incidentally
  zero, so the test scribbles the frame the sender's `Send` will occupy — and that
  is best-effort and toolchain-dependent by its own measurement (it reports
  `0xdededede` for a default-initialized `ChunkHeader` under MinGW/GCC 14 but
  PASSES under MSVC 19.41 `/O2`, where `Send` is inlined and the scribbled frame is
  never reused). It is a bug detector on one toolchain, not a guarantee on any. The
  value-initialization is the contract; the test is a tripwire for removing it.
* **`maxStreamSize` = 1 GiB is a wire ceiling, not a tuning knob, and it binds the
  SENDER too** (SPEC §3.3.2 / §3.3.4). `ChunkHeader.dstOffset` is `uint32`, so the
  bound is what makes the field wide enough at all, *and* it is an accept/reject
  parity value. It is the one row of the §3.3.4 bounds table that MUST NOT be
  raised: an implementation exposing it as mutable must refuse or clamp a larger
  value (C++ does — `shm::kMaxStreamSize`, `StreamReassemblerConfig::IsValid()`, a
  `static_assert` on the default, and a constructor clamp; Go's `MaxStreamSize` is
  a `const`). A **sender** MUST refuse a stream whose offsets it cannot express
  rather than truncate `dstOffset` into 32 bits — a truncated offset is a
  legal-looking value, so relying on the peer's `STREAM_START` bound is not
  equivalent. Do not "make maxStreamSize configurable for large transfers";
  raising it past 4 GiB requires widening `dstOffset`, which moves `padding@20` and
  is its own `SHM_VERSION` bump.
  Both peers implement the sender obligation: C++ `StreamSender::Send` and Go
  `sendStart` (`go/stream_sender.go`), each refusing before a slot is acquired.
  Regression: `tests/test_stream_size_ceiling.cpp` covers **both** obligations —
  the config half (`IsValid()` reports an over-ceiling value; the constructor clamp
  is observable through the new `StreamReassembler::Config()` accessor) and the
  sender half (`StreamSender::Send` → `Error::InvalidArgs`);
  `go/stream_sender_size_ceiling_test.go` covers the Go sender half
  (`TestStreamSender_RefusesStreamAboveWireCeiling` asserts no request reaches the
  host and the guest slot is left untouched;
  `TestStreamSender_AcceptsStreamAtWireCeilingBoundary` pins the comparison as
  strict `>`, so an exactly-`MaxStreamSize` stream is not refused). Note that the sender's
  ceiling check is deliberately placed **before** any slot acquisition and before
  `data` is dereferenced; that ordering is what lets the test reach the guard
  without committing a 1 GiB buffer, so moving the check later breaks the test for
  a reason that will not be obvious from the failure. There is deliberately **one**
  ceiling constant with one meaning — do not add a second, looser
  "physically expressible in a uint32" (4 GiB) limit anywhere: enforcing 4 GiB
  would leave the 1–4 GiB accept/reject parity break, which is the defect this
  closed.
* `go/race_annotate_on.go` / `race_annotate_off.go` are **not dead code and not
  synchronization** (2026-07-26). Slot ownership is handed over through the
  atomics on `SlotHeader.State`, which lives in the file-mapped region. Go's
  race detector silently drops every acquire/release annotation *and* every
  access instrumentation whose address falls outside the Go arena and data
  segment (`runtime.isvalidaddr`, `racecalladdr<>` in `race_amd64.s`) — a
  `MapViewOfFile` region is in neither. So every shm-mediated handoff is
  invisible to tsan, and the next owner's perfectly legal use of
  `slotContext.nextMsgSeq` gets reported as a data race. That is a detector
  blind spot, **not** a Go-memory-model race: `sync/atomic` is sequentially
  consistent, so the `State` CAS pair is a genuine release/acquire edge.
  `releaseSlotHandoff`/`acquireSlotHandoff` re-publish that edge on a Go-heap
  address at the real handoff points; `//go:build !race` compiles them to empty
  inlined functions, so production builds are unaffected. **Do not delete them
  and do not "fix" the underlying race by making `nextMsgSeq` atomic** — the
  counter is not the problem. Placement is load-bearing: release goes
  immediately *before* the state store that publishes the slot as claimable and
  only from a goroutine that actually owns it (a forfeited owner publishes
  nothing, so a genuine use-after-forfeit still trips the detector); acquire
  goes immediately *after* the claiming CAS.

### The guest-call wait gate has exactly ONE implementation (2026-08-03)

`GuestCallWorker::WaitForRequest(timeoutMs)` is the single implementation of the
SPECIFICATION.md §3.5 doorbell gate: the `anyRequestReady` seq_cst predicate, the
`HOST_STATE_WAITING → recheck → WaitEvent → ACTIVE` Dekker, and the spin phase.
`GuestWorkerLoop` calls it; it does not carry its own copy. **Do not fork it for a new
caller — add a parameter.** A second copy of a Dekker is how a lost wakeup ships, and
this one is load-bearing for the +836% the worker/doorbell pair bought (EXPERIMENTS.md,
2026-07-04 round 5).

`WakeWaiter()` / `DirectHost::WakeGuestCallWaiter()` release a parked waiter. `Stop()`
calls it before joining: without that the join blocks for the full park quantum (1 s)
on every shutdown of an idle host, because a flag flip is invisible to a thread blocked
in `WaitForSingleObject`.

**WHY THE PUBLIC `DirectHost::WaitForGuestCall` EXISTS, and it is not a convenience.**
`hostState` on guest slots is written ONLY from inside this wait. A host that runs its
own loop instead of `Start()` therefore leaves it `HOST_STATE_ACTIVE` forever, the Go
sender's `if hostState == HOST_STATE_WAITING` doorbell **never fires**, and a
hand-rolled wait in that loop expires on its own timeout on every single call. That is
why such hosts have had to burn a core spinning — the spin was not merely wasteful, it
was the only thing keeping latency at ~150 ns. xll-gen's XLL worker is the live case.

Single-waiter only: one thread at a time (the shared request event is auto-reset, so two
waiters would race for one wake). Do not combine with `Start()`.

The two fallback paths that have no event to park on (no guest slots; no shared request
event) poll at `kFallbackPollMs = 100`, **not** at the caller's timeout — on those paths
the timeout is a poll interval, and honouring a 1 s park would make both request latency
and shutdown latency 1 s, a 10x regression against the pre-extraction loop.

Wire-neutral: no byte, no layout, `SHM_VERSION` untouched. Guest-call A/B (medians of 3,
baseline worktree, same session): 1T +2.2~2.3%, multi-thread cells all within their own
spread. See EXPERIMENTS.md for why the single-shot run of that A/B was misleading.

### Over-defensive-logic audit (2026-06-25)

A cross-repo audit looked for guards that are redundant because a caller already
guarantees a safe/complete value. **One** was removed (v0.7.10): the dead
`len(reqBuf) < 24` re-check in `StreamSender.Send`'s chunk goroutine
(`go/stream_sender.go`), subsumed by the preceding
`len(reqBuf) < chunkHeaderSize+len(chunkSlice)` check on the same un-resliced
internal slice. The following look similar but are **load-bearing — do NOT
remove**:

* The `len(reqBuf) < 24` / size guard on the **exported `StreamStart` path** —
  it sits on the public API surface and the C++↔Go shared-memory **wire
  boundary**. Local provability does not license removal here.
* `HandleAck`'s `len(chunkData) == 0` guard — internally redundant given
  `GetNextChunk`'s return contract, but kept for its **exported wire-dispatch**
  posture (it validates a value that, on the dispatch path, originates from peer
  bytes).
* `GuestWorkerLoop`'s `numSlots+numGuestSlots <= slots.size()` bounds conjunct —
  `DirectHost::Init` sets the counts *before* the fallible `resize`, leaving a
  real partial-Init window; the check guards a raw `operator[]` over
  independently-mutable public members. Keep it.

**Rule of thumb.** Two ordered length checks on the *same un-resliced* buffer
collapse: if an earlier guard tests `len(buf) < K + n` (`n >= 0`, `K` a
compile-time-pinned constant) a later `len(buf) < K` is dead. But a guard is
safe to delete *only* when it lives entirely inside one trust domain over an
internally-produced value — never on an exported API, the C++↔Go SHM wire, or
raw peer bytes, and never when it doubles as a behavior-selecting branch. Prefer
**compile-time size asserts** (the dual `_ [K - sizeof] / [sizeof - K]` pattern)
over runtime size re-checks.

### Performance ideas already tried and REFUTED — do not re-propose

Migrated 2026-07-26 from the workspace `IMPROVEMENT_BACKLOG.md` §6, which is
being retired. Every entry below was **measured**, not reasoned about, and every
one is the kind of plausible-sounding optimization a performance sweep will
propose again. Re-proposing one costs a benchmark cycle to re-refute, so read
this list before opening any throughput work. Where a refutation is scoped to
this host's topology, that scope is stated — those may be revisited on genuinely
different hardware, but only with numbers.

* **Time-based calibration of the spin window** — refuted twice (2026-06-25 VM,
  2026-06-26 native Windows). A 200 µs target cost **−65% throughput**; a 3 ms
  target merely matched the legacy behavior (zero gain). Re-measured natively on
  a Ryzen 9 3900X: still no throughput gain. The iteration-count window stays.
* **`PAUSE` on every spin iteration is deliberate policy, not an oversight**
  (2026-06-26 native sweep). Four variants were benchmarked — PAUSE every
  iteration, every 2, every 4, and none. Best case was ≤ +3% throughput, and the
  1024 B single-thread cell **regressed −16%**. The user's standing decision is
  that HT/SMT cache-contention hygiene outweighs that margin, so
  `go/spin_amd64.s` keeps PAUSE on every iteration. Do not "optimize" it away.
* **Single-thread RTT has a ~447 ns floor here; no single change buys >30%**
  (2026-06-26, third multi-agent RTT decomposition). A frequently-cited "0.04 µs"
  figure is measurement-resolution noise — the real RTT is 1/2.24M ≈ 447 ns, of
  which roughly 100–160 ns is cross-CCD cache-line bounce, a physical property of
  the Ryzen 9 3900X. Treat proposals that promise a large 1T RTT win as suspect
  until they account for that floor.
* **Stream `maxInFlight` pipelining — no dramatic gain on a single memory
  controller** (2026-06-26, re-confirmed 2026-07-09). Hypothesis was +150%;
  measured +8% at 16 MiB down to −31% at 1 MiB, and under co-location every
  depth-2 cell came out −56% or worse. The library default is in-flight 1.
  Scope: single-memory-controller hosts. A NUMA/multi-controller topology is the
  one place this is worth re-measuring.
* **`reqOffset = 64` slot geometry — refuted** (2026-07-04), scoped to this host
  with the Sibling affinity default. Sibling affinity places both endpoints on
  the same physical core sharing L1d, so separating the header and data lines
  saves a cross-core transfer that never happens; every default harness cell
  estimated ~0%. Reconsider only for a non-SMT or deliberately remote placement.
* **Large-page (`SEC_LARGE_PAGES`) stream mapping — refuted** (2026-07-04),
  scoped to this host. The stream plateau is a memory-controller bandwidth
  bottleneck (established by the `maxInFlight` experiment above), not a TLB
  bottleneck, and `SeLockMemoryPrivilege` carries a real deployment burden.
  Revisit only on a NUMA/multi-controller topology.

**Consumer fact, not a perf item (recorded 2026-08-03):** the Excel host does
**not** adopt held slots. xll-gen's UDF hot path deliberately stays on a per-call
`GetZeroCopySlot()`; the full reasoning and the two re-proposal conditions live in
`xll-gen/AGENTS.md` → *Confirmed-Correct Decisions*. Two consequences for work in
this repo. (1) **This is not a deprecation** — `HeldSlot`, `SendHeld`,
`AcquireHeldSlot`/`TryAcquireHeldSlot` stay exactly as they are, they are the
intended API for tight-loop low-RTT **non-Excel** hosts, and `tests/test_held_slot.cpp`
plus the harness held cells remain the contract. Do not "clean up" or narrow the
held path on the theory that nobody uses it. (2) **Do not price a proposal against
an Excel consumer that will not exist** — a design whose payoff assumes "the XLL
holds a slot per calculation thread" is computing its benefit against a
configuration that was declined. **One recorded proposal has already broken on
exactly that** (2026-07-25, *"ZeroCopySlot/HeldSlot lifetime-tracking: split the
global `shared_ptr` control block per slot"*): its affinity premise fails in the
`numHostSlots` < Excel-calc-thread-count configuration, because
`SlotAllocator.h:285-295`'s `thread_local` cache falls back to the shared
`nextSlot` round-robin on a failed claim. Two neighbouring held-path proposals from
the same day — conditional removal of the `SendHeld` per-send lease heartbeat, and
an in-place held response accessor — were refuted on **different** grounds
(SPEC §3.6 `SPECIFICATION.md:300` makes the per-send refresh normative, and
`TryReclaimAbandonedSlot` treats every `state != SLOT_FREE` slot as a candidate);
do not fold them into this one, and do not re-refute them with this argument.

**Working rule, not a perf item:** `gofmt -l` flags a large number of files in
these repos because every existing file is CRLF. That is noise, not a formatting
defect. Format only the files you actually modified; never bulk-reformat.

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
* `SLOT_DONE = 3` annotated as reserved-for-future-extensions in C++, Go, and SPECIFICATION.md §3.1. **Superseded (2026-07-29):** `SLOT_DONE` is now the responder's publish-lock state — see "Confirmed-Correct Decisions" above and SPECIFICATION.md §3.1/§3.4/§3.5. The constant is no longer reserved.
* CHANGELOG.md v0.6.0 entry augmented.
* ChunkHeader C++ side padded to 24 bytes — cross-language parity restored. static_assert added. SPECIFICATION.md §3.3.2 was already canonical.

### **Completed (post-v0.7.7)**

* **R25 (2026-06-18):** Per-field offset guards added for `SlotHeader`/`ExchangeHeader` on both C++ (`offsetof` `static_assert`) and Go (`unsafe.Offsetof` dual zero-array) sides — see the Protocol & Memory Layout constraint above. Catches a size-preserving field reorder that the existing size-only asserts miss.
* **spin.go benchstats gate (2026-06-19):** The `WaitStatsSpinSuccess`/`SleepFallback`/`IterCount` counters were `atomic.Add`-ed on every `Wait`/`WaitState` in the production hot path. The writes now route through `recordSpinSuccess`/`recordSleepFallback`/`recordIters`, which are no-ops unless built with `-tags shm_benchstats` (`spin_stats_on.go` / `spin_stats_off.go`); the counters stay declared so the benchmark module always compiles. The 5 benchmark build scripts (`run.ps1`/`run.sh`/`run_profile.sh`/`run_stream.sh`/`harness.ps1`) now pass the tag to keep reporting real numbers. Production build performs zero atomic adds on those three counters.

## **CLAUDE.md / Agent Tool Compatibility**

This repository is configured so that AI tools using `CLAUDE.md` (Claude Code) read this `AGENTS.md` as the authoritative source. **All durable agent guidance must live here, not in `CLAUDE.md`.** `CLAUDE.md`, if present, must contain only a one-line redirect to this file.
