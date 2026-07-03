package shm

import (
	"runtime"
	"testing"
)

// TestCcxMasksTopology asserts the host reports at least one shared-L3
// logical-processor mask. On Ryzen / Threadripper / Epyc we expect
// multiple CCXs; on consumer Intel a single L3 typically covers the whole
// die. Either is acceptable — a true zero result only happens on very
// constrained VMs or on Windows versions that don't surface
// GetLogicalProcessorInformationEx data, and AffinityLocal degrades
// gracefully to a no-op in that case (affinityMaskForSlot returns 0,
// pinSlotWorker returns without pinning).
func TestCcxMasksTopology(t *testing.T) {
	masks := CcxMasks()
	if len(masks) == 0 {
		t.Skip("no L3 cache reported (VM or stripped Windows); affinity falls back to AffinityNone")
	}
	for i, m := range masks {
		if m == 0 {
			t.Errorf("CCX[%d] mask is zero — GetLogicalProcessorInformationEx returned empty LP set", i)
		}
	}
	t.Logf("enumerated %d CCX masks: %v", len(masks), masks)
}

// TestAffinityMaskForSlot exercises the explicit (non-Auto) CCX
// round-robin mapping. AffinityAuto's chipset-aware decision is covered
// separately by TestAffinityAutoChipsetAware below — this test forces
// AffinityLocal so the result is independent of SMT-pair availability.
func TestAffinityMaskForSlot(t *testing.T) {
	if got := affinityMaskForSlot(0, 1, AffinityNone); got != 0 {
		t.Errorf("AffinityNone must return mask=0, got %#x", got)
	}

	masks := CcxMasks()
	if len(masks) == 0 {
		t.Skip("no L3 cache reported; cannot exercise AffinityLocal")
	}

	if len(masks) == 1 {
		// Explicit AffinityLocal still pins to the lone CCX (caller asked for it).
		if got := affinityMaskForSlot(0, 1, AffinityLocal); got != masks[0] {
			t.Errorf("AffinityLocal on single-L3 host should return masks[0]=%#x, got %#x", masks[0], got)
		}
		return
	}

	// Multi-L3 (chiplet AMD, multi-socket Xeon). AffinityLocal pins.
	// Slot 0 and slot numCCX must map to the same CCX.
	a := affinityMaskForSlot(0, 1, AffinityLocal)
	b := affinityMaskForSlot(len(masks), 1, AffinityLocal)
	if a == 0 || a != b {
		t.Errorf("Local: slot 0 and slot %d should share CCX mask; got %#x and %#x", len(masks), a, b)
	}
	// Adjacent slots within numCCX should differ.
	c := affinityMaskForSlot(0, 1, AffinityLocal)
	d := affinityMaskForSlot(1, 1, AffinityLocal)
	if c == d {
		t.Errorf("Local: slot 0 and slot 1 share same CCX mask with %d CCXs — round-robin broken", len(masks))
	}
}

// TestAffinityAutoChipsetAware verifies the v0.8.2 Auto policy:
//  1. when SmtPairs() fits numSlots → resolve to Sibling
//  2. when SMT pairs are absent/insufficient and multi-CCX → resolve to Local
//  3. else → AffinityNone (no-pin)
func TestAffinityAutoChipsetAware(t *testing.T) {
	pairs := SmtPairs()
	masks := CcxMasks()

	// This test exercises the topology decision only; keep GOMAXPROCS above
	// every numSlots used below so the oversubscription gate (verified
	// separately in TestAffinityAutoOversubscriptionGate) never fires here,
	// including under `go test -cpu=1`.
	prev := runtime.GOMAXPROCS(0)
	if want := len(pairs) + 8; prev < want {
		runtime.GOMAXPROCS(want)
		defer runtime.GOMAXPROCS(prev)
	}

	if len(pairs) > 0 {
		// Case 1: numSlots <= numPairs → Sibling. Slot 0 must equal pairs[0].Guest.
		if got := affinityMaskForSlot(0, len(pairs), AffinityAuto); got != pairs[0].Guest {
			t.Errorf("Auto with %d pairs / numSlots=%d should pick Sibling guest LP %#x; got %#x",
				len(pairs), len(pairs), pairs[0].Guest, got)
		}
		if got := resolveAuto(len(pairs), AffinityAuto); got != AffinitySibling {
			t.Errorf("resolveAuto(numSlots=%d, Auto) = %v, want AffinitySibling", len(pairs), got)
		}

		// Case 2a: numSlots > numPairs → Sibling no longer safe; should
		// fall through to Local if multi-CCX, otherwise None.
		over := len(pairs) + 1
		expect := AffinityNone
		if len(masks) > 1 {
			expect = AffinityLocal
		}
		if got := resolveAuto(over, AffinityAuto); got != expect {
			t.Errorf("resolveAuto(numSlots=%d, Auto) with %d pairs %d CCXs = %v, want %v",
				over, len(pairs), len(masks), got, expect)
		}
	} else {
		// Case 2b: no SMT pairs reported. Auto → Local on multi-CCX, else None.
		expect := AffinityNone
		if len(masks) > 1 {
			expect = AffinityLocal
		}
		if got := resolveAuto(1, AffinityAuto); got != expect {
			t.Errorf("resolveAuto(numSlots=1, Auto) with 0 pairs %d CCXs = %v, want %v",
				len(masks), got, expect)
		}
	}

	// Auto must NEVER return AffinityAuto itself.
	if got := resolveAuto(8, AffinityAuto); got == AffinityAuto {
		t.Errorf("resolveAuto must reduce AffinityAuto to a concrete mode")
	}
	// Non-Auto modes pass through unchanged.
	for _, m := range []AffinityMode{AffinityNone, AffinityLocal, AffinitySibling} {
		if got := resolveAuto(8, m); got != m {
			t.Errorf("resolveAuto(numSlots=8, %v) = %v, want passthrough %v", m, got, m)
		}
	}
}

// TestAffinityAutoOversubscriptionGate verifies that AffinityAuto disables
// pinning when GOMAXPROCS is below the worker (slot) count. A pinned worker
// holds its P while spinning; under oversubscription that only adds P-level
// contention without the cache-locality payoff, so Auto must degrade to
// AffinityNone. Explicit (non-Auto) modes are the caller's responsibility and
// are NOT gated — verified at the end.
func TestAffinityAutoOversubscriptionGate(t *testing.T) {
	prev := runtime.GOMAXPROCS(0)
	defer runtime.GOMAXPROCS(prev)

	pairs := SmtPairs()
	masks := CcxMasks()

	// Pick a numSlots that Auto would pin at ample GOMAXPROCS, and that is
	// >= 2 so we can set GOMAXPROCS one below it (GOMAXPROCS(n<=0) is a no-op).
	var numSlots int
	switch {
	case len(pairs) >= 2:
		numSlots = len(pairs) // resolves to Sibling
	case len(masks) > 1:
		numSlots = 2 // resolves to Local (multi-CCX, numSlots > pairs)
	default:
		t.Skip("host offers no Auto pinning (no SMT pairs / single L3); gate is a no-op here")
	}

	// Ample Ps (GOMAXPROCS >= numSlots): Auto keeps pinning.
	runtime.GOMAXPROCS(numSlots)
	if got := resolveAuto(numSlots, AffinityAuto); got == AffinityNone {
		t.Fatalf("GOMAXPROCS=%d >= numSlots=%d: Auto should still pin, got AffinityNone", numSlots, numSlots)
	}

	// Oversubscribed (GOMAXPROCS < numSlots): Auto must back off to None.
	runtime.GOMAXPROCS(numSlots - 1)
	if got := resolveAuto(numSlots, AffinityAuto); got != AffinityNone {
		t.Errorf("GOMAXPROCS=%d < numSlots=%d: Auto must degrade to AffinityNone, got %v", numSlots-1, numSlots, got)
	}

	// Explicit modes are never gated — the caller owns thread placement.
	runtime.GOMAXPROCS(1)
	for _, m := range []AffinityMode{AffinityLocal, AffinitySibling} {
		if got := resolveAuto(numSlots, m); got != m {
			t.Errorf("explicit %v must pass through even when oversubscribed, got %v", m, got)
		}
	}
}

// TestAffinityModeString covers the diagnostic stringer for logging paths.
func TestAffinityModeString(t *testing.T) {
	cases := []struct {
		mode AffinityMode
		want string
	}{
		{AffinityAuto, "auto"},
		{AffinityNone, "none"},
		{AffinityLocal, "local"},
		{AffinitySibling, "sibling"},
	}
	for _, tc := range cases {
		if got := tc.mode.String(); got != tc.want {
			t.Errorf("AffinityMode(%d).String() = %q, want %q", tc.mode, got, tc.want)
		}
	}
}

// TestSmtPairsTopology asserts that on Windows hosts reporting SMT cores
// the enumeration produces a non-empty list of single-bit host/guest LP
// masks that point at distinct LPs of the same physical core.
func TestSmtPairsTopology(t *testing.T) {
	pairs := SmtPairs()
	if len(pairs) == 0 {
		t.Skip("no SMT pairs reported (no-SMT CPU, hybrid E-only, or stripped Windows); AffinitySibling degrades to no-pin")
	}
	for i, p := range pairs {
		if p.Host == 0 || p.Guest == 0 {
			t.Errorf("pair[%d] has zero mask (host=%#x guest=%#x)", i, p.Host, p.Guest)
		}
		if p.Host == p.Guest {
			t.Errorf("pair[%d] host and guest masks collide (%#x) — would put both endpoints on the same LP, defeating sibling-mode invariant", i, p.Host)
		}
		if p.Host&p.Guest != 0 {
			t.Errorf("pair[%d] host (%#x) and guest (%#x) overlap", i, p.Host, p.Guest)
		}
		if popcount64(p.Host) != 1 || popcount64(p.Guest) != 1 {
			t.Errorf("pair[%d] masks must be single-bit (host=%#x guest=%#x)", i, p.Host, p.Guest)
		}
	}
	t.Logf("enumerated %d SMT pairs: %v", len(pairs), pairs)
}

// TestAffinityMaskForSlotSibling verifies that sibling mode returns the
// guest LP of the round-robin-selected pair and falls back to 0 when the
// host reports no SMT pairs.
func TestAffinityMaskForSlotSibling(t *testing.T) {
	pairs := SmtPairs()
	if len(pairs) == 0 {
		if got := affinityMaskForSlot(0, 1, AffinitySibling); got != 0 {
			t.Errorf("AffinitySibling with no SMT pairs must return 0, got %#x", got)
		}
		t.Skip("no SMT pairs reported; sibling mode degrades to no-pin (verified) — skipping round-robin assertions")
	}

	// Slot 0 and slot len(pairs) must map to the same guest LP.
	a := affinityMaskForSlot(0, 1, AffinitySibling)
	b := affinityMaskForSlot(len(pairs), 1, AffinitySibling)
	if a == 0 || a != b {
		t.Errorf("sibling: slot 0 and slot %d should share guest LP; got %#x and %#x", len(pairs), a, b)
	}
	if a != pairs[0].Guest {
		t.Errorf("sibling: slot 0 guest LP = %#x, want pairs[0].Guest = %#x", a, pairs[0].Guest)
	}
	// Adjacent slots must differ when multiple pairs exist.
	if len(pairs) > 1 {
		if c := affinityMaskForSlot(0, 1, AffinitySibling); c == affinityMaskForSlot(1, 1, AffinitySibling) {
			t.Errorf("sibling: slot 0 and slot 1 share guest LP with %d pairs — round-robin broken", len(pairs))
		}
	}
}
