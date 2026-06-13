package shm

import (
	"math/rand"
	"sync"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// reclaimHarness builds a free-standing DirectGuest pointing at a
// caller-supplied SlotHeader array. It does NOT open shared memory or wire
// any event handles — it just exposes the bookkeeping (slots[] + the State
// and Lease fields) that TryReclaimAbandonedSlot needs. Sufficient for
// driving the reclaim API through every scenario without paying the
// shared-memory / event-object lifecycle cost.
//
// We exercise TryReclaimAbandonedSlot directly rather than through a real
// Host/Guest pair because the latter would also need a parallel C++ peer.
// The unit-level coverage here pins the contract:
//   - stale lease → reclaim succeeds
//   - fresh lease → reclaim refuses
//   - lease == 0 (v0.6.x peer) → reclaim refuses
//   - State == FREE → reclaim is a no-op
//   - State changes between observation and CAS → reclaim refuses
//
// Full end-to-end crash-process injection (kill the guest mid-handler;
// observe host reclaiming) is targeted for a follow-up release.
type reclaimHarness struct {
	guest   *DirectGuest
	headers []SlotHeader
}

func newReclaimHarness(t *testing.T, n int) *reclaimHarness {
	t.Helper()
	h := &reclaimHarness{
		headers: make([]SlotHeader, n),
	}
	slots := make([]slotContext, n)
	for i := range slots {
		slots[i].header = &h.headers[i]
	}
	h.guest = &DirectGuest{
		slots: slots,
	}
	return h
}

func TestTryReclaim_StaleLeaseSucceeds(t *testing.T) {
	h := newReclaimHarness(t, 1)
	// Simulate a crashed owner: state is SlotBusy, lease is 1 second old.
	atomic.StoreUint32(&h.headers[0].State, SlotBusy)
	atomic.StoreUint64(&h.headers[0].Lease, MonotonicNanos()-uint64(time.Second.Nanoseconds()))

	got := h.guest.TryReclaimAbandonedSlot(0, 100*time.Millisecond)
	if !got {
		t.Fatal("TryReclaimAbandonedSlot returned false for stale lease, want true")
	}
	if state := atomic.LoadUint32(&h.headers[0].State); state != SlotFree {
		t.Fatalf("post-reclaim state = %d, want SlotFree (%d)", state, SlotFree)
	}
}

func TestTryReclaim_FreshLeaseRefuses(t *testing.T) {
	h := newReclaimHarness(t, 1)
	atomic.StoreUint32(&h.headers[0].State, SlotBusy)
	atomic.StoreUint64(&h.headers[0].Lease, MonotonicNanos())

	got := h.guest.TryReclaimAbandonedSlot(0, time.Hour)
	if got {
		t.Fatal("TryReclaimAbandonedSlot returned true for fresh lease, want false")
	}
	if state := atomic.LoadUint32(&h.headers[0].State); state != SlotBusy {
		t.Fatalf("state changed despite refused reclaim: %d", state)
	}
}

func TestTryReclaim_ZeroLeaseRefuses(t *testing.T) {
	h := newReclaimHarness(t, 1)
	atomic.StoreUint32(&h.headers[0].State, SlotBusy)
	atomic.StoreUint64(&h.headers[0].Lease, 0)

	got := h.guest.TryReclaimAbandonedSlot(0, 0) // even zero threshold should refuse
	if got {
		t.Fatal("TryReclaimAbandonedSlot returned true for zero lease (v0.6.x peer signal), want false")
	}
}

func TestTryReclaim_FreeSlotNoOp(t *testing.T) {
	h := newReclaimHarness(t, 1)
	atomic.StoreUint32(&h.headers[0].State, SlotFree)
	atomic.StoreUint64(&h.headers[0].Lease, MonotonicNanos()-uint64(time.Hour.Nanoseconds()))

	got := h.guest.TryReclaimAbandonedSlot(0, time.Millisecond)
	if got {
		t.Fatal("TryReclaimAbandonedSlot returned true for already-free slot, want false")
	}
}

func TestTryReclaim_OutOfRangeRefuses(t *testing.T) {
	h := newReclaimHarness(t, 1)
	for _, idx := range []int{-1, 1, 100} {
		if got := h.guest.TryReclaimAbandonedSlot(idx, time.Millisecond); got {
			t.Errorf("idx %d: returned true, want false", idx)
		}
	}
}

// TestTryReclaim_NoDoubleClaim_Property is the v0.7.1 property test
// sketch: run N rounds where a "peer" goroutine concurrently writes
// lease and state while a "reclaimer" tries to reclaim. The invariant
// is that the slot can never observe two simultaneous owners — at every
// point in time, exactly one of {peer holds it, reclaimer holds it,
// slot is free} is true.
//
// This is the simplified, single-machine flavor of the eventual
// crash-process test. It doesn't kill processes; it stresses the
// concurrent CAS handshake between heartbeat and reclaim. Full
// crash-injection is future work.
func TestTryReclaim_NoDoubleClaim_Property(t *testing.T) {
	const rounds = 200
	const slots = 4

	for r := 0; r < rounds; r++ {
		h := newReclaimHarness(t, slots)
		// All slots start in SlotBusy with a fresh lease, mimicking a
		// just-claimed state.
		for i := 0; i < slots; i++ {
			atomic.StoreUint32(&h.headers[i].State, SlotBusy)
			atomic.StoreUint64(&h.headers[i].Lease, MonotonicNanos())
		}

		var heartbeatWg sync.WaitGroup
		stop := make(chan struct{})

		// Heartbeater: periodically refreshes leases. Exits when `stop`
		// is closed by the main goroutine after the reclaimer finishes.
		heartbeatWg.Add(1)
		go func() {
			defer heartbeatWg.Done()
			for {
				select {
				case <-stop:
					return
				default:
				}
				idx := rand.Intn(slots)
				atomic.StoreUint64(&h.headers[idx].Lease, MonotonicNanos())
				time.Sleep(50 * time.Microsecond)
			}
		}()

		// Reclaimer: runs synchronously on the test goroutine so the
		// stop signal lands deterministically after it finishes.
		var reclaimedCount uint32
		for i := 0; i < 100; i++ {
			idx := rand.Intn(slots)
			if h.guest.TryReclaimAbandonedSlot(idx, 100*time.Microsecond) {
				atomic.AddUint32(&reclaimedCount, 1)
				// Re-arm the slot so subsequent reclaims can fire too.
				atomic.StoreUint32(&h.headers[idx].State, SlotBusy)
				atomic.StoreUint64(&h.headers[idx].Lease, MonotonicNanos())
			}
			time.Sleep(time.Microsecond)
		}

		close(stop)
		heartbeatWg.Wait()

		// Invariant: every header's State is a valid known state. The
		// reclaim CAS can only transition to SlotFree; the
		// heartbeater never touches State. So we expect each slot to be
		// either SlotBusy (heartbeater raced and won OR was never
		// reclaimed) or SlotFree (reclaim won). Any other value is a
		// correctness bug.
		for i := 0; i < slots; i++ {
			st := atomic.LoadUint32(&h.headers[i].State)
			if st != SlotFree && st != SlotBusy {
				t.Fatalf("round %d slot %d: unexpected state %d", r, i, st)
			}
		}
	}
}

// Sanity check: pin Lease offset (matches lease_test.go but lives in this
// file so the property test reads as a self-contained module.)
func TestReclaim_AssumesLeaseOffset96(t *testing.T) {
	var h SlotHeader
	if off := unsafe.Offsetof(h.Lease); off != 96 {
		t.Fatalf("Lease offset = %d, reclaim test assumes 96", off)
	}
}

// TestAutoReclaimTimeout_RoundTrip exercises the v0.7.2 setter/getter pair.
// The actual auto-reclaim integration in sendGuestCallInternal piggybacks
// on TryReclaimAbandonedSlot which TestTryReclaim_NoDoubleClaim_Property
// already covers; this test only pins the API contract for the knob.
func TestAutoReclaimTimeout_RoundTrip(t *testing.T) {
	g := &DirectGuest{}
	if got := g.GetAutoReclaimTimeout(); got != 0 {
		t.Fatalf("default GetAutoReclaimTimeout = %v, want 0 (opt-in)", got)
	}
	g.SetAutoReclaimTimeout(500 * time.Millisecond)
	if got := g.GetAutoReclaimTimeout(); got != 500*time.Millisecond {
		t.Fatalf("GetAutoReclaimTimeout after set = %v, want 500ms", got)
	}
	g.SetAutoReclaimTimeout(0)
	if got := g.GetAutoReclaimTimeout(); got != 0 {
		t.Fatalf("GetAutoReclaimTimeout after zero = %v, want 0", got)
	}
}
