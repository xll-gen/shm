//go:build !amd64

package shm

import "sync/atomic"

// spinUntilEq32 is the portable fallback for the asm primitive in
// spin_amd64.s. The runtime target is windows/amd64 only, so this exists
// purely so `go build` on other architectures (CI containers, dev laptops)
// stays green.
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
