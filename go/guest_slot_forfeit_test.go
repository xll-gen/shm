package shm

import (
	"sync/atomic"
	"testing"
	"time"
)

// TestGuestSlot_ForfeitedHandleCannotClobberNewOwner pins the ownership
// boundary that the `lost` flag draws.
//
// Two live *GuestSlot handles on ONE slotContext is a legitimate, by-design
// state: after a Send times out the caller still holds its handle, while the
// Case-2 zombie reclaim (AcquireGuestSlot, SPEC §3.6.1) or lease reclamation
// may hand the same slot to a new owner. Before this fix `lost` was consulted
// in exactly one place — Release() — so the forfeited handle could still
// Send: that writes ReqSize / MsgType / MsgSeq, stores ActiveWait=1 and
// publishes SlotReqReady straight over the new owner's in-flight transaction,
// and races the new owner on the (non-atomic) slotContext.nextMsgSeq counter.
//
// The assertion is deterministic — it does not rely on -race catching an
// interleaving. The forfeited Send must be refused before touching anything.
func TestGuestSlot_ForfeitedHandleCannotClobberNewOwner(t *testing.T) {
	env := newZombieStealEnv(t, "ForfeitClobberSHM", 1)
	defer env.Close()

	guest, err := NewDirectGuest(env.name)
	if err != nil {
		t.Fatalf("NewDirectGuest failed: %v", err)
	}
	defer guest.Close()

	ctx := &guest.slots[1]
	hdr := env.slotHeader(1)
	stride := uint32(len(guest.slots))

	// A acquires the slot and times out: no host is answering.
	a, err := guest.AcquireGuestSlot()
	if err != nil {
		t.Fatalf("AcquireGuestSlot(A) failed: %v", err)
	}
	seqA := ctx.nextMsgSeq
	copy(a.RequestBuffer(), "ping")
	if _, _, err := a.SendWithTimeout(4, MsgTypeGuestCall, 50*time.Millisecond); err == nil {
		t.Fatal("A.SendWithTimeout should have timed out")
	}
	if !a.lost {
		t.Fatal("a timed-out Send must forfeit ownership (lost=true)")
	}
	if got := ctx.nextMsgSeq; got != seqA+stride {
		t.Fatalf("nextMsgSeq after A's send = %d, want %d", got, seqA+stride)
	}

	// The host's late response turns the slot into a zombie, and the Case-2
	// reclaim hands it to a NEW owner B while A's handle is still alive.
	hdr.RespSize = 0
	hdr.MsgType = MsgTypeNormal
	atomic.StoreUint32(&hdr.State, SlotRespReady)

	b, err := guest.AcquireGuestSlot()
	if err != nil {
		t.Fatalf("Case-2 zombie reclaim failed to hand the slot to a new owner: %v", err)
	}
	if b.slot != a.slot {
		t.Fatalf("test setup: B got a different slotContext (%p) than A (%p)", b.slot, a.slot)
	}

	// Snapshot everything B's transaction depends on.
	var (
		wantState   = atomic.LoadUint32(&hdr.State)
		wantReqSize = hdr.ReqSize
		wantMsgType = hdr.MsgType
		wantMsgSeq  = hdr.MsgSeq
		wantGen     = atomic.LoadUint64(&hdr.Gen)
		wantSeq     = ctx.nextMsgSeq
	)
	if wantState != SlotGuestBusy {
		t.Fatalf("new owner's slot state = %d, want SlotGuestBusy (%d)", wantState, SlotGuestBusy)
	}

	// A retries on its forfeited handle. This must be refused instantly and
	// must not touch a single byte of B's transaction.
	start := time.Now()
	if _, _, err := a.SendWithTimeout(4, MsgTypeGuestCall, 2*time.Second); err == nil {
		t.Fatal("Send on a forfeited handle returned nil; it published a request into the new owner's slot")
	}
	if elapsed := time.Since(start); elapsed > 500*time.Millisecond {
		t.Errorf("forfeited Send blocked for %v; it must be refused before publishing anything", elapsed)
	}
	if a.RequestBuffer() != nil {
		t.Error("RequestBuffer on a forfeited handle must be nil — it belongs to the new owner")
	}
	if a.ResponseBuffer() != nil {
		t.Error("ResponseBuffer on a forfeited handle must be nil — it belongs to the new owner")
	}

	if got := atomic.LoadUint32(&hdr.State); got != wantState {
		t.Errorf("header.State = %d, want %d — forfeited Send clobbered the new owner's state", got, wantState)
	}
	if hdr.ReqSize != wantReqSize {
		t.Errorf("header.ReqSize = %d, want %d — forfeited Send overwrote it", hdr.ReqSize, wantReqSize)
	}
	if hdr.MsgType != wantMsgType {
		t.Errorf("header.MsgType = %d, want %d — forfeited Send overwrote it", hdr.MsgType, wantMsgType)
	}
	if hdr.MsgSeq != wantMsgSeq {
		t.Errorf("header.MsgSeq = %d, want %d — forfeited Send overwrote it", hdr.MsgSeq, wantMsgSeq)
	}
	if got := atomic.LoadUint64(&hdr.Gen); got != wantGen {
		t.Errorf("header.Gen = %d, want %d — forfeited Send advanced the claim generation", got, wantGen)
	}
	if got := atomic.LoadInt32(&ctx.ActiveWait); got != 0 {
		t.Errorf("ActiveWait = %d, want 0 — forfeited Send installed itself as the active waiter", got)
	}
	// The sequencing invariant the -race report was pointing at: exactly one
	// owner advances nextMsgSeq per transaction.
	if got := ctx.nextMsgSeq; got != wantSeq {
		t.Errorf("nextMsgSeq = %d, want %d — forfeited Send consumed the new owner's sequence number", got, wantSeq)
	}

	a.Release() // no-op on a forfeited handle; must not free B's slot
	if got := atomic.LoadUint32(&hdr.State); got != wantState {
		t.Fatalf("header.State = %d after A.Release, want %d", got, wantState)
	}

	// B's transaction must still work end to end, with the next sequence
	// number in the slot's series.
	hostDone := make(chan struct{})
	go func() {
		defer close(hostDone)
		env.respondOnce(t, 1, []byte("PONG"))
	}()

	copy(b.RequestBuffer(), "pong")
	n, _, err := b.SendWithTimeout(4, MsgTypeGuestCall, 5*time.Second)
	<-hostDone
	if err != nil {
		t.Fatalf("new owner's Send failed: %v", err)
	}
	if hdr.MsgSeq != wantSeq {
		t.Errorf("new owner used MsgSeq %d, want %d", hdr.MsgSeq, wantSeq)
	}
	if got := string(b.ResponseBuffer()[:n]); got != "PONG" {
		t.Errorf("new owner's response = %q, want %q", got, "PONG")
	}
	b.Release()
}

// TestGuestSlot_SendAfterReleaseIsRefused pins the other half of the same
// boundary: a released handle no longer owns anything either. It used to nil
// -dereference inside SendWithTimeout (s.slot.header); after Release the slot
// is very likely owned by someone else, so this must be a clean error.
func TestGuestSlot_SendAfterReleaseIsRefused(t *testing.T) {
	env := newZombieStealEnv(t, "ForfeitReleasedSHM", 1)
	defer env.Close()

	guest, err := NewDirectGuest(env.name)
	if err != nil {
		t.Fatalf("NewDirectGuest failed: %v", err)
	}
	defer guest.Close()

	s, err := guest.AcquireGuestSlot()
	if err != nil {
		t.Fatalf("AcquireGuestSlot failed: %v", err)
	}
	s.Release()

	if _, _, err := s.SendWithTimeout(4, MsgTypeGuestCall, time.Second); err == nil {
		t.Fatal("Send on a released handle returned nil")
	}
	if s.RequestBuffer() != nil || s.ResponseBuffer() != nil {
		t.Error("buffers of a released handle must be nil")
	}
	if got := atomic.LoadUint32(&env.slotHeader(1).State); got != SlotFree {
		t.Fatalf("slot state = %d after Release + refused Send, want SlotFree", got)
	}
}
