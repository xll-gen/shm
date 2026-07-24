//go:build !amd64

package shm

import "sync/atomic"

// spinUntilEq32 is the portable fallback for the asm primitive in
// spin_amd64.s. The runtime target is windows/amd64 only (see AGENTS.md
// "Platform Targets"); this file is now UNREACHABLE by any buildable target:
// windows/386 is rejected at compile time by platform_unsupported_arch.go, and
// non-Windows builds fail earlier in platform.go (the Win32 syscall primitives
// have no non-Windows definition). It is retained solely so that the
// windows/386 build fails with the SINGLE clear diagnostic from that guard
// file, instead of cascading "undefined: spinUntilEq32 / cpuPause" errors that
// would bury the intended message. Do not rely on this fallback for a real
// cross-arch build — there is no such supported build.
func spinUntilEq32(addr *uint32, want uint32, max uintptr) (uintptr, bool) {
	for i := uintptr(0); i < max; i++ {
		if atomic.LoadUint32(addr) == want {
			return i, true
		}
	}
	return max, false
}

// cpuPause is a no-op on non-amd64 builds (the amd64 version emits PAUSE).
// Build-portability fallback only; the runtime target is windows/amd64.
func cpuPause() {}
