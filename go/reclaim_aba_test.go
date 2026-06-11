package shm

import (
	"sync"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// TestReclaim_GenOffset104 pins the v0.7.5 claim-generation field at byte
// offset 104, immediately after Lease (96..104). The C++ SlotHeader places
// std::atomic<uint64_t> gen at the same offset; a mismatch breaks the ABI.
func TestReclaim_GenOffset104(t *testing.T) {
	var h SlotHeader
	if off := unsafe.Offsetof(h.Gen); off != 104 {
		t.Fatalf("Gen offset = %d, want 104 (ABI parity with C++ SlotHeader::gen)", off)
	}
	if sz := unsafe.Sizeof(h); sz != 128 {
		t.Fatalf("SlotHeader size = %d, want 128 (ABI)", sz)
	}
}

// Regression for the TryReclaimAbandonedSlot ABA window
// (IMPROVEMENT_BACKLOG.md §2 MEDIUM shm; direct.go:408-420).
//
// The bug: the function decided staleness from a lease value it loaded
// early, then later CAS'd State observed->SlotFree. Between those two
// points a peer could legitimately:
//   (a) finish the slot (State -> SlotFree),
//   (b) have the slot reused, and
//   (c) re-claim it so State landed back on the SAME observed value, now
//       backed by a FRESH lease (written per §3.6 right after the
//       claiming CAS).
// The bare CAS then succeeded and reclaimed a now-busy, freshly-leased,
// LIVE slot — classic ABA. The staleness check had been made against the
// OLD lease; the new owner's fresh lease was never re-checked.
//
// The fix re-validates the lease immediately before the CAS and refuses
// if it changed. These tests drive the exact window deterministically via
// the reclaimPreCASHook seam: the hook simulates the peer's
// finish-reuse-reclaim happening after the staleness decision but before
// the CAS.

// TestTryReclaim_ABA_ReusedSlotSameState is the core regression. It
// reproduces the A->B->A state path with a refreshed lease squarely in
// the window. On the pre-fix code the reclaim wrongly SUCCEEDS (CAS sees
// State back at SlotBusy); the post-fix code REFUSES because the lease
// changed.
func TestTryReclaim_ABA_ReusedSlotSameState(t *testing.T) {
	h := newReclaimHarness(t, 1)

	// Initial: abandoned-looking slot. State=SlotBusy, lease 1s stale.
	staleLease := MonotonicNanos() - uint64(time.Second.Nanoseconds())
	atomic.StoreUint32(&h.headers[0].State, SlotBusy)
	atomic.StoreUint64(&h.headers[0].Lease, staleLease)

	// In the ABA window: the original owner finished (State->SlotFree),
	// the slot was reused, and a new live owner re-claimed it back to
	// SlotBusy and published a FRESH lease (§3.6). State ends up at the
	// SAME value (SlotBusy) the reclaimer observed — the "A" in ABA.
	var hookFired int32
	freshLease := MonotonicNanos()
	reclaimPreCASHook = func(slotIdx int) {
		atomic.AddInt32(&hookFired, 1)
		atomic.StoreUint32(&h.headers[slotIdx].State, SlotFree) // A -> B (finish)
		// New owner re-claims: bump Gen (§3.6.1) BEFORE the state CAS, then
		// publish the fresh lease AFTER it — the protocol ordering that
		// exposes the lease-publication-lag the lease re-load alone misses.
		claimSlotGen(&h.headers[slotIdx])
		atomic.StoreUint32(&h.headers[slotIdx].State, SlotBusy)   // B -> A (re-claim)
		atomic.StoreUint64(&h.headers[slotIdx].Lease, freshLease) // new owner heartbeat
	}
	t.Cleanup(func() { reclaimPreCASHook = nil })

	got := h.guest.TryReclaimAbandonedSlot(0, 100*time.Millisecond)

	if atomic.LoadInt32(&hookFired) != 1 {
		t.Fatalf("ABA injection hook fired %d times, want 1 (test seam not exercised)", hookFired)
	}
	if got {
		t.Fatal("TryReclaimAbandonedSlot reclaimed a re-claimed, freshly-leased LIVE slot (ABA hazard): want false")
	}
	if state := atomic.LoadUint32(&h.headers[0].State); state != SlotBusy {
		t.Fatalf("post-call state = %d, want SlotBusy (%d): reclaimer must not have stolen the live slot", state, SlotBusy)
	}
	if lease := atomic.LoadUint64(&h.headers[0].Lease); lease != freshLease {
		t.Fatalf("post-call lease = %d, want fresh lease %d preserved", lease, freshLease)
	}
}

// TestTryReclaim_ABA_LeasePublicationLag is the residual-window regression
// that a bare lease re-load CANNOT catch and that the generation counter
// closes. It models the protocol-exact ordering of §3.6 / §3.6.1: a new
// owner bumps Gen, CAS's State SlotFree->SlotBusy, and ONLY THEN publishes
// its fresh lease. The reclaimer's window lands AFTER the state re-claim
// but BEFORE the lease store — so the lease still reads the STALE value
// (lease re-load passes!) while the slot is already live.
//
// Without the Gen guard the reclaimer's CAS(SlotBusy->SlotFree) succeeds
// and frees a live slot. With it, the bumped Gen is detected and the
// reclaim is refused.
func TestTryReclaim_ABA_LeasePublicationLag(t *testing.T) {
	h := newReclaimHarness(t, 1)

	staleLease := MonotonicNanos() - uint64(time.Second.Nanoseconds())
	atomic.StoreUint32(&h.headers[0].State, SlotBusy)
	atomic.StoreUint64(&h.headers[0].Lease, staleLease)

	reclaimPreCASHook = func(slotIdx int) {
		// New owner re-claims but its lease store has NOT landed yet:
		// State is back at SlotBusy, lease is STILL the stale value.
		atomic.StoreUint32(&h.headers[slotIdx].State, SlotFree)
		claimSlotGen(&h.headers[slotIdx]) // Gen bumped BEFORE the state CAS
		atomic.StoreUint32(&h.headers[slotIdx].State, SlotBusy)
		// NOTE: deliberately do NOT store the fresh lease — the lag window.
	}
	t.Cleanup(func() { reclaimPreCASHook = nil })

	got := h.guest.TryReclaimAbandonedSlot(0, 100*time.Millisecond)

	// Sanity: confirm the lease re-load alone would NOT have caught this.
	if atomic.LoadUint64(&h.headers[0].Lease) != staleLease {
		t.Fatalf("test setup error: lease changed (%d), the lag window requires it to stay stale", h.headers[0].Lease)
	}
	if got {
		t.Fatal("reclaimed a live slot during the lease-publication-lag window (Gen guard failed): want false")
	}
	if state := atomic.LoadUint32(&h.headers[0].State); state != SlotBusy {
		t.Fatalf("post-call state = %d, want SlotBusy (%d): live slot must survive", state, SlotBusy)
	}
}

// TestTryReclaim_ABA_HeartbeatInWindow covers the simpler in-window
// heartbeat race: the live owner did not change State, it merely refreshed
// its lease between the reclaimer's observation and the CAS. The slot is
// NOT abandoned. Pre-fix this slipped through (State unchanged -> CAS wins);
// post-fix the changed lease causes a refusal.
func TestTryReclaim_ABA_HeartbeatInWindow(t *testing.T) {
	h := newReclaimHarness(t, 1)

	staleLease := MonotonicNanos() - uint64(time.Second.Nanoseconds())
	atomic.StoreUint32(&h.headers[0].State, SlotGuestBusy)
	atomic.StoreUint64(&h.headers[0].Lease, staleLease)

	freshLease := MonotonicNanos()
	reclaimPreCASHook = func(slotIdx int) {
		// Live owner heartbeats; State stays put.
		atomic.StoreUint64(&h.headers[slotIdx].Lease, freshLease)
	}
	t.Cleanup(func() { reclaimPreCASHook = nil })

	if h.guest.TryReclaimAbandonedSlot(0, 100*time.Millisecond) {
		t.Fatal("reclaimed a slot whose owner heartbeated in the window: want false")
	}
	if state := atomic.LoadUint32(&h.headers[0].State); state != SlotGuestBusy {
		t.Fatalf("post-call state = %d, want SlotGuestBusy (%d)", state, SlotGuestBusy)
	}
}

// TestTryReclaim_ABA_GenuinelyAbandonedStillReclaims is the negative
// control: when NOTHING happens in the window (hook is a no-op, lease
// unchanged and still stale), the genuinely abandoned slot must still be
// reclaimed. This guards against the ABA fix over-rotating into refusing
// all reclamation.
func TestTryReclaim_ABA_GenuinelyAbandonedStillReclaims(t *testing.T) {
	h := newReclaimHarness(t, 1)

	atomic.StoreUint32(&h.headers[0].State, SlotBusy)
	atomic.StoreUint64(&h.headers[0].Lease, MonotonicNanos()-uint64(time.Second.Nanoseconds()))

	var hookFired int32
	reclaimPreCASHook = func(int) { atomic.AddInt32(&hookFired, 1) } // no-op: nothing changes
	t.Cleanup(func() { reclaimPreCASHook = nil })

	if !h.guest.TryReclaimAbandonedSlot(0, 100*time.Millisecond) {
		t.Fatal("genuinely abandoned slot (stale lease, no in-window change) was NOT reclaimed: want true")
	}
	if atomic.LoadInt32(&hookFired) != 1 {
		t.Fatalf("hook fired %d times, want 1", hookFired)
	}
	if state := atomic.LoadUint32(&h.headers[0].State); state != SlotFree {
		t.Fatalf("post-reclaim state = %d, want SlotFree (%d)", state, SlotFree)
	}
}

// TestTryReclaim_ABA_ConcurrentReuseStress runs the crash-recovery scenario
// faithfully under concurrency. The contract (SPEC §3.6) targets a CRASHED
// owner: the original owner stopped heartbeating and never relinquishes its
// slot. A DISTINCT new owner can therefore only ever claim the slot AFTER
// it becomes SlotFree — it never CAS's the original's busy state directly.
//
// We race two parties on a slot seeded as abandoned (stale lease, busy):
//   - reclaimer: TryReclaimAbandonedSlot,
//   - newOwner: spins on CAS(SlotFree -> SlotGuestBusy) and, on success,
//     bumps Gen first (§3.6.1) then publishes a fresh lease (§3.6), marking
//     itself live for the rest of the round.
//
// The no-double-claim invariant: the new owner and the reclaimer must
// linearize. Concretely, if the new owner ever holds the slot (won the
// claim CAS), the reclaimer must NOT subsequently free it — checked by
// having the new owner, after going live, assert it still observes itself
// as the owner (State busy + its fresh lease). A reclaim that frees the
// live new owner trips this. The Gen handshake in TryReclaimAbandonedSlot
// is what makes the claim and the reclaim mutually exclusive.
func TestTryReclaim_ABA_ConcurrentReuseStress(t *testing.T) {
	const rounds = 20000
	h := newReclaimHarness(t, 1)

	for r := 0; r < rounds; r++ {
		// Seed an abandoned-looking slot: stale lease, State busy. The
		// "crashed" original owner never touches it again this round.
		staleLease := MonotonicNanos() - uint64(time.Second.Nanoseconds())
		atomic.StoreUint32(&h.headers[0].State, SlotBusy)
		atomic.StoreUint64(&h.headers[0].Lease, staleLease)

		var wg sync.WaitGroup
		start := make(chan struct{})
		freshLease := MonotonicNanos()

		// New owner: may ONLY claim a free slot (it is a different process
		// than the crashed owner). It wins the slot only if the reclaimer
		// freed it first. A clean handoff (reclaim -> new claim) is legal;
		// a steal (new owner live, then reclaimer frees) is the bug.
		var newOwnerLive int32
		var stealObserved int32
		wg.Add(1)
		go func() {
			defer wg.Done()
			<-start
			for i := 0; i < 1000; i++ {
				if atomic.LoadUint32(&h.headers[0].State) != SlotFree {
					continue
				}
				claimSlotGen(&h.headers[0]) // Gen bump BEFORE the claim CAS (§3.6.1)
				if atomic.CompareAndSwapUint32(&h.headers[0].State, SlotFree, SlotGuestBusy) {
					atomic.StoreUint64(&h.headers[0].Lease, freshLease) // publish lease (§3.6)
					atomic.StoreInt32(&newOwnerLive, 1)
					// We are the live owner now. The reclaimer must never
					// free us. Watch for a steal for the rest of the round.
					for k := 0; k < 200; k++ {
						if atomic.LoadUint32(&h.headers[0].State) != SlotGuestBusy {
							atomic.StoreInt32(&stealObserved, 1)
							return
						}
					}
					return
				}
			}
		}()

		wg.Add(1)
		go func() {
			defer wg.Done()
			<-start
			h.guest.TryReclaimAbandonedSlot(0, 100*time.Millisecond)
		}()

		close(start)
		wg.Wait()

		// Invariant 1: the reclaimer must never have freed the new owner
		// while it held the slot.
		if atomic.LoadInt32(&stealObserved) == 1 {
			t.Fatalf("round %d: reclaimer freed the slot out from under a live new owner — ABA double-claim", r)
		}
		// Invariant 2: terminal consistency. If the new owner is live, the
		// final state must reflect it; if not, the reclaimer's free must
		// leave the slot recoverable (SlotFree) or untouched-busy.
		if atomic.LoadInt32(&newOwnerLive) == 1 {
			if st := atomic.LoadUint32(&h.headers[0].State); st != SlotGuestBusy {
				t.Fatalf("round %d: new owner live but final state = %d (want SlotGuestBusy) — slot stolen post-claim", r, st)
			}
			if ls := atomic.LoadUint64(&h.headers[0].Lease); ls != freshLease {
				t.Fatalf("round %d: new owner live but lease = %d (want fresh) — ownership lost", r, ls)
			}
		}
	}
}
