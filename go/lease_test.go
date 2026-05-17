package shm

import (
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// TestSlotHeader_LeaseOffset locks down the wire offset of the lease field
// at 96 bytes — see SPECIFICATION.md §2.2.1. The compile-time size assert
// (in direct.go) already guarantees total size == 128; this asserts the
// position of Lease specifically so the C++ and Go sides stay in lockstep
// on heartbeat reads/writes.
func TestSlotHeader_LeaseOffset(t *testing.T) {
	var h SlotHeader
	off := unsafe.Offsetof(h.Lease)
	if off != 96 {
		t.Fatalf("SlotHeader.Lease offset = %d, want 96 (ABI break vs C++ Platform::MonotonicNanos write site)", off)
	}
}

// TestMonotonicNanos_NonZero confirms the helper returns a sane value and
// that two calls separated by 1 ms produce strictly increasing values.
// Wall-clock based, so an NTP step during the test would make this flaky;
// keep the gap short to minimize that window.
func TestMonotonicNanos_NonZero(t *testing.T) {
	t0 := MonotonicNanos()
	if t0 == 0 {
		t.Fatal("MonotonicNanos returned 0")
	}
	time.Sleep(1 * time.Millisecond)
	t1 := MonotonicNanos()
	if t1 <= t0 {
		t.Fatalf("MonotonicNanos went backward or flat: t0=%d t1=%d", t0, t1)
	}
	if t1-t0 < 500_000 { // 0.5 ms — sanity floor, well under the 1 ms sleep
		t.Fatalf("MonotonicNanos delta suspiciously small: %d ns", t1-t0)
	}
}

// TestLeaseField_WriteableViaAtomic exercises the wire-level contract: the
// Lease field can be atomically stored and reloaded. The shared-memory
// counterpart (real Host claiming a slot) does the same StoreUint64; this
// test catches any unintentional change to the field type or alignment.
func TestLeaseField_WriteableViaAtomic(t *testing.T) {
	var h SlotHeader
	want := MonotonicNanos()
	atomic.StoreUint64(&h.Lease, want)
	got := atomic.LoadUint64(&h.Lease)
	if got != want {
		t.Fatalf("atomic round-trip mismatch: stored=%d loaded=%d", want, got)
	}
}
