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

// TestAffinityMaskForSlot exercises the slot-to-CCX round-robin mapping
// and the Auto-vs-Local opt-out semantics on monolithic-L3 hosts.
func TestAffinityMaskForSlot(t *testing.T) {
	if got := affinityMaskForSlot(0, AffinityNone); got != 0 {
		t.Errorf("AffinityNone must return mask=0, got %#x", got)
	}

	masks := CcxMasks()
	if len(masks) == 0 {
		t.Skip("no L3 cache reported; cannot exercise AffinityLocal")
	}

	if len(masks) == 1 {
		// Single L3 (monolithic Intel desktop, single-CCX Zen, VM).
		if got := affinityMaskForSlot(0, AffinityAuto); got != 0 {
			t.Errorf("AffinityAuto on single-L3 host must return 0 (no coherency benefit), got %#x", got)
		}
		// Explicit AffinityLocal still pins (caller asked for it).
		if got := affinityMaskForSlot(0, AffinityLocal); got != masks[0] {
			t.Errorf("AffinityLocal on single-L3 host should still return masks[0]=%#x, got %#x", masks[0], got)
		}
		return
	}

	// Multi-L3 (chiplet AMD, multi-socket Xeon). Auto and Local both pin.
	for _, mode := range []AffinityMode{AffinityAuto, AffinityLocal} {
		// Slot 0 and slot numCCX must map to the same CCX.
		a := affinityMaskForSlot(0, mode)
		b := affinityMaskForSlot(len(masks), mode)
		if a == 0 || a != b {
			t.Errorf("%s: slot 0 and slot %d should share CCX mask; got %#x and %#x", mode, len(masks), a, b)
		}
		// Adjacent slots within numCCX should differ.
		c := affinityMaskForSlot(0, mode)
		d := affinityMaskForSlot(1, mode)
		if c == d {
			t.Errorf("%s: slot 0 and slot 1 share same CCX mask with %d CCXs — round-robin broken", mode, len(masks))
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
	}
	for _, tc := range cases {
		if got := tc.mode.String(); got != tc.want {
			t.Errorf("AffinityMode(%d).String() = %q, want %q", tc.mode, got, tc.want)
		}
	}
}
