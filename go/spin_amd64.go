//go:build amd64

package shm

// spinUntilEq32 spins emitting x86 PAUSE until *addr == want, or max
// iterations elapse. Returns (iters consumed, observed). Implementation in
// spin_amd64.s — see that file for the loop body. The whole loop runs in
// assembly: no Go-side per-iteration CALL, no TLS reload, no spill traffic.
//
// PAUSE itself takes 30–140 cycles on modern x86; the inter-iteration cost
// is otherwise a single uncached load + compare + branch.
func spinUntilEq32(addr *uint32, want uint32, max uintptr) (iters uintptr, ok bool)
