# Bug Tracker

This file tracks identified bugs, security vulnerabilities, and code quality issues.
Each item requires user judgment before action is taken.

| ID | Severity | Issue | Description | Proposed Fix | User Judgment | Status |
|----|----------|-------|-------------|--------------|---------------|--------|
| BUG-001 | **Critical** | `ZeroCopySlot` Use-After-Free | `~ZeroCopySlot` may access `DirectHost` memory after `DirectHost::Shutdown` unmaps it, because `weak_ptr` validity check races with `~DirectHost` body. | Explicitly reset `sharedState` pointer at the start of `DirectHost::Shutdown` or `~DirectHost` to invalidate all `weak_ptr`s before unmapping memory. | Fixed | **Resolved** |
| BUG-002 | **High** | Integer Overflow in Go `SendGuestCall` | `int32(len(data))` in `SendGuestCall` will overflow if `data` > 2GB, causing negative size (end-alignment) or corruption. | Add check: `if len(data) > math.MaxInt32 { return error }`. | Fixed | **Resolved** |
| BUG-003 | **Medium** | `WaitEvent` Clock Jump Susceptibility (Linux) | `sem_timedwait` uses `CLOCK_REALTIME`. System time adjustments (NTP) can cause premature timeouts or hangs. | No easy fix with POSIX semaphores. Document risk or switch to Futex-based implementation (complex). Recommendation: Document limitation. | | Pending |
| BUG-004 | **Low** | Unsafe Integer Cast in `DirectHost::Send` | Calculation `(0u - (uint32_t)size)` relies on implicit casting behavior when handling negative `int32_t`. While strictly valid C++, it is brittle. | Use `std::abs` or explicit logical branching for clarity and safety assurance. | | Pending |
| BUG-005 | **Low** | Missing `MsgType` Validation | `ProcessGuestCalls` passes any `MsgType` (including internal protocol types) to the user handler. Malicious guests could spoof types. | Filter out internal system types (0-127) in `ProcessGuestCalls` unless explicitly allowed? Or document that handlers must validate `msgType`. | | Pending |
| BUG-006 | **Critical** | Go Guest Panic on `INT_MIN` | Host sending `ReqSize = INT_MIN` causes `rLen` overflow and panic in `workerLoop` due to slice bounds check failure. DoS vector. | Validate `rLen > 0` and `rLen <= len(buffer)` safely using `uint32` logic or explicit checks in `go/direct.go`. | Approved | **Resolved** |
| BUG-007 | **High** | C++ UB on `INT_MIN` Negation | `DirectHost::ProcessGuestCalls` performs `-reqSize` on `INT_MIN` (`-2^31`), which is Undefined Behavior in C++. | Use `(0u - (uint32_t)reqSize)` for safe negation to handle 2's complement correctly. | | Pending |
| BUG-008 | **Medium** | FD Leak on Exec (Linux) | `sem_open` and `shm_open` lack `O_CLOEXEC`, causing file descriptors to leak to child processes if `exec` is called. | Add `O_CLOEXEC` flag to `open` calls in `go/platform_linux.go`. | | Pending |
