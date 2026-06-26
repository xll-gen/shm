package shm

import (
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

// TestAffinityMaskForSlot exercises the slot-to-CCX round-robin mapping.
// Two slots `k` apart (where k = numCCX) land on the same mask; any
// degenerate case (AffinityNone, no CCXs) returns 0.
func TestAffinityMaskForSlot(t *testing.T) {
	if got := affinityMaskForSlot(0, AffinityNone); got != 0 {
		t.Errorf("AffinityNone must return mask=0, got %#x", got)
	}

	masks := CcxMasks()
	if len(masks) == 0 {
		t.Skip("no L3 cache reported; cannot exercise AffinityLocal")
	}

	// Slot 0 and slot numCCX must map to the same CCX.
	a := affinityMaskForSlot(0, AffinityLocal)
	b := affinityMaskForSlot(len(masks), AffinityLocal)
	if a == 0 || a != b {
		t.Errorf("slot 0 and slot %d should share CCX mask; got %#x and %#x", len(masks), a, b)
	}

	// Adjacent slots within numCCX should differ (when numCCX > 1).
	if len(masks) > 1 {
		c := affinityMaskForSlot(0, AffinityLocal)
		d := affinityMaskForSlot(1, AffinityLocal)
		if c == d {
			t.Errorf("slot 0 and slot 1 share same CCX mask under AffinityLocal with %d CCXs — round-robin broken", len(masks))
		}
	}
}

// TestAffinityModeString covers the diagnostic stringer for logging paths.
func TestAffinityModeString(t *testing.T) {
	if AffinityNone.String() != "none" {
		t.Errorf("AffinityNone.String() = %q, want \"none\"", AffinityNone.String())
	}
	if AffinityLocal.String() != "local" {
		t.Errorf("AffinityLocal.String() = %q, want \"local\"", AffinityLocal.String())
	}
}
