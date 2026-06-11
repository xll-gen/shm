package shm

import "testing"

// TestOpenShm_RejectsUndersizedSection verifies the attach-time size guard:
// openShm must refuse a section smaller than the caller-expected size before
// any ExchangeHeader/slot dereference. Linux enforces this via fstat; Windows
// via VirtualQuery on the mapped view. The test is platform-agnostic — it
// creates a small section and asks openShm for a larger size.
func TestOpenShm_RejectsUndersizedSection(t *testing.T) {
	const name = "OpenShmUndersizeTest"
	const small = 4096
	const large = 1 << 20 // 1 MiB, far larger than the section

	unlinkShm(name) // best-effort: clear any stale section from a prior run

	h, addr, err := createShm(name, small)
	if err != nil {
		t.Fatalf("createShm(%d) failed: %v", small, err)
	}
	defer closeShm(h, addr, small)
	defer unlinkShm(name)

	// Negative: opening with an oversized expectation must be rejected, and the
	// failed open must not leak a mapped view (handle/addr both zero).
	oh, oaddr, oerr := openShm(name, large)
	if oerr == nil {
		closeShm(oh, oaddr, large)
		t.Fatalf("openShm accepted an undersized section (expected %d-byte view from a %d-byte section)", large, small)
	}
	if oh != 0 || oaddr != 0 {
		t.Fatalf("rejected openShm returned non-zero handle/addr: h=%d addr=%#x", oh, oaddr)
	}

	// Positive: opening with the real size must still succeed (guard must not
	// reject a correctly-sized section).
	oh2, oaddr2, oerr2 := openShm(name, small)
	if oerr2 != nil {
		t.Fatalf("openShm rejected a correctly-sized section: %v", oerr2)
	}
	closeShm(oh2, oaddr2, small)
}
